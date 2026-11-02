# Overlay Branch Internal Implementation

> Implementation of the `overlay_branch` extension: architecture, BranchScan,
> concurrency control, keyless ctid identity and performance.
> The only user-facing companion document is [example_sql.md](example_sql.md)
> (runnable SQL examples); everything internal lives here.

---

## Contents

- [0. Module structure](#0-module-structure)
- [1. Architecture and basic implementation](#1-architecture-and-basic-implementation)
  - [1.1 Overview & the four-layer architecture](#11-overview--the-four-layer-architecture)
  - [1.2 Branch model: write redirect + read overlay](#12-branch-model-write-redirect--read-overlay)
  - [1.3 Apply semantics & conflict detection](#13-apply-semantics--conflict-detection)
  - [1.4 Catalog tables](#14-catalog-tables)
  - [1.5 The PK as row identity](#15-the-pk-as-row-identity)
  - [1.6 Incremental write path](#16-incremental-write-path)
  - [1.7 Incremental read path](#17-incremental-read-path)
  - [1.8 Scope & gating + ACL double-defense](#18-scope--gating--acl-double-defense)
- [2. BranchScan implementation](#2-branchscan-implementation)
  - [2.1 Injection mechanism](#21-injection-mechanism)
  - [2.2 The 2-pass overlay algorithm and the streaming iterator](#22-the-2-pass-overlay-algorithm-and-the-streaming-iterator)
  - [2.3 P0 PK-equality fast path (`WHERE pk = const`)](#23-p0-pk-equality-fast-path-where-pk--const)
  - [2.4 Predicate pushdown for `IN` / range / `OR`](#24-predicate-pushdown-for-in--range--or)
  - [2.5 DML-target BranchScan (ACC-1)](#25-dml-target-branchscan-acc-1)
  - [2.6 Executor unpacking protocol](#26-executor-unpacking-protocol)
  - [2.7 ReScan semantics](#27-rescan-semantics)
  - [2.8 MemoryContext lifetime hard constraint](#28-memorycontext-lifetime-hard-constraint)
- [3. Concurrency control](#3-concurrency-control)
  - [3.1 The four-layer concurrency defense tower](#31-the-four-layer-concurrency-defense-tower)
  - [3.2 Layer 1 - cross-session state invalidation](#32-layer-1---cross-session-state-invalidation)
  - [3.3 Layer 2 - branch-level advisory lock](#33-layer-2---branch-level-advisory-lock)
  - [3.4 Layer 3 - `applying` transient state + delta-write guard](#34-layer-3---applying-transient-state--delta-write-guard)
  - [3.5 Layer 4 - SNAPSHOT vs LIVE mode](#35-layer-4---snapshot-vs-live-mode)
  - [3.6 FR6 BranchScan ReScan freshness](#36-fr6-branchscan-rescan-freshness)
  - [3.7 Apply/discard idempotency and the D→U→I replay](#37-applydiscard-idempotency-and-the-dui-replay)
  - [3.8 Mandatory engineering conventions](#38-mandatory-engineering-conventions)
- [4. Keyless ctid identity (experimental)](#4-keyless-ctid-identity-experimental)
  - [4.1 Goals & non-goals](#41-goals--non-goals)
  - [4.2 GUC semantics](#42-guc-semantics)
  - [4.3 Anchor identity encoding](#43-anchor-identity-encoding)
  - [4.4 Write / read / apply path mapping](#44-write--read--apply-path-mapping)
  - [4.5 Anchor-invalidation triggers](#45-anchor-invalidation-triggers)
  - [4.6 Persistence & retention](#46-persistence--retention)
  - [4.7 MVP scope](#47-mvp-scope)
- [5. Performance](#5-performance)
  - [5.1 Performance model & notation](#51-performance-model--notation)
  - [5.2 Identified bottlenecks / first-round measurement conclusions](#52-identified-bottlenecks--first-round-measurement-conclusions)
  - [5.3 Read-path optimizations](#53-read-path-optimizations)
  - [5.4 Write-path optimization WR-1 and redundant index](#54-write-path-optimization-wr-1-and-redundant-index)
  - [5.5 Apply batching (AP-1)](#55-apply-batching-ap-1)
  - [5.6 Delta size: current state and observability](#56-delta-size-current-state-and-observability)
  - [5.7 Measurement pitfalls](#57-measurement-pitfalls)
  - [5.8 Baseline methodology](#58-baseline-methodology)
- [6. Key invariants and corner cases](#6-key-invariants-and-corner-cases)
  - [6.1 Invariants](#61-invariants)
  - [6.2 Corner cases](#62-corner-cases)

---

## 0. Module structure

| File | Responsibility |
|------|------|
| [overlay_branch.c](../src/overlay_branch.c) | `_PG_init`, GUCs, executor + utility hooks, guards, SQL dispatch, `overlay_main_plus_delta` SRF |
| [branch_lifecycle.c](../src/branch_lifecycle.c) | create/use/apply/discard, advisory lock + CAS, snapshot/mode caches, NOTIFY, invalidation, ACL |
| [write_redirect.c](../src/write_redirect.c) | redirect INSERT/UPDATE/DELETE, pure-delta post-processing, UPSERT promotion, RETURNING |
| [delta_store.c](../src/delta_store.c) | PK/tuple serialization, `old_version` token, delta-table SPI, typed PK compare |
| [branch_scan.c](../src/branch_scan.c) | planner hook, CustomScan 2-pass overlay, cost model, pathlist pruning |

All logic is in the five `.c` files; `include/*.h` holds declarations only.

---

## 1. Architecture and basic implementation

### 1.1 Overview & the four-layer architecture

`overlay_branch` adds git-like draft branches over plain heap tables: MAIN is
never copied or written inside a branch, which stores only its own delta. Reads
overlay MAIN with the delta; the branch is resolved with `apply_branch` (3-pass
merge) or `discard_branch`. The real primitive is **Overlay Scan + Redirected
Modify + Transactional Apply**; `create_branch` is a thin SQL skin over it.

- **Layer 1 — Branch Context**: `BranchExecContext` (`bid`, `mode`, `stmt_snap`)
  is bound once per statement in `overlay_ExecutorStart` and kept on a fixed
  backend-local stack rather than in `es_query_cxt` (an error abort skips
  `ExecutorEnd`); recovery runs through the transaction and subtransaction-abort
  callbacks.
- **Layer 2 — Write Redirect**: intercepts at `ExecutorRun` before `ModifyTable`,
  routing mutations into the Delta Store so no MAIN heap write occurs.
- **Layer 3 — BranchScan**: a `CustomScan` replacing ordinary relation scans
  while a branch is active, producing the MAIN ⊕ Delta overlay.
- **Layer 4 — Delta Store**: latest-wins tuple deltas per
  `(branch_id, relid, pk)`.

### 1.2 Branch model: write redirect + read overlay

A branch is `{ base_snapshot, delta_tables, metadata }`. In branch mode an
UPDATE/DELETE does not touch MAIN; it writes a delta row
`(branch_id, op, pk, new_tuple, old_version)`. Reads are
`BranchScan(t) = BaseScan(t @ base_snapshot) ⊕ DeltaScan(t)` with delta priority:
a delta UPDATE overwrites the base tuple, a delta DELETE masks it, and a delta
INSERT is additionally returned.

- **Live** (default): Read Committed MAIN + overlay — the branch sees the latest
  MAIN unless it modified the row itself. A workspace, not a strict fork.
- **Snapshot**: freezes the MAIN read view on entry; later MAIN writes are
  invisible while the delta stays latest-committed. Bounded by
  `overlay_branch.snapshot_max_hold_minutes`; on timeout the session is forcibly
  exited so
  long-lived snapshotters cannot pin the MVCC horizon.

### 1.3 Apply semantics & conflict detection

`apply_branch` re-validates the branch result and commits it as a new MAIN
transaction (not `MERGE`) in an independent SPI transaction; any conflict rolls
everything back. Order is **Pass 1 DELETE** (child→parent FK safe), **Pass 2
UPDATE** (conflict detection here), **Pass 3 INSERT** (parent→child FK safe).

Conflict detection uses `old_version = "<blk>:<off>-x<xmin>"` (`ctid:xmin`): the
MAIN row is fetched by `_base_ctid` and its `xmin` must equal the recorded
`_base_xmin` (mismatch → ERROR 55000); pure-origin rows skip the check. A single
codec (`overlay_base_ver_*`) is shared by the write path, apply and the read
path. **HOT secondary check**: since a HOT update leaves `ctid`/`xmin` unchanged
while rewriting data columns, the `_base` image in the delta is compared against
the current MAIN row, normalized by key.

**Pure-delta rules** (`old_version IS NULL` = the row never existed in MAIN):
`op=D` (inserted then deleted inside the branch) is a NOP; `op=I` goes to Pass 3;
`op=U` is not produced by the MVP (treated as DELETE + INSERT). **PK type
constraint**: the chain must use the typmod-aware type name and `rtrim` BPCHAR
keys on both sides, else `BPCHAR(N)` / `NUMERIC(p,s)` keys mismatch.

### 1.4 Catalog tables

`pg_branch`: `branch_id` (sequence-backed), `branch_name` (unique), `owner`
(drives ACL), `created_at`, `mode` (`live`/`snapshot`), `state`
(`active`/`applying`/`applied`/`discarding`/`discarded`), plus the drift/identity
columns `schema_hash`, `creation_snapshot_xmin`, `tablespace_list`,
`col_signature`. `pg_branch_delta`: `branch_id`, `relid`, `key` (PK values as
JSON), `op` ∈ `I`/`U`/`D`, `old_version` (conflict token), `tuple_data` (row JSON,
`NULL` for DELETE), PK `(branch_id, relid, key)`.

The registry schema fingerprint is written **lazily** at the first
`use_branch`/`apply_branch`, so `create_branch → CREATE TABLE → use_branch` keeps
working and the snapshot is taken at entry time.

### 1.5 The PK as row identity

The PK is the durable row identity within a branch: the delta key is the JSON of
the PK values, unique with `(branch_id, relid)`. UPDATE/DELETE derive it from the
MAIN row fetched by ctid; BranchScan Pass 1 serializes each MAIN row's PK and
`bsearch`-es the delta; INSERT probes delta and MAIN independently for the UPSERT
promotion; `old_version` derives from the row's `ctid + xmin`. No-PK tables are
rejected by default; under `overlay_branch.keyless_tables=on` they fall back to a
best-effort ctid anchor after a one-shot WARNING (§4).

### 1.6 Incremental write path

Entry is the `ExecutorRun` intercept. Core invariant: the MAIN heap is never
written; every change is appended/overwritten into the delta by
`overlay_delta_insert`, idempotent on `(branch_id, relid, key)`.

**UPDATE / DELETE**, per junk subplan row: read the ctid from the junk slot;
re-fetch a clean row by ctid (under the overlay-helper recursion guard so the
planner hook does not re-inject BranchScan into its SPI SELECT); compute
`old_version`; for UPDATE merge the SET columns into the clean row (PK may not
change; `SET col = NULL` is stored as JSON `null`); serialize PK/tuple; write the
delta row.

**INSERT + UPSERT** does a 2-phase conflict check (delta, then MAIN) and
dispatches on `(main_hit, pure_hit, oc_action)`:

| main_hit | pure_hit | oc_action | promo | Meaning |
|---|---|---|---|---|
| false | false | NONE/DO_UPD/DO_NOTHING | 1 | no conflict → plain INSERT |
| any | any | DO_NOTHING | 0 | skip |
| true | false | DO_UPD + WHERE ok | 2 | UPDATE MAIN row |
| false | true | DO_UPD + WHERE ok | 3 | UPDATE pure-delta row |
| true | true | DO_UPD + WHERE ok | 2 | MAIN wins the tie-break |

**Pure-delta post-processing**: rows living only in the delta (op I/U) are never
seen by the UPDATE/DELETE subplan (which scans only MAIN). A post-pass collects
delta I/U rows, keeps the latest per key, runs the subplan's qual on each
candidate, then writes DELETE or UPDATE for survivors. A non-empty qual is
required: whole-table DML skips pure-delta rows to avoid deleting freshly
inserted rows.

**RETURNING** captures the affected rows into slots in `es_query_cxt` and replays
them through `dest->receiveSlot()`.

### 1.7 Incremental read path

A streaming iterator over MAIN and the delta:

- **Pass 1**: scan MAIN row by row, serialize the PK and `bsearch` the sorted
  delta — no match emits the MAIN row, a DELETE match drops it, an I/U match
  emits the delta override.
- **Pass 2**: emit deltas not consumed by Pass 1 (pure INSERT / UPDATE orphans).

Rows are emitted on demand (one per `next()`), so `LIMIT`/cursors stop early.
Cost is O(M log M + N log M) (sort the delta once, then `bsearch` per MAIN row);
the delta PK guarantees key uniqueness, so no executor-level dedup is needed. A
**P0 PK-equality fast path** handles `WHERE pk = const` with one delta and one
MAIN row, and comparison is typed (native btree operators, not `strcmp`) so
BPCHAR / NUMERIC / TIMESTAMPTZ stay correct (§2).

### 1.8 Scope & gating + ACL double-defense

Unsupported objects are rejected at `create_branch` time (0A000/55000):
partitioned / foreign tables, views, matviews, foreign keys, multi-column PKs,
non-PK UNIQUE. The guard checks cheapest-first (relkind, `relispartition`, user
triggers, foreign keys, then no-PK). A utility-hook gate intercepts user DDL with
a session-private TEMP whitelist. At the very top of the executor intercept,
`COPY FROM`, `MERGE` and data-modifying CTEs raise 0A000; a write that reaches
`standard_ExecutorRun` without the redirect raises ERROR 55000 (write-through
choke).

**ACL double-defense** — the SQL layer REVOKEs from PUBLIC and grants only to
`overlay_branch_administrators` (the `public.*` synonyms are `SECURITY INVOKER`
with a fixed `search_path`); the C layer runs `ob_acl_check_lifecycle()` as the
first line of every SQL-callable wrapper, raising 42501 on denial. Effective
identities: superuser; a member of the reserved NOLOGIN role
`overlay_branch_administrators`; or the branch owner. CREATE_BRANCH requires
superuser/mgmt (no self-create, avoiding resource DoS); USE/APPLY/DISCARD also
allow the owner; exiting to Main is open to everyone. apply/discard are
idempotent, and a discarded row is kept as `state='discarded'` for audit.

---

## 2. BranchScan implementation

Branch reads are served by a `CustomScan` (`overlay_branch_branchscan`) injected
through `set_rel_pathlist_hook`, so a plain `SELECT` transparently returns the
MAIN ⊕ Delta overlay; it shares one streaming iterator with the
`overlay_main_plus_delta` SRF.

### 2.1 Injection mechanism

The hook runs the previous hook first, then applies a **strict pre-check order** —
reordering it causes recursive re-entry or a wrong-branch read:

| Step | Check | Why here |
|------|-------|----------|
| B0 | schema skip (`pg_catalog` / `information_schema` / `pg_toast*` / `overlay_branch`) | cheapest; must precede `is_active()` |
| B1 | recursion guard (`ob_in_apply_operation` / `ob_in_write_redirect` / `ob_in_overlay_helper` / `ob_in_invalidation_check` / `ob_in_planner_hook`) | internal SPI must not be re-wrapped |
| R16 | `hasModifyingCTE` → ERROR 0A000 | reject before building a path |
| B2 | inject only for `CMD_SELECT`, plus `CMD_UPDATE`/`CMD_DELETE` on `resultRelation` (§2.5) | never touch other DML |
| — | `overlay_branch_is_active()` | **after B0/B1**: it runs SPI that re-enters this hook |

It then skips non-relation / non-`RELKIND_RELATION` / no-PK RTEs, builds a
`CustomPath` (relid in `custom_private`), `lappend`s it onto `rel->pathlist`
(bypassing `add_path` dominance) and prunes non-`CustomPath` entries to force
selection (guarded by a non-empty list, so a dummy-`Result` baserel keeps its
pathlist).

### 2.2 The 2-pass overlay algorithm and the streaming iterator

- **Pass 1**: walk MAIN row by row; serialize the PK, `bsearch` the sorted delta —
  no match emits the MAIN row, `op='D'` skips (tombstone), `op='I'`/`'U'` emits the
  delta override and marks the entry consumed.
- **Pass 2**: emit every delta `I`/`U` entry not consumed by Pass 1, only after
  Pass 1 is exhausted.

Latest-wins holds because the delta PK guarantees one row per key. Rows are
emitted on demand, so `LIMIT`/cursors stop early; order is "Pass 1 in MAIN order,
Pass 2 in delta append order". Cost O(M log M + N log M).

### 2.3 P0 PK-equality fast path (`WHERE pk = const`)

Two-layer O(1): L1 pushes `WHERE pkcol = <val>` into the MAIN baseline read (PK
uniqueness bounds it to ≤1 row, keeping the index scan); L2 uses an O(1) exact
delta lookup instead of the O(M) full load. The predicate is decompiled over the
whole `OpExpr(Var(pk), Const)` so the `Const`'s type drives its literal; any
ERROR / NULL / oversize deparse disables the fast path. L1 only filters —
`scan.plan.qual` re-checks every row, so a mis-pushed predicate can pull extra
MAIN rows but never a wrong result.

### 2.4 Predicate pushdown for `IN` / range / `OR`

L1 widens from `pk = const` to any btree comparison built solely from the PK
column: `OpExpr` with the PK `Var` and a `Const` (`eq/lt/le/gt/ge`),
`ScalarArrayOpExpr` with `useOr` and all-`Const` elements (`IN` / `= ANY`), and
`BoolExpr(AND/OR)` with recursively pushable args. All matching clauses are
AND-joined and deparsed into one `pk_where_sql` fragment for the MAIN read.
Strategy — **L1 relaxed, L2 degraded**: L1 accepts the whole text; L2 stays O(1)
only for a single point equality, otherwise falling back to the full-list delta
path. Clauses with an outer `Var` / `PARAM_EXEC` are removed before deparse and
re-filtered by `scan.plan.qual`. Correctness rests on `scan.plan.qual` always
re-checking and on the Pass 2 orphan rule restoring rows moved into range by a
branch UPDATE.

### 2.5 DML-target BranchScan (ACC-1)

The hook injects only for `CMD_SELECT` by default, so the UPDATE/DELETE subplan
scans MAIN rather than the overlay — consecutive in-branch writes then break
(`SET v = v||'+1'` then `SET v = v||'+2'` lands on the MAIN value). ACC-1 also
injects for the DML target baserel (`CMD_UPDATE`/`CMD_DELETE` when the RTE is
`root->parse->resultRelation`), flagged in `custom_private`. This makes `ctid`
reachable so WR can anchor `old_version`; the emitted slot is a real heap tuple
allocated in `es_query_cxt` (**not** the scan context, which is deleted before
`ExecResetTupleTable`). WR's pre-image becomes "MAIN row for anchoring, overlay
row for values"; a Pass 2 pure-delta row has an invalid ctid and takes the "no
ctid → skip" branch, so the SELECT path is unaffected.

### 2.6 Executor unpacking protocol

`custom_private` is a fixed 8-element list (relid placeholder, PK flag +
predicate, PK key, general-WHERE flag + predicate, the plan-time branch id, the
DML-target flag), unpacked with a safe per-element fallback. The **branch-id
stamp** is the plan-cache fallback assertion: a mismatch against the branch bound
to the statement raises ERROR 0A000 (PostgreSQL forbids replanning inside
`ExecutorStart`), turning a silent wrong-branch read into a loud error.

### 2.7 ReScan semantics

ReScan discards the iterator and reopens from scratch (re-reading the delta and
restarting the MAIN scan); the path omits mark/restore support, so the planner
inserts a `Materialize` when needed. ReScan is what makes nested-loop joins and
correlated subplans correct.

### 2.8 MemoryContext lifetime hard constraint

A one-shot SPI helper's `SPI_connect()` re-hangs `CurrentMemoryContext` on the
short-lived SPI context, and `SPI_finish()` deletes it. Any value that must
outlive `SPI_finish()` (delta key, `old_version`, `tuple_data` and every string
crossing the boundary) must be copied into `TopMemoryContext` **before** the
finish; error-path message copies instead go to the caller's context (the error
path never returns, so a `TopMemoryContext` copy would leak on every failure).
The iterator's per-row slot and its copies live in a child context reset on every
`next()`, otherwise memory grows linearly with the scan (§5.2, MF-1).

---

## 3. Concurrency control

### 3.1 The four-layer concurrency defense tower

```
Layer 4  semantic correctness     SNAPSHOT mode MAIN freeze / LIVE mode delta visible
Layer 3  write-path protection    state in {applying, applied, discarded} rejects delta writes
Layer 2  xact-level exclusion     branch-level advisory lock; CAS-gated, apply/discard only
Layer 1  invalidation awareness   NOTIFY + threshold SPI fallback recheck
```

Correctness does not hinge on any single layer: Layer 1's NOTIFY is an
optimization, and Layer 2's advisory lock is auxiliary to the Layer 3 CAS.

### 3.2 Layer 1 - cross-session state invalidation

**Broadcast.** Publishers send `<bid>:<new_state>` on the notification channel
unconditionally at the apply tail, after discard, and at the CAS win (a guard
that skips a broadcast is forbidden). Backends do not `LISTEN`; every throttled
invalidation call drains the sinval bus, which also carries async-notify
delivery.

**Throttle — three-source OR.** The heavy SPI recheck fires when a call counter
crosses `overlay_branch.invalidation_check_threshold` (default 32), a wall clock
(`overlay_branch.invalidation_check_interval_ms`, default 100 ms) elapses, or on
the first call; the sinval drain runs every call but is not a trigger.

**Kickout — DQL vs DML.** When `state != active`, the idempotent cleanup runs
first (unregister snapshot, clear ctx, reset GUC), then DQL emits a NOTICE and
returns false (statement completes against MAIN), while DML raises ERROR 55000
and aborts so all delta writes roll back.

**Observer suppression.** Destructive side effects run only for the two opted-in
call sites (the write-redirect `ExecutorRun` entry and
`force_invalidation_check()`); every other caller returns a boolean without
changing state. The opt-in may be set only for DML command types and
non-catalog-only statements — pure SELECT, `DECLARE CURSOR` and utility
statements stay observer-only, else a kickout could let the next DML write MAIN
silently.

### 3.3 Layer 2 - branch-level advisory lock

The advisory key is a magic high-16-bit namespace OR'd with the low 32-bit
`branch_id`, mapped onto PG's `pg_advisory_xact_lock(int8)` locktag; only
`ExclusiveLock` is ever taken, by the apply/discard CAS winner.

| | use_branch | apply X | discard X |
|---|---|---|---|
| use_branch (no lock) | compatible | compatible at lock level (kickout via Layer 1/3) | compatible at lock level |
| apply X | - | conflict | conflict |
| discard X | - | conflict | conflict |

`use_branch` takes **no** advisory lock (it only reads state); apply/discard take
it after resolving the bid and before the state check, always before the first
user-table `RowExclusiveLock` or catalog UPDATE. Strict order: advisory →
user-table `RowExclusiveLock` (inside the 3-pass) → catalog locks. The historical
V0 protocol (use takes `ShareLock`, apply/discard upgrade it) is deleted: two
upgraders is PostgreSQL's classic 40P01 deadlock with no automatic break. After
the change no shared→exclusive upgrade edge exists, so the wait-for graph cannot
cycle; the CAS is the primary mutex and the advisory lock is auxiliary.

### 3.4 Layer 3 - `applying` transient state + delta-write guard

| # | Transition | Legal |
|---|---|---|
| T1 | `active` → `applying` (apply CAS UPDATE) | the only legal way in |
| T2 | `applying` → `applied` (apply success) | yes |
| T3 | `applying` → `active` (xact ABORT) | yes |
| T4 | `active` → `discarded` (discard UPDATE) | yes |
| T5 | `applying` → `discarded` | forbidden |
| T6 | `applied`/`discarded` → any | forbidden (terminal) |

**Atomic CAS.** Apply step 1.5 is a single `UPDATE ... SET state='applying' WHERE
branch_id=$1 AND state='active'`; row count 1 proceeds, row count 0 raises ERROR
55000. A conditional UPDATE (rather than SELECT-then-UPDATE) means two concurrent
applies cannot both observe `active`; the final UPDATE predicate is
`state='applying'`.

**Rollback auto-restore.** No code writes `state='active'` manually: the
`applying` UPDATE is a heap version stamped with the current `xmin`, so any ABORT
makes readers see the pre-UPDATE tuple (`active`). A catch-block restore would
fight MVCC and could wipe a genuine terminal state.

**delta-insert barrier.** At the top of `overlay_delta_insert`, before any SPI, a
plain state SELECT (no row lock, to stay usable in a STABLE function) rejects
`state != 'active'` with ERROR 55000. Every delta write funnels through this one
function.

### 3.5 Layer 4 - SNAPSHOT vs LIVE mode

In SNAPSHOT mode MAIN reads are frozen at entry while the delta stays latest. A
global hash table keyed by `branch_id` owns the unique frozen capture; the branch
context holds only a reference pin (first capture copies the snapshot into
`TopMemoryContext` and pins it, re-entry reuses the same pointer, local exit
unpins, terminal cleanup frees). The pseudo-registration count keeps the pinned
snapshot from being freed underneath the branch. Snapshot allocation:

| Phase | Snapshot |
|---|---|
| Pass 1 MAIN read | the frozen `branch_main_snapshot` (live: active snapshot) |
| Pass 2 delta read | latest |
| WR `old_version` ctid lookup | latest (must be the physically current tuple) |
| planner hook | none (no heap read) |

Push/pop are balanced on TRY and CATCH paths (an imbalance would drift the
active-snapshot stack and PANIC). Snapshot mode freezes new MAIN writes but never
the delta (delta reads use the latest snapshot).
`overlay_branch.snapshot_max_hold_minutes` (default 10 min) forcibly exits branch
mode on timeout.

### 3.6 FR6 BranchScan ReScan freshness

The older path reused the result set materialized at Begin, so a delta committed
by another session (or by a `WITH HOLD` cursor across transactions) stayed
invisible. ReScan now runs the throttled invalidation check, closes the iterator,
resets the scan context, and the next `ExecCustomScan` lazily reopens, rerunning
Pass 1 + Pass 2. Planner-decided static fields are preserved; generic PlanState
fields are left to the upper `nodeCustom.c` `ExecReScan`. An EXPLAIN-without-
ANALYZE ReScan is a no-op; a `WITH HOLD` re-open sees newly committed delta rows;
reverse `SCROLL CURSOR` movement is borne by the upper-layer tuplestore spool. A
fresh ReScan costs O(|MAIN| + |DELTA|).

### 3.7 Apply/discard idempotency and the D→U→I replay

The step 1.5 row count is the single winner gate; a loser gets ERROR 55000
without blocking, and a second apply (or discarding an applied/discarded branch)
also errors immediately. The delta is replayed as **D → U → I** (delete
conflicting keys first, then update, then insert) to avoid unique-constraint
conflicts on insert; conflict detection uses the `ctid:xmin` token re-fetched
`FOR UPDATE` plus the HOT base-image check. A branch-local (`old_version IS
NULL`) UPDATE whose MAIN row never existed is replayed as an INSERT in the
MAIN-miss branch instead of a false conflict. On success apply moves
`applying → applied` and deletes the bid's delta; discard moves `active →
discarded` and bulk-deletes it; both broadcast the new state and drop the
snapshot cache entry. A mid-way failure ABORTs back to `applying → active` with
the delta untouched, so the user can retry.

### 3.8 Mandatory engineering conventions

1. Planner pre-check order fixed `B0 → B1 → R16 → B2 → is_active` (§2.1).
2. Depth-1 recursion guard as the first line of the throttled check.
3. Write-redirect dual DML pre-guard: compute the command split and skip
   catalog-only DML before calling the throttled check.
4. Utility whitelist (SET/SHOW, LISTEN/NOTIFY, transaction/discard statements,
   locks, prepared-statement commands, publication/subscription, ...) skips
   `is_active()`; only user DDL runs the guard.
5. Opt-in destructive side effects, saved/restored around the two write/force
   entries; observers never mutate state.
6. SPI probe pattern: a nested `SPI_connect()` uses a "did-connect" flag and only
   finishes when it connected (finishing unconditionally double-frees).
7. GUC check/assign split: the check hook is side-effect-free read-only
   validation; the assign hook acts and never raises ERROR.
8. Single idempotent cleanup path; recursion guards balanced under `PG_TRY`/
   `PG_FINALLY`; C90 top-declaration is a hard build requirement.
9. Two-step plan-cache invalidation on branch switch —
   `CacheInvalidateRelcacheAll()` **then** `CommandEndInvalidationMessages()`.

**API compatibility.** `use_branch` ships the original one-argument form (still
resolves for a legacy `unknown` literal) and the extended
`use_branch(name text, isolation_mode text DEFAULT 'read committed')`;
`isolation_mode` ∈ `{'read committed', 'snapshot'}`, and `NULL` exits branch mode.

---

## 4. Keyless ctid identity (experimental)

Implemented in the identity helpers (delta store), the read path (BranchScan),
the write path (write-redirect) and apply positioning (lifecycle); regression
`test/regress/sql/overlay_branch_keyless.sql`.

### 4.1 Goals & non-goals

Goal: allow a branch on a **no-PK table** behind a GUC, switching row identity to
a physical anchor (`ctid` + `xmin`) — the hard PK prerequisite is a heavy
out-of-the-box blocker. Non-goals: PK tables are **byte-for-byte unchanged** (PK
always wins), no ambition to match the PK path's robustness (ctid identity is
fragile under concurrent MAIN writes, §4.5), and **off by default**.

### 4.2 GUC semantics

`overlay_branch.keyless_tables`: `bool`, default `false`, `PGC_SUSET`.

| GUC | Table | Behavior |
|---|---|---|
| `off` | Has PK / No PK | status quo: PK path / no-PK guard raises 0A000 |
| `on` | Has PK | PK path (unchanged) |
| `on` | No PK | one-line WARNING, then ctid path |

The WARNING fires **once per `(session, relid)`** on first branch entry (the
write-redirect entry or the first scan after `use_branch`), never in the planner
hook. It states that identity is the physical `(ctid, xmin)`, that concurrent
UPDATE/DELETE may change a row's ctid and cause a silent delta miss (detected at
apply time as a conflict), and hints at adding a PRIMARY KEY.

### 4.3 Anchor identity encoding

`pg_branch_delta.key` is unchanged; the tag prefix never collides with a PK key
(a JSON array starting with `[`):

| Row origin | key encoding | Notes |
|---|---|---|
| Present in MAIN (anchored) | `A:<blk>:<off>` | from the slot's `ctid` |
| Not in MAIN (synthetic INSERT) | `S:<seq>` | monotonic per `(branch_id, relid)`, `seq = 1 + MAX(existing S: seq)` |

`old_version` stays a `blk:off-x<xmin>` token.

### 4.4 Write / read / apply path mapping

**Write** uses one unified identity serializer: PK relation → PK serialization
(status quo); non-PK INSERT with no MAIN origin → `S:<nextseq>`; otherwise
`A:<ctid>`. **Read** keeps its 2-pass structure; only identity extraction and
comparison change — Pass 1 uses the MAIN slot's ctid as `A:<blk>:<off>` with an
empty typed key (comparator falls back to `strcmp`), and Pass 2 appends leftover
`op='I'` plus unanchored `A:*` orphans. The planner hook injects only under the
GUC and **skips** the P0 PK fast path. Pure-delta identity propagation must reuse
the original `key` carried with each delta row (a rebuilt keyless slot has no
usable ctid), else repeated UPSERTs mint new keys and duplicate on read.
**Apply** rebuilds the WHERE clause from the key: `A:` → `ctid =
'(<blk>,<off>)'::tid`; `S:` → no positioning (pure INSERT); PK JSON → status quo;
conflict validation reuses the ctid/xmin/`_base` chain, decoupled from the PK.

### 4.5 Anchor-invalidation triggers

An anchor is valid iff its ctid still points to the same physical version of the
same logical row; other sessions can break it:

| Trigger | Read side | apply side |
|---|---|---|
| Concurrent UPDATE of an anchored row | delta unmatched → **silent drop** | no visible row → 0A000 |
| Concurrent DELETE | same as above | same as above |
| ctid reuse (new INSERT after VACUUM takes the slot) | ctid match hits the new row; the `xmin` check deems it stale → dropped (**no misread**) | `old_version` `xmin` mismatch → 0A000 |
| Whole-table rewrite (`VACUUM FULL`/`CLUSTER`/`REINDEX`/`TRUNCATE` refill) | all anchors mismatch → **silent drop** | 0A000 row by row |

The read side's `xmin` check: after a ctid equality match, Pass 1 compares the
`-x<xmin>` suffix of `old_version` against the MAIN tuple's raw `xmin`; on
mismatch the row falls into the Pass 2 orphan rule (unmatched `A:*` orphans are
not revived), so ctid reuse degrades to a silent drop rather than a misread.
Rewrite-type DDL is blocked while a branch is active, but during the inactive
window (after exit, before apply) it cannot be, so the apply-side `old_version`
is the only fallback (0A000).

Plain VACUUM / autovacuum does not itself invalidate anchors (it only reclaims
dead-tuple line pointers, enabling later ctid reuse); the root cause is
concurrent DML by other sessions. Levers, by cost-effectiveness: add a PRIMARY
KEY (eliminates it); shorten branch lifetime (linear reduction of the window);
serialize writers while active (removes the root cause); disable autovacuum on
the table (cuts only the ctid-reuse path, **does not** improve silent delta
loss). There is no general "turn off autovacuum to protect anchors" solution.

### 4.6 Persistence & retention

An anchor survives the branch lifetime only if the old version stays resolvable
along the version chain and the old tuple is not reclaimed (global `OldestXmin`
pinned).

- **Approach A — best-effort, no retention (implemented).** Accept the §4.5
  fragility; a broken anchor is reported cleanly at apply time as 0A000, while the
  read side may silently drop the delta. Lifetimes are short, blast radius is
  `lifetime × table write rate`, and there is no cluster-level side effect.
- **Approach B — persistent replication slot (not implemented).** A persistent
  slot holding the branch-creation horizon would pin old tuples into
  `OldestXmin` at the cost of cluster-wide VACUUM suppression (bloat) and HOT
  invalidation. It is **explicitly not implemented** — no switch, no code path; if
  added later it must be an independent GUC with a max-hold TTL auto-release.

### 4.7 MVP scope

**Implemented:** SELECT / INSERT / UPDATE / DELETE on no-PK tables (best-effort
Approach A); the identity serializer, the read-side anchor `xmin` check, apply
positioning and conflict validation, and the per-`(session, relid)` WARNING.
**Unsupported (existing 0A000 style):** keyless + `INSERT ... ON CONFLICT`
(UPSERT depends on the PK); keyless + partitioned / foreign-key / `MERGE` /
`COPY FROM` / DML-CTE; consistency under concurrent MAIN writes (silent drop,
apply error as fallback); version-chain resolution for stale ctids
(`heap_get_latest_tid`) — a future enhancement.

---

## 5. Performance

### 5.1 Performance model & notation

`N` = MAIN rows; `M` = delta rows; `A` = user columns; `S` = average bytes/row.
The delta PK is `(branch_id, relid, key)` and every write lands in the
`ON CONFLICT ... DO UPDATE` of `overlay_delta_insert`, so **`M` = distinct rows
touched, independent of operation count** (the latest-wins `I→U→D` collapse in
the read path is structurally unreachable). A delta row is far heavier than a
heap tuple — key is the PK JSON, the tuple is the whole row JSON, and
UPDATE/DELETE also store the previous image as `_base` — so a U/D row is roughly
2× the row.

| Path | Form | Complexity |
|---|---|---|
| Read: delta load | PK prefix `(branch_id, relid)`; P0 hit = point lookup | O(M) / O(1) |
| Read: delta sort | `qsort`, operators rebuilt per comparison | O(M log M) |
| Read: Pass 1 match | `bsearch` + typed key per MAIN row | O(N log M) |
| Read: Pass 2 | delta scan per miss | O(M) |
| Read: materialization | streaming, early stop | O(N + M) memory |
| Write: per DML row | state query + fetch + token + UPSERT | 2-4 SPI |
| Apply: per delta row | validation + batched DML per `(relid, op)` | O(1)/op |

### 5.2 Identified bottlenecks / first-round measurement conclusions

Environment: PG 17.11, `max_parallel_workers_per_gather=0` (**required**),
`N=200000, M=20000`, serial (metrics M1..M8 in §5.8). **The read bottleneck is
not delta size but a per-row constant overhead plus a session memory leak** — an
empty branch (`M=0`) is already ~440× a native scan.

- **MF-1 | session memory leak (highest priority) — fixed.** A full overlay grew
  backend memory ~350 MB, not reclaimed by discard: per-row string copies on the
  delta path were never freed. Fixed with a per-scan child context reset per row,
  dropping the redundant copies (also on the write and error paths); verified by
  `overlay_branch_memgrow`.
- **MF-2 | parallel path bypassed the overlay (silent misread) — fixed.** The hook
  cleared `rel->pathlist` but not `rel->partial_pathlist`, so a parallel seq scan
  ignored the delta. Fixed by clearing the partial pathlist and disabling
  parallelism for the relation (`overlay_branch_parallel`).
- **MF-3 | hard error on parallel queries — disappears with MF-2.** A worker
  restoring the branch GUC needed an SPI snapshot and failed.
- **MF-4 | per-row constant ~10 us (holds at M=0) — fixed.** Root cause was the
  per-MAIN-row PK serialization SPI; skipping all key work when the delta is
  empty cut the `M=0` full scan ~15×.
- **MF-5 | non-PK pushdown 2-4× slower — fixed.** Pushdown shrank Pass 1's hit
  set, so covering deltas fell into the Pass 2 orphan path; removing the
  redundant O(M²) folds eliminated it (~30×).

### 5.3 Read-path optimizations

- **sort + bsearch (O(M log M + N log M))**: sort the delta once, then binary
  search each MAIN row, avoiding an N×M full scan.
- **P0 PK-equality fast path**: one delta row and one MAIN row for
  `WHERE pk = const`; recognition relaxed to the PK's five btree operators plus
  `IN` / `= ANY` and `AND`/`OR` merged into one deparsed fragment; the delta side
  stays O(1) only for a single-point equality.
- **Typed PK comparison**: native btree operators via memoized comparison
  functions per type, keeping BPCHAR / NUMERIC / TIMESTAMPTZ correct.
- **Streaming early stop**: `LIMIT`/cursors truncate instead of materializing.
- **De-SPI MAIN baseline**: read the heap directly (with executor restriction
  info as prefilter); no key work when the delta is empty, typed-key construction
  when possible, per-row fallback on failure.
- **Supporting changes**: one shared tuple descriptor per pass, per-relation PK
  metadata memoized by relid, and key uniqueness collapsing the two O(M²) dedups
  to one O(M) pass — small gains that are the prerequisite for the typed path.

### 5.4 Write-path optimization WR-1 and redundant index

**WR-1** cuts the per-row SPI/planning cost of DML redirection. Statements were
rebuilt inline with literals, so `SPI_execute` fully re-parsed+planned every
time; WR-1 switches to `$1..$n` parameters cached with `SPI_prepare` +
`SPI_keepplan` and reused via `SPI_execute_plan`, backed by a `TopMemoryContext`
hash cache keyed by SQL text. Invalidation delegates to the regular plan cache;
`tuple_data` moves from a text literal to a `bytea` datum.

**Redundant index drop.** A relation-index on `(branch_id, relid)` is a strict
prefix of the delta PK; the internal table is rarely ANALYZEd, so the planner
picked the narrower index and degraded the key lookup to a filter, making the
plain-INSERT existence probe O(delta) and ~20× slower. Dropping it made INSERT
~20× faster, on par with UPDATE/DELETE; all delta accesses use only the
`(branch_id, relid)` prefix, which the PK index covers.

### 5.5 Apply batching (AP-1)

The pre-batching chain used up to 8 SPI per delta row (key→WHERE, fetch MAIN
row, version token, `_base` probe, `_base` compare, DML) with two pure wastes:
the fetch materialized the whole row while only ctid + xmin were needed, and the
token re-queried the heap for those two values.

- **Stage A (per-row merge).** Compress key→WHERE to one SPI (WHERE assembled in
  C), merge the fetch + version into a single `SELECT ctid, xmin ... FOR UPDATE
  LIMIT 1`, scan the raw tuple bytes in C for `_base`, and memoize per-relation
  metadata. A combined probe+compare SQL was a net loss because per-row literals
  make the plan unusable — measure **planning** time, not SPI count.
- **Stage B (set-based batching; `overlay_branch.apply_batch`, default `on`).** Per
  `(relid, op)`: a validation SELECT (first conflicting row by `ORDER BY key
  LIMIT 1`) plus a batched DML (`jsonb_populate_record` + a PK join). Key
  uniqueness makes op key sets mutually exclusive, preserving D→U→I error
  precedence; the DML's own row locks replace per-row `FOR UPDATE`, and any
  conflict rolls back all batched DML. Falls back per-row for GUC off / keyless
  relation / any delta row containing `_base`. A 20000-row apply drops from
  ~1261 ms to ~166 ms (~7×).

Invariants preserved: D→U→I order; per-row optimistic conflict semantics
(missing MAIN / `old_version` mismatch / HOT base-image divergence → 55000);
pure-delta cases; keyless identity; lock order advisory → catalog → user
`RowExclusiveLock`; the CAS state machine with the xact-scoped exclusive advisory
lock. Error messages stay byte-for-byte aligned with the baselines.

### 5.6 Delta size: current state and observability

`M` is independent of operation count, so "the delta grows without bound" does
not hold; the real risk is a branch touching near the whole table, plus the
per-row weight doubled by `_base`. There is no size query interface or bound.

| Metric | Definition |
|---|---|
| `delta_rows` | `count(*)` on `(branch_id, relid)` |
| `delta_payload_bytes` | `sum` of key + old_version + tuple_data octet lengths |
| `delta_relation_bytes` | `pg_total_relation_size('pg_branch_delta')` |
| `main_bytes` | `pg_total_relation_size(relid)` |
| `amplification` | `delta_payload_bytes / main_bytes` (includes the `_base` doubling) |

Reference queries are in `test/bench/perf_baseline.sql`; productization (a stats
view + throttled WARNING) is a separate future increment.

### 5.7 Measurement pitfalls

- Use **`LC_ALL=C.utf8`** (multibyte column widths otherwise cause a false diff).
- **Disable parallelism** (`max_parallel_workers_per_gather=0`) for a stable
  serial comparison.
- **Measure memory between steps**; the "object outliving `SPI_finish` in
  `TopMemoryContext`" family is easy to reintroduce.
- Do not reuse a `PREPARE` across a branch enter/leave (plan cache invalidated).
- **autovacuum noise**: clear/vacuum the delta table before comparing
  `pg_total_relation_size`.
- Do not compare pgbench tps directly with these baselines (different mix).
- **For the per-row SPI path, look at planning time, not SPI count**: set
  `pg_stat_statements.track_planning=on` and A/B within one session.

### 5.8 Baseline methodology

Harness: `test/bench/perf_baseline.sql` + `test/bench/run_perf_baseline.sh`
(`run_perf_baseline.sh 200000 20000`). It is psql-driven (`\timing` + `EXPLAIN
(ANALYZE, BUFFERS)`) and collects M1 (MAIN `count(*)`), M2/M3 (branch `count(*)`),
M4 (P0 PK equality), M5 (non-PK pushdown), M6 (EXPLAIN confirming the BranchScan),
M7 (delta size, §5.6), M8 (`apply_branch()`), M9 (MAIN after apply) and M10
(build-delta split into UPDATE/DELETE/INSERT, per-row microseconds). The delta is
built with set-based DML so `M` stays controllable. Output goes to
`perf_baseline_<N>_<M>.log`; compare the **same** `N/M` on the **same** machine,
**interleaving** A/B within one session and rotating order across rounds
(Latin-square) so drift/warm-up do not bias a configuration.

---

## 6. Key invariants and corner cases

### 6.1 Invariants

1. **The MAIN heap is never written inside a branch** — all changes go into
   `pg_branch_delta`.
2. **No-PK tables are rejected by default**; `overlay_branch.keyless_tables=on`
   switches them to best-effort ctid anchor identity (§4).
3. **The planner hook pre-check order is fixed** — B0 → B1 → R16 → B2 →
   `is_active` (§2.1).
4. **The GUC check hook is side-effect free**; only the assign hook acts, and it
   never raises ERROR.
5. **Recursion guards are balanced under `PG_TRY`/`PG_FINALLY`**, including the
   abort path.
6. **Cross-SPI memory lives in `TopMemoryContext`**, copied out before
   `SPI_finish()`; error-path copies use the caller's context (§2.8).
7. **`use_branch` takes no lock; apply/discard use CAS** — no shared→exclusive
   upgrade edge, hence deadlock-free (§3.3).
8. **Observers never run destructive cleanup** — kickout runs only on the DML
   path (§3.2).
9. **Branch identity is bound once per statement** — `bid`/`mode`/`stmt_snap` from
   `BranchExecContext`, bound before `standard_ExecutorStart` (§1.1).
10. **One codec for the conflict token** — `<blk>:<off>-x<xmin>` formatting/parsing
    goes only through the shared helpers (§1.3).
11. **The MAIN row is verified before overwrite** — fetch by `_base_ctid`, assert
    `xmin == _base_xmin` (mismatch → 55000), plus the HOT base-image check
    (§1.3, §3.7).
12. **Latest-wins delta, no double apply** — the delta PK guarantees one row per
    key; the D→U→I replay NOPs on an empty net change (§3.7, §5.5).

### 6.2 Corner cases

- **Junk attribute mapping.** The UPDATE/DELETE subplan slot carries junk
  attributes (ctid, tableoid, ...), so slot positions do not match the relation
  descriptor; a helper maps a real attno to its slot position (§1.6).
- **Deferred registry fingerprint.** The schema fingerprint is written lazily at
  the first `use_branch`/`apply_branch`, so `create_branch → CREATE TABLE →
  use_branch` keeps working (§1.4).
- **Self apply/discard returns to Main.** A session that applies/discards its own
  branch cleans up immediately (exit branch, clear GUC, invalidate plan cache),
  with a NOTICE for discard.
- **Mode cache.** A session-local `bid → mode` hash table replaces the old catalog
  UPDATE of `pg_branch.mode`, removing the row-lock interlock of concurrent use;
  a one-argument GUC SET reuses the cached mode so snapshot mode survives
  leave→re-enter.
- **Temp-table whitelist.** DDL interception (§1.8) exempts session-private TEMP
  objects.
- **Publication leak (G06).** PG 17 has no table-level `publish=false` for plain
  heaps, so a throttled one-shot WARNING is emitted at the lifecycle entry points
  when a publication covering the delta would break isolation, failing open under
  `PG_TRY` (§1.8).
