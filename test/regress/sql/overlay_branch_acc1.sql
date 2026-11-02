-- ==========================================================
-- ACC-1 regression suite: DML-target BranchScan (overlay pre-image).
--
-- Before ACC-1 the UPDATE/DELETE DML subplan scanned the MAIN baserel
-- (the planner hook returned early for non-SELECT commandType), which
-- produced three gap classes for in-branch chained DML against a
-- MAIN-present row:
--   G1  SET expression value dependence  (v = v || suffix chain)
--   G2  WHERE predicate over an overlay-only column value
--   G3  non-SET column falling back to the MAIN value
-- ACC-1 injects a BranchScan over the DML target rel and makes the
-- write-redirect loop use the OVERLAY row (MAIN (+) delta) as the
-- pre-image, eliminating all three.
--
-- Style rules (identical to the sibling regression files so pg_regress
-- expected .out files stay diff-stable across machines/runs):
--   * No \meta commands, no timestamps/OIDs/ctid/xmin in SELECT output.
--   * Every expected ERROR is trapped in a DO $$ ... EXCEPTION ... $$ block.
--   * Assertions project a deterministic 'PASS:...' / 'FAIL:...' literal.
-- ==========================================================

DROP EXTENSION IF EXISTS overlay_branch CASCADE;
CREATE EXTENSION overlay_branch;

CREATE TABLE acc1_t (
    id  int PRIMARY KEY,
    v   text,
    w   text
);
INSERT INTO acc1_t (id, v, w) VALUES (1, 'base', 'W0'), (2, 'y', 'Y0');
SELECT create_branch('acc1_b');
SELECT use_branch('acc1_b');

-- ===== ACC1_CHAIN (G1): value-dependent chained SET =====
-- v = v || '+1' then v = v || '+2' must compound (base+1+2), not (base+2).
UPDATE acc1_t SET v = v || '+1' WHERE id = 1;
UPDATE acc1_t SET v = v || '+2' WHERE id = 1;
SELECT CASE WHEN v = 'base+1+2' THEN 'PASS:ACC1_CHAIN'
            ELSE 'FAIL:ACC1_CHAIN v=' || COALESCE(v,'NULL') END AS acc1_chain
FROM acc1_t WHERE id = 1;

-- ===== ACC1_CROSSCOL (G3): non-SET column keeps the overlay value =====
-- The 2nd SET must leave w at the 1st SET's overlay value 'W1', not the
-- MAIN value 'W0'.
UPDATE acc1_t SET w = 'W1' WHERE id = 2;
UPDATE acc1_t SET v = 'y2' WHERE id = 2;
SELECT CASE WHEN (v, w) = ('y2', 'W1') THEN 'PASS:ACC1_CROSSCOL'
            ELSE 'FAIL:ACC1_CROSSCOL(' || COALESCE(v,'NULL') || ',' ||
                 COALESCE(w,'NULL') || ')' END AS acc1_crosscol
FROM acc1_t WHERE id = 2;

-- ===== ACC1_WHERE (G2): predicate over an overlay-only value =====
-- MAIN id=2 is 'y'; the branch set it to 'CHANGED'.  A following UPDATE
-- whose WHERE matches on 'CHANGED' MUST hit the row (overlay semantics).
UPDATE acc1_t SET v = 'CHANGED' WHERE id = 2;
UPDATE acc1_t SET w = 'HIT' WHERE id = 2 AND v = 'CHANGED';
SELECT CASE WHEN (v, w) = ('CHANGED', 'HIT') THEN 'PASS:ACC1_WHERE'
            ELSE 'FAIL:ACC1_WHERE(' || COALESCE(v,'NULL') || ',' ||
                 COALESCE(w,'NULL') || ')' END AS acc1_where
FROM acc1_t WHERE id = 2;

-- ===== ACC1_DELETE: DELETE of a MAIN-present row that has overlay edits =====
UPDATE acc1_t SET v = 'pre-del' WHERE id = 2;
DELETE FROM acc1_t WHERE id = 2;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ACC1_DELETE'
            ELSE 'FAIL:ACC1_DELETE count=' || count(*) END AS acc1_delete
