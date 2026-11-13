--
-- RD-1 / RD-2 (MF-5) regression: non-PK qual pushdown + Pass2 orphan emission.
--
-- Background
--   The BranchScan merge runs in two passes:
--     Pass1 : SPI-scan MAIN (optionally filtered by the pushed-down WHERE
--             fragment, custom_private[5] `general_where_sql`) and overlay
--             each MAIN row with a bsearch in the delta list by key.
--     Pass2 : emit every delta row that Pass1 did NOT match, i.e. the
--             "orphans" — pure-delta INSERTs and UPDATEs whose MAIN row no
--             longer satisfies the pushed WHERE fragment (moved into the
--             queried set), after re-checking the qualifier on the
--             reconstructed overlay tuple.
--
--   RD-1 and RD-2 were two O(M^2) "latest-wins" folds that were only ever
--   reachable when two delta rows could share a key.  `pg_branch_delta` is
--   keyed PRIMARY KEY (branch_id, relid, key) and the scan loads a list with
--   an exact (branch_id, relid) filter, so keys are structurally unique and
--   both folds degenerated to identity while costing O(M^2) per scan.  They
--   were deleted.  A *more* selective pushdown shrinks Pass1's match set and
--   pushes more overlays into the Pass2 orphan path, which is what made the
--   old fold cost blow up (the observed MF-5 symptom: a selective non-PK
--   predicate was SLOWER than no predicate at all).
--
-- What this test pins
--   1. Correctness of the non-PK pushdown + Pass2 orphan path: results must
--      not lose or duplicate any row when the predicate is pushed down and
--      Pass1's match set shrinks (up to the extreme where Pass1 matches
--      nothing and every delta row is an orphan).
--   2. The structural premise that made the RD-1/RD-2 folds removable — delta
--      keys are unique per (branch_id, relid).  If a future schema change
--      drops that PK the removals become unsafe and this assertion fails.
--   3. A volatile qualifier is never double-evaluated: PD.6 pins that it is
--      not folded into the pushed-down SPI fragment (PK-clause path), PD.7
--      that it does not leak into the Pass-1 MAIN prefilter (full-table
--      path).  Either way it is evaluated exactly once per output row.
--
-- Fixture
--   ob_pd(id PK, grp, payload); 200 MAIN rows, grp = (id-1)/20 (0..9),
--   payload = 'm'||id.  The branch then edits grp=7 (ids 141..160) with all
--   four op shapes:
--     - DELETE 141..145      (MAIN match, dropped from grp=7)
--     - UPDATE 146..150      (MAIN match, in-place overlay)
--     - UPDATE 156..160 -> grp=8 (MAIN match for grp=8, dropped from grp=7)
--     - UPDATE 1..5     -> grp=7 (orphan, moved INTO the queried set)
--     - INSERT 201..205      (orphan, pure delta)
--   Expected grp=7 set = {1..5, 146..155, 201..205} = 20 rows.
--
--   Plan-shape probing goes through a DO block with SPI EXECUTE because the
--   A7 gate rejects DDL (including CREATE TEMP TABLE) while a branch is
--   active; all DDL here runs before use_branch / after discard_branch.
--
-- Tags: RD-1, RD-2, MF-5, pushdown, pass2, orphan, branch_scan, volatile

CREATE EXTENSION IF NOT EXISTS overlay_branch;

CREATE TABLE ob_pd (
    id      int PRIMARY KEY,
    grp     int NOT NULL,
    payload text NOT NULL
);
INSERT INTO ob_pd SELECT g, (g - 1) / 20, 'm' || g FROM generate_series(1, 200) g;
ANALYZE ob_pd;
SELECT count(*) AS pd_main_cnt FROM ob_pd;                       -- expect 200

-- Volatile predicate for PD.6: declared VOLATILE so the planner must not fold
-- it, and backed by a sequence so the number of evaluations is observable.
CREATE SEQUENCE ob_pd_seq;
CREATE FUNCTION ob_pd_vol_true(x int) RETURNS boolean LANGUAGE sql VOLATILE
  AS $$ SELECT nextval('ob_pd_seq') > 0 $$;

SELECT overlay_branch.create_branch('ob_pd_b1') > 0 AS ob_pd_b1_created;
SELECT overlay_branch.use_branch('ob_pd_b1');

