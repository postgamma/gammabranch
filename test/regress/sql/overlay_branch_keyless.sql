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
 *   Part F — keyless UPSERT (INSERT ... ON CONFLICT): the two-phase
 *            detector is rebound to the statement's UNIQUE arbiter
 *            columns (single- or multi-column); DO NOTHING skips, DO
 *            UPDATE rewrites a MAIN or a branch-born row in place (no
 *            synthetic duplicate), and an arbiter that is not one
 *            plain-column UNIQUE index is still rejected with 0A000.
 *   Part G — read-side version-chain resolution (B2): after a concurrent
 *            MAIN rewrite moves an anchored row's ctid, an overlay SELECT
 *            still shows the branch edit over the moved row (no silent
 *            drop), while apply keeps refusing the stale anchor (0A000).
 *   Part H — set-based apply coverage (B3): the keyless delta replays
 *            through the ctid-anchor batch join; the batch and per-row
 *            replays are byte-identical (differential off vs on).
 *   Part I — keyless UPSERT with a NULL arbiter value: the arbiter probe
 *            must follow the UNIQUE index's null handling — a NULL never
 *            matches under the default NULLS DISTINCT index (degrade to a
 *            plain INSERT), while it does conflict under NULLS NOT
 *            DISTINCT.  Regression: a NULL arbiter value used to bind a
 *            bogus (Datum) 0 text payload and crash the server.
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
         || 'Use `make installcheck` or pass '
         || '--temp-config=$(pwd)/temp_instance_shared_libs.conf to pg_regress.';
    RAISE EXCEPTION '%', _msg;
  END IF;
END $$;

-- ===== Idempotent cleanup =====
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.kn_off, public.kn_on, public.kn_race,
                     public.kn_pk,  public.kn_ups, public.kn_ups2,
                     public.kn_move, public.kn_nd, public.kn_nd2 CASCADE;
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
 * Part F — keyless UPSERT (INSERT ... ON CONFLICT) is supported (B4).
 *   A no-PK table arbitrates on the UNIQUE constraint the statement
 *   names, not on the ctid identity that keys the delta.  The redirect
 *   rebinds the PK path's two-phase detector to the arbiter columns: a
 *   delta hit reuses the existing delta key/version, a MAIN hit
 *   re-anchors to that row's ctid, and an arbiter that is not exactly
 *   one plain-column UNIQUE index stays refused (0A000).
 * ===================================================================== */
CREATE TABLE public.kn_ups (id int UNIQUE, v text);
INSERT INTO public.kn_ups VALUES (1, 'a'), (2, 'b');
SELECT create_branch('kn_ups_br') AS id_kn_ups_br;
SELECT use_branch('kn_ups_br');

-- F.1 DO NOTHING over a MAIN conflict is a no-op: the overlay still shows
--     the MAIN image and no delta row is minted.
INSERT INTO public.kn_ups VALUES (1, 'IGNORED') ON CONFLICT (id) DO NOTHING;
SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b'
            THEN 'PASS:F1_UPSERT_DO_NOTHING_SKIP'
            ELSE 'FAIL:F1_UPSERT_DO_NOTHING:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS f1
FROM public.kn_ups;

