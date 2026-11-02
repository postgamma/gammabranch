/*
 * overlay_branch_keyless.sql — Experimental keyless-table support
 *                              (overlay_branch.keyless_tables).
 *
 * See doc/internals.md §4 for the design.  A "keyless" table is an
 * ordinary heap table WITHOUT a PRIMARY KEY.  Because there is no durable
 * logical row identity, the extension falls back to a best-effort physical
 * identity derived from the MAIN row's ctid:
 *
 *     "A:<blk>:<off>"  anchored to a MAIN row (UPDATE/DELETE source)
 *     "S:<seq>"        branch-only INSERT (no MAIN anchor)
 *
 * This file pins the OBSERVABLE contract of that feature:
 *
 *   Part A — GUC OFF (default, "current status"): no-PK DML inside a branch is still
 *            rejected with SQLSTATE 0A000, exactly as before the feature.
 *            A plain SELECT is NOT redirected (no BranchScan) and returns
 *            raw MAIN rows without error.
 *   Part B — GUC ON: one-shot WARNING + BranchScan engaged + INSERT/UPDATE/
 *            DELETE on a no-PK table produce a correct MAIN⊕delta overlay
 *            while MAIN stays pristine.
 *   Part C — apply_branch materializes the keyless delta onto MAIN; the
 *            applied branch cannot be re-applied.
 *   Part D — apply conflict detection: a concurrent MAIN rewrite of an
 *            anchored row moves its ctid, so apply raises a conflict ERROR
 *            and leaves the independently-committed MAIN write intact.
 *   Part E — GUC ON does NOT disturb the PK code path (regression): a
 *            normal PK table still uses PK identity.
 *   Part F — OUT-OF-SCOPE (cluster-stability risk, deliberately not
 *            implemented): INSERT ... ON CONFLICT (UPSERT) on a keyless
 *            table is rejected with 0A000.
 *
 * Style rules (identical to the sibling regression files so pg_regress
 * expected .out files stay diff-stable across machines/runs):
 *   * No \meta commands, no timestamps/OIDs/ctid/xmin in SELECT output.
 *   * Every expected ERROR is trapped in a DO $$ ... EXCEPTION ... $$ block
 *     (raw ERRORs embed ctid/xmin tokens that drift per run).
 *   * Assertions project a deterministic 'PASS:...' / 'FAIL:...' literal.
 */

-- ===== 0. Prerequisite guard (same as the other regression files) =====
DO $$
DECLARE
  _s text := current_setting('shared_preload_libraries', true);
  _msg text;
BEGIN
  IF _s NOT LIKE '%overlay_branch%' THEN
    _msg := 'Postmaster started WITHOUT shared_preload_libraries=''overlay_branch''.  '
         || 'Use `make check` (temp-instance, hooks auto-loaded) or pass '
         || '--temp-config=$(pwd)/temp_instance_shared_libs.conf to pg_regress.';
    RAISE EXCEPTION '%', _msg;
  END IF;
END $$;

-- ===== Idempotent cleanup =====
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.kn_off, public.kn_on, public.kn_race,
                     public.kn_pk,  public.kn_ups CASCADE;
DROP EXTENSION IF EXISTS overlay_branch CASCADE;
RESET client_min_messages;
CREATE EXTENSION IF NOT EXISTS overlay_branch;

/* =====================================================================
 * Part A — GUC OFF (default): baseline "no PRIMARY KEY" behavior.
 * ===================================================================== */
SET overlay_branch.keyless_tables = off;
CREATE TABLE public.kn_off (v text);
INSERT INTO public.kn_off VALUES ('a'), ('b');
SELECT create_branch('kn_off_br') AS id_kn_off_br;
SELECT use_branch('kn_off_br');

-- A.1 INSERT is rejected (0A000), MAIN/delta untouched.
DO $$
BEGIN
  INSERT INTO public.kn_off VALUES ('x');
  RAISE NOTICE 'FAIL:A1_OFF_INSERT_WAS_ALLOWED';
EXCEPTION
  WHEN SQLSTATE '0A000' THEN RAISE NOTICE 'PASS:A1_OFF_INSERT_0A000';
  WHEN OTHERS           THEN RAISE NOTICE 'FAIL:A1_OFF_INSERT_OTHER_SQLSTATE=%', SQLSTATE;
