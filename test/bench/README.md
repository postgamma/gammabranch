# T8 L2 pgbench Concurrency Stress Fixture (non-gating, for manual debugging)

> R7 constraint restated: correctness first, performance second. **This L2 fixture is not a `make installcheck` gate** (gates = L1 15/15 isolation specs + L3 21/21 pg_regress), and only needs to be run manually in the following scenarios:
> - When you want to stress-test the FR2 lock protocol under high concurrency (high-frequency CAS winner/loser)
> - When you want to probabilistically trigger the NOTIFY path (stable long-lived LISTEN)
> - When you want to reproduce an edge-case race that the L1 15 specs cannot cover

---

## Directory contents

| File | Purpose | When to run |
|---|---|---|
| `apply_contention_fixture.sql` | **One-time initialization script**: CREATE EXTENSION / pgbench_accounts 10000-row baseline rows / 8 branches `bs_pgbench_{1..8}` | Before the first stress run, run **only once** (idempotent) |
| `apply_contention.sql` | **pgbench custom script**: each connection is assigned one of 4 roles by `CLIENT_ID mod 10` (60% WR writer / 20% branch switcher / 10% applyer / 10% discarder), random DML / apply / discard / live↔snapshot rotate | Executed continuously during each stress run, timed by `pgbench -T 60` |
| `run_perf_baseline.sh` | **Performance-baseline driver script**: `run_perf_baseline.sh [N] [M]` (default 200000/20000) → `LC_ALL=C.utf8 psql -f perf_baseline.sql`, and `tee`s the full transcript to `perf_baseline_<N>_<M>.log` | Run manually when you want comparable wall-clock numbers for a given (N,M) size (non-gating) |
| `perf_baseline.sql` | **Reproducible performance baseline**: builds/cleans up `public.ob_perf_base` (controllable set-based delta), and per the internals.md §5 cost centers measures in sequence M1 MAIN seqscan / M2 M=0 overlay / M3 full overlay / M4 PK equality (P0) / M5 non-PK (P2) / M6 EXPLAIN / M7 delta size amplification / **M10a/b/c write-path timing (one segment each for UPDATE/DELETE/INSERT, §5.4 WR-1 baseline)** / M8 apply timing / M9 MAIN after apply; each step is sandwiched with `pg_backend_memory_contexts` leak observation | Run manually to get before/after comparisons for performance work, or to verify the effect of the MF-1/MF-4/MF-5 fixes |
| `perf_baseline_<N>_<M>.log` | **Run artifact** (not hand-edited): the full psql transcript `tee`d by the script above, archived for before/after comparison | Auto-generated in this directory by `run_perf_baseline.sh`; **not committed** |
| `srf_peak_rss.sh` | **Peak-memory probe**: `srf_peak_rss.sh [rows] [max_growth_kb]` (default 300000/16384). Runs two full-table scans of `overlay_main_plus_delta()`, and judges PASS/FAIL solely on the VmHWM growth of the **second** scan | Run manually to verify that MF-1 leak 9 (SRF per-row scratch allocated in the multi-call ctx) does not regress (non-gating) |
| `mixed_impact_fixture.sql` | **One-time initialization script** (G05/S28): creates two tables that share no rows — `public.mi_accounts` (MAIN foreground, 100000 rows) / `public.mi_agent` (Agent branch, 50000 rows); idempotent | Before the first run of the G05/S28 mixed-impact baseline, run **only once** (idempotent) |
| `mixed_main_oltp.sql` | **MAIN foreground OLTP workload** (G05/S28): per transaction one indexed point `UPDATE` + one `SELECT`, **does not involve overlay_branch** | Used by `run_mixed_impact.sh` as the fixed MAIN load for Phase A/B/C |
| `mixed_agent.sql` | **Agent branch workload** (G05/S28): per transaction `use_branch(:brname,'live')` + one overlay read `count(*)` + one delta write | Used by `run_mixed_impact.sh` as the Agent load for Phase B |
| `mixed_agent_nobranch.sql` | **CONTROL workload** (G05/S28): query shape/load is **exactly the same** as `mixed_agent.sql`, but it **does not enter a branch** (no `use_branch`); reads/writes go straight to MAIN | Passed to `run_mixed_impact.sh` via `PGAGENTSQL=` as Phase C, to separate the "Agent overlay path" from "just one more concurrent client" |
| `run_mixed_impact.sh` | **G05/S28 mixed foreground impact baseline driver**: `run_mixed_impact.sh [N] [T] [M]` (default 100/30/1) → interleaved repeats + Latin-square phase rotation, median summary of MAIN tps/p50/p95/p99 + CPU busy + Agent p99, `tee`d to `mixed_impact_<N>_<T>_<M>[_tag].log` | Run manually when you need to quantify "the impact of having vs. not having an Agent branch session on MAIN foreground on the same machine/config" (**non-gating, no PASS/FAIL**) |
| `mixed_impact_<N>_<T>_<M>[_tag].log` | **Run artifact** (not hand-edited): the full transcript `tee`d by the script above (with per-phase raw samples + SUMMARY), archived for before/after comparison | Auto-generated in this directory by `run_mixed_impact.sh`; **not committed** |
| `mixed_agent_own.sql` | **Own-branch Agent workload** (multi-Agent scalability probe): each client `use_branch(:brpfx \|\| :client_id, 'live')` into its **own** branch + one overlay read + one delta write; query shape is exactly the same as `mixed_agent.sql` | Used by `run_agent_scale.sh` as the Agent load for the B\<M\> case |
| `run_agent_scale.sh` | **Multi-Agent concurrency scalability probe driver**: `run_agent_scale.sh [N] [T] "M_LIST"` (default 16 15 "1 2 4 8") → fixed MAIN, with Latin-square rotation over the M list within the same interleaved run, median summary per M of MAIN tps/Δtps%/p50/p95/p99 + Agent p99/tps + CPU busy% | Run manually when you need the "degradation shape/knee as the number M of concurrent **own-branch** Agents grows on MAIN foreground" (**non-gating, no PASS/FAIL**) |
| `agent_scale_<N>_<T>_<MLIST>[_tag].log` | **Run artifact** (not hand-edited): the full transcript `tee`d by the script above (with per-phase raw samples + SUMMARY), archived for before/after comparison | Auto-generated in this directory by `run_agent_scale.sh`; **not committed** |

