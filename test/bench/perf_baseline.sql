-- perf_baseline.sql -- reproducible performance baseline for overlay_branch.
--
-- Purpose : produce comparable wall-clock numbers for the four cost centers
--           documented in doc/internals.md §5 (branch merge read, PK/non-PK
--           read, delta size, apply).  NOT a correctness gate and NOT part of
--           `make check`.
-- Coverage: MAIN seqscan vs BranchScan overlay read; P0 PK-equality fast path;
--           P2 non-PK pushdown; delta payload amplification; apply timing.
-- Deps    : shared_preload_libraries='overlay_branch'; run against a live
--           server (plain psql).  It creates/drops its OWN table and branches.
--
-- Usage (prefer the driver, which also tees a log):
--     psql -p 5432 -d postgres -v n=200000 -v m=20000 -f perf_baseline.sql
--
-- n = MAIN baseline rows, m = distinct rows touched by the branch (delta rows).
--
-- Reproducibility notes:
--   * parallel gather is disabled (a) so MAIN seqscan and BranchScan are
--     compared on the same footing, and (b) because overlay_branch currently
--     does NOT cover parallel paths: with parallelism on, the planner emits
--     "Parallel Seq Scan" (no BranchScan, delta ignored) and the query then
--     fails in the worker.  See doc/internals.md §5.2 MF-2 / MF-3.
--   * the delta is built with SET-BASED DML, so m is controlled and the build
--     cost does not pollute the measurements.
--   * each measurement is a fresh statement, therefore every BranchScan read
--     is "cold" (BeginCustomScan -> full overlay materialisation).
--   * backend memory is sampled around the heavy scans: BranchScan currently
--     leaks into TopMemoryContext, so wall-clock numbers drift upward unless
--     this is watched (doc/internals.md §5.2 MF-1 / §5.6).

\set ON_ERROR_STOP on

\if :{?n}
\else
  \set n 200000
\endif
\if :{?m}
\else
  \set m 20000
\endif

SET client_min_messages = warning;
SET statement_timeout = 0;
SET max_parallel_workers_per_gather = 0;

CREATE EXTENSION IF NOT EXISTS overlay_branch;
RESET ALL;
SET client_min_messages = warning;
SET statement_timeout = 0;
SET max_parallel_workers_per_gather = 0;

-- ---------------------------------------------------------------- cleanup
-- Best-effort removal of any previous baseline branch so repeated runs are
-- independent.  Falls back to a direct catalog purge if discard is refused.
DO $$
DECLARE
    r record;
BEGIN
    FOR r IN
        SELECT branch_name
          FROM overlay_branch.pg_branch
         WHERE branch_name LIKE 'ob\_perf\_%'
    LOOP
        BEGIN
            PERFORM overlay_branch.discard_branch(r.branch_name);
        EXCEPTION WHEN OTHERS THEN
            NULL;
        END;
    END LOOP;
END $$;

DELETE FROM overlay_branch.pg_branch_delta
 WHERE branch_id IN (SELECT branch_id FROM overlay_branch.pg_branch
                      WHERE branch_name LIKE 'ob\_perf\_%');
DELETE FROM overlay_branch.pg_branch_registry
 WHERE branch_id IN (SELECT branch_id FROM overlay_branch.pg_branch
                      WHERE branch_name LIKE 'ob\_perf\_%');
DELETE FROM overlay_branch.pg_branch
 WHERE branch_name LIKE 'ob\_perf\_%';

DROP TABLE IF EXISTS public.ob_perf_base;

-- ---------------------------------------------------------------- fixture
CREATE TABLE public.ob_perf_base
(
    id      int  NOT NULL PRIMARY KEY,
    grp     int  NOT NULL,
    payload text NOT NULL
);

INSERT INTO public.ob_perf_base (id, grp, payload)
SELECT g, g % 100, repeat('x', 80)
  FROM generate_series(1, :n) AS g;

ANALYZE public.ob_perf_base;

SELECT format('ob_perf_%s_%s', :n, :m) AS branch \gset

\echo ''
\echo '============================== environment =============================='
SELECT version() AS server_version;
SELECT current_setting('max_parallel_workers_per_gather') AS parallel_gather;
SELECT :n AS param_n_main_rows, :m AS param_m_delta_rows, :'branch' AS branch;

-- ================================================================= M1
\echo ''
\echo '=========== M1: MAIN seqscan count(*)  (plain heap baseline) ==========='
\timing on
SELECT count(*) FROM public.ob_perf_base;

