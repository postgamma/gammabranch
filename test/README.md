# Tests

The extension is verified by three layers. Only **L1 + L3** are gates for
`make installcheck`; **L2** is a manual stress/performance harness.

| Layer | Harness | Count | Gate? | What it proves |
|---|---|---|---|---|
| L3 | `pg_regress` (`test/regress/`) | 21 suites | yes | Functional correctness of the public API, guards, overlay semantics, planner pushdown, error paths |
| L1 | `pg_isolation_regress` (`test/isolation/`) | 15 specs | yes | Concurrency correctness — CAS mutex, applying-freeze, snapshot mode, invalidation kickout, cross-session ReScan, pure-delta, keyless |
| L2 | `pgbench` (`test/bench/`) | manual | no | High-concurrency stress and reproducible performance baselines |

## Running

PGXS does not provide a temp-instance `make check`; install first and run against a server
with `shared_preload_libraries='overlay_branch'`:

```bash
make USE_PGXS=1 install
make USE_PGXS=1 installcheck          # runs L3 + L1
```

Always use a real installed UTF-8 locale — `LC_ALL=C.utf8`. `LANG=C` mis-measures
multibyte column widths and produces bogus column-width diffs.

## L3 — `pg_regress` (`test/regress/sql/`, expected in `test/regress/expected/`)

Each file is standalone-idempotent: it starts with
`DROP EXTENSION IF EXISTS overlay_branch CASCADE; RESET ALL; CREATE EXTENSION ...`.

| File | Focus |
|---|---|
| `overlay_branch_basic` | Engineering happy path across the four lifecycle steps; branch-context GUC semantics; guard and idempotency coverage |
| `overlay_branch_user` | Clean user-facing end-to-end story (mirrors `doc/user_guide.md`) |
| `overlay_branch_view_unify` | Read/write view unification: self-join with two BranchScan instances, same-CSS rescan |
| `overlay_branch_scan_pk` | BranchScan P0/P1/P2 core paths: PK fast path, RETURNING, non-PK qual pushdown |
| `overlay_branch_scan_type` | PK type diversity (SMALLINT/BIGINT/NUMERIC/BPCHAR/VARCHAR/TIMESTAMPTZ/TEXT/UUID) and PK edge cases |
| `overlay_branch_pushdown` | Non-PK qual pushdown and Pass 2 orphan emission |
| `overlay_branch_planner_cost` | CustomScan cost model is non-zero and derives from the O(M log M + N log M) merge |
| `overlay_branch_puredelta` | Pure-delta UPDATE/DELETE MAIN-miss chains (I→U, I→D, apply/discard) |
| `overlay_branch_puredelta_mix` | Pure-delta mixed DML lifecycle, rebirth chains, BPCHAR/NUMERIC typmod PK |
| `overlay_branch_upsert` | Pure-delta UPSERT 2-phase conflict detection and the 4-way promotion matrix |
| `overlay_branch_acc1` | DML-target BranchScan (ACC-1): overlay pre-image for chained in-branch DML |
| `overlay_branch_execcontext` | Per-statement branch identity (`BranchExecContext`) A1/A6 |
| `overlay_branch_plancache` | WR-1a parameterized prepared-plan cache produces identical delta bytes |
| `overlay_branch_parallel` | No parallel scan over bare MAIN while a branch is active (MF-2/MF-3) |
| `overlay_branch_memgrow` | Repeated full-table reads do not grow backend memory (MF-1) |
| `overlay_branch_keyless` | Experimental keyless ctid identity (`overlay_branch.keyless_tables`) |
| `overlay_branch_deltastats` | Branch delta size observability (`branch_delta_stats`) and the delta row-count warning GUC |
| `overlay_branch_mvcc_usage` | MVCC basic usage, mode/threshold GUCs, invalidation DQL/DML split |
| `overlay_branch_mvcc_bounds` | MVCC advanced boundaries: advisory-lock key equivalence, cross-branch isolation, API bounds |
| `overlay_branch_mvcc_review` | API/versioning contracts and R15/R18 apply conflict detection |
| `overlay_branch_rentry` | Entry drift-write hard interception and snapshot TTL |

## L1 — `pg_isolation_regress` (`test/isolation/specs/`)

| Spec | Focus |
|---|---|
| `ob_apply_mutex` | Dual-apply CAS mutex; the loser fast-fails without deadlock |
| `ob_applying_freeze` | `applying` transient state freezes delta writes during the merge |
| `ob_state_inval` | FR4 kickout via the throttled SPI counter; DML hard error on a stale branch |
| `ob_snapshot_mode` | SNAPSHOT freeze contract: MAIN frozen, delta stays visible |
| `ob_branchscan_rescan` | Cross-session fresh ReScan sees a newly committed delta |
| `ob_view_unify_rescan` | Same-CSS ReScan inside a nested-loop/LATERAL plan |
| `pure_delta_upsert` | Pure-delta UPSERT conflict promotion across sessions |
| `pure_delta_update` | Pure-delta UPDATE MAIN-miss permutations |
| `pure_delta_delete` | Pure-delta DELETE MAIN-miss permutations |
| `keyless_basic` | Keyless GUC warning, overlay consistency, independent branches |
| `keyless_pure_delta_repeat` | Keyless synthetic-key stability across repeated UPDATEs |
| `keyless_apply_conflict` | Keyless cross-session apply conflict → `0A000`, branch stays active |
| `keyless_version_chain` | Keyless read-side version-chain resolution: the overlay still shows the branch edit after a concurrent MAIN rewrite moves the anchor's ctid |
| `keyless_upsert` | Keyless cross-session UPSERT arbitration: MAIN-hit re-anchoring, delta-hit key reuse, no-hit synthetic insert; a stale anchor → `0A000` |
| `keyless_delete_invalidation` | Keyless anchor invalidated by a concurrent DELETE: the delta is dropped on the read side, apply → `0A000`, branch stays active |

## L2 — `pgbench` (`test/bench/`)

Manual only (not a gate): concurrency stress fixtures, the reproducible
performance baseline, the SRF peak-memory probe, and the mixed-foreground /
multi-agent scalability probes. See [bench/README.md](bench/README.md).