-- ---- Overlay edits (all four op shapes against grp=7) ----
DELETE FROM ob_pd WHERE id BETWEEN 141 AND 145;                  -- matched, dropped
UPDATE ob_pd SET payload = 'upd' WHERE id BETWEEN 146 AND 150;   -- matched, overlay
UPDATE ob_pd SET grp = 8, payload = 'out' WHERE id BETWEEN 156 AND 160;  -- moved OUT
UPDATE ob_pd SET grp = 7, payload = 'in'  WHERE id BETWEEN 1 AND 5;      -- moved IN (orphan)
INSERT INTO ob_pd SELECT g, 7, 'ins' FROM generate_series(201, 205) g;   -- pure delta (orphan)

-- Delta store holds exactly the 25 edits above.
SELECT CASE WHEN count(*) = 25 THEN 'PD_DELTA_ROWCOUNT_PASS'
            ELSE 'PD_DELTA_ROWCOUNT_FAIL: got=' || count(*) END AS res
  FROM overlay_branch.pg_branch_delta d
  WHERE d.branch_id = (SELECT branch_id FROM overlay_branch.pg_branch
                       WHERE branch_name = 'ob_pd_b1');

-- ------------------------------------------------------------
-- PD.1 Non-PK pushdown, moderate selectivity (Pass1 sees ~20 MAIN rows)
-- ------------------------------------------------------------
SELECT CASE WHEN count(*) = 20 THEN 'PD_GRP7_COUNT_PASS'
            ELSE 'PD_GRP7_COUNT_FAIL: got=' || count(*) END AS res
  FROM ob_pd WHERE grp = 7;

-- No row may be emitted twice by the Pass1/Pass2 split.
SELECT CASE WHEN count(*) = count(DISTINCT id) THEN 'PD_GRP7_NO_DUP_PASS'
            ELSE 'PD_GRP7_NO_DUP_FAIL' END AS res
  FROM ob_pd WHERE grp = 7;

-- Definitive row-level assertion: exact ordered listing of the overlay set.
SELECT id, grp, payload FROM ob_pd WHERE grp = 7 ORDER BY id;

-- Moved-IN orphans must be present with their post-image;
-- moved-OUT / deleted rows must be absent.
SELECT CASE WHEN count(*) = 5 THEN 'PD_MOVED_IN_PASS'
            ELSE 'PD_MOVED_IN_FAIL' END AS res
  FROM ob_pd WHERE id <= 5 AND grp = 7 AND payload = 'in';
SELECT CASE WHEN count(*) = 0 THEN 'PD_MOVED_OUT_ABSENT_PASS'
            ELSE 'PD_MOVED_OUT_ABSENT_FAIL' END AS res
  FROM ob_pd WHERE id BETWEEN 156 AND 160 AND grp = 7;
SELECT CASE WHEN count(*) = 0 THEN 'PD_DELETED_ABSENT_PASS'
            ELSE 'PD_DELETED_ABSENT_FAIL' END AS res
  FROM ob_pd WHERE id BETWEEN 141 AND 145;

-- ------------------------------------------------------------
-- PD.2 Non-PK pushdown, extreme selectivity (Pass1 matches NOTHING;
--      every delta row becomes a Pass2 orphan — the MF-5 worst case)
-- ------------------------------------------------------------
SELECT CASE WHEN count(*) = 5 THEN 'PD_PAYLOAD_UPD_COUNT_PASS'
            ELSE 'PD_PAYLOAD_UPD_COUNT_FAIL: got=' || count(*) END AS res
  FROM ob_pd WHERE payload = 'upd';
SELECT id, grp, payload FROM ob_pd WHERE payload = 'upd' ORDER BY id;

SELECT CASE WHEN count(*) = 0 THEN 'PD_NO_MATCH_PASS'
            ELSE 'PD_NO_MATCH_FAIL' END AS res
  FROM ob_pd WHERE payload = 'no-such-payload';

-- ------------------------------------------------------------
-- PD.3 Broader non-PK predicate (grp < 3 -> 3 MAIN groups, minus moved-out)
-- ------------------------------------------------------------
SELECT CASE WHEN count(*) = 55 THEN 'PD_GRP_LT3_COUNT_PASS'
            ELSE 'PD_GRP_LT3_COUNT_FAIL: got=' || count(*) END AS res
  FROM ob_pd WHERE grp < 3;

-- ------------------------------------------------------------
-- PD.4 Structural invariant: delta keys are unique per (branch_id, relid)
--      => the removed RD-1/RD-2 folds were identity and O(M^2); if this
--         ever fails the removals must be revisited.
-- ------------------------------------------------------------
SELECT CASE
         WHEN count(*) = count(DISTINCT (d.branch_id, d.relid, d.key))
         THEN 'PD_KEY_UNIQUE_PASS'
         ELSE 'PD_KEY_UNIQUE_FAIL'
       END AS res
  FROM overlay_branch.pg_branch_delta d;

