# pg_overlay_branch — PostgreSQL 18 / 19 Version Support Assessment

Status: assessment only. No code change is proposed or applied here.

## Scope

Estimate the effort to make `pg_overlay_branch` (currently developed and tested
on PostgreSQL 17 only) build and run on PostgreSQL 18 and PostgreSQL 19 (rc).

## Method

All breakages below were obtained from real builds, not inferred:

| Target | Server source | Build outcome |
|---|---|---|
| PG 17.11 | `/home/ubuntu/workspace_ub/postgres` | `make -k USE_PGXS=1` → **0 error / 0 warning** (green baseline) |
| PG 18.6 | `/home/ubuntu/workspace_ub/postgres2` | `make -k USE_PGXS=1` → **6 error + 1 warning** |
| PG 19rc1 | `/home/ubuntu/workspace_ub/postgres2` (branch `rel19`) | `make -k USE_PGXS=1` → **16 error + 1 warning** |

Each server was built in-tree and installed (`--enable-cassert --enable-debug`).
`make -k` (keep-going) is used so that every error in every translation unit is
collected, instead of stopping at the first file.

## Verdict

The port is mechanical. Breakage is confined to seven API changes spanning 5
files and roughly 20 code sites. PG 18 is a small effort; PG 19 is small to
medium. The dominant cost is **not** the code edit — it is test baseline
regeneration plus runtime re-validation, and the introduction of a version-gating
layer (the project currently has none).

## Breakage inventory

### PG 18 (6 errors + 1 warning)

| # | Location | Change | Fix class |
|---|---|---|---|
| 1 | `include/overlay_branch.h` (hook typedef), `src/overlay_branch.c` (`_PG_init` + `overlay_ExecutorRun`), `src/write_redirect.c` (intercept + fall-through) | `ExecutorRun` / `standard_ExecutorRun` / `ExecutorRun_hook_type` drop the `bool execute_once` argument | Version-gated signature (typedef + wrapper + call sites) |
| 2 | `src/write_redirect.c` (range-table walk) | `ExecGetRangeTableRelation` gains a third argument `bool isResultRel` | Pass argument (this site is a generic rtable walk → `false`) |
| 3 | `src/overlay_branch.c` (`CurrentBranchExecContext` definition) | PG 18 enables `-Wmissing-variable-declarations` by default | Add an `extern` declaration in a header (mandatory under the zero-warning policy) |

### PG 19 (in addition to PG 18: +10 errors)

| # | Location | Change | Fix class |
|---|---|---|---|
| 4 | `src/write_redirect.c` (5 sites) | `PlannedStmt.resultRelations` (List) becomes `resultRelationRelids` (Bitmapset) | Iteration rewrite (`foreach` → `bms_next_member`), not a rename |
| 5 | `src/branch_scan.c` (scan open) | `table_beginscan` gains a fifth argument `uint32 flags` (PG 17/18 take 4) | Pass `, 0` |
| 6 | `src/overlay_branch.c` (2 sites) | `ClusterStmt` / `T_ClusterStmt` are removed; `CLUSTER` folds into the new `RepackStmt` (`T_RepackStmt`, `command = REPACK_COMMAND_CLUSTER`) | Semantic migration + version gate; guard scope must be decided |
| 7 | `src/branch_lifecycle.c` (2 sites) | `index_open` / `index_close` are no longer transitively declared (PG 19 header reorganisation drops `access/genam.h` from the include chain) | Add explicit `#include "access/genam.h"` |

Note: the `plan->resultRelations` uses in `src/write_redirect.c` are fields of
**ModifyTable**, which PG 19 keeps as a List. They are **not** affected by #4.

## Effort grading

| Item | Size | Notes |
|---|---|---|
| Compile fixes | Small | 7 change patterns, ~20 sites, 5 files, well under ~120 lines |
| Version-gating layer | Small–Medium | No gating exists today. Introduce `#if PG_VERSION_NUM >= 180000 / 190000`, ideally behind a small `include/ob_compat.h` wrapping four concerns: the ExecutorRun hook signature, `ExecGetRangeTableRelation`, result-relation relid iteration, and the CLUSTER/REPACK statement test |
| CI matrix | Small | CI currently pins PG 17 only; extend to a 17/18/19 build + installcheck matrix |
| Test baseline rebuild + runtime validation | **Medium–High (dominant)** | See below |

## Real cost (beyond compilation)

1. **Baseline regeneration.** The L3 (21 files) and L1 (15 specs) baselines were
   generated under PG 17. Major releases commonly shift: error-message wording,
   planner cost/plan text, NOTICE wording, and CustomScan EXPLAIN node naming.
   Planner-cost-style tests are especially sensitive. Each version needs its own
   baseline (per-version `expected/` directories or version-robust assertions).
2. **Runtime correctness (compiles ≠ correct).** The extension leans on internal
   structures that churn every major release: `ModifyTableState`,
   `ResultRelInfo`, `OnConflictSetState`. PG 18 already added fields to
   `ModifyTableState` and renamed `ri_ConstraintExprs`. The write-redirect path
   must be re-validated per version even when it compiles.
3. **PG 18 ExecutorRun rework.** The removal of `execute_once` is tied to a
   broader executor refactor; the intercept's assumptions about call timing must
   be re-checked, not just re-compiled.

## Structural risk

- The read path (`src/branch_scan.c`, CustomScan / ExecMethods) is **stable** —
  PG 19 broke it only via the single `table_beginscan` change.
- The write path (`src/write_redirect.c`) is the **fragile surface**: it reaches
  into ModifyTable / ResultRelInfo internals, so every future major release
  requires re-validation of the redirect path.
- All use is through public API (hooks + CustomScan + tableam); no direct tableam
  vtable calls. The blast radius stays bounded.

## Suggested implementation order (when work is authorised)

1. Add the `include/ob_compat.h` version-gating layer (one macro group for PG 18,
   one for PG 19).
2. Fix the seven patterns in PG 18 → PG 19 order, driving `make -k` to
   zero errors and zero warnings at each step.
3. Extend CI to a 17/18/19 matrix; partition `expected/` per version.
4. Run L1/L3 on each version and confirm the write-redirect and apply paths
   behave identically at runtime.