-- ============================================================ create + use
SELECT overlay_branch.create_branch(:'branch');
SELECT overlay_branch.use_branch(:'branch');

-- ================================================================= M2
\echo ''
\echo '========== M2: branch count(*) with M=0  (BranchScan, no delta) ========'
SELECT count(*) FROM public.ob_perf_base;
\echo '--- backend memory after M2 (leak watch, see MF-1) ---'
SELECT pg_size_pretty(sum(total_bytes)) AS backend_mem FROM pg_backend_memory_contexts;

-- ------------------------------------------------------------ build delta
-- M10 (write path): the three redirected DML kinds are timed separately so
-- the per-row redirect cost (delta upsert + ctid fetch + version) is
-- measurable.  See doc/internals.md §5.4 (WR-1).
\echo ''
\echo '===== M10a: write path UPDATE  (m/2 rows, redirected to delta) ====='
\timing on
UPDATE public.ob_perf_base
   SET payload = payload || '_u'
 WHERE id <= (:m) / 2;
\echo '===== M10b: write path DELETE  (m/4 rows, redirected to delta) ====='
DELETE FROM public.ob_perf_base
 WHERE id > (:n) - (:m) / 4;
\echo '===== M10c: write path INSERT  (m/4 rows, redirected to delta) ====='
INSERT INTO public.ob_perf_base (id, grp, payload)
SELECT (:n) + g, 0, repeat('y', 80)
  FROM generate_series(1, (:m) / 4) AS g;
\timing off

-- delta shape sanity (rows per op must sum to m)
\echo ''
\echo '---------------- delta shape (op -> rows) ----------------'
SELECT op, count(*) AS rows
  FROM overlay_branch.pg_branch_delta
 WHERE branch_id = (SELECT branch_id FROM overlay_branch.pg_branch
                     WHERE branch_name = :'branch')
 GROUP BY op
 ORDER BY op;

-- ================================================================= M3
\echo ''
\echo '=========== M3: branch count(*) full overlay  (N merged with M) ========'
\timing on
SELECT count(*) FROM public.ob_perf_base;
\echo '--- M3 repeat (verifies run-to-run stability) ---'
SELECT count(*) FROM public.ob_perf_base;
\echo '--- backend memory after M3 (leak watch, see MF-1) ---'
SELECT pg_size_pretty(sum(total_bytes)) AS backend_mem FROM pg_backend_memory_contexts;

-- ================================================================= M4
\echo ''
\echo '=========== M4: branch PK-equality read  (P0 fast path) ================'
SELECT * FROM public.ob_perf_base WHERE id = 42;

-- ================================================================= M5
\echo ''
\echo '=========== M5: branch non-PK read  (P2 pushdown, grp=7) ==============='
SELECT count(*) FROM public.ob_perf_base WHERE grp = 7;

-- ================================================================= M6
\echo ''
\echo '=========== M6: EXPLAIN ANALYZE branch full scan  (node check) ========='
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT count(*) FROM public.ob_perf_base;

-- ================================================================= M7
\echo ''
\echo '=========== M7: delta size report  (logical payload, pre-apply) ========='
\timing off
WITH d AS
(
    SELECT relid,
           count(*) AS delta_rows,
           coalesce(sum(octet_length(key)
                        + octet_length(coalesce(old_version, ''))
                        + coalesce(octet_length(tuple_data), 0)), 0)::bigint
               AS delta_payload_bytes
      FROM overlay_branch.pg_branch_delta
     WHERE branch_id = (SELECT branch_id FROM overlay_branch.pg_branch
                         WHERE branch_name = :'branch')
     GROUP BY relid
)
SELECT relid::regclass                                        AS rel,
       delta_rows,
       pg_size_pretty(delta_payload_bytes)                    AS delta_logical,
       pg_size_pretty(pg_total_relation_size(relid))          AS main_total,
       round(delta_payload_bytes::numeric
             / nullif(pg_total_relation_size(relid), 0), 4)   AS amplification
  FROM d;

SELECT pg_size_pretty(pg_total_relation_size('overlay_branch.pg_branch_delta'))
       AS delta_relation_total;

-- ================================================================= M8
\echo ''
\echo '=========== M8: apply_branch timing  (3-pass D->U->I) =================='
\timing on
SELECT overlay_branch.apply_branch(:'branch');

-- ================================================================= M9
\echo ''
\echo '=========== M9: MAIN mode after apply (delta drained) =================='
SELECT count(*) FROM public.ob_perf_base;
\timing off

\echo ''
\echo '=============================== done ==================================='
\echo 'baseline finished; artifacts kept for inspection (public.ob_perf_base).'