FROM acc1_t WHERE id = 2;

-- ===== ACC1_MIX: pure-delta rows and MAIN-present rows in one branch =====
-- id=3 is a branch-only INSERT followed by a pure-delta UPDATE; id=1 is a
-- MAIN-present row updated again.  Both routes must stay correct and
-- disjoint (no double handling).
INSERT INTO acc1_t (id, v, w) VALUES (3, 'i3', 'I3');
UPDATE acc1_t SET v = 'i3u' WHERE id = 3;
UPDATE acc1_t SET v = v || '!' WHERE id = 1;
SELECT CASE WHEN v = 'base+1+2!' THEN 'PASS:ACC1_MIX_MAINPRESENT'
            ELSE 'FAIL:ACC1_MIX v=' || COALESCE(v,'NULL') END AS acc1_mix1
FROM acc1_t WHERE id = 1;
SELECT CASE WHEN v = 'i3u' THEN 'PASS:ACC1_MIX_PUREDELTA'
            ELSE 'FAIL:ACC1_MIX_PD v=' || COALESCE(v,'NULL') END AS acc1_mix3
FROM acc1_t WHERE id = 3;

-- ===== ACC1_APPLY: materialise the branch onto MAIN =====
SELECT use_branch(NULL);
SELECT apply_branch('acc1_b');
SELECT CASE WHEN (v, w) = ('base+1+2!', 'W0') THEN 'PASS:ACC1_APPLY_R1'
            ELSE 'FAIL:ACC1_APPLY_R1(' || COALESCE(v,'NULL') || ',' ||
                 COALESCE(w,'NULL') || ')' END AS acc1_apply1
FROM acc1_t WHERE id = 1;
SELECT CASE WHEN v = 'i3u' THEN 'PASS:ACC1_APPLY_R3'
            ELSE 'FAIL:ACC1_APPLY_R3 v=' || COALESCE(v,'NULL') END AS acc1_apply3
FROM acc1_t WHERE id = 3;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ACC1_APPLY_DEL'
            ELSE 'FAIL:ACC1_APPLY_DEL count=' || count(*) END AS acc1_apply_del
FROM acc1_t WHERE id = 2;

DROP TABLE acc1_t;

-- ===== ACC1_NEG: no-PK table must not be affected by ACC-1 (no regression) =====
-- With the keyless GUC off (default), in-branch DML on a no-PK table is
-- rejected with 0A000 exactly as before, and a plain SELECT reads MAIN
-- unredirected.  This pins that ACC-1's result-relation injection does not
-- change the no-PK contract.
SET overlay_branch.keyless_tables = off;
CREATE TABLE acc1_neg (v text);
INSERT INTO acc1_neg VALUES ('a'), ('b');
SELECT create_branch('acc1_neg_b');
SELECT use_branch('acc1_neg_b');

DO $$
BEGIN
  UPDATE acc1_neg SET v = 'a2' WHERE v = 'a';
  RAISE NOTICE 'FAIL:ACC1_NEG_UPDATE_WAS_ALLOWED';
EXCEPTION
  WHEN SQLSTATE '0A000' THEN RAISE NOTICE 'PASS:ACC1_NEG_UPDATE_0A000';
  WHEN OTHERS           THEN RAISE NOTICE 'FAIL:ACC1_NEG_OTHER_SQLSTATE=%', SQLSTATE;
END $$;

SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b' THEN 'PASS:ACC1_NEG_RAW_MAIN'
            ELSE 'FAIL:ACC1_NEG_RAW_MAIN:' || string_agg(v, ',' ORDER BY v)
       END AS acc1_neg FROM acc1_neg;

SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT discard_branch('acc1_neg_b');
DROP TABLE acc1_neg;
RESET overlay_branch.keyless_tables;

DROP EXTENSION overlay_branch CASCADE;
