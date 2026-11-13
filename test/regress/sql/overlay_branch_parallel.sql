--
-- MF-2 / MF-3 regression: an active branch must never plan a parallel scan
-- over the bare MAIN heap.
--
-- Background
--   The overlay merge lives in a CustomScan injected from
--   set_rel_pathlist_hook.  Core PG builds partial (parallel-aware) paths
--   *before* the hook runs and converts them into Gather paths *after* it
--   returns, so unless the hook also prunes rel->partial_pathlist the planner
--   can pick
--       Gather -> Parallel Seq Scan on <MAIN>
--   which silently returns pre-branch data (MF-2).  The workers it launched
--   used to abort while restoring GUC state (MF-3).
--
-- Fixture
--   min_parallel_table_scan_size=0 plus zero parallel costs make the parallel
--   path the cheapest option even on this fixture, so the control query below
--   really does show a Gather plan while no branch is active.  Index scans are
--   disabled only to keep that control plan stable across platforms.
--
--   The plan-shape assertion is done through SPI inside a DO block because the
--   A7 gate rejects DDL (including CREATE TEMP TABLE) while a branch is active.
--
-- Tags: MF-2, MF-3, parallel, planner_hook, partial_pathlist, EXPLAIN

CREATE EXTENSION IF NOT EXISTS overlay_branch;

SET max_parallel_workers_per_gather = 4;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;

CREATE TABLE ob_par (id int PRIMARY KEY, v text);
INSERT INTO ob_par SELECT g, 'm' || g FROM generate_series(1, 20000) g;
ANALYZE ob_par;

-- Control (no active branch): the planner must be able to produce a parallel
-- plan here, otherwise the assertions below would be vacuous.
EXPLAIN (COSTS OFF) SELECT count(*) FROM ob_par;

SELECT overlay_branch.create_branch('ob_par_b1') > 0 AS ob_par_b1_created;
SELECT overlay_branch.use_branch('ob_par_b1');

UPDATE ob_par SET v = 'b' || id WHERE id <= 10;                              -- 10 overlaid rows
DELETE FROM ob_par WHERE id > 10 AND id <= 15;                               -- 5 deleted
INSERT INTO ob_par SELECT g, 'b' || g FROM generate_series(20001, 20020) g;  -- 20 pure delta

-- Visual: inside a branch the merge must be the only plan (no Gather).
EXPLAIN (COSTS OFF) SELECT count(*) FROM ob_par;

DO $$
DECLARE
  r          record;
  has_scan   boolean := false;
  has_gather boolean := false;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT count(*) FROM ob_par'
  LOOP
    IF r."QUERY PLAN" ~* 'overlay_branch_branchscan' THEN has_scan := true; END IF;
    IF r."QUERY PLAN" ~* 'Gather' THEN has_gather := true; END IF;
  END LOOP;

  IF has_scan AND NOT has_gather THEN
    RAISE NOTICE 'MF2_NO_PARALLEL_IN_BRANCH_PASS';
  ELSE
    RAISE NOTICE 'MF2_NO_PARALLEL_IN_BRANCH_FAIL: branchscan=% gather=%',
                 has_scan, has_gather;
  END IF;
END $$;

-- The delta really is visible: 20000 - 5 + 20 = 20015.
SELECT CASE WHEN count(*) = 20015 THEN 'MF2_OVERLAY_COUNT_PASS'
            ELSE 'MF2_OVERLAY_COUNT_FAIL: got=' || count(*) END AS res
  FROM ob_par;
SELECT CASE WHEN count(*) = 1 THEN 'MF2_PURE_DELTA_ROW_PASS'
            ELSE 'MF2_PURE_DELTA_ROW_FAIL' END AS res
  FROM ob_par WHERE id = 20015;
SELECT CASE WHEN count(*) = 10 THEN 'MF2_OVERLAID_ROW_PASS'
            ELSE 'MF2_OVERLAID_ROW_FAIL' END AS res
  FROM ob_par WHERE id <= 10 AND v = 'b' || id;
SELECT CASE WHEN count(*) = 0 THEN 'MF2_DELETED_ROW_PASS'
            ELSE 'MF2_DELETED_ROW_FAIL' END AS res
  FROM ob_par WHERE id = 13;

-- Cleanup
SELECT overlay_branch.use_branch(NULL);
SELECT overlay_branch.discard_branch('ob_par_b1');
DROP TABLE ob_par;
RESET ALL;