-- Overall overlay integrity: 200 - 5 DEL + 5 INS = 200, no duplicates.
SELECT CASE WHEN count(*) = 200 AND count(*) = count(DISTINCT id)
            THEN 'PD_TOTAL_PASS' ELSE 'PD_TOTAL_FAIL: got=' || count(*) END AS res
  FROM ob_pd;
SELECT CASE WHEN count(*) = 25 THEN 'PD_GRP8_COUNT_PASS'
            ELSE 'PD_GRP8_COUNT_FAIL: got=' || count(*) END AS res
  FROM ob_pd WHERE grp = 8;

-- ------------------------------------------------------------
-- PD.6 A volatile predicate must NOT be pushed into the hidden SPI MAIN
--      query.  Pushed, it is evaluated once there (to pick MAIN rows) and
--      again by the scan qual (on the overlay), so a side effect such as
--      nextval() fires twice per row and the two evaluations can disagree,
--      silently dropping rows the qual would have kept.  `id >= 1` keeps
--      has_pk_pred true, which is the only path that consults general_where.
--      The predicate is backed by a sequence, so the evaluation count is
--      observable: it must equal the 200 output rows, not twice that.
-- ------------------------------------------------------------
SELECT setval('ob_pd_seq', 1, false);
SELECT CASE WHEN count(*) = 200 THEN 'PD_VOLATILE_COUNT_PASS'
            ELSE 'PD_VOLATILE_COUNT_FAIL: got=' || count(*) END AS res
  FROM ob_pd WHERE id >= 1 AND ob_pd_vol_true(id);
SELECT CASE WHEN last_value = 200 THEN 'PD_VOLATILE_ONCE_PASS'
            ELSE 'PD_VOLATILE_ONCE_FAIL: evals=' || last_value END AS res
  FROM ob_pd_seq;

-- ------------------------------------------------------------
-- PD.7 A volatile qualifier on the FULL-TABLE path (no PK clause) must not be
--      double-evaluated by the Pass-1 MAIN prefilter.  PD.6 carries `id >= 1`,
--      which turns on the P0 point-row fast path where the prefilter never
--      runs; here the predicate is a bare volatile function so `has_pk_pred`
--      is false and the prefilter is the code path under test.  Left
--      enabled, the prefilter would run the clause on every raw MAIN row and
--      the post-merge qual would run it again on the overlay row, doubling
--      nextval(); the volatile gate keeps it to exactly one evaluation per
--      output row (200 rows, 200 evaluations).
-- ------------------------------------------------------------
SELECT setval('ob_pd_seq', 1, false);
SELECT CASE WHEN count(*) = 200 THEN 'PD_VOLATILE_FULLSCAN_COUNT_PASS'
            ELSE 'PD_VOLATILE_FULLSCAN_COUNT_FAIL: got=' || count(*) END AS res
  FROM ob_pd WHERE ob_pd_vol_true(id);
SELECT CASE WHEN last_value = 200 THEN 'PD_VOLATILE_FULLSCAN_ONCE_PASS'
            ELSE 'PD_VOLATILE_FULLSCAN_ONCE_FAIL: evals=' || last_value END AS res
  FROM ob_pd_seq;

-- ------------------------------------------------------------
-- PD.5 Plan shape: the pushed predicate is a plain Filter above a single
--      Custom Scan (no Gather — see overlay_branch_parallel.sql).
-- ------------------------------------------------------------
DO $$
DECLARE
  r        record;
  has_scan boolean := false;
  has_gt   boolean := false;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT * FROM ob_pd WHERE grp = 7'
  LOOP
    IF r."QUERY PLAN" ~* 'overlay_branch_branchscan' THEN has_scan := true; END IF;
    IF r."QUERY PLAN" ~* 'Gather' THEN has_gt := true; END IF;
  END LOOP;

  IF has_scan AND NOT has_gt THEN
    RAISE NOTICE 'PD_PLAN_SHAPE_PASS';
  ELSE
    RAISE NOTICE 'PD_PLAN_SHAPE_FAIL: branchscan=% gather=%', has_scan, has_gt;
  END IF;
END $$;

-- Cleanup
SELECT overlay_branch.use_branch(NULL);
SELECT overlay_branch.discard_branch('ob_pd_b1');
DROP TABLE ob_pd;
DROP FUNCTION ob_pd_vol_true(int);
DROP SEQUENCE ob_pd_seq;
