--
-- D-3 REVIEW-260926 / A8: cost_branchscan() cost model + join-order stability.
-- Purpose:
--   Verify that our CustomScan no longer reports artificially zero cost
--   (previously min_cost * 1.0e-5, effectively 0.00 in EXPLAIN) and
--   instead reports a realistic non-zero cost derived from R11/R12
--   complexity O(M log M + N log M).
--
-- Assertion style (pg_regress-safe):
--   - EXPLAIN visual outputs (manually inspected by expected diff)
--   - SQL-level auto-assertions via `CASE WHEN ... THEN 'PASS' ELSE 'FAIL' END`
--
-- Tags: A8, D-3, cost_model, planner, join_order, PK_pred_fastpath, EXPLAIN

CREATE EXTENSION IF NOT EXISTS overlay_branch;

-- ============================================================
-- A8_FIXTURE: 200 MAIN rows; write 14 rows inside a branch
-- => N=200, M_est=0.05*200=10 clamped.
-- ============================================================
CREATE TABLE a8_planner (
    id    INTEGER PRIMARY KEY,
    grp   INTEGER NOT NULL,
    amt   NUMERIC(10,2) NOT NULL
);
INSERT INTO a8_planner(id, grp, amt)
  SELECT g, (g-1)/20 + 1, (g*1.37)::numeric(10,2)
  FROM generate_series(1, 200) g;
ANALYZE a8_planner;
SELECT count(*) AS a8_main_cnt FROM a8_planner;       -- expect 200

SELECT overlay_branch.create_branch('a8_cost_branch') > 0 AS a8_branch_created;
SELECT overlay_branch.use_branch('a8_cost_branch');

-- Overlay edits: 13 rows (6.5% × 200 matches our default M_est ratio)
DELETE FROM a8_planner WHERE id = 99;             -- 1 DEL (id=99 removed from MAIN 200)
INSERT INTO a8_planner VALUES (201, 1, 101.10);   -- 13 INS below = 201..213
INSERT INTO a8_planner VALUES (202, 1, 102.20);
INSERT INTO a8_planner VALUES (203, 2, 103.30);
INSERT INTO a8_planner VALUES (204, 2, 104.40);
INSERT INTO a8_planner VALUES (205, 3, 105.50);
INSERT INTO a8_planner VALUES (206, 3, 106.60);
INSERT INTO a8_planner VALUES (207, 4, 107.70);
INSERT INTO a8_planner VALUES (208, 4, 108.80);
INSERT INTO a8_planner VALUES (209, 5, 109.90);
INSERT INTO a8_planner VALUES (210, 5, 110.00);
INSERT INTO a8_planner VALUES (211, 6, 111.11);
INSERT INTO a8_planner VALUES (212, 6, 112.22);
INSERT INTO a8_planner VALUES (213, 7, 113.33);

-- Overlay row count ground truth (200 - 1 + 13 = 212)
SELECT CASE WHEN count(*) = 212 THEN 'A8_FIXTURE_PASS' ELSE 'A8_FIXTURE_FAIL: got=' || count(*) END AS res
  FROM a8_planner;

SET enable_hashjoin=off;
SET enable_mergejoin=off;
SET enable_nestloop=on;
SET client_min_messages = 'log';

-- ============================================================
-- A8.1 SEQSCAN COST != 0.00 AUTO-ASSERTION
-- ============================================================
-- (Subquery reformatting trick for pg_regress: build a temp table of
--  EXPLAIN lines, then assert on them.)
CREATE TEMP TABLE a8_seq_explain (line text);
INSERT INTO a8_seq_explain
  EXPLAIN (FORMAT text, COSTS on) SELECT * FROM a8_planner;
-- Full visual output
SELECT line AS a8_1_seqscan_explain FROM a8_seq_explain;

-- Assert: at least one line contains "overlay_branch_branchscan"
SELECT CASE
         WHEN EXISTS (SELECT 1 FROM a8_seq_explain WHERE line ~* 'overlay_branch_branchscan')
         THEN 'A8_1A_SCAN_NODE_PASS'
         ELSE 'A8_1A_SCAN_NODE_FAIL'
       END AS res;
-- Assert: first line cost NOT 0.00..0.00 pattern (not like "cost=0.00..0.00")
SELECT CASE
         WHEN EXISTS (SELECT 1 FROM a8_seq_explain
                       WHERE line ~ 'cost='
                         AND line !~ 'cost=0\.00\.\.0\.00')
           AND NOT EXISTS (SELECT 1 FROM a8_seq_explain
                            WHERE line ~ 'cost=0\.00\.\.0\.00' AND line ~* 'overlay_branch_branchscan')
         THEN 'A8_1B_COST_NONZERO_PASS'
         ELSE 'A8_1B_COST_NONZERO_FAIL: see below visual'
       END AS res;