-- F.2 DO UPDATE over a MAIN conflict rewrites the row inside the branch
--     (anchored to the MAIN row's ctid, not a synthetic key).
INSERT INTO public.kn_ups VALUES (2, 'b2') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v;
SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b2'
            THEN 'PASS:F2_UPSERT_DO_UPDATE_OVERLAY'
            ELSE 'FAIL:F2_UPSERT_DO_UPDATE:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS f2
FROM public.kn_ups;

-- F.3 A second DO UPDATE on the same conflicting row reuses the delta key
--     (no synthetic duplicate): still exactly two rows.
INSERT INTO public.kn_ups VALUES (2, 'b3') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v;
SELECT CASE WHEN count(*) = 2 AND string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b3'
            THEN 'PASS:F3_UPSERT_REPEAT_SAME_KEY'
            ELSE 'FAIL:F3_UPSERT_REPEAT:'||count(*)||':'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS f3
FROM public.kn_ups;

-- F.4 No conflict → plain branch INSERT (synthetic key).
INSERT INTO public.kn_ups VALUES (3, 'c') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v;
-- F.5 UPSERT over a branch-born row (delta-only) updates it in place.
INSERT INTO public.kn_ups VALUES (3, 'c2') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v;
SELECT CASE WHEN count(*) = 3 AND string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b3,3=c2'
            THEN 'PASS:F5_UPSERT_BRANCH_BORN'
            ELSE 'FAIL:F5_UPSERT_BRANCH_BORN:'||count(*)||':'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS f5
FROM public.kn_ups;

-- F.6 MAIN stays pristine.
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b'
            THEN 'PASS:F6_UPSERT_MAIN_PRISTINE'
            ELSE 'FAIL:F6_UPSERT_MAIN:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS f6
FROM public.kn_ups;

-- F.7 apply materializes the branch image onto MAIN.
SELECT apply_branch('kn_ups_br');
SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b3,3=c2'
            THEN 'PASS:F7_UPSERT_APPLIED'
            ELSE 'FAIL:F7_UPSERT_APPLIED:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS f7
FROM public.kn_ups;

-- F.8 An arbiter that does not resolve to one plain-column UNIQUE index
--     (here an expression index) stays refused with 0A000.
CREATE TABLE public.kn_expr (lc text);
CREATE UNIQUE INDEX kn_expr_lc ON public.kn_expr (lower(lc));
INSERT INTO public.kn_expr VALUES ('a');
SELECT create_branch('kn_expr_br') AS id_kn_expr_br;
SELECT use_branch('kn_expr_br');
DO $$
BEGIN
  INSERT INTO public.kn_expr VALUES ('A') ON CONFLICT (lower(lc)) DO NOTHING;
  RAISE NOTICE 'FAIL:F8_KEYLESS_EXPR_UPSERT_WAS_ALLOWED';
EXCEPTION
  WHEN SQLSTATE '0A000' THEN RAISE NOTICE 'PASS:F8_KEYLESS_EXPR_UPSERT_0A000';
  WHEN OTHERS           THEN RAISE NOTICE 'FAIL:F8_KEYLESS_EXPR_UPSERT_OTHER=%', SQLSTATE;
END $$;
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT discard_branch('kn_expr_br');

-- F.9 A multi-column UNIQUE arbiter is accepted (it is still exactly one
--     plain-column UNIQUE index): the detector probes every arbiter column,
--     a MAIN hit overwrites one row and a no-hit lands a synthetic insert.
CREATE TABLE public.kn_ups2 (a int, b int, v text, UNIQUE (a, b));
INSERT INTO public.kn_ups2 VALUES (1, 1, 'x');
SELECT create_branch('kn_ups2_br') AS id_kn_ups2_br;
SELECT use_branch('kn_ups2_br');
INSERT INTO public.kn_ups2 VALUES (1, 1, 'x2') ON CONFLICT (a, b) DO UPDATE SET v = EXCLUDED.v;
INSERT INTO public.kn_ups2 VALUES (2, 2, 'y') ON CONFLICT (a, b) DO UPDATE SET v = EXCLUDED.v;
SELECT CASE WHEN string_agg(a::text||','||b::text||'='||v, ';' ORDER BY a, b) = '1,1=x2;2,2=y'
            THEN 'PASS:F9_KEYLESS_MULTICOL_UPSERT'
            ELSE 'FAIL:F9_KEYLESS_MULTICOL_UPSERT:'||string_agg(a::text||','||b::text||'='||v, ';' ORDER BY a, b) END AS f9
FROM public.kn_ups2;
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT apply_branch('kn_ups2_br');
SELECT CASE WHEN string_agg(a::text||','||b::text||'='||v, ';' ORDER BY a, b) = '1,1=x2;2,2=y'
            THEN 'PASS:F9_KEYLESS_MULTICOL_APPLIED'
            ELSE 'FAIL:F9_KEYLESS_MULTICOL_APPLIED:'||string_agg(a::text||','||b::text||'='||v, ';' ORDER BY a, b) END AS f9b
FROM public.kn_ups2;

/* =====================================================================
 * Part G — read-side version-chain resolution (B2): a concurrent MAIN
 * rewrite moves an anchored row's ctid; an overlay SELECT re-anchors it
 * along the t_ctid chain and still shows the branch edit, while apply
 * keeps refusing the now-stale anchor.
 * ===================================================================== */
SET overlay_branch.keyless_tables = on;
CREATE TABLE public.kn_move (v text);
INSERT INTO public.kn_move VALUES ('m1'), ('m2');
SELECT create_branch('kn_move_br') AS id_kn_move_br;
SELECT use_branch('kn_move_br');

-- G.1 Branch UPDATE anchors id-row 'm1' to its current MAIN ctid.
UPDATE public.kn_move SET v = 'm1-branch' WHERE v = 'm1';

-- G.2 Step back to MAIN and rewrite the same row: this writes a NEW tuple
--     version at a different ctid, leaving a dangling t_ctid link at the
--     old line pointer (the branch anchor).
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
UPDATE public.kn_move SET v = 'M1-MAIN' WHERE v = 'm1';

-- G.3 Re-enter and READ: the stale anchor is chased down the version chain
--     to the row's current ctid, so the branch edit shows over the moved row.
SELECT use_branch('kn_move_br');
SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'm1-branch,m2'
            THEN 'PASS:G3_REANCHORED_READ'
            ELSE 'FAIL:G3_READ:'||string_agg(v, ',' ORDER BY v) END AS g3
FROM public.kn_move;

-- G.4 Read-only guarantee: apply still cannot locate the stale anchor and
--     raises a conflict, so the independently-committed MAIN write survives.
DO $$
BEGIN
  PERFORM overlay_branch.apply_branch('kn_move_br');
  RAISE NOTICE 'FAIL:G4_APPLY_WAS_EXPECTED_TO_CONFLICT';
EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'PASS:G4_APPLY_CONFLICT_RAISED';
END $$;
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';

-- G.5 MAIN keeps the moved, independently-committed image.
SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'M1-MAIN,m2'
            THEN 'PASS:G5_MAIN_MOVED_IMAGE'
            ELSE 'FAIL:G5_MAIN:'||string_agg(v, ',' ORDER BY v) END AS g5
FROM public.kn_move;

-- G.6 Discard the conflicted branch.
SELECT discard_branch('kn_move_br');

/* =====================================================================
 * Part H — set-based apply coverage (B3): a keyless delta replays through
 * the ctid-anchor batch join (apply_batch=on).  Two identically-populated
 * tables run the same D+U+I edits, one replayed per-row and one batched;
 * the resulting images must be byte-identical.
 * ===================================================================== */
SET overlay_branch.keyless_tables = on;
CREATE TABLE public.kn_da (v text);
CREATE TABLE public.kn_db (v text);
INSERT INTO public.kn_da VALUES ('k1'), ('k2'), ('k3');
INSERT INTO public.kn_db VALUES ('k1'), ('k2'), ('k3');

-- H.1 clone A replayed with apply_batch = off (legacy per-row).
SET overlay_branch.apply_batch = off;
SELECT create_branch('kn_da_br') AS id_kn_da_br;
SELECT use_branch('kn_da_br');
DELETE FROM public.kn_da WHERE v = 'k2';
UPDATE public.kn_da SET v = 'k1-br' WHERE v = 'k1';
INSERT INTO public.kn_da VALUES ('kn-ins');
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT apply_branch('kn_da_br');

-- H.2 clone B replayed with apply_batch = on (set-based ctid-anchor join).
SET overlay_branch.apply_batch = on;
SELECT create_branch('kn_db_br') AS id_kn_db_br;
SELECT use_branch('kn_db_br');
DELETE FROM public.kn_db WHERE v = 'k2';
UPDATE public.kn_db SET v = 'k1-br' WHERE v = 'k1';
INSERT INTO public.kn_db VALUES ('kn-ins');
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT apply_branch('kn_db_br');

-- H.3 Both replays agree, and match the expected post-image.
SELECT CASE WHEN a.s = b.s AND a.s = 'k1-br,k3,kn-ins'
            THEN 'PASS:H_KEYLESS_BATCH_EQUIV'
            ELSE 'FAIL:H off=' || COALESCE(a.s,'NULL') || ' on=' || COALESCE(b.s,'NULL')
       END AS h
FROM  (SELECT string_agg(v, ',' ORDER BY v) AS s FROM public.kn_da) a,
      (SELECT string_agg(v, ',' ORDER BY v) AS s FROM public.kn_db) b;
RESET overlay_branch.apply_batch;

/* =====================================================================
 * Part I — keyless UPSERT with a NULL arbiter value.
 *   The arbiter probe must mirror the UNIQUE index's null handling:
 *     * default index (NULLS DISTINCT): a NULL arbiter value never
 *       matches anything, so ON CONFLICT degrades to a plain INSERT;
 *     * NULLS NOT DISTINCT index: NULL is a comparable value, so it
 *       conflicts with an existing NULL row (DO NOTHING skips, DO UPDATE
 *       rewrites that row).
 *   Regression: a NULL arbiter value used to be bound as a (Datum) 0
 *   text payload with a NULL nulls array, making SPI dereference a bogus
 *   pointer and crash the server.
 * ===================================================================== */
SET overlay_branch.keyless_tables = on;

-- I.1 default UNIQUE (NULLS DISTINCT): the NULL arbiter never conflicts,
--     so every statement is a plain INSERT (three distinct NULL rows).
CREATE TABLE public.kn_nd (id int, u int UNIQUE, v text);
INSERT INTO public.kn_nd VALUES (1, 1, 'a');
SELECT create_branch('kn_nd_br') AS id_kn_nd_br;
SELECT use_branch('kn_nd_br');
INSERT INTO public.kn_nd VALUES (2, NULL, 'n2') ON CONFLICT (u) DO UPDATE SET v = 'u2';
INSERT INTO public.kn_nd VALUES (3, NULL, 'n3') ON CONFLICT (u) DO NOTHING;
SELECT CASE WHEN count(*) = 3
             AND string_agg(id::text||'='||COALESCE(u::text,'~')||'/'||v, ',' ORDER BY id) = '1=1/a,2=~/n2,3=~/n3'
            THEN 'PASS:I1_NULLS_DISTINCT_NO_CONFLICT'
            ELSE 'FAIL:I1:'||count(*)||':'||string_agg(id::text||'='||COALESCE(u::text,'~')||'/'||v, ',' ORDER BY id) END AS i1
FROM public.kn_nd;
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT apply_branch('kn_nd_br');
SELECT CASE WHEN count(*) = 3
             AND string_agg(id::text||'='||COALESCE(u::text,'~')||'/'||v, ',' ORDER BY id) = '1=1/a,2=~/n2,3=~/n3'
            THEN 'PASS:I1_NULLS_DISTINCT_APPLIED'
            ELSE 'FAIL:I1_APPLIED:'||count(*)||':'||string_agg(id::text||'='||COALESCE(u::text,'~')||'/'||v, ',' ORDER BY id) END AS i1b
FROM public.kn_nd;

-- I.2 NULLS NOT DISTINCT: NULL conflicts, so DO NOTHING skips the second
--     NULL and DO UPDATE rewrites the first NULL row in place.
CREATE TABLE public.kn_nd2 (id int, u int, v text, UNIQUE NULLS NOT DISTINCT (u));
INSERT INTO public.kn_nd2 VALUES (1, 1, 'a');
SELECT create_branch('kn_nd2_br') AS id_kn_nd2_br;
SELECT use_branch('kn_nd2_br');
INSERT INTO public.kn_nd2 VALUES (2, NULL, 'n2');
INSERT INTO public.kn_nd2 VALUES (3, NULL, 'n3') ON CONFLICT (u) DO NOTHING;
INSERT INTO public.kn_nd2 VALUES (4, NULL, 'n4') ON CONFLICT (u) DO UPDATE SET v = 'n4';
SELECT CASE WHEN count(*) = 2
             AND string_agg(id::text||'='||COALESCE(u::text,'~')||'/'||v, ',' ORDER BY id) = '1=1/a,2=~/n4'
            THEN 'PASS:I2_NULLS_NOT_DISTINCT_CONFLICT'
            ELSE 'FAIL:I2:'||count(*)||':'||string_agg(id::text||'='||COALESCE(u::text,'~')||'/'||v, ',' ORDER BY id) END AS i2
FROM public.kn_nd2;
SELECT use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT apply_branch('kn_nd2_br');
SELECT CASE WHEN count(*) = 2
             AND string_agg(id::text||'='||COALESCE(u::text,'~')||'/'||v, ',' ORDER BY id) = '1=1/a,2=~/n4'
            THEN 'PASS:I2_NULLS_NOT_DISTINCT_APPLIED'
            ELSE 'FAIL:I2_APPLIED:'||count(*)||':'||string_agg(id::text||'='||COALESCE(u::text,'~')||'/'||v, ',' ORDER BY id) END AS i2b
FROM public.kn_nd2;

-- ===== Teardown =====
RESET overlay_branch.keyless_tables;
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.kn_off, public.kn_on, public.kn_race,
                     public.kn_pk,  public.kn_ups, public.kn_ups2,
                     public.kn_expr, public.kn_move, public.kn_da,
                     public.kn_db, public.kn_nd, public.kn_nd2 CASCADE;
RESET client_min_messages;
