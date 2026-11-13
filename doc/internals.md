# Overlay Branch Internal Implementation

> Implementation of the `overlay_branch` extension: architecture, BranchScan,
> concurrency control, keyless ctid identity and performance.
> The only user-facing companion document is [user_guide.md](user_guide.md)
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
  - [4.7 Scope](#47-scope)
- [5. Performance](#5-performance)
  - [5.1 Performance model & notation](#51-performance-model--notation)
  - [5.2 Identified bottlenecks / first-round measurement conclusions](#52-identified-bottlenecks--first-round-measurement-conclusions)
  - [5.3 Read-path optimizations](#53-read-path-optimizations)
  - [5.4 Write-path optimization WR-1 and redundant index](#54-write-path-optimization-wr-1-and-redundant-index)
  - [5.5 Apply batching (AP-1)](#55-apply-batching-ap-1)
  - [5.6 Delta size: observability](#56-delta-size-observability)
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
| [write_redirect.c](../src/write_redirect.c) | redirect INSERT/UPDATE/DELETE, pure-delta target rows, UPSERT promotion, RETURNING |
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

`apply_branch` re-validates the branch result and merges it into MAIN (not
`MERGE`) **inside the caller's transaction** — there is no autonomous/independent
SPI transaction; the 3-pass DML and the catalog state updates share the caller's
xact, so any conflict, error, or crash rolls everything back together (`applying`
reverts to `active` via MVCC). Order is **Pass 1 DELETE** (child→parent FK safe),
**Pass 2
UPDATE** (conflict detection here), **Pass 3 INSERT** (parent→child FK safe).

Conflict detection uses `old_version = "<blk>:<off>-x<xmin>"` (`ctid:xmin`): the
MAIN row is fetched by `_base_ctid` and its `xmin` must equal the recorded
`_base_xmin` (mismatch → ERROR 55000); pure-origin rows skip the check. A single
codec (`overlay_base_ver_*`) is shared by the write path, apply and the read
path. **HOT secondary check**: since a HOT update leaves `ctid`/`xmin` unchanged
while rewriting data columns, the `_base` image in the delta is compared against
the current MAIN row, normalized by key.

**Pure-delta rules** (`old_version IS NULL` = the row never existed in MAIN):
`op=D` (inserted then deleted inside the branch) is a NOP; `op=I` goes to Pass 3.
An `op=U` row always carries a version token — the MAIN baseline it was derived
from, or (after a DELETE→re-INSERT rebirth collapse, §1.6) the token of the deleted
MAIN row — so Pass 2 validates it; a pure-delta UPDATE with no MAIN baseline is
materialised by Pass 3. **PK type
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
the PK values, unique with `(branch_id, relid)`. **Composite (multi-column) PKs
are fully supported**: the delta key is the JSON array of every key column, and
the typed comparator (`overlay_typed_pk_cmp`) declares two keys equal only when
**every** column compares equal — an early return on the first equal column would
alias distinct rows that share a leading key value (corrupting overlay reads) and
make a real PK mutation look like a no-op (defeating the R03 guard).
UPDATE/DELETE derive it from the
MAIN row fetched by ctid; BranchScan Pass 1 serializes each MAIN row's PK and
`bsearch`-es the delta; INSERT probes delta and MAIN independently for the UPSERT
promotion; `old_version` derives from the row's `ctid + xmin`. No-PK tables are
rejected by default; under `overlay_branch.keyless_tables=on` they fall back to a
best-effort ctid anchor after a one-shot WARNING (§4).

### 1.6 Incremental write path

Entry is the `ExecutorRun` intercept. Core invariant: the MAIN heap is never
written; every change is appended/overwritten into the delta by
`overlay_delta_insert`, idempotent on `(branch_id, relid, key)`.

**Rebirth collapse.** Because the delta key is `(branch_id, relid, key)`, a
DELETE followed by a re-INSERT of the same PK in the same branch overwrites the
tombstone. Collapsing that to a plain INSERT (`op='I'`, `old_version NULL`) would
lose the fact that the row exists in MAIN, and the apply Pass 3 would raise a
spurious "MAIN row already present" conflict even though the branch deleted the
row. The R21 pre-check therefore permits the rebirth while a tombstone is
present, and `overlay_delta_insert` rewrites a `D → I` overwrite that carried a
MAIN baseline (`d.op='D'` with `d.old_version IS NOT NULL`, incoming `op='I'` with
`old_version NULL`) to `op='U'`, preserving the deleted row's version token. The
net branch effect is "replace the MAIN row", which is exactly what the apply
UPDATE pass validates and replays. A pure-delta tombstone (no MAIN baseline)
still collapses to a plain INSERT.

**Locating the `ModifyTableState`.** For every supported top-level DML the
planner places `ModifyTable` at the plan root, so the intercept takes
`mt = queryDesc->planstate` directly. A defensive DFS over the planstate tree
(stopping at scan nodes so it cannot descend into `BitmapIndexScan` internals)
finds the first `ModifyTableState` descendant in the hypothetical case of a
wrapping node; the found node is held in a **local** only — `queryDesc->planstate`
is a caller-owned field that names the whole tree for `ExecEndPlan`, so
overwriting it with an interior node would skip `ExecEndNode` on the wrappers
above and leak their resources.

**UPDATE / DELETE**, per junk subplan row: read the ctid from the junk slot;
re-fetch a clean row by ctid (under the overlay-helper recursion guard so the
planner hook does not re-inject BranchScan into its SPI SELECT); compute
`old_version`; for UPDATE merge the SET columns into the clean row (PK may not
change; `SET col = NULL` is stored as JSON `null`); serialize PK/tuple; write the
delta row.

**INSERT + UPSERT** does a 2-phase conflict check (delta, then MAIN) and
dispatches on `(main_hit, pure_hit, oc_action)`. The delta is the latest visible
version — it shadows MAIN — so a delta hit always wins: both the `WHERE`
predicate and the `SET` post-image are evaluated against the delta row when one
exists, never the stale MAIN row.

| main_hit | pure_hit | oc_action | promo | Meaning |
|---|---|---|---|---|
| false | false | NONE/DO_UPD/DO_NOTHING | 1 | no conflict → plain INSERT |
| any | any | DO_NOTHING | 0 | skip |
| true | false | DO_UPD + WHERE ok | 2 | UPDATE MAIN row (first touch) |
| false | true | DO_UPD + WHERE ok | 3 | UPDATE pure-delta row |
| true | true | DO_UPD + WHERE ok | 3 | delta shadows MAIN |

The Phase II MAIN probe engages for **any** keyed table, single- or
multi-column PK alike (and for a keyless table with a resolved UNIQUE arbiter).
For a single-column PK it binds `(col) = $1::text::<type>`; for a **composite
PK** it folds one `col = $n::text::<type>` conjunct per key column (the same text
round-trip used by R21 / the serialized identity, so BPCHAR / NUMERIC /
TIMESTAMPTZ compare as typed values, not as text). A NULL key column leaves the
probe unmatched (a NULL never conflicts) and the statement degrades to a plain
INSERT. Gating the probe on the single-column fast path alone would silently skip
it for a composite PK and misclassify every `ON CONFLICT` as a fresh INSERT.

**Delta tombstone shadows MAIN.** Phase I records the delta's latest op for the
key — including `op='D'` tombstones, not only the live `op='I'`/`'U'` states. A
tombstone means the branch deleted the row, so an `INSERT ... ON CONFLICT` after
a branch-local DELETE must rebirth it rather than re-read the still-present MAIN
row as a conflict: the Phase II MAIN probe is suppressed and the dispatch falls
through to `promo=1` (plain INSERT), where the rebirth collapse rewrites the
overwrite into the `op='U'` replay (§1.6, "Rebirth collapse"). Without this,
Phase I saw no delta hit, Phase II re-found the MAIN row, and the row silently
vanished from the branch for `DO NOTHING` / `DO UPDATE ... WHERE <false>`.

**Delta-first shadow on the arbiter and targetless probes.** The tombstone
suppression above works because the PK path's Phase I probe reads the delta by
`key` (the row identity) and therefore sees an `op='D'` row directly. The
**arbiter** path does not: Phase I matches the delta row's *stored image* on the
arbiter columns, which a tombstone lacks (it stores no tuple) and an `op='U'`
rewrites to the new value. So after the branch DELETE-d a row, or UPDATE-d it so
its arbiter value moved away, Phase I finds nothing and the phase-two MAIN probe
re-finds the stale MAIN row — resurrecting a row the branch removed, or
`DO UPDATE`-ing a row whose arbiter value the branch already changed. The MAIN
probe (and its per-index sibling `ob_unique_target_conflict`, used by the
targetless `DO NOTHING` path) therefore excludes any MAIN row the branch already
shadows: a `NOT EXISTS` over the delta matched on `d.old_version = <the MAIN
row's "<blk>:<off>-x<xmin>" token>` — the same CAS token the apply path compares
against. A row whose current token equals some delta `old_version` is logically
dead / rewritten in the branch and must not be re-found as a conflict; the
candidate INSERTs instead. A branch UPDATE that leaves the arbiter value
unchanged still hits Phase I by its stored image and keeps `DO UPDATE` semantics.

**Same-statement duplicate arbiter keys.** The command counter does not advance
between the rows of a single command, so the Phase I/II and R21 SPI probes can
never see a delta row written by an earlier row of the *same* command. The
INSERT loop therefore keeps a per-statement hash set of the arbiter keys it has
already claimed (any table with a PK; keyed by the full serialized identity, so
it covers composite PKs too). A row claims its key exactly
when it is written — `promo=1/2/3` add the key; a `DO NOTHING` skip or a
`DO UPDATE ... WHERE <false>` skip does not. When a later candidate's key is
already claimed, the loop reproduces native behavior: plain INSERT raises
`23505` `unique_violation`, `DO UPDATE` raises `21000` `cardinality_violation`,
and `DO NOTHING` skips the row (first row wins). This mirrors stock PostgreSQL,
where a duplicate arbiter value inside one command is caught at the index level
for plain INSERT / DO NOTHING and by the "cannot affect row a second time" check
for `DO UPDATE`. A keyless table has no arbiter constraint to violate, so the
set is not built and every row is inserted, exactly as upstream.

**Eager NOT NULL / CHECK validation.** The write-redirect short-circuits
`standard_ExecutorRun()`, so the executor's own `ExecConstraints()` never runs
for a redirected row; without an explicit check an invalid post-image would be
stored in the delta and only rejected at apply time. The path therefore calls
`ExecConstraints()` on the post-image BEFORE `overlay_delta_insert`, so a
branch-local UPDATE/INSERT/UPSERT fails eagerly with the native SQLSTATE
(`23502` NOT NULL / `23514` CHECK), message and `Failing row contains (...)`
detail. For `INSERT ... ON CONFLICT` the check runs on the **candidate** row
before conflict resolution (mirroring `ExecInsert()`, which validates the
proposed tuple first) and again on the merged row for a `DO UPDATE` (mirroring
`ExecUpdate()`), so a candidate that is invalid even though `DO UPDATE` /
`DO NOTHING` would discard it is still rejected, exactly as in MAIN.

**INSERT uniqueness pre-check (R21).** Before a promoted INSERT row is written
into the delta, the redirect probes the branch delta and then MAIN for the same
row identity, so a branch-local INSERT that duplicates an existing key fails
eagerly with `23505` before any delta write, instead of silently storing an
unappliable delta row (the conflict would otherwise surface only at
`apply_branch`). The check covers **single- and multi-column PKs**: the delta
probe binds the full serialized PK identity (the composite JSON key *is* the
delta `key`), and the MAIN probe folds a per-PK-column
`col = $n::text::<type>` conjunctive `WHERE`. A delta tombstone (`op='D'`) makes
the MAIN row logically invisible in the branch, so the MAIN probe is skipped and
a rebirth is permitted (see "Rebirth collapse").

**Resolvable non-PK UNIQUE enforcement.** A branch-local plain INSERT or UPSERT
fall-through that would duplicate a value in a non-primary `UNIQUE` index is
rejected eagerly with `23505`, matching native PostgreSQL, instead of being
stored in the delta and surfacing only at `apply_branch`. The probe binds the
indexed columns as text against the branch delta (live `I`/`U` rows) and then the
shadow-aware MAIN side, and — as the command counter does not advance between
rows of one command — a per-statement "claimed" set catches a duplicate proposed
by an earlier row of the same statement (the same device the arbiter path uses
for its PK / arbiter key). Only plain-column, non-partial indexes are probed; an
expression or partial index is not resolvable by the text check, so a violation
of one is not caught here and surfaces at apply, like any other branch-local
write the redirect cannot model.

**Eager UPDATE uniqueness.** `CMD_UPDATE` builds the same per-statement set and,
before writing each merged post-image, probes every resolvable non-PK `UNIQUE`
index, **excluding the row being rewritten** by its MAIN anchor ctid and its
delta key. That exclusion is what keeps an UPDATE that leaves the `UNIQUE` column
untouched legal: without it, the row's own MAIN anchor (which still holds the OLD
values) or its pre-existing delta row (the PRE-image) would match and raise a
spurious `23505`. An UPSERT `DO UPDATE` post-image is enforced identically, with
the conflicted source row excluded the same way.

**Arbiter selection and the eager residual check.** `ON CONFLICT (target)`
resolves to exactly one arbiter index. A **PK arbiter** keeps the fast
`d.key` / PK-ctid probe; a **non-primary UNIQUE arbiter** (keyed or keyless)
rebinds both phases to the arbiter columns, and the delta row is written under
the *conflicted* row's real identity, never the proposed one. An arbiter that
does **not** resolve to exactly one **non-partial** UNIQUE index over plain
columns — an expression index, a **partial** index, or several inferred
indexes — is **refused (0A000) for both keyless and keyed tables**: the plain
column probe cannot honour an index predicate or an expression, so proceeding
would silently match a row outside the predicate's domain (a spurious
`DO NOTHING` skip, or a `DO UPDATE` against a row the statement never named).
A **targetless** `DO NOTHING` (`arbiterIndexes == NIL`) carries no arbiter:
native checks *every* unique index, so the dispatch scans the PK and every
other resolvable plain-column, non-partial unique index, and a hit is a skip.

Once the arbiter does **not** match, a targeted `ON CONFLICT` proceeds with the
INSERT, and native hard-errors `23505` if the candidate violates the PK (which a
non-primary arbiter leaves unprobed) or any *other* unique index (the arbiter
does not cover them). The redirect reproduces this eagerly: the PK is re-checked
through the R21 path (tombstone-aware rebirth included) and every remaining
resolvable unique index through a delta+MAIN probe, so the violation is raised
before any delta write instead of being deferred to apply. For this *residual*
scan, expression and partial indexes are skipped, as they are everywhere: they
are not resolvable by the plain-column text probe, so their violation is not
caught here and surfaces at apply.

**Pure-delta target rows (synthetic ctid).** A row that exists only in the delta
(op `I`/`U`, no MAIN baseline) has no physical ctid, so the DML-target BranchScan
stamps it with a **synthetic ctid** (block `OB_SYNTH_CTID_BLOCK`, offset = the
1-based index of that row's delta key in the scan's recorded key list). The
redirect loop sees `slot_get_ctid_cstr()` report "no ctid" for such a row, calls
`overlay_dml_target_pure_delta_key()` to resolve the recorded key, reconstructs
the pre-image from the delta payload, and processes the row **inline** in the
same loop as a MAIN-anchored one — same SET-merge / post-image / eager-constraint
path, then `overlay_delta_insert` with `op='U'`/`'D'` and the stored key as the
identity. The statement's own join/qual filtering has already been applied by the
executor before the subplan yields the row, so every row that reaches the
redirect is one the DML actually targets: there is no separate qual pass and no
whole-table special case. The DML target is bounded to `OB_SYNTH_MAX_IDX`
pure-delta rows per statement.

**RETURNING** captures the affected rows into slots in `es_query_cxt` and replays
them through the plan's `DestReceiver`. Because the write path short-circuits
`standard_ExecutorRun()`, the receiver lifecycle is **not** driven by the
executor here: the replay must bracket `receiveSlot()` with
`dest->rStartup(dest, operation, tupDesc)` and `dest->rShutdown(dest)` exactly as
`standard_ExecutorRun()` does for a `sendTuples` plan. This is load-bearing for
any receiver that allocates state in `rStartup` — SPI's `DestSPI` creates its
`SPITupleTable` there, so omitting it makes `spi_printtup()` abort with
"improper call to spi_printtup" (plpgsql `DML ... RETURNING ... INTO`), and a
zero-row write additionally fails SPI's tuple-count check because no tuple table
was created. The bracket runs whenever the plan `hasReturning`, including the
zero-row case.

**EXPLAIN ANALYZE root-node instrumentation.** The same short-circuit means the
executor never runs the ModifyTable node's `ExecProcNode`, so the node's
`Instrumentation` is never started and EXPLAIN ANALYZE prints the root node as
`(never executed)`. The replay therefore brackets itself with one
`InstrStartNode(mt->ps.instrument)` / `InstrStopNode(mt->ps.instrument, rows)`
pair (rows = the RETURNING cardinality, 0 without RETURNING) so the node folds to
`actual rows=N loops=1`. On the UPSERT path the two counters `nodeModifyTable.c`
maintains are reproduced by hand: `InstrCountTuples2` for every proposed row that
found a conflict (whether DO UPDATE applied it or DO NOTHING / `WHERE false`
skipped it) and `InstrCountFiltered1` for a conflict removed by the
`DO UPDATE ... WHERE` filter. That makes `Tuples Inserted` (source rows −
conflicts), `Conflicting Tuples` and `Rows Removed by Conflict Filter` match
stock. All calls are no-ops without instrumentation (`instrument == NULL`).

A UPSERT whose `DO UPDATE SET` targetlist touches a PK column is refused
(`0A000`, promo 2/3) before any delta write: the delta row is keyed by the
conflict identity (pre-image PK), so a mutated post-image PK would desync
the delta key from the payload, exactly as on the plain UPDATE path.

**Slot aliasing: `WHERE` evaluation and the promo SET-merge.** Both the
`DO UPDATE ... WHERE` predicate and the promo 2/3 `SET` projection run with the
ModifyTable per-tuple `ExprContext` bound to slots the write path does not own:
`ecxt_scantuple` is the caller's live pre-image slot and `ecxt_innertuple` is
the plan's EXCLUDED slot (the subplan's current-row slot). Both are consumed
again afterwards, and on a **multi-row** UPSERT the EXCLUDED slot is the very
same subplan slot that `ExecProcNode` refills for the *next* row. The evaluation
must therefore save and restore the incoming `ecxt_*` pointers rather than
`ExecClearTuple` them. Clearing marks the caller's slot empty — `TupIsNull()`
then misreports a populated row as NULL (the `WHERE` case), and on the following
promo 2/3 row it releases the tuple behind the freshly fetched row's EXCLUDED
values, so a varlena `EXCLUDED.*` read faults on freed memory (the multi-row
`SET` case, observed as "compression method lz4 not supported"). Any slot handed
to an `ExprContext` and read afterwards follows this save/restore discipline.

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
partitioned / foreign tables, views, matviews, foreign keys. The
guard checks cheapest-first (relkind, `relispartition`, user
triggers, foreign keys, then no-PK). The per-DML executor guard validates only
the statement's DML **target** relations (the `ModifyTable` result relations):
a range-table entry that is merely read — the FROM/USING side of `INSERT ...
SELECT` / `UPDATE ... FROM` / `DELETE ... USING` — is not a target and is read
through the ordinary overlay path (BranchScan injects for every read RTE, §2.5),
so a managed read source sees this branch's writes. A no-PK read source is
skipped by BranchScan's own no-PK test, so it reads MAIN and does not abort the
statement. A utility-hook gate intercepts user DDL with
a session-private TEMP whitelist. At the very top of the executor intercept,
`COPY FROM`, `MERGE` and data-modifying CTEs raise 0A000; a write that reaches
`standard_ExecutorRun` without the redirect raises ERROR 55000 (write-through
choke).

**Row-locking clause gate.** A `SELECT ... FOR UPDATE/SHARE` over a
branch-managed relation also raises 0A000. A locking clause makes the planner
emit a `PlanRowMark` with strength `LCS_FORUPDATE` / `LCS_FORSHARE` /
`LCS_FORNOKEYUPDATE`, and the `LockRows` node then calls `heap_lock_tuple()` on
the ctid of every returned row. BranchScan emits overlay rows through a virtual
slot that carries no physical MAIN ctid, and a pure-delta row has no MAIN heap
row at all, so that call trips the `ItemPointerIsValid` assertion and aborts the
backend. Refusing is also semantically right: branch writes land in the delta
store and never touch MAIN rows, so a MAIN tuple lock is meaningless. The guard
lives in `ob_branchscan_begin` (the point a BranchScan is initialised for the
locked relation) and inspects `EState.es_plannedstmt->rowMarks`; it fires only
for a real locking clause (`strength != LCS_NONE`). The `ROW_MARK_REFERENCE`
row marks the planner adds for the **read** side of `UPDATE` / `DELETE` /
`MERGE` carry `LCS_NONE` and pass through untouched, so in-branch DML keeps
working.

**System-column gate.** Only `ctid` (a row's MAIN storage anchor) and `tableoid`
(the relation OID) are meaningful on a branch scan slot; the remaining system
columns (`xmin` / `xmax` / `cmin` / `cmax`) describe a physical storage tuple
that no branch row owns, so reading them is either a generic
`tts_virtual_getsysattr` "cannot retrieve a system column in this context" error
(plain SELECT) or a silent `0` off a heap-backed DML-target slot — the latter
made `DELETE ... WHERE xmin = 0` match and delete **every** row. `ob_branchscan_begin`
therefore walks the scan node's own targetlist and qual for a negative-attno
`Var` of the scan relid (excluding `ctid` / `tableoid`) and raises `0A000` at
plan-unpack time, mirroring the COPY / MERGE / FOR UPDATE gates.

**ACL double-defense** — branch administration is privileged. The SQL
layer grants DML on `pg_branch` / `pg_branch_delta` only to
`overlay_branch_administrators` (delta is revoked from PUBLIC — R22
business-data no-leak); the C layer runs `ob_acl_check_lifecycle()` as the
first line of every SQL-callable wrapper, raising a uniform 42501 on
denial. The `public.*` synonyms are `SECURITY INVOKER` with a fixed
`search_path` and are not the enforcement point. Effective identities:
superuser, or a member of the reserved NOLOGIN role
`overlay_branch_administrators`. CREATE/USE/APPLY/DISCARD all require that
identity (the branch catalogs they touch are DML-able only by it, so a
plainer caller could never complete the work); exiting to Main is open to
everyone. The `pg_branch.owner` column is provenance metadata, not an
authority grant. apply/discard are idempotent, and a discarded row is kept
as `state='discarded'` for audit.

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
| B2 | inject for every READ RTE: `CMD_SELECT`, the ACC-1 UPDATE/DELETE target, and the DML read sources; skip only the `INSERT` target (§2.5) | never wrap the insert target |
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

**Emitted slot system columns.** An overlay row is synthesised (a virtual tuple
on a plain SELECT, a heap tuple on a DML target), so it carries no storage
identity of its own. The scan therefore sets `tts_tableOid` to the relation OID
and `tts_tid` to the row's MAIN anchor, **after** the slot is stored (the virtual
`clear()` resets both fields): a MAIN-origin or merged row inherits its MAIN
`ctid`, while a pure-delta orphan (no MAIN row) keeps an invalid `ctid` so a
`WHERE ctid = ...` predicate is a safe no-op. Leaving them unset made a plain
`tableoid` read `0` and a `DELETE ... WHERE ctid IN (SELECT ctid FROM t)`
silently match nothing; the other system columns are gate-rejected (§1.8).

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

**Volatile clauses are never pushed down.** A `contain_volatile_functions()`
guard keeps any volatile clause out of the hidden SPI MAIN query. The clause is
otherwise evaluated twice — once by the SPI query to select MAIN rows and again
by `scan.plan.qual` row by row — so the two evaluations can disagree (silently
dropping the MAIN rows the SPI pass filtered out) and any side effect
(`nextval()`, ...) fires twice. Stable/immutable clauses are safe: the SPI query
runs in the same statement and snapshot, so both sites see the same value. The
guard is exercised by `test/regress/sql/overlay_branch_pushdown.sql`.

**The Pass-1 MAIN prefilter is volatile-gated too.** On the full-table read path
(`has_pk_pred == false`) the scan may pre-filter MAIN rows with
`scan.plan.qual` before merging. The same hazard applies: a volatile clause left
in the prefilter runs once on the raw MAIN row and again by the post-merge
`ExecQual` on the overlay row, so a side effect fires twice and the two results
can disagree. A volatile clause therefore also disables the prefilter and is
evaluated only by the single post-merge `ExecQual`. Note the prefilter exists
only on the no-PK-clause path — a query carrying a PK clause takes the P0
point-row fast path (§2.3), which never prefilters — so the two volatile paths
(PK-clause vs full-table) are pinned separately by PD.6 and PD.7 in
`test/regress/sql/overlay_branch_pushdown.sql`.

### 2.5 DML-target BranchScan (ACC-1) and read sources

The hook injects for `CMD_SELECT` and for every RTE of a DML that is **read**.
The UPDATE/DELETE DML *target* baserel (`CMD_UPDATE`/`CMD_DELETE` when the RTE
is `root->parse->resultRelation`) is injected too and flagged in
`custom_private`: without it the UPDATE/DELETE subplan scans MAIN rather than the
overlay, so consecutive in-branch writes break (`SET v = v||'+1'` then
`SET v = v||'+2'` lands on the MAIN value). This makes `ctid` reachable so WR can
anchor `old_version`; the emitted slot is a real heap tuple allocated in
`es_query_cxt` (**not** the scan context, which is deleted before
`ExecResetTupleTable`). WR's pre-image becomes "MAIN row for anchoring, overlay
row for values".

A DML-target scan must also let WR act on a **pure-delta** target row (a row the
**branch itself inserted**, so it exists only in the delta and has no MAIN ctid).
The scan records the delta key of each Pass 2 orphan it emits and stamps that row
with a synthetic ctid (`OB_SYNTH_CTID_BLOCK` + a 1-based key index);
`overlay_dml_target_pure_delta_key()` resolves it back to the key so WR processes
the row inline (§1.6). The synthetic block is chosen above any real heap block
number, and a plain `SELECT` (non-DML-target) scan never stamps it, so the
SELECT path is unaffected.

Every other RTE of a DML is a read source — the FROM/USING side of
`UPDATE ... FROM` / `DELETE ... USING` and the source of `INSERT ... SELECT` —
and is injected the same way, so it sees this branch's writes exactly like a
standalone `SELECT` or a correlated-subquery read source (a `CMD_SELECT` Query,
which was always injected). The one RTE deliberately **not** wrapped is the
`INSERT` target: it is not read at all (its conflict detection runs through WR's
own SPI probe, not this scan). Injecting the read sources keeps the same read
consistent whether it is written as a top-level `FROM` or as a subquery; a no-PK
read source is skipped by the injection's own no-PK test (§1.8), so it still
reads MAIN.

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

**`PG_TRY`/`PG_CATCH` locals must be `volatile`.** A local written inside a
`PG_TRY` block and read inside its `PG_CATCH` is *indeterminate* after the
`longjmp` (C99 7.13.2.1) unless declared `volatile`; the catch can then read the
initializer (e.g. `false`) and skip a cleanup (`SPI_finish()`, `PopActiveSnapshot()`)
it must run. Every such flag — `overlay_delta_count()`'s `did_connect`,
`ob_delta_size_warn_check()`'s `snap_pushed`, the PK point-lookup block's
`pushed_snap_`/`pop_done_`, `ob_check_publication()`'s `spi_connected` — is
declared `volatile` for this reason; the missed cleanup otherwise surfaces as a
`transaction left non-empty SPI stack` / `still active` WARNING at COMMIT.
`overlay_branch._debug_delta_count_error` (bool, `PGC_SUSET`, default off) is a
debug hook that makes the internal delta-count statement raise right after
`SPI_connect()`, driving this cleanup path deterministically in
`test/regress/sql/overlay_branch_deltastats.sql` (Section F).

---

## 3. Concurrency control

### 3.1 The four-layer concurrency defense tower

```
Layer 4  semantic correctness     SNAPSHOT mode MAIN freeze / LIVE mode delta visible
Layer 3  write-path protection    delta write takes the branch ShareLock first; then
                                  state in {applying, applied, discarded} rejects it
Layer 2  xact-level exclusion     branch advisory lock: writers ShareLock, apply/discard
                                  non-blocking ExclusiveLock
Layer 1  invalidation awareness   NOTIFY + threshold SPI fallback recheck
```

Correctness does not hinge on any single layer. Layer 1's NOTIFY is an
optimization. The cross-session freeze of `applying` hinges on Layer 2 (the
writer's `ShareLock`), because Layer 3's state check alone is MVCC-blind to the
apply transaction's **uncommitted** `applying` version (§3.4).

**Isolation-level requirement.** The freeze rests on an advisory lock plus a
*fresh* state re-read after any lock wait. Neither mechanism can see past a
transaction-snapshot snapshot, so the feature requires **READ COMMITTED**. Under
`REPEATABLE READ` / `SERIALIZABLE` (`IsolationUsesXactSnapshot()`) every read is
served from the fixed transaction snapshot, a concurrent apply/discard commit
stays invisible, and a late delta write could slip past the freeze to append an
orphan row. `use_branch` and the delta-write path therefore reject those
isolation levels with `0A000` rather than corrupt silently (§3.3, §3.4).

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

**Transaction-end GUC restore (LAYER-1b).** The `overlay_branch.current` assign
hook re-enters or leaves a branch as a *side effect*, so it must stay safe on
every path PostgreSQL calls it. `AtEOXact_GUC()` / `AtEOSubXact_GUC()` replay a
transaction's GUC changes on COMMIT **and on ABORT** — `ROLLBACK`, `ROLLBACK TO
SAVEPOINT`, and a PL/pgSQL `EXCEPTION` block — and on the abort path
`AbortTransaction()` / `AbortSubTransaction()` have already set the
current-transaction state to `TRANS_ABORT`, so `IsTransactionState()` is
**false**. Any SPI / relation access there (the mode-preserve lookup and
`use_internal()`) reaches `RelationIdGetRelation()`, whose
`Assert(IsTransactionState())` fires and `SIGABRT`s the backend. A
`!IsTransactionState()` guard therefore short-circuits the hook before any SPI:
if the live context already names `newval` there is nothing to do, otherwise the
context points at a branch entered inside the aborted (sub)transaction and the
hook drops to the safe no-branch state with a WARNING. The GUC value itself was
already written by PostgreSQL and cannot be undone from an assign hook, so
re-entering the old branch is deliberately not attempted (invariant 4).

**Abort-restore guard (LAYER-1).** A PL/pgSQL `EXCEPTION` block or any
(sub)transaction abort replays its GUC changes with the context possibly already
inactive (throttle / discard / apply kicked the session off the branch and ran
`exit_cleanup`). Re-entering there would run SPI inside an abort unwind, so the
guard returns early — but **only when there is no live transaction**
(`!IsTransactionState()`). A plain user `SET overlay_branch.current = '<branch>'`
also runs with an inactive context (e.g. right after an explicit
`use_branch(NULL)`), and with a live transaction it is *not* a restore: it must
enter the branch. Gating the guard on `!IsTransactionState()` keeps the abort
path SPI-free while letting the user SET take effect.

**`current_branch()` reflects the context, not the GUC.** The SQL function
returns the branch the writer is actually on — `overlay_branch_get_current_name()`
over the live `BranchContext` — never the raw `overlay_branch.current` string. The
two diverge on the ROLLBACK corner case above: the GUC is restored to the
pre-transaction name while the context has been dropped to no-branch, and
reporting the GUC would claim a branch the session is not writing to (§6.2).

### 3.3 Layer 2 - branch-level advisory lock

The advisory key is a magic high-16-bit namespace OR'd with the low 32-bit
`branch_id`, mapped onto PG's `pg_advisory_xact_lock(int8)` locktag. Two lock
modes are used on the *same* key, both transaction-scoped (auto-released on
COMMIT / ABORT):

- **`ShareLock` — delta writers.** `overlay_delta_insert` takes it **blocking**
  at the top of every delta write and holds it for the rest of the transaction.
  Re-acquiring it in the same xact is a no-op, so a bulk write pays one acquire.
  The state re-read that follows (§3.4) runs only on a *newly* acquired lock:
  `LOCKACQUIRE_OK` (first Share this xact) and `LOCKACQUIRE_NOT_AVAIL` (blocked
  behind an in-flight apply/discard) both re-read; `LOCKACQUIRE_ALREADY_HELD` is
  skipped, because the held lock pins the state until the transaction ends.
- **`ExclusiveLock` — the apply/discard CAS winner**, taken **non-blocking**
  (`dontWait`). A failed acquire raises ERROR 55000 immediately, before the CAS.

| | writer Share | apply X (dontWait) | discard X (dontWait) |
|---|---|---|---|
| writer Share | compatible | conflict | conflict |
| apply X | conflict | conflict | conflict |
| discard X | conflict | conflict | conflict |

`use_branch` takes **no** advisory lock (it only reads state). apply/discard
resolve the bid, then acquire the lock before the state check and before the
first user-table `RowExclusiveLock` or catalog UPDATE. Strict order: advisory →
user-table `RowExclusiveLock` (inside the 3-pass) → catalog locks.

**Isolation requirement.** `use_branch` and every delta write reject
`IsolationUsesXactSnapshot()` (REPEATABLE READ / SERIALIZABLE) with `0A000`: the
lock-then-re-read freeze is meaningless when reads cannot advance past the
transaction snapshot (§3.1). Leaving branch mode (`use_branch(NULL)`) stays
allowed — it only cleans up session state.

**Why `dontWait` (and no upgrade edge).** Making apply/discard block on
`ExclusiveLock` would reintroduce the historical V0 40P01: a session that wrote
delta (holding `ShareLock`) and then applies/discards the *same* branch requests
a `ShareLock → ExclusiveLock` upgrade, PostgreSQL's classic unbreakable upgrade,
and two such sessions deadlock. With a non-blocking acquire the wait-for graph
has a single direction — **only writers wait (behind an in-flight apply/discard);
apply and discard never wait** — so it cannot cycle. The trade-off: apply/discard
fail fast with ERROR 55000 when a writer (or another apply/discard) holds the
lock, and the user retries once that transaction ends.

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

**delta-insert barrier (lock + state).** At the top of `overlay_delta_insert`,
before any SPI, the writer (1) takes the blocking xact-scoped `ShareLock` (§3.3)
and (2) runs a state SELECT that rejects `state != 'active'` with ERROR 55000.
Every delta write funnels through this one function.

The state SELECT **must not** run on the statement's active snapshot. A single
long statement (e.g. `INSERT ... SELECT pg_sleep(...)`) can take its snapshot
before a concurrent apply commits and reach the delta write afterwards, at which
point its statement snapshot still reads the pre-apply `active` while the branch
is already terminal — the writer would then append an orphan delta row. The
check therefore reads a **fresh** snapshot (`read_only = false` →
`GetTransactionSnapshot()`, recomputed per call under READ COMMITTED), not the
frozen statement snapshot. It is skipped entirely on the
`LOCKACQUIRE_ALREADY_HELD` fast path (§3.3): a Share taken earlier in the same
transaction already verified `active` and pins the state until the transaction
ends.

The lock is what actually freezes `applying` **across sessions**. `applying`
exists only as the apply transaction's **uncommitted** heap version, so it is
invisible to every other session's MVCC snapshot — a plain state SELECT cannot
see it, and `state='applying'` is therefore *not* a cross-session signal on its
own. An advisory lock, being transaction-scoped rather than MVCC-scoped, is. So a
writer arriving while an apply is mid-flight blocks on the apply's
`ExclusiveLock`, resumes only after the apply ends, then reads the committed
terminal state (`applied`) and is rejected; a writer that got there first makes
the apply fail fast (its `dontWait` `ExclusiveLock` is refused). Either way the
invariant holds: no delta row is written while an apply is in progress, and no
in-flight (uncommitted) delta is ever merged.

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

The forced exit is a *local* kickout (no SPI round-trip), so its diagnostic must
name the TTL reason rather than the generic "invalidated by another session"
text; the error hint reports `snapshot_max_hold_minutes (R19 TTL)` and the guard
flag is cleared before the `ereport` so a subsequent error cannot observe it.
Because `snapshot_max_hold_minutes` is `PGC_POSTMASTER` with a one-minute floor,
`overlay_branch._debug_snapshot_ttl_seconds` (`int`, `PGC_SUSET`, default -1,
seconds) overrides the cap at second granularity so the kickout is reachable
deterministically in a regression test; -1 (default) honours the production cap.
Both the diagnostic and the debug GUC are exercised by
`test/regress/sql/overlay_branch_rentry.sql`.

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

The apply/discard `ExclusiveLock` acquire is non-blocking (§3.3): a concurrent
apply/discard, or an apply while a delta writer is in flight, fails fast with
ERROR 55000. Once the lock is held, the step 1.5 CAS row count is the inner
winner gate; a second apply (or discarding an applied/discarded branch)
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
   `PG_FINALLY`; C90 top-declaration is a hard build requirement. The seven
   bypass-stack flags (`ob_in_*` / the opt-in bool) are saved and restored as a
   block on the same subxact save/restore as the executor-context stack, so an
   error that longjmps past a matching `overlay_*_exit()` cannot leave a flag
   stuck true for the rest of the session (a stuck flag silently disables the
   overlay and reads fall back to MAIN).
   Two guard-adjacent changes are **defensive and carry no dedicated regression
   test**: the planner-hook `PG_TRY`/`PG_FINALLY` wrap (redundant given the
   subxact save/restore plus the xact reset — its trigger, `is_active()` raising
   during planning, is not reachable from SQL) and the `SPI_connect`-failure
   error-prefix correction (an unreachable path).
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
conflict validation reuses the ctid/xmin/`_base` chain, decoupled from the PK;
the set-based replay joins keyless deltas on that ctid anchor instead of a PK.

### 4.5 Anchor-invalidation triggers

An anchor is valid iff its ctid still points to the same physical version of the
same logical row; other sessions can break it:

| Trigger | Read side | apply side |
|---|---|---|
| Concurrent UPDATE of an anchored row | version chain followed → delta **re-anchored** to the current version | no visible row → 0A000 |
| Concurrent DELETE | delta unmatched → **silent drop** | no visible row → 0A000 |
| ctid reuse (new INSERT after VACUUM takes the slot) | ctid match hits the new row; the `xmin` check deems it stale → dropped (**no misread**) | `old_version` `xmin` mismatch → 0A000 |
| Whole-table rewrite (`VACUUM FULL`/`CLUSTER`/`REINDEX`/`TRUNCATE` refill) | all anchors mismatch → **silent drop** | 0A000 row by row |

The read side's `xmin` check: after a ctid equality match, Pass 1 compares the
`-x<xmin>` suffix of `old_version` against the MAIN tuple's raw `xmin`; on
mismatch the row falls into the Pass 2 orphan rule (unmatched `A:*` orphans are
not revived), so ctid reuse degrades to a silent drop rather than a misread.

**Version-chain resolution (read side).** When a concurrent UPDATE moves an
anchored row to a new ctid, the delta's stale anchor would otherwise go
unmatched. At scan setup the reader resolves each keyless `A:` anchor along the
`t_ctid` chain with `heap_get_latest_tid` (under the scan's snapshot) and, when
the row has moved, registers the delta under the current ctid so Pass 1 still
matches it. The resolution is guarded: the anchor line pointer must still hold
the same logical version (its raw `xmin` equals the `old_version` suffix), so a
recycled line pointer is never re-anchored. It is deliberately read-only — the
persisted anchor key is left untouched, so apply still reports the moved row as
a 0A000 conflict. `DELETE`d rows and whole-table rewrites have no `t_ctid`
successor to follow and keep the silent-drop behavior.

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

### 4.7 Scope

**Implemented:** SELECT / INSERT / UPDATE / DELETE on no-PK tables (best-effort
Approach A); the identity serializer, the read-side anchor `xmin` check and
version-chain resolution, apply positioning and conflict validation, the
per-`(session, relid)` WARNING, and `INSERT ... ON CONFLICT` (see below).

**Keyless UPSERT.** A no-PK table arbitrates a conflict on the UNIQUE constraint
the statement names, not on the ctid identity that keys the delta. The redirect
reuses the PK path's two-phase detector with its probes rebound to the arbiter
columns, so all downstream promotion and apply behaviour is unchanged:

- the arbiter must resolve to exactly one UNIQUE index (multi-column allowed);
  several candidate indexes, or an expression index, keep the 0A000 guard;
- phase one matches a delta row whose stored image carries the proposed arbiter
  values, restricted to the live insert/update states (a tombstone stores no
  image, so it is never matched here — see the limitation below);
- phase two matches a MAIN row carrying the same arbiter values;
- a NULL arbiter value follows the UNIQUE index's null handling: under the
  default NULLS DISTINCT index it matches nothing, so the statement degrades to
  a plain INSERT, while under a NULLS NOT DISTINCT index it matches an existing
  NULL row. The probe therefore picks `=` (NULLS DISTINCT) or `IS NOT DISTINCT
  FROM` (NULLS NOT DISTINCT) per the index's `ii_NullsNotDistinct`, and always
  binds the value as a real SQL NULL — a NULL arbiter column passed as a bogus
  datum once crashed the server when SPI dereferenced it;
- on a MAIN hit the delta is re-anchored to that row's ctid with its version
  token, on a delta hit the existing delta key and version are reused, and with
  no hit the row takes a synthetic key as a plain insert;
- a branch-local DELETE frees the arbiter value: the tombstone stores no arbiter
  image, so phase one cannot see it, but the phase-two MAIN probe drops any row
  the delta shadows (its token equals a delta `old_version`), so the later
  UPSERT rebirths the row — a plain insert under a fresh synthetic key, exactly
  like the keyed path (see "Delta-first shadow", §1.6).

**Unsupported (existing 0A000 style):** keyless + partitioned / foreign-key /
`MERGE` / `COPY FROM` / DML-CTE, and a keyless UPSERT whose arbiter does not
resolve to exactly one UNIQUE index; consistency under concurrent `DELETE` /
whole-table rewrites (silent drop on the read side, apply error as fallback).

---

## 5. Performance

### 5.1 Performance model & notation

`N` = MAIN rows; `M` = delta rows; `A` = user columns; `S` = average bytes/row.
The delta PK is `(branch_id, relid, key)` and every write lands in the
`ON CONFLICT ... DO UPDATE` of `overlay_delta_insert`, so **`M` = distinct rows
touched, independent of operation count** (the write-side upsert performs the
latest-wins collapse — including the DELETE→re-INSERT `D→U` rewrite of §1.6 — so
the read path never sees two delta rows for one key). A delta row is far heavier than a
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
  conflict rolls back all batched DML. Keyless relations batch through a
  ctid-anchor join (the `A:<blk>:<off>` key rendered as `m.ctid = ...`) instead
  of a PK join. Falls back per-row for GUC off / any delta row containing
  `_base`. A 20000-row apply drops from ~1261 ms to ~166 ms (~7×).

Invariants preserved: D→U→I order; per-row optimistic conflict semantics
(missing MAIN / `old_version` mismatch / HOT base-image divergence → 55000);
pure-delta cases; keyless identity; lock order advisory → catalog → user
`RowExclusiveLock`; the CAS state machine with the xact-scoped exclusive advisory
lock. Error messages stay byte-for-byte aligned with the baselines.

### 5.6 Delta size: observability

`M` is independent of operation count, so "the delta grows without bound" does
not hold; the real risk is a branch touching near the whole table, plus the
per-row weight doubled by `_base`. Two read-only products expose this:

| Metric | Definition |
|---|---|
| `delta_rows` | `count(*)` on `(branch_id, relid)` |
| `delta_insert` / `delta_update` / `delta_delete` | per-`op` row counts |
| `delta_payload_bytes` | `sum` of key + old_version + tuple_data octet lengths |
| `main_bytes` | `pg_total_relation_size(relid)` |
| `amplification` | `delta_payload_bytes / main_bytes` (includes the `_base` doubling) |

**Query interface — `branch_delta_stats(branch_name)`.** A read-only SQL
function returning one row per `(branch, relation)`, plus a `public` synonym.
It exposes counts and byte sizes only — never delta row content. Access mirrors
`list_branches()`: it reads the delta catalog, which is administrator-only per
R22, so SECURITY INVOKER is retained.

**Throttled WARNING.** `overlay_branch.delta_size_warn_rows` (int, default 0 =
disabled, PGC_USERSET). When > 0, the single delta-write choke point
(`overlay_delta_insert`) counts the current branch's delta rows once every
`overlay_branch.delta_size_check_interval` (int, default 1024) writes and emits
one WARNING per session when the count first exceeds the threshold, naming the
branch and pointing at `branch_delta_stats()`. The count is amortized over the
interval, the counter is session-local and never persisted, and the check runs
inside `PG_TRY` fail-open so a catalog/SPI problem can never break user DML.
Both GUCs are no-ops when the threshold is 0, so the default write path is
unchanged.

Reference queries remain in `test/bench/perf_baseline.sql`.

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
   never raises ERROR and never runs SPI / relation access when
   `IsTransactionState()` is false (the abort-path GUC restore, §3.2).
5. **Recursion guards are balanced under `PG_TRY`/`PG_FINALLY`**, including the
   abort path.
6. **Cross-SPI memory lives in `TopMemoryContext`**, copied out before
   `SPI_finish()`; error-path copies use the caller's context (§2.8).
7. **Writers hold `ShareLock`, apply/discard a non-blocking `ExclusiveLock`** on
   the branch advisory key; `use_branch` takes none — only writers ever wait, so
   there is no upgrade edge and no cycle (§3.3).
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
13. **Branch operations require READ COMMITTED** — `use_branch` and the delta
    write reject `IsolationUsesXactSnapshot()` (REPEATABLE READ / SERIALIZABLE)
    with `0A000`; the freeze cannot observe a concurrent commit past a
    transaction snapshot (§3.1, §3.3).
14. **The delta-write state check reads a fresh snapshot** — never the statement's
    active snapshot, which may predate a concurrent apply commit; it re-runs only
    on a newly acquired `ShareLock` (§3.4).
15. **`PG_TRY`/`PG_CATCH` cleanup flags are `volatile`** — a local written in the
    `PG_TRY` and read in the `PG_CATCH` is indeterminate after the `longjmp`
    (C99 7.13.2.1); the abort path must still `SPI_finish()` / pop snapshots
    (§2.8).
16. **Volatile clauses are never pushed into the hidden SPI MAIN query** — only
    stable/immutable clauses, so double evaluation cannot drop rows or fire a
    side effect twice (§2.4).
17. **Only `ctid` and `tableoid` are served on a branch scan slot**; a negative
    `Var` for `xmin` / `xmax` / `cmin` / `cmax` raises `0A000` at plan-unpack
    time, because no branch row owns a storage tuple (§1.8).
18. **The emitted slot's `tts_tableOid` / `tts_tid` are set after the store** —
    `tableoid` names the relation, `ctid` the row's MAIN anchor (invalid for a
    pure-delta orphan), so system-column reads and a ctid-subquery DELETE work
    (§2.2).
19. **`current_branch()` reports the live branch context, never the raw GUC
    string** — the two diverge after a cross-branch ROLLBACK, where the GUC keeps
    the pre-transaction name while the context is dropped to no-branch (§3.2,
    §6.2).

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
- **ROLLBACK after an in-transaction `use_branch`.** A `use_branch()` inside a
  transaction that then aborts makes `AtEOXact_GUC()` / `AtEOSubXact_GUC()`
  restore the pre-transaction GUC value, re-firing the assign hook after
  `AbortTransaction()` has set the state to `TRANS_ABORT`. The LAYER-1b guard
  keeps the hook SPI-free: on the same branch it is a no-op, on a changed branch
  it warns and drops to no-branch (the GUC value itself was already restored by
  PostgreSQL and cannot be undone) (§3.2). After such a rollback the session
  sits outside any branch even though `overlay_branch.current` still names the
  pre-transaction branch: `current_branch()` reports no branch and writes go to
  MAIN (invariant 19).
- **`SET overlay_branch.current` after an explicit exit.** Right after
  `use_branch(NULL)` the context is inactive but the session still has a live
  transaction, so a plain `SET overlay_branch.current = '<branch>'` re-enters the
  branch — the LAYER-1 abort-restore guard is gated on `!IsTransactionState()`
  and must not swallow it (§3.2).