END $$;

-- A.2 UPDATE is rejected (0A000).
DO $$
BEGIN
  UPDATE public.kn_off SET v = 'a2' WHERE v = 'a';
  RAISE NOTICE 'FAIL:A2_OFF_UPDATE_WAS_ALLOWED';
EXCEPTION
  WHEN SQLSTATE '0A000' THEN RAISE NOTICE 'PASS:A2_OFF_UPDATE_0A000';
  WHEN OTHERS           THEN RAISE NOTICE 'FAIL:A2_OFF_UPDATE_OTHER_SQLSTATE=%', SQLSTATE;
END $$;

-- A.3 A plain SELECT is not redirected: BranchScan bails out for a no-PK
--     table when the GUC is off, so we silently read raw MAIN rows.
SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b'
            THEN 'PASS:A3_OFF_SELECT_RAW_MAIN'
            ELSE 'FAIL:A3_OFF_SELECT:'||string_agg(v, ',' ORDER BY v) END AS a3
FROM public.kn_off;

-- Leave MAIN and drop the (empty) rejection-only branch.
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT discard_branch('kn_off_br');

/* =====================================================================
 * Part B — GUC ON: WARNING + overlay read/write consistency.
 * ===================================================================== */
SET overlay_branch.keyless_tables = on;
CREATE TABLE public.kn_on (v text);
INSERT INTO public.kn_on VALUES ('a'), ('b'), ('c');
SELECT create_branch('kn_on_br') AS id_kn_on_br;
SELECT use_branch('kn_on_br');

-- B.1 First keyless DML emits the one-shot WARNING (see design §2.3).
INSERT INTO public.kn_on VALUES ('x');
INSERT INTO public.kn_on VALUES ('y');
-- B.2 UPDATE / DELETE of MAIN-anchored rows.
UPDATE public.kn_on SET v = 'b2' WHERE v = 'b';
DELETE FROM public.kn_on WHERE v = 'c';

-- B.3 BranchScan is engaged for the keyless relation.
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT * FROM public.kn_on;

-- B.4 Overlay view = MAIN{a,b} minus deleted{c} plus branch{b2}=update +{x,y}.
SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b2,x,y'
            THEN 'PASS:B4_OVERLAY_VIEW'
            ELSE 'FAIL:B4_OVERLAY_VIEW:'||string_agg(v, ',' ORDER BY v) END AS b4
FROM public.kn_on;

-- B.5 MAIN is pristine while inside the branch.
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b,c'
            THEN 'PASS:B5_MAIN_PRISTINE'
            ELSE 'FAIL:B5_MAIN_DIRTY:'||string_agg(v, ',' ORDER BY v) END AS b5
FROM public.kn_on;

/* =====================================================================
 * Part C — apply_branch materializes the keyless delta onto MAIN.
 * ===================================================================== */
SELECT use_branch('kn_on_br');
SELECT apply_branch('kn_on_br');
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';

-- C.1 MAIN now holds the post-branch image.
SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b2,x,y'
            THEN 'PASS:C1_APPLY_MAIN_IMAGE'
            ELSE 'FAIL:C1_APPLY_MAIN:'||string_agg(v, ',' ORDER BY v) END AS c1
FROM public.kn_on;

-- C.2 state=applied and the delta rows are gone.
SELECT CASE WHEN b.state = 'applied'
             AND (SELECT count(*) FROM overlay_branch.pg_branch_delta d
                   WHERE d.branch_id = b.branch_id) = 0
            THEN 'PASS:C2_APPLIED_DELTA_CLEARED'
            ELSE 'FAIL:C2_STATE_OR_DELTA:'||b.state END AS c2
FROM public.pg_branch b WHERE b.branch_name = 'kn_on_br';

-- C.3 Re-apply is blocked (state=applied, not active).
DO $$
BEGIN
  PERFORM overlay_branch.apply_branch('kn_on_br');
  RAISE NOTICE 'FAIL:C3_REAPPLY_WAS_ALLOWED';
EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'PASS:C3_REAPPLY_BLOCKED';
END $$;

/* =====================================================================
 * Part D — apply conflict: concurrent MAIN rewrite moves an anchored ctid.
 * ===================================================================== */