> The directory is split into five categories: `apply_contention*` = **concurrency correctness** L2 stress; `perf_baseline*` = **performance baseline** L2 fixture (against the [internals.md](../../doc/internals.md) §5 cost centers; numbers only, no PASS/FAIL); `srf_peak_rss.sh` = **peak-memory probe** (the only L2 script with a PASS/FAIL judgment); `mixed_impact*` + `run_mixed_impact.sh` = **G05/S28 mixed foreground impact baseline**; `mixed_agent_own.sql` + `run_agent_scale.sh` = **G05/S28 multi-Agent concurrency scalability probe** (see the next section; numbers only, no PASS/FAIL).

### srf_peak_rss.sh (MF-1 leak 9)

`overlay_main_plus_delta()` is a FROM-clause SRF; leak 9 used to allocate the per-row scratch (`values[]`/`isnulls[]`) in the SRF's multi-call ctx (deleted only when the SRF ends), making a full-table scan grow linearly with row count. That growth is a **transient peak within the statement**, released right after the statement ends, so the `TopMemoryContext` before/after assertions in `test/regress` cannot see it — only the process peak RSS can.

**Why run twice**: even after the fix, the **first** full-table scan still raises VmHWM by O(rows) (executor per-tuple ctx, SRF tuplestore, heap pages churning in the allocator — glibc keeps freed pages in the process heap and does not return them to the OS, so RSS stays high after the statement ends). This is a **one-off warm-up, bounded, not a leak**. A leak has a different signature: **every** scan grows the same memory. So this probe judges on the VmHWM growth of the **second** scan, with a default threshold of 16 MB (measured second-scan growth ≈ 0.4 MB; with the leak it is about 80 MB per scan).