SELECT line AS a8_1_fail_ctx FROM a8_seq_explain WHERE line ~ 'cost=';

-- ============================================================
-- A8.2 PK-PRED COST != 0.00 + FAST PATH << SEQ COST
-- ============================================================
CREATE TEMP TABLE a8_pk_explain (line text);
INSERT INTO a8_pk_explain
  EXPLAIN (FORMAT text, COSTS on) SELECT * FROM a8_planner WHERE id = 42;
SELECT line AS a8_2_pk_pred_explain FROM a8_pk_explain;

SELECT CASE
         WHEN EXISTS (SELECT 1 FROM a8_pk_explain WHERE line ~* 'overlay_branch_branchscan')
         THEN 'A8_2A_PKSCAN_NODE_PASS' ELSE 'A8_2A_PKSCAN_NODE_FAIL' END AS res;
SELECT CASE
         WHEN EXISTS (SELECT 1 FROM a8_pk_explain
                       WHERE line ~ 'cost='
                         AND line !~ 'cost=0\.00\.\.0\.00')
           AND NOT EXISTS (SELECT 1 FROM a8_pk_explain
                            WHERE line ~ 'cost=0\.00\.\.0\.00' AND line ~* 'overlay_branch_branchscan')
         THEN 'A8_2B_PK_COST_NONZERO_PASS'
         ELSE 'A8_2B_PK_COST_NONZERO_FAIL'
       END AS res;

-- Extract the total-cost number from both explain first lines
-- (cost=startup..total rows=X ...) then assert PK total < seq total.
CREATE TEMP TABLE a8_cost_numeric(label text, total_cost float);
INSERT INTO a8_cost_numeric
  SELECT 'seq', substring(line, 'cost=[0-9.]+[.][.]([0-9.]+)')::float
    FROM a8_seq_explain WHERE line ~ 'cost=' LIMIT 1;
INSERT INTO a8_cost_numeric
  SELECT 'pk',  substring(line, 'cost=[0-9.]+[.][.]([0-9.]+)')::float
    FROM a8_pk_explain  WHERE line ~ 'cost=' LIMIT 1;
SELECT label, total_cost AS a8_total_cost FROM a8_cost_numeric ORDER BY label;

SELECT CASE
         WHEN (SELECT total_cost FROM a8_cost_numeric WHERE label='pk')
            < (SELECT total_cost FROM a8_cost_numeric WHERE label='seq')
         THEN 'A8_2C_PK_LT_SEQ_COST_PASS'
         ELSE 'A8_2C_PK_LT_SEQ_COST_FAIL: pk=' ||
              (SELECT total_cost FROM a8_cost_numeric WHERE label='pk') ||
              ' seq=' || (SELECT total_cost FROM a8_cost_numeric WHERE label='seq')
       END AS res;

-- ============================================================
-- A8.3 SELF-JOIN STABLE: 2 overlay_branch_branchscan instances
--       (branch active => planner has no alternative; pathlist prune
--        guarantees both sides use BranchScan.)
-- ============================================================
CREATE TEMP TABLE a8_sj_explain (line text);
INSERT INTO a8_sj_explain
  EXPLAIN (FORMAT text, COSTS on)
    SELECT count(*) FROM a8_planner a JOIN a8_planner b ON (a.id = b.id + 1);
SELECT line AS a8_3_selfjoin_explain FROM a8_sj_explain;

SELECT CASE
         WHEN (SELECT count(*) FROM a8_sj_explain WHERE line ~* 'overlay_branch_branchscan') = 2
         THEN 'A8_3A_TWO_CUSTOMSCAN_PASS'
         ELSE 'A8_3A_TWO_CUSTOMSCAN_FAIL: cnt=' ||
              (SELECT count(*) FROM a8_sj_explain WHERE line ~* 'overlay_branch_branchscan')
       END AS res;

-- Correctness (same assertion as A2_SJ1 12-pair variant; verify we do NOT
-- regress the self-join semantic under A8 cost model.)
SELECT CASE WHEN count(*) = 210 THEN 'A8_3B_SJ_CORRECT_PASS' ELSE 'A8_3B_SJ_CORRECT_FAIL:'||count(*) END AS res
  FROM a8_planner a JOIN a8_planner b ON (a.id = b.id + 1);

-- ============================================================
-- A8_CLEANUP
-- ============================================================
SELECT overlay_branch.use_branch(NULL);
SELECT overlay_branch.discard_branch('a8_cost_branch');
DROP TABLE a8_planner;
RESET client_min_messages;