CREATE TABLE public.kn_race (v text);
INSERT INTO public.kn_race VALUES ('r1'), ('r2'), ('r3');
SELECT create_branch('kn_race_br') AS id_kn_race_br;
SELECT use_branch('kn_race_br');

-- D.1 Branch UPDATE anchors id-row 'r1' to its current MAIN ctid.
UPDATE public.kn_race SET v = 'r1-branch' WHERE v = 'r1';

-- D.2 Step back to MAIN and rewrite the same row: this writes a NEW tuple
--     version at a different ctid, invalidating the branch's anchor.
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
UPDATE public.kn_race SET v = 'RACE-MAIN' WHERE v = 'r1';

-- D.3 Re-enter and apply: the anchored MAIN row is no longer locatable →
--     conflict ERROR (raised inside a sub-transaction so MAIN survives).
SELECT use_branch('kn_race_br');
DO $$
BEGIN
  PERFORM overlay_branch.apply_branch('kn_race_br');
  RAISE NOTICE 'FAIL:D3_APPLY_WAS_EXPECTED_TO_CONFLICT';
EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'PASS:D3_APPLY_CONFLICT_RAISED';
END $$;
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';

-- D.4 The independently-committed MAIN write is preserved (apply replay
--     sub-tx rollback must not touch writes committed OUTSIDE the branch).
SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'RACE-MAIN,r2,r3'
            THEN 'PASS:D4_MAIN_RACE_WRITE_PRESERVED'
            ELSE 'FAIL:D4_MAIN_DIRTY:'||string_agg(v, ',' ORDER BY v) END AS d4
FROM public.kn_race;

-- D.5 Discard the conflicted branch.
SELECT discard_branch('kn_race_br');

/* =====================================================================
 * Part E — regression: GUC ON leaves the PK code path untouched.
 * ===================================================================== */
CREATE TABLE public.kn_pk (id int PRIMARY KEY, v text);
INSERT INTO public.kn_pk VALUES (1, 'a'), (2, 'b');
SELECT create_branch('kn_pk_br') AS id_kn_pk_br;
SELECT use_branch('kn_pk_br');
INSERT INTO public.kn_pk VALUES (3, 'c');
UPDATE public.kn_pk SET v = 'b2' WHERE id = 2;

SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b2,3=c'
            THEN 'PASS:E1_PK_OVERLAY_VIEW'
            ELSE 'FAIL:E1_PK_OVERLAY:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS e1
FROM public.kn_pk;

SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b'
            THEN 'PASS:E2_PK_MAIN_PRISTINE'
            ELSE 'FAIL:E2_PK_MAIN:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS e2
FROM public.kn_pk;
SELECT discard_branch('kn_pk_br');

/* =====================================================================
 * Part F — OUT-OF-SCOPE: keyless UPSERT is rejected (0A000).
 *   The 2-phase ON CONFLICT detector is PK-only; silently mis-promoting
 *   would risk cluster-level corruption, so the feature is refused
 *   rather than approximated (design §6 / §8).  A UNIQUE arbiter (but no
 *   PRIMARY KEY) is what makes the ON CONFLICT clause reach a real
 *   arbiter index, and therefore what triggers the explicit rejection.
 * ===================================================================== */
CREATE TABLE public.kn_ups (id int UNIQUE, v text);
INSERT INTO public.kn_ups VALUES (1, 'a');
SELECT create_branch('kn_ups_br') AS id_kn_ups_br;
SELECT use_branch('kn_ups_br');
DO $$
BEGIN
  INSERT INTO public.kn_ups VALUES (1, 'b') ON CONFLICT (id) DO NOTHING;
  RAISE NOTICE 'FAIL:F1_KEYLESS_UPSERT_WAS_ALLOWED';
EXCEPTION
  WHEN SQLSTATE '0A000' THEN RAISE NOTICE 'PASS:F1_KEYLESS_UPSERT_0A000';
  WHEN OTHERS           THEN RAISE NOTICE 'FAIL:F1_KEYLESS_UPSERT_OTHER_SQLSTATE=%', SQLSTATE;
END $$;
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT discard_branch('kn_ups_br');

-- ===== Teardown =====
RESET overlay_branch.keyless_tables;
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.kn_off, public.kn_on, public.kn_race,
                     public.kn_pk,  public.kn_ups CASCADE;
RESET client_min_messages;