```bash
PGDATABASE=ob_bench ./test/bench/srf_peak_rss.sh 300000 16384
#   SRF_PEAK_RSS_PASS  (second-scan growth ≈ 0.4 MB)
```

> The target database must support `create_branch` (G01 support matrix: the database must not contain views or other non-`r`/`S`/`i` relkinds). The default `ubuntu` database contains the `pg_buffercache` view and will error, so the example uses a clean database `ob_bench`.

### run_mixed_impact.sh (mixed foreground impact)

**Purpose**: quantify the impact on MAIN foreground OLTP of "attaching one extra Agent branch session on the same instance and configuration". **Numbers only, no PASS/FAIL** — thresholds must be frozen after being determined by the actual deployed target workload.

The three cases run interleaved (each case repeated R times, median taken per column):

| case | Composition | Purpose |
|---|---|---|
| A | N MAIN OLTP clients only | Baseline (no Agent) |
| B | N MAIN + M Agent (enter branch) | "With Agent" |
| C (optional) | N MAIN + M control (same load but no branch) | Separate "overlay path" from "one more concurrent client" |

```bash
export PGPORT=5432 PGUSER=ubuntu PGDATABASE=ob_bench
psql -f test/bench/mixed_impact_fixture.sql          # run only once
./test/bench/run_mixed_impact.sh 100 30 1            # A/B, about 4.5 min
# add control (Phase C):
PGEXTRASQL=test/bench/mixed_agent_nobranch.sql PGTAG=abc \
    ./test/bench/run_mixed_impact.sh 100 30 1
```

**Methodology (artifacts removed, all required)**:

- **Interleaved repeats + median**: a single A/B is dominated by run-to-run variance; R interleaved repeats (`PGREPEAT`, default 4, must be even) take the median.
- **Latin-square phase rotation**: each round places each case at a different position (first/second/…), otherwise the position effect of "the phase that runs later is faster" cannot be distinguished from the real Agent impact.
- **Symmetric idle lead-in**: before each phase starts, spin idle for `PGLEADIN` seconds (default 2), and **the Agent must not run during the lead-in** — otherwise the Agent only pre-warms CPU/pages for that phase, causing bias.
- **null test** (`M=0`): with no Agent, A and B should be equal, used to verify the harness itself is unbiased.
- **control case** (C): same load but no branch, isolating the effect of "one more concurrent client".

**Measured (2026-10-30, 8 vCPU, `max_connections=200`)**:

| Run | Conclusion |
|---|---|
| null test (M=0, R=4) | A=9753.20 B=9748.61 **-0.05%** → harness unbiased |
| canonical A/B (100/30/1, R=4) | A=10214.25 B=10957.14 **+7.27%**; Agent p99 249.91 ms |
| 3-case (R=3) | A=9699.51 B=10328.93 **+6.49%** C=9439.30 **-2.68%**; B-C=**+9.42%**; Agent p99 branch 222 / control 77 ms |

**Interpretation**: the Δs are all **positive** (MAIN is faster when the Agent is present), so this cannot be a cost the overlay read path imposes on MAIN; the 3-case shows B(branch) > C(control), indicating the gap tracks "**where the extra client writes**" (branch → the isolated `pg_branch_delta`; control → MAIN tables → autovacuum/WAL contention), not the Agent load itself. **These numbers are only archived as a baseline, and are not used as an acceptance basis.**

### run_agent_scale.sh (multi-Agent concurrency scalability probe)

**Purpose**: quantify "under a fixed MAIN foreground load, the shape and knee of MAIN degradation as the number M of concurrent **own-branch** Agents grows", i.e. the concurrency scalability of multiple Agents. **Numbers only, no PASS/FAIL** (the G05 threshold is still OPEN).

Difference from `run_mixed_impact.sh`: the latter puts **all** Agent clients into the **same** branch; this script puts each Agent client into its **own** branch (`mixed_agent_own.sql`), so it measures **cross-branch** interference (registry rows / per-branch advisory lock / GUC mode cache / state-change NOTIFY) rather than intra-branch delta write contention.

**Key methodology**: the `A` baseline + `B<M>` (each M in M_LIST) are **all placed into the same run** for Latin-square phase rotation, then the median is taken. Splitting the M sweep into "run each M separately" would misattribute run-to-run drift to the M axis — especially fatal on shared/small machines, where it invents a knee that does not exist. `PGSCALE_CONTROL=1` appends `C<M>` (same load but no branch) to separate the "overlay path" from "just a few more concurrent clients". Each Agent phase creates a fresh batch of empty branches with a **phase-unique prefix**, ensuring every phase starts from an empty delta and is comparable.

```bash
export PGPORT=5432 PGUSER=ubuntu PGDATABASE=ob_bench
psql -f test/bench/mixed_impact_fixture.sql          # run only once (reuse the G05 fixture)
./test/bench/run_agent_scale.sh 16 15 "1 2 4 8"               # A + B1/B2/B4/B8
PGSCALE_CONTROL=1 ./test/bench/run_agent_scale.sh 16 15 "1 2 4 8"   # append C1..C8
```

**Measured (2026-11-01, 8 vCPU / 9.7 GB VM, `N=16, T=15, R=4`, `max_connections=100`)**:

| case | MAIN tps | Δtps% | p99 ms | Agent p99 ms | CPU% | Agent tps |
|---|---|---|---|---|---|---|
| A (M=0) | 7647.48 | — | 3.87 | — | 49.50 | — |
| B1 | 7459.38 | −2.46 | 4.44 | 51.21 | 60.05 | 33.70 |
| B2 | 6277.85 | −17.91 | 5.52 | 59.14 | 71.90 | 62.24 |
| B4 | 4973.03 | −34.97 | 8.54 | 81.67 | 84.70 | 101.53 |
| B8 | 3647.48 | −52.30 | 12.28 | 122.14 | 93.00 | 139.02 |
| C1 | 7221.51 | −5.57 | 4.53 | 13.14 | 58.40 | 140.74 |
| C2 | 6864.92 | −10.23 | 5.44 | 15.07 | 67.10 | 266.35 |
| C4 | 5626.75 | −26.42 | 6.78 | 20.37 | 76.45 | 421.99 |
| C8 | 4641.62 | −39.31 | 9.23 | 35.03 | 84.15 | 554.67 |

**Interpretation (honest boundaries)**:

- **On this 8 vCPU VM, no "branch-specific concurrency ceiling" is measurable**: CPU% rises monotonically with M up to 93% (B8) / 84% (C8); the box is saturated by CPU first, so any branch-specific serialization point is masked by CPU saturation before it becomes visible. The absolute numbers have **no capacity meaning**; they only reflect this machine's specs (nproc/mem are recorded in the logs).
- **At the same M, B degrades more heavily than C** (M=8: −52.3% vs −39.3%): the per-transaction CPU cost of a branch Agent is about **3–4×** that of control (Agent p99 51 vs 13 ms; Agent tps 33.7 vs 140.7 @M=1), which is the main reason B drags MAIN down more. At M=1, B is actually **better** than C (−2.5% vs −5.6%), because the control Agent, being 4× faster, pours more write load into the MAIN tables — consistent with the existing G05/S28 conclusion about "where writes land".
- **Agent throughput scalability with M is almost identical between B and C** (M=1→8: B 4.1× / C 3.9×, both ≈50% efficiency), all explained by CPU saturation; **within the tested range there is no evidence of a branch-specific bottleneck (registry / advisory lock serialization)**.

---

## Typical run steps

Assuming PostgreSQL is compiled and installed at `~/pg17`, the extension has been `make install`ed, and the PG instance is running, port 5432, test database `obtest`:

```bash
# environment variables
export PGPORT=5432
export PGUSER=ubuntu
export PGDATABASE=obtest
export PG_REGRESS_DB=$PGDATABASE   # match the variables in the shell comments at the top of apply_contention.sql

cd <repo root>

# ===== (1) one-time fixture initialization =====
psql -f test/bench/apply_contention_fixture.sql
# expect no ERROR in the output; the last 8 branches exist
psql -c "SELECT branch_name,state FROM overlay_branch.pg_branch WHERE branch_name LIKE 'bs_pgbench_%' ORDER BY 1;"
# should see 8 rows (bs_pgbench_1..8, state='active')

# ===== (2) 60-second 32-client stress test =====
# -n: no vacuum of pgbench default tables (we did not create pgbench_history/branches/tellers, so it is not needed)
# -M prepared: use prepared statements to reduce parse overhead and really hit lock contention
# -j 2: 2 pgbench worker threads
pgbench -n -c 32 -j 2 -T 60 -M prepared -f test/bench/apply_contention.sql
```

Output example (actual numbers depend on hardware):
```
transaction type: test/bench/apply_contention.sql
scaling factor: 1
query mode: prepared
number of clients: 32
number of threads: 2
duration: 60 s
number of transactions actually processed: 1234567
latency average = 1.234 ms
tps = 25943.212345 (including connections establishing)
tps = 25978.112345 (excluding connections establishing)
```

---

## Distribution of the 4 roles (fixed per connection = `CLIENT_ID % 10`)

| Role | Share | What it does per transaction | Target coverage |
|---|---|---|---|
| **WR writer (0..5)** | 60% | BEGIN; SELECT count WHERE aid=randid; UPSERT (INSERT ON CONFLICT DO NOTHING) id=10000+randid; UPDATE abalance+1; COMMIT | high-frequency write_redirect DELTA INSERT; delta visibility in snapshot/live mode; FR6 ReScan fresh materialization |
| **Branch switcher (6..7)** | 20% | alternating `use_branch(bs_pgbench_{1..8}, live | snapshot)`; cache-set on entry; assign-hook RESTORE path | GUC assign hook cache-hit path; ob_mode_cache set/remove; live↔snapshot switching does not trigger an FR2 deadlock (zero-lock guarantee) |
| **Applyer (8)** | 10% | `apply_branch(random active branch)`; **normally catches a lot of 55000** (another session already applied it; the FR2 CAS loser fast-fails with ERROR and does not deadlock) | FR3 CAS winner/loser; FR2 Exclusive advisory held by only 1; MAIN merge write path; apply→mode_cache_remove; ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE(55000) triggered correctly at high frequency (not a hang/deadlock) |
| **Discarder (9)** | 10% | `discard_branch(random applied branch)`; here too 55000 is a normal branch | discarding CAS; bulk DELETE on the delta table; symmetric snapshot/mode cache remove |

---

## Post-stress verification SQL (most important, must run)

```sql
-- ===== FR3 state machine has no leak (any abnormal state counts as FAIL) =====
SELECT state, count(*) FROM overlay_branch.pg_branch
 WHERE branch_name LIKE 'bs_pgbench_%' GROUP BY 1 ORDER BY 1;
-- ideal PASS distribution: mostly active + applied; a few discarded;
-- FAIL condition: rows with applying / discarding appear (= state machine stuck in an intermediate state = BUG)

-- ===== absolutely no applying / discarding residue =====
SELECT count(*) AS stuck_applying FROM overlay_branch.pg_branch WHERE state = 'applying';
SELECT count(*) AS stuck_discarding FROM overlay_branch.pg_branch WHERE state = 'discarding';
-- ✅ PASS: both rows = 0

-- ===== MAIN row count baseline exactly 10000 (= the seed at initialization) =====
-- NOTE: the aid=10000+randid range of the UPSERT in the WR writer transaction is [10001,19999]
-- if write_redirect works correctly, these rows are written only to the WR delta, not into MAIN;
-- if there is any write-to-MAIN leak (a variant of the CmdType split BUG of FAIL#1),
-- MAIN count would be >10000. This is our most sensitive DRIFT CHECK.
SELECT count(*) AS main_count, count(*) = 10000 AS main_baseline_ok
  FROM public.pgbench_accounts WHERE aid BETWEEN 1 AND 10000;
SELECT count(*) AS main_leak_rows  -- ✅ ideal = 0; >0 = write-to-MAIN leak = CRITICAL BUG
  FROM public.pgbench_accounts WHERE aid > 10000;

-- ===== V4: FR4 NOTIFY fallback statistics (NOTIFY should actually be consumed with long-lived connections) =====
-- you can check whether postgresql.conf has log_min_messages=debug1 enabled to count;
-- or directly query pg_stat_activity for whether the number of connections with a non-empty overlay_branch GUC is reasonable
SELECT count(*) AS sessions_in_branch FROM pg_stat_activity
 WHERE backend_type = 'client backend'
   AND (current_setting('overlay_branch.current_branch', true) IS NOT NULL
    AND current_setting('overlay_branch.current_branch', true) <> '');
```

---

## What output = "stress PASS"

Meeting all 4 of the following counts as passing (**none of them is a tps threshold**; R7 correctness first):

1. ✅ `pgbench` itself has no `connection failed` / `FATAL` / `SIGABRT`-level errors (a normal ERROR 55000 is expected; pgbench does not treat ERROR as FAIL, it only counts it toward latency)
2. ✅ **stuck = 0** (nothing stuck in the applying/discarding intermediate state)
3. ✅ **main_leak_rows = 0** (no WR-writer leaks of rows >10000 into the MAIN table)
4. ✅ **no psql backend process crash / coredump** (`dmesg` or `journalctl -u postgresql` to look for segfault)

> tps level, ERROR 55000 count, and average latency are all **performance metrics**, not used for PASS/FAIL judgment; record them in a separate performance table when needed (reconsider later).

---

## Known flakiness & handling

| Symptom | Cause | Handling |
|---|---|---|
| 1st run ERROR `branch bs_pgbench_X does not exist` | fixture.sql initialization never ran successfully, or another test script discarded the branch | re-run fixture.sql once |
| lots of 55000 ERROR from apply/discard | **normal** (FR2 CAS loser fast-fails; it is actually the correctness demonstrated by L1 ob_apply_mutex). tps will be a bit lower but it does not affect the correctness judgment | ignore. If you want to see fewer 55000, use `-c 8` for fewer clients, or lower the apply/discard share |
| MAIN leak_rows comes out >0 | **CRITICAL BUG**, highest priority. Stop the stress run immediately, take the minimal reproduction and run L1 isolation tests to see whether it can be reduced to a permutation-level spec | open a new debug session, record the minimal reproduction and conclusion item by item |
| stuck_applying > 0 for a long time >30 seconds | the CAS winner is stuck during the MAIN merge (possibly a MAIN table-level lock or DDL conflict) | check pg_locks; if it is truly stuck, kill -9 the stuck backend and count it as FAIL |

---

## Relationship diagram with the L1 / L3 gates

```
[L1 pg_isolation 15 specs] — "all concurrent critical topologies precisely scheduled"
         │  (FR2/FR3/FR4/FR5/FR6 semantic verify one by one)
         ▼
[L3 pg_regress 21 suites] — "functionality outside L1 + FR4 LISTEN single-connection path"
         │  (this extension's default make installcheck gate = L1 + L3 totaling 36/36 zero-diff 2×)
         ▼
[L2 pgbench (this directory, manual)] — "correctness backstop sanity check under high-concurrency real load"
         │  (NOT a gate. For performance exploration / triggering edge races not covered by L1/L3)
         ▼
     discover BUG  →  reduce to minimal reproduction  →  write a new L1 spec  or  new L3 SQL test  →  back to the gate
```

**Rule**: any correctness issue found in L2 **must be sedimented into an L1 or L3 test** (once reduced to a stably reproducible minimal case, it is frozen into the pg_regress / isolation gate), and must not stay at the L2 manual-inspection level indefinitely.
