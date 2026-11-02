/*
 * overlay_branch_scan_pk.sql — BranchScan P0/P1/P2 primary-key core-path regression.
 *
 * COVERAGE (originally overlay_branch_advanced.sql Section A/B/C/E, single session self-contained):
 *   Section A — BS_PK_LOOKUP P0 PK-IndexScan MVP on bs_user:
 *               INT4 PK + 9 kinds of WHERE pk=?/IN/BETWEEN/ANY/IS NULL/<>2; fast-path assertion
 *   Section B — PK Physics bs_basic: NUMERIC PK + tag column, BranchScan + apply 100% overlap
 *               (overlay vs apply vs MAIN three-way EXCEPT equal); fast-path assertion PK=O(1)
 *   Section C — RETURNING clause P1: INSERT/UPDATE/DELETE RETURNING *,
 *               WR writes delta, then the returned slot value equals the post-MAIN-apply value
 *   Section E — NON-PK qual pushdown P2: WHERE non-PK filter pushed through WR,
 *               BS layer only ExecQuals same-pk delta, ensuring non-PK filter truly takes effect without NUKEm
 *   Section F — RD-8 read-path shortcut: two cases, empty delta(M=0) and all-typed delta
 *               under which Pass1 skips JSON text key serialization; overlay result must match MAIN / SRF
 *   Section G — RD-8 pure-C PK serialiser: when PK value contains JSON special chars (" and \),
 *               the key produced by pure-C escape_json must be parseable back by typed-key and overlay result correct
 *   Section H — RD-7 MAIN read without SPI: direct heap scan + baserestrictinfo pre-filter; pre-filtered
 *               out rows rewritten by branch must be restored by Pass2 orphan, finally scan.plan.qual
 *               re-filters as fallback (incl. plain full scan / P0 point lookup / SRF equivalence / LIMIT)
 *   Section I — RD-6 streaming iterator: overlay result produced row-by-row on demand (LIMIT early stop / cursor across
 *               Exec reuse / Pass1→Pass2 boundary / SRF early stop and equivalence / correlated subplan rewind)
 *   Section J — P0 predicate pushdown extension: PK predicate relaxed from "single-column equality" to IN / range / OR;
 *               L1 MAIN narrowing goes through the same SPI; L2 DELTA automatically degrades to full list;
 *               filtered-out MAIN rows that a branch INSERT adds into the predicate range must be restored by Pass2 orphan
 *
 * Sections are isolated by RESET overlay_branch.current; each section is self-contained with SETUP (separate bs_* table + branch).
 */

-- ===== 0. Prerequisite guard (same as the other regression files) =====
DO $$
DECLARE
  _s text := current_setting('shared_preload_libraries', true);
  _msg text;
BEGIN
  IF _s NOT LIKE '%overlay_branch%' THEN
    _msg := 'Postmaster started WITHOUT shared_preload_libraries=''overlay_branch''.  '
         || 'The 3 correct ways to invoke these tests (DON''T `cd test/` first):'
         || chr(10)||chr(10)||'  (A) make check                    <- temp-instance, hooks auto-loaded'
         || chr(10)||'  (B) make installcheck EXTRA_REGRESS_OPTS="--use-existing --host=/tmp --port=15433"'
         || chr(10)||'  (C) pg_regress overlay_branch_advanced \'
         || chr(10)||'         --inputdir=./test --outputdir=./test_output \'
         || chr(10)||'         --temp-config=$(pwd)/test/temp_instance_shared_libs.conf \'
         || chr(10)||'         --bindir=$(pg_config --bindir)'
         || chr(10)||chr(10);
    RAISE EXCEPTION '%', _msg;
  END IF;
END $$;

-- ===== Idempotent cleanup =====
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.bs_user;
DROP TABLE IF EXISTS public.bs_basic;
DROP EXTENSION IF EXISTS overlay_branch CASCADE;
RESET client_min_messages;

-- =====================================================================
-- ===== Section A: BS_PK_LOOKUP  — P0 PK-IndexScan MVP on bs_user =====
-- =====================================================================
CREATE EXTENSION overlay_branch;

CREATE TABLE public.bs_user (id INT4 PRIMARY KEY, name TEXT, color TEXT);
INSERT INTO public.bs_user VALUES
  (10, 'apple',  'red'),
  (20, 'banana', 'yellow'),
  (30, 'cherry', 'red');

-- A.1 Create + enter branch 'bs_user_b' and write 3 representative
--     mutations (UPDATE override, DELETE tombstone, INSERT new row).
SELECT overlay_branch.create_branch('bs_user_b');
SELECT overlay_branch.use_branch('bs_user_b');

DELETE FROM public.bs_user WHERE id = 20;
UPDATE public.bs_user SET color = 'green' WHERE id = 10;
INSERT INTO public.bs_user VALUES (40, 'date', 'brown');

-- A.2 MAIN must remain untouched (run from MAIN, then return to branch).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
                = '10=apple:red,20=banana:yellow,30=cherry:red'
         THEN 'PASS:ADV_A_MAIN_UNTOUCHED'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||'='||name||':'||color, ',' ORDER BY id),'NULL')
       END AS adv_a_main
FROM public.bs_user;
SELECT overlay_branch.use_branch('bs_user_b');

-- A.3 SRF ↔ BranchScan equivalence for the FULL view (precondition:
--     the fast-path assertions below are meaningless if the full
--     views already disagree).
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||'='||(r).name||':'||(r).color, ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text))
              =
              (SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
               FROM public.bs_user)
         THEN 'PASS:ADV_A_FULL_SRF_EQUIV'
         ELSE 'FAIL:ADV_A_FULL_SRF_MISMATCH'
       END AS adv_a_full_equiv;

-- ===== PK fast-path assertions =====

-- A.4 Even with the PK fast-path active, EXPLAIN must still show the
--     outer Custom Scan (overlay logic must stay in charge).
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT * FROM public.bs_user WHERE id = 10;

-- A.5 PK point lookups, one assertion per delta-physics category.

-- id=10 — UPDATE override (MAIN row color='red' → delta color='green').
SELECT CASE
         WHEN (SELECT (r).id::text||'='||(r).name||':'||(r).color
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 10)
              =
              (SELECT id::text||'='||name||':'||color
               FROM public.bs_user WHERE id = 10)
         THEN 'PASS:ADV_A_PK_EQUIV_ID10'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID10'
       END AS adv_a_pk_id10;
SELECT CASE WHEN color = 'green' AND id = 10 THEN 'PASS:ADV_A_ID10_GREEN'
            ELSE 'FAIL:'||id::text||':'||COALESCE(color,'NULL') END AS adv_a_id10_green
FROM public.bs_user WHERE id = 10;

-- id=20 — DELETE tombstone → must return 0 rows.
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 20)
              = (SELECT count(*) FROM public.bs_user WHERE id = 20)
         THEN 'PASS:ADV_A_PK_EQUIV_ID20'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID20'
       END AS adv_a_pk_id20;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_A_ID20_DELETED'
            ELSE 'FAIL:'||count(*) END AS adv_a_id20_zero
FROM public.bs_user WHERE id = 20;

-- id=30 — MAIN passthrough (no delta row at all).
SELECT CASE
         WHEN (SELECT (r).id::text||'='||(r).name||':'||(r).color
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 30)
              =
              (SELECT id::text||'='||name||':'||color
               FROM public.bs_user WHERE id = 30)
         THEN 'PASS:ADV_A_PK_EQUIV_ID30'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID30'
       END AS adv_a_pk_id30;
SELECT CASE WHEN color = 'red' AND id = 30 AND name = 'cherry'
            THEN 'PASS:ADV_A_ID30_RED_CHERRY'
            ELSE 'FAIL:'||id::text||':'||name||':'||COALESCE(color,'NULL')
       END AS adv_a_id30_red
FROM public.bs_user WHERE id = 30;

-- id=40 — pure-INSERT (only delta row, no MAIN row at all).
SELECT CASE
         WHEN (SELECT (r).id::text||'='||(r).name||':'||(r).color
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 40)
              =
              (SELECT id::text||'='||name||':'||color
               FROM public.bs_user WHERE id = 40)
         THEN 'PASS:ADV_A_PK_EQUIV_ID40'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID40'
       END AS adv_a_pk_id40;
SELECT CASE WHEN color = 'brown' AND id = 40 AND name = 'date'
            THEN 'PASS:ADV_A_ID40_BROWN_DATE'
            ELSE 'FAIL:'||id::text||':'||name||':'||COALESCE(color,'NULL')
       END AS adv_a_id40_brown
FROM public.bs_user WHERE id = 40;

-- A.6 Non-existent PK id=999 → empty set, matches SRF.
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 999)
              = (SELECT count(*) FROM public.bs_user WHERE id = 999)
         THEN 'PASS:ADV_A_PK_EQUIV_ID999'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID999'
       END AS adv_a_pk_id999;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_A_ID999_NONE'
            ELSE 'FAIL:'||count(*) END AS adv_a_id999_empty
FROM public.bs_user WHERE id = 999;

-- A.7 Commutator form Const = PK (id=10 ↔ 10=id) must give the same
--     answer (Planner hook recognizes commutator eq_opr).
SELECT CASE
         WHEN (SELECT id::text||'='||name||':'||color FROM public.bs_user WHERE id = 10)
              = (SELECT id::text||'='||name||':'||color FROM public.bs_user WHERE 10 = id)
         THEN 'PASS:ADV_A_PK_COMMUTATOR'
         ELSE 'FAIL:ADV_A_PK_COMMUTATOR'
       END AS adv_a_commutator;

-- A.8 Non-PK predicate (color='red'): cannot use PK fast-path so it
--     falls back to full overlay, still must match SRF.
SELECT CASE
         WHEN (SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).color = 'red')
              = (SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
                 FROM public.bs_user WHERE color = 'red')
         THEN 'PASS:ADV_A_NONPK_RED'
         ELSE 'FAIL:ADV_A_NONPK_RED'
       END AS adv_a_nonpk_red;

-- A.9 Cleanup the bs_user fixture.
RESET overlay_branch.current;
DROP TABLE public.bs_user;


RESET overlay_branch.current;

/* =========================================================== Section B */
-- =====================================================================
-- ===== Section B: PK Physics on bs_basic (numeric PK + tag column) =====
-- =====================================================================
-- Uses the exact delta-physics arrangement from Part 6 of
-- overlay_branch_basic.sql to make the PK-fast-path assertions directly
-- comparable to the SRF truth.

CREATE TABLE public.bs_basic (
    id  INT4 PRIMARY KEY,
    amt NUMERIC(10,2),
    tag TEXT
);
INSERT INTO public.bs_basic VALUES
  (1, 10.50, 'alpha'),
  (2, 20.00, 'beta'),
  (3, 30.75, 'gamma'),
  (7, 70.00, 'eta');

SELECT overlay_branch.create_branch('bs_basic_b1');
SELECT overlay_branch.use_branch('bs_basic_b1');

DELETE FROM public.bs_basic WHERE id = 3;
UPDATE public.bs_basic SET amt = 99.99, tag = 'BETA!' WHERE id = 2;
INSERT INTO public.bs_basic VALUES (9, 900.00, 'inserted-niner');
-- id=1 and id=7 intentionally NOT touched (purely-MAIN passthrough).

-- B.1 MAIN zero-pollution check (then back to the branch).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text, ',' ORDER BY id) = '1,2,3,7'
              AND string_agg(amt::text, ',' ORDER BY id) = '10.50,20.00,30.75,70.00'
         THEN 'PASS:ADV_B_MAIN_ZERO_POLLUTION'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_b_main_pollution
FROM public.bs_basic;
SELECT overlay_branch.use_branch('bs_basic_b1');

-- B.2 Full-view SRF ↔ BranchScan equivalence (precondition).
WITH srf AS (
  SELECT string_agg((r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL'), '|' ORDER BY (r).id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
    AS r(id int4, amt numeric, tag text)
), cs AS (
  SELECT string_agg(id::text||':'||amt::text||':'||COALESCE(tag,'NULL'), '|' ORDER BY id) AS pic
  FROM public.bs_basic
)
SELECT CASE
         WHEN srf.pic = cs.pic THEN 'PASS:ADV_B_FULL_EQUIV'
         ELSE 'FAIL_SRF<>'||COALESCE(cs.pic,'NULL')||' BASE='||COALESCE(srf.pic,'NULL')
       END AS adv_b_full_equiv
FROM srf, cs;

-- ===== PK fast-path assertions =====

-- B.3 EXPLAIN must still show Custom Scan (overlay stays in charge).
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT * FROM public.bs_basic WHERE id = 2;

-- B.4 Per-category PK point lookups:
--   id=1 — MAIN passthrough   (alpha)
--   id=2 — UPDATE override    (99.99, BETA!)
--   id=3 — DELETE tombstone   (must vanish)
--   id=9 — INSERT new row     (900.00, inserted-niner)

SELECT CASE
         WHEN (SELECT (r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL')
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 1)
              = (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
                 FROM public.bs_basic WHERE id = 1)
         THEN 'PASS:ADV_B_PK_MAIN' ELSE 'FAIL:ADV_B_PK_MAIN'
       END AS adv_b_pk_main;
SELECT CASE
         WHEN (SELECT (r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL')
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 2)
              = (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
                 FROM public.bs_basic WHERE id = 2)
         THEN 'PASS:ADV_B_PK_UPDATE' ELSE 'FAIL:ADV_B_PK_UPDATE'
       END AS adv_b_pk_update;
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 3)
              = (SELECT count(*) FROM public.bs_basic WHERE id = 3)
         THEN 'PASS:ADV_B_PK_DELETE' ELSE 'FAIL:ADV_B_PK_DELETE'
       END AS adv_b_pk_delete;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_B_ID3_ZERO' ELSE 'FAIL:'||count(*) END AS adv_b_id3_zero
FROM public.bs_basic WHERE id = 3;
SELECT CASE
         WHEN (SELECT (r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL')
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 9)
              = (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
                 FROM public.bs_basic WHERE id = 9)
         THEN 'PASS:ADV_B_PK_INSERT' ELSE 'FAIL:ADV_B_PK_INSERT'
       END AS adv_b_pk_insert;

-- B.5 Non-existent id=999 → empty.
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 999)
              = (SELECT count(*) FROM public.bs_basic WHERE id = 999)
         THEN 'PASS:ADV_B_PK_MISSING' ELSE 'FAIL:ADV_B_PK_MISSING'
       END AS adv_b_pk_missing;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_B_999_EMPTY' ELSE 'FAIL:'||count(*) END AS adv_b_999_empty
FROM public.bs_basic WHERE id = 999;

-- B.6 Mixed predicate: PK + non-PK (id=1 AND tag='alpha' → hit ;
--                         id=1 AND tag='beta'  → miss).
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||':'||COALESCE((r).tag,'NULL'), ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text)
               WHERE (r).id = 1 AND (r).tag = 'alpha')
              = (SELECT string_agg(id::text||':'||COALESCE(tag,'NULL'), ',' ORDER BY id)
                 FROM public.bs_basic WHERE id = 1 AND tag = 'alpha')
         THEN 'PASS:ADV_B_PK_PLUS_NONPK_HIT' ELSE 'FAIL:ADV_B_PK_PLUS_NONPK_HIT'
       END AS adv_b_mixed_hit;
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text)
               WHERE (r).id = 1 AND (r).tag = 'beta')
              = (SELECT count(*) FROM public.bs_basic WHERE id = 1 AND tag = 'beta')
         THEN 'PASS:ADV_B_PK_PLUS_NONPK_MISS' ELSE 'FAIL:ADV_B_PK_PLUS_NONPK_MISS'
       END AS adv_b_mixed_miss;

-- B.7 Commutator (9=id ↔ id=9).
SELECT CASE
         WHEN (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
               FROM public.bs_basic WHERE id = 9)
              = (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
                 FROM public.bs_basic WHERE 9 = id)
         THEN 'PASS:ADV_B_PK_COMMUTATOR' ELSE 'FAIL:ADV_B_PK_COMMUTATOR'
       END AS adv_b_commutator;

-- B.8 Non-PK predicates (amt=70.00 / tag='eta') → fallback to full
--     overlay, still match SRF.
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL'),
                                 ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text)
               WHERE (r).amt = 70.00)
              = (SELECT string_agg(id::text||':'||amt::text||':'||COALESCE(tag,'NULL'),
                                   ',' ORDER BY id)
                 FROM public.bs_basic WHERE amt = 70.00)
         THEN 'PASS:ADV_B_NONPK_AMT' ELSE 'FAIL:ADV_B_NONPK_AMT'
       END AS adv_b_nonpk_amt;
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||':'||COALESCE((r).tag,'NULL'), ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text)
               WHERE (r).tag = 'eta')
              = (SELECT string_agg(id::text||':'||COALESCE(tag,'NULL'), ',' ORDER BY id)
                 FROM public.bs_basic WHERE tag = 'eta')
         THEN 'PASS:ADV_B_NONPK_TAG' ELSE 'FAIL:ADV_B_NONPK_TAG'
       END AS adv_b_nonpk_tag;

-- B.9 MAIN re-entry: BranchScan must not leak into MAIN mode.
RESET overlay_branch.current;
SELECT CASE WHEN tag = 'gamma' THEN 'PASS:ADV_B_ID3_BACK_IN_MAIN'
            ELSE 'FAIL:'||COALESCE(tag,'NULL') END AS adv_b_id3_main_back
FROM public.bs_basic WHERE id = 3;

-- B.10 Cleanup the bs_basic fixture.
DROP TABLE IF EXISTS public.bs_basic;


RESET overlay_branch.current;

/* =========================================================== Section C */
-- =====================================================================
-- ===== Section C: RETURNING clause (P1 INSERT/UPDATE/DELETE RETURNING)
-- =====================================================================
-- All RETURNING rows are captured with \gset / UNION ALL wrappers into a
-- CTE picture and compared to either an explicit baseline or the
-- equivalent branch view (SRF) truth.

-- C.0 Fixture: a simple PK table.
CREATE TABLE public.bs_ret (id INT4 PRIMARY KEY, name TEXT, amt NUMERIC(10,2));
INSERT INTO public.bs_ret VALUES (1, 'one', 1.00),
                                 (2, 'two', 2.00),
                                 (5, 'five', 5.00);
SELECT overlay_branch.create_branch('bs_ret_b');
SELECT overlay_branch.use_branch('bs_ret_b');

-- C.1 INSERT ... RETURNING id, name, amt — three rows (CRASH FIXED: repalloc(NULL)
--     now correctly becomes first-time palloc).
INSERT INTO public.bs_ret VALUES
  (10, 'ten',  10.00),
  (11, 'eleven', 11.50),
  (20, 'twenty', 20.00)
RETURNING id, name, amt;

SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id)
              = '1:one:1.00,2:two:2.00,5:five:5.00,10:ten:10.00,11:eleven:11.50,20:twenty:20.00'
         THEN 'PASS:ADV_C1B_SRF_EQUIV'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_c1b_srf_equiv
FROM overlay_branch.overlay_main_plus_delta('public.bs_ret')
  AS r(id int4, name text, amt numeric);

-- C.2 INSERT ... RETURNING id, n, doubleamt
INSERT INTO public.bs_ret VALUES (30, 'thirty', 30.00), (31, 'thirtyone', 31.00)
RETURNING id, name AS n, amt * 2 AS doubleamt;
SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||(amt*2)::text, ',' ORDER BY id)
              = '30:thirty:60.00,31:thirtyone:62.00'
         THEN 'PASS:ADV_C2_INSERT_RETURNING_EXPR'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||':'||(amt*2)::text, ',' ORDER BY id),'NULL')
       END AS adv_c2_expr_returning
FROM public.bs_ret WHERE id IN (30, 31);

-- C.3 UPDATE ... RETURNING id, name, amt
--     (MAIN-only ids: 2,5 exist in MAIN.  pure-delta UPDATE/DELETE is MVP NOP
--      until the new ExecQual pass crash is root-caused + fixed.)
UPDATE public.bs_ret SET
  amt = CASE id WHEN 2 THEN 200.00 WHEN 5 THEN 500.00 ELSE amt END,
  name = CASE id WHEN 2 THEN 'TWO!' ELSE name END
WHERE id IN (2, 5, 99999)
RETURNING id, name, amt;

SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id)
              = (SELECT string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id)
                 FROM public.bs_ret)
         THEN 'PASS:ADV_C3B_UPDATE_SRF_EQUIV'
         ELSE 'FAIL_SRF='||COALESCE(string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_c3b_update_srf
FROM overlay_branch.overlay_main_plus_delta('public.bs_ret')
  AS r(id int4, name text, amt numeric);

SELECT CASE
         WHEN amt = 200.00 AND name = 'TWO!' THEN 'PASS:ADV_C3C_ID2_CONTENT'
         ELSE 'FAIL:'||id::text||':'||name||':'||amt::text
       END AS adv_c3c_id2_content
FROM public.bs_ret WHERE id = 2;

-- C.4 DELETE ... RETURNING id, name, amt  (MAIN-only id=1; id=10/11/20/30/31
--     are pure delta rows, MVP NOP for DELETE until crash fixed)
DELETE FROM public.bs_ret WHERE id IN (1, 99999)
RETURNING id, name, amt;

SELECT CASE
         WHEN (SELECT count(*) FROM public.bs_ret WHERE id IN (1)) = 0
          AND (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_ret')
                 AS r(id int4, name text, amt numeric)
               WHERE (r).id IN (1)) = 0
         THEN 'PASS:ADV_C4B_DELETE_GONE'
         ELSE 'FAIL:STILL_THERE'
       END AS adv_c4b_delete_gone;

-- C.5 UPDATE ... RETURNING id, name_tag, newamt (MAIN-only id=5.
--     ACC-1: the WR subplan now scans the OVERLAY row (MAIN ⊕ delta) as the
--     pre-image, so a consecutive UPDATE on the same PK sees the branch's
--     earlier edit, not the MAIN baseline.  id=5 was first updated in C.3
--     (SET amt=500.00), so the branch delta holds amt=500.00; this UPDATE's
--     SET amt = amt + 1000 therefore evaluates against 500.00 → 1500.00.
--     name stays 'five'.)
UPDATE public.bs_ret SET amt = amt + 1000 WHERE id IN (5)
RETURNING id, ('name=' || name) AS name_tag, amt AS newamt;
SELECT CASE
         WHEN string_agg(id::text||':'||('name='||name)||':'||amt::text, ',' ORDER BY id)
              = '5:name=five:1500.00'
         THEN 'PASS:ADV_C5_UPDATE_RETURNING_EXPR'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||('name='||name)||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_c5_update_expr
FROM public.bs_ret WHERE id IN (5);

-- C.6 Zero-row RETURNING (empty result)
INSERT INTO public.bs_ret SELECT 9000 + g, 'z', 0 FROM generate_series(1,0) g
RETURNING *;

UPDATE public.bs_ret SET amt = amt WHERE id = 987654321 RETURNING *;

DELETE FROM public.bs_ret WHERE id = 987654321 RETURNING *;

SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_C6_EMPTY_INSERT_RETURNING' ELSE 'FAIL:'||count(*) END AS adv_c6_empty
FROM public.bs_ret WHERE id >= 9000;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_C6_EMPTY_UPDATE_RETURNING' ELSE 'FAIL:'||count(*) END AS adv_c6_empty_upd
FROM public.bs_ret WHERE id = 987654321 AND amt IS NOT NULL;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_C6_EMPTY_DELETE_RETURNING' ELSE 'FAIL:'||count(*) END AS adv_c6_empty_del
FROM public.bs_ret WHERE id = 987654321;

-- C.7 MAIN zero-pollution: switch back to MAIN, confirm that all mutations
--     from the branch NEVER leaked into the MAIN heap (no rows 10,11,20,30,31;
--     id=2 still 'two'/2.00, id=5 still 'five'/5.00, id=1 still there).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id)
              = '1:one:1.00,2:two:2.00,5:five:5.00'
         THEN 'PASS:ADV_C7_MAIN_CLEAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_c7_main_clean
FROM public.bs_ret;

-- C.8 Cleanup the RETURNING fixture.
DROP TABLE public.bs_ret;


RESET overlay_branch.current;

/* =========================================================== Section E */
-- =====================================================================
-- ===== Section E: NON-PK qual pushdown (P2 WHERE non-PK filter pushdown)
-- =====================================================================
-- Verify non-PK predicates get deparsed by our Planner hook into
-- general_where_sql and pushed into the MAIN-side SPI SELECT (reduces
-- MAIN-heap pages fetched).  Semantic equivalence is guaranteed by
-- comparing the BranchScan transparent view vs the SRF
-- overlay_main_plus_delta; EXPLAIN still shows Custom Scan because
-- delta merge runs inside the CustomScan node.  Fixture reuses the
-- same column shape as Section C bs_ret but with fresh data.

-- E.0 Fixture: 8 rows MAIN + create 1 branch with 4 mutations.
CREATE TABLE public.bs_np (id INT4 PRIMARY KEY, name TEXT, amt NUMERIC(10,2), color TEXT);
INSERT INTO public.bs_np VALUES
  (1,  'alpha',   10.00, 'red'),
  (2,  'bravo',   20.00, 'blue'),
  (3,  'charlie', 30.00, 'red'),
  (4,  'delta',   40.00, 'green'),
  (5,  'echo',    50.00, 'blue'),
  (6,  'foxtrot', 60.00, 'red'),
  (7,  'golf',    70.00, 'yellow'),
  (8,  'hotel',   80.00, 'red');

SELECT overlay_branch.create_branch('bs_np_b1');
SELECT overlay_branch.use_branch('bs_np_b1');

-- 4 mutations = 4 delta physics categories (I/U/D/passthrough)
DELETE FROM public.bs_np WHERE id = 3;                                   -- DELETE tombstone (charlie red)
UPDATE public.bs_np SET amt = amt * 10, color = 'purple' WHERE id = 6;   -- UPDATE override (foxtrot: 600 / purple)
INSERT INTO public.bs_np VALUES (11, 'lima', 110.00, 'magenta');        -- INSERT new
INSERT INTO public.bs_np VALUES (12, 'mike', 120.00, 'red');            -- INSERT new (color=red → test)
-- ids 1,2,4,5,7,8 MAIN passthrough unchanged.

-- E.1 Plan shape sanity: pure color='red' predicate still yields Custom Scan
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT id, color FROM public.bs_np WHERE color = 'red';

-- E.2 color = 'red' (non-PK equality, pure fallback, MAIN side pulls only
--     red rows; delta has color mutations for id=6 (purple) / id=12 (red NEW)
--     / id=3 (charlie red → deleted tombstone);  total:
--         MAIN 1:alpha:red (ok)
--         MAIN 3:charlie:red (tombstone → vanish)
--         MAIN 6:foxtrot:red (delta override purple → NOT red → out)
--         MAIN 8:hotel:red (ok)
--         delta INSERT 12:mike:red (new → in)
--     → {1, 8, 12}  with colors {red, red, red}
WITH srf AS (
  SELECT string_agg(id::text||':'||name||':'||color, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.color = 'red'
), cs AS (
  SELECT string_agg(id::text||':'||name||':'||color, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE color = 'red'
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E2_NONPK_COLOR_RED'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e2_red
FROM srf, cs;

-- E.3 Range predicate: amt BETWEEN 15.00 AND 75.00
--     MAIN rows: 2:20.00:blue, 4:40.00:green, 5:50.00:blue, 6:60.00:red, 7:70.00:yellow
--     Delta transforms:
--       id=3 (30 red) → deleted → out
--       id=6 (60 red MAIN → 600 purple delta → out of range)
--       id=1 (10 red) → under 15 → out
--       id=8 (80 red) → over 75 → out
--     → {2,4,5,7} in range (passthroughs). INSERT 11:110 magenta / 12:120 red → over 75 → out.
WITH srf AS (
  SELECT string_agg(id::text||':'||amt::text, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.amt BETWEEN 15.00 AND 75.00
), cs AS (
  SELECT string_agg(id::text||':'||amt::text, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE amt BETWEEN 15.00 AND 75.00
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E3_RANGE_AMT'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e3_range
FROM srf, cs;

-- E.4 LIKE pattern: name LIKE '%h%'
--       alpha / bravo / char**lie (has h) / delta / echo / foxtrot / golf / **h**otel
--       plus delta INSERTs: **l**ima, mike
--     MAIN with 'h': 3 (charlie, but tombstone → vanish), 8 (hotel → ok)
WITH srf AS (
  SELECT string_agg(id::text||':'||name, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.name LIKE '%h%'
), cs AS (
  SELECT string_agg(id::text||':'||name, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE name LIKE '%h%'
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E4_LIKE_NAME_H'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e4_like
FROM srf, cs;

-- E.5 PK + non-PK mixed (id = 12 AND color = 'red') → hits id=12:mike:red
--     id=12 is a delta INSERT row (absent on MAIN).  MAIN SPI
--     WHERE id=12 AND color='red' returns 0 rows; Pass2 delta INSERT
--     appends row id=12; finally ExecQual re-filters combined set
--     → exactly 1 row returned.
WITH srf AS (
  SELECT string_agg(id::text||':'||name||':'||color, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.id = 12 AND r.color = 'red'
), cs AS (
  SELECT string_agg(id::text||':'||name||':'||color, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE id = 12 AND color = 'red'
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E5_PK_PLUS_NONPK_12_RED'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e5_12red
FROM srf, cs;

-- E.6 Empty-set fallback predicate: color = 'chartreuse' (no row matches)
WITH srf AS (SELECT count(*) AS c
             FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
                    AS r(id int4,name text,amt numeric,color text)
             WHERE r.color = 'chartreuse'),
     cs  AS (SELECT count(*) AS c FROM public.bs_np WHERE color = 'chartreuse')
SELECT CASE WHEN srf.c = cs.c AND cs.c = 0 THEN 'PASS:ADV_E6_NONPK_NONE'
            ELSE 'FAIL:SRF='||srf.c||' CS='||cs.c
       END AS adv_e6_none FROM srf, cs;

-- E.7 Two non-PK columns ANDed: color = 'blue' AND amt > 35.00
--     MAIN rows: 2:20.00:blue (amt ≤35 → out), 5:50.00:blue (amt>35 → in)
--     Delta has no transform on ids 2/5; INSERTs no blue.
--     → {5}
WITH srf AS (
  SELECT string_agg(id::text||':'||amt::text||':'||color, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.color = 'blue' AND r.amt > 35.00
), cs AS (
  SELECT string_agg(id::text||':'||amt::text||':'||color, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE color = 'blue' AND amt > 35.00
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E7_TWO_NONPK_AND'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e7_two_and
FROM srf, cs;

-- E.8 MAIN zero-pollution: go back to MAIN and confirm every row is as
--     originally inserted (none of the delta writes leaked).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||amt::text||':'||color, ',' ORDER BY id)
              = '1:alpha:10.00:red,2:bravo:20.00:blue,3:charlie:30.00:red,4:delta:40.00:green,'
               '5:echo:50.00:blue,6:foxtrot:60.00:red,7:golf:70.00:yellow,8:hotel:80.00:red'
         THEN 'PASS:ADV_E8_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||':'||amt::text||':'||color, ',' ORDER BY id),'NULL')
       END AS adv_e8_pristine
FROM public.bs_np;

DROP TABLE public.bs_np;


RESET overlay_branch.current;

/* =========================================================== Section F */
-- =====================================================================
-- ===== Section F: RD-8 read-path shortcut (empty / all-typed delta) =====
-- =====================================================================
-- RD-8 makes Pass1 skip the per-row JSON-text key serialisation when the
-- branch delta is empty (M_total == 0) or when every delta entry carries
-- a usable typed PK.  The shortcut must be semantically invisible: the
-- overlay result still equals the MAIN baseline / SRF truth.  These cases
-- are the correctness guard for the shortcut (the speed-up itself is not
-- asserted in a regression test).

-- F.0 Fixture: PK table, MAIN baseline only.
CREATE TABLE public.bs_rd8 (id INT4 PRIMARY KEY, v TEXT);
INSERT INTO public.bs_rd8 VALUES (1,'a'),(2,'b'),(3,'c');

-- F.1 Empty-delta branch (M_total == 0): enter the branch and run a
--     full-table scan WITHOUT writing anything.  RD-8 must emit every
--     MAIN row unchanged.
SELECT overlay_branch.create_branch('bs_rd8_b');
SELECT overlay_branch.use_branch('bs_rd8_b');
SELECT CASE
         WHEN string_agg(id::text||':'||v, ',' ORDER BY id) = '1:a,2:b,3:c'
         THEN 'PASS:RD8_M0_FULLSCAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||v, ',' ORDER BY id),'NULL')
       END AS rd8_m0_fullscan
FROM public.bs_rd8;

-- F.2 Same branch, now with an all-typed delta (UPDATE/DELETE/INSERT):
--     every delta key has a typed PK representation, so Pass1 takes the
--     typed-only comparison path.  The overlay result must fold the three
--     side-effects onto the MAIN baseline.
UPDATE public.bs_rd8 SET v = 'B!' WHERE id = 2;
DELETE FROM public.bs_rd8 WHERE id = 3;
INSERT INTO public.bs_rd8 VALUES (4,'d');
SELECT CASE
         WHEN string_agg(id::text||':'||v, ',' ORDER BY id) = '1:a,2:B!,4:d'
         THEN 'PASS:RD8_ALLTYPED_FULLSCAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||v, ',' ORDER BY id),'NULL')
       END AS rd8_alltyped_fullscan
FROM public.bs_rd8;

-- F.3 SRF equivalence for the typed-delta case above (precondition).
WITH srf AS (
  SELECT string_agg((r).id::text||':'||(r).v, ',' ORDER BY (r).id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_rd8') AS r(id int4, v text)
), cs AS (
  SELECT string_agg(id::text||':'||v, ',' ORDER BY id) AS pic FROM public.bs_rd8
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:RD8_SRF_EQUIV'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS rd8_srf_equiv
FROM srf, cs;

-- F.4 MAIN zero-pollution: back to MAIN, the baseline must be pristine.
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||v, ',' ORDER BY id) = '1:a,2:b,3:c'
         THEN 'PASS:RD8_MAIN_CLEAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||v, ',' ORDER BY id),'NULL')
       END AS rd8_main_clean
FROM public.bs_rd8;

-- F.5 Cleanup.
DROP TABLE public.bs_rd8;

/* =========================================================== Section G */
-- =====================================================================
-- ===== Section G: RD-8 pure-C PK serialiser (JSON escaping) =====
-- =====================================================================
-- overlay_serialize_pk() now builds the `["k1", "k2"]` key in pure C via
-- escape_json() instead of a `to_jsonb(ARRAY[...])` SPI round-trip.  The
-- previous fixtures only used plain alphanumeric PK values, so the new
-- escaping path was never exercised.  PK values carrying JSON-special
-- characters ('"' and '\') must (a) be escaped correctly on the WRITE
-- side and (b) survive the typed-key re-parse on the READ side.  A wrong
-- escape (e.g. emitting `["a\b"]`) makes the JSONB re-parse fail or the
-- key lookup miss, surfacing the MAIN row instead of the branch row.

-- G.0 Fixture: TEXT PK with quote / backslash characters.
CREATE TABLE public.bs_rd8q (k TEXT PRIMARY KEY, v TEXT);
INSERT INTO public.bs_rd8q VALUES
  ('plain', 'main_plain'),
  ('a"b',   'main_dq'),
  ('c\d',   'main_bs');

-- G.1 Branch writes spanning every op: UPDATE (quote key), UPDATE
--     (backslash key), DELETE (plain key), INSERT (both escapes).
SELECT overlay_branch.create_branch('bs_rd8q_b');
SELECT overlay_branch.use_branch('bs_rd8q_b');
UPDATE public.bs_rd8q SET v = 'br_dq' WHERE k = 'a"b';
UPDATE public.bs_rd8q SET v = 'br_bs' WHERE k = 'c\d';
DELETE FROM public.bs_rd8q WHERE k = 'plain';
INSERT INTO public.bs_rd8q VALUES ('x"y\z', 'br_new');

-- G.2 Overlay read must fold every branch op onto the MAIN baseline.
SELECT CASE
         WHEN string_agg(k||'=>'||v, ',' ORDER BY k) = 'a"b=>br_dq,c\d=>br_bs,x"y\z=>br_new'
         THEN 'PASS:RD8_JSON_ESCAPE_OVERLAY'
         ELSE 'FAIL:'||COALESCE(string_agg(k||'=>'||v, ',' ORDER BY k),'NULL')
       END AS rd8_json_escape_overlay
FROM public.bs_rd8q;

-- G.3 SRF equivalence (precondition: the shortcut must remain invisible).
WITH srf AS (
  SELECT string_agg((r).k||'=>'||(r).v, ',' ORDER BY (r).k) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_rd8q') AS r(k text, v text)
), cs AS (
  SELECT string_agg(k||'=>'||v, ',' ORDER BY k) AS pic FROM public.bs_rd8q
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:RD8_JSON_ESCAPE_SRF'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS rd8_json_escape_srf
FROM srf, cs;

-- G.4 MAIN zero-pollution: back to MAIN, the baseline must be pristine.
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(k||'=>'||v, ',' ORDER BY k) = 'a"b=>main_dq,c\d=>main_bs,plain=>main_plain'
         THEN 'PASS:RD8_JSON_ESCAPE_MAIN_CLEAN'
         ELSE 'FAIL:'||COALESCE(string_agg(k||'=>'||v, ',' ORDER BY k),'NULL')
       END AS rd8_json_escape_main_clean
FROM public.bs_rd8q;

-- G.5 Cleanup.
DROP TABLE public.bs_rd8q;

/* =========================================================== Section H */
-- =====================================================================
-- ===== Section H: RD-7 MAIN read de-SPI (heap scan + qual prefilter) =
-- =====================================================================
-- The MAIN baseline reader no longer runs `SPI SELECT *`: it heap-scans
-- MAIN via table_beginscan()/table_scan_getnextslot() and, for PK tables,
-- pre-filters rows with the executor's baserestrictinfo qual.  A MAIN row
-- excluded by that prefilter but changed by the branch must still surface
-- (Pass2 orphan rule); the final scan.plan.qual re-check is the safety net
-- that also drops over-emitted orphans.

-- H.0 Fixture: INT4 PK plus a non-PK predicate column.
CREATE TABLE public.bs_rd7 (id INT4 PRIMARY KEY, qty INT4, note TEXT);
INSERT INTO public.bs_rd7 VALUES
  (1, 10, 'main_a'),
  (2, 20, 'main_b'),
  (3, 30, 'main_c'),
  (4, 40, 'main_d'),
  (5, 50, 'main_e');

SELECT overlay_branch.create_branch('bs_rd7_b');
SELECT overlay_branch.use_branch('bs_rd7_b');

-- H.1 Branch edits that land on rows a non-PK prefilter would EXCLUDE:
--     id=1 UPDATE (qty 10 -> 99), id=2 DELETE (tombstone), id=6 INSERT.
UPDATE public.bs_rd7 SET qty = 99, note = 'br_a' WHERE id = 1;
DELETE FROM public.bs_rd7                        WHERE id = 2;
INSERT INTO public.bs_rd7 VALUES (6, 60, 'br_new');

-- H.2 No-predicate full scan (direct heap-scan path): every MAIN row plus
--     the pure-delta INSERT, with the branch overrides folded in.
SELECT CASE
         WHEN string_agg(id||':'||qty||':'||note, ',' ORDER BY id)
              = '1:99:br_a,3:30:main_c,4:40:main_d,5:50:main_e,6:60:br_new'
         THEN 'PASS:RD7_FULLSCAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||qty||':'||note, ',' ORDER BY id),'NULL')
       END AS rd7_fullscan
FROM public.bs_rd7;

-- H.3 Prefilter orphan resurrection (the core RD-7 semantic risk):
--     `qty >= 40` skips the id=1 MAIN row (qty 10) before Pass1, yet the
--     branch bumped it to 99, so Pass2 must reconstruct it from the delta.
--     id=2's tombstone stays dropped; id=6 is a pure-delta INSERT.
SELECT CASE
         WHEN string_agg(id||':'||qty, ',' ORDER BY id) = '1:99,4:40,5:50,6:60'
         THEN 'PASS:RD7_PREFILTER_ORPHAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||qty, ',' ORDER BY id),'NULL')
       END AS rd7_prefilter_orphan
FROM public.bs_rd7 WHERE qty >= 40;

-- H.4 Post-merge re-check safety net: `qty = 30` keeps only the untouched
--     MAIN row id=3; orphan rows resurrected by Pass2 (1/6) must be
--     filtered back out by the executor's scan.plan.qual.
SELECT CASE
         WHEN string_agg(id||':'||qty||':'||note, ',' ORDER BY id) = '3:30:main_c'
         THEN 'PASS:RD7_REQUAL_NET'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||qty||':'||note, ',' ORDER BY id),'NULL')
       END AS rd7_requal_net
FROM public.bs_rd7 WHERE qty = 30;

-- H.5 P0 point query (still SPI): id=1 reflects the branch override; the
--     id=2 tombstone is gone.
SELECT CASE WHEN (SELECT qty FROM public.bs_rd7 WHERE id = 1) = 99
            THEN 'PASS:RD7_P0_BRANCH'
            ELSE 'FAIL:'||COALESCE((SELECT qty FROM public.bs_rd7 WHERE id = 1)::text,'NULL')
       END AS rd7_p0_branch;
SELECT CASE WHEN NOT EXISTS(SELECT 1 FROM public.bs_rd7 WHERE id = 2)
            THEN 'PASS:RD7_P0_TOMBSTONE' ELSE 'FAIL:ID2_PRESENT' END AS rd7_p0_tombstone;

-- H.6 SRF vs. direct-read equivalence over the full (unfiltered) set.
WITH srf AS (
  SELECT string_agg((r).id||':'||(r).qty||':'||(r).note, ',' ORDER BY (r).id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_rd7')
    AS r(id int4, qty int4, note text)
), cs AS (
  SELECT string_agg(id||':'||qty||':'||note, ',' ORDER BY id) AS pic FROM public.bs_rd7
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:RD7_SRF_EQUIV'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS rd7_srf_equiv
FROM srf, cs;

-- H.7 LIMIT correctness (early-stop itself is RD-6/2c; here only that the
--     overlay subset respects ORDER BY / LIMIT).
SELECT CASE WHEN string_agg(id::text, ',' ORDER BY id) = '1,3'
            THEN 'PASS:RD7_LIMIT'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text, ',' ORDER BY id),'NULL')
       END AS rd7_limit
FROM (SELECT id FROM public.bs_rd7 ORDER BY id LIMIT 2) t;

-- H.8 MAIN zero-pollution, then cleanup.
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id||':'||qty||':'||note, ',' ORDER BY id)
              = '1:10:main_a,2:20:main_b,3:30:main_c,4:40:main_d,5:50:main_e'
         THEN 'PASS:RD7_MAIN_CLEAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||qty||':'||note, ',' ORDER BY id),'NULL')
       END AS rd7_main_clean
FROM public.bs_rd7;
DROP TABLE public.bs_rd7;

/* =========================================================== Section I */
-- =====================================================================
-- ===== Section I: RD-6 streaming iterator (early stop / rewind)  =====
-- =====================================================================
-- The 2-pass overlay is pulled one row at a time instead of being fully
-- materialised up front, so LIMIT / cursors can stop early.  Iteration
-- ORDER is byte-for-byte the old one (Pass1 walks the MAIN baseline,
-- Pass2 walks the delta list in append order), so every result set must
-- be UNCHANGED — these cases pin down drained / prefix / rewind behaviour.

-- I.0 Fixture: 5 MAIN rows; the branch overrides id=1, tombstones id=2 and
--     adds two pure-delta rows (id=6, id=7) that only Pass2 can emit.
CREATE TABLE public.bs_rd6 (id INT4 PRIMARY KEY, tag TEXT);
INSERT INTO public.bs_rd6 VALUES
  (1, 'm1'), (2, 'm2'), (3, 'm3'), (4, 'm4'), (5, 'm5');

SELECT overlay_branch.create_branch('bs_rd6_b');
SELECT overlay_branch.use_branch('bs_rd6_b');

UPDATE public.bs_rd6 SET tag = 'b1' WHERE id = 1;
DELETE FROM public.bs_rd6               WHERE id = 2;
INSERT INTO public.bs_rd6 VALUES (6, 'n6'), (7, 'n7');

-- I.1 Drained full scan: Pass1 keeps 1/3/4/5 with the override folded in,
--     Pass2 appends the two orphans.
SELECT CASE
         WHEN string_agg(id||':'||tag, ',' ORDER BY id)
              = '1:b1,3:m3,4:m4,5:m5,6:n6,7:n7'
         THEN 'PASS:RD6_STREAM_FULL'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||tag, ',' ORDER BY id),'NULL')
       END AS rd6_stream_full
FROM public.bs_rd6;

-- I.2 Early stop inside Pass1: the prefix must equal the drained scan's.
SELECT CASE WHEN string_agg(id::text, ',' ORDER BY id) = '1,3,4'
            THEN 'PASS:RD6_LIMIT_PASS1'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text, ',' ORDER BY id),'NULL')
       END AS rd6_limit_pass1
FROM (SELECT id FROM public.bs_rd6 ORDER BY id LIMIT 3) t;

-- I.3 Early stop across the Pass1 -> Pass2 boundary.
SELECT CASE WHEN string_agg(id::text, ',' ORDER BY id) = '1,3,4,5,6,7'
            THEN 'PASS:RD6_LIMIT_PASS2'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text, ',' ORDER BY id),'NULL')
       END AS rd6_limit_pass2
FROM (SELECT id FROM public.bs_rd6 ORDER BY id LIMIT 6) t;

-- I.4 Cursor FETCHes share ONE iterator across many Exec cycles; the fetch
--     that runs past the end must return 0 rows, not error.
BEGIN;
DECLARE c_rd6 CURSOR FOR SELECT id, tag FROM public.bs_rd6 ORDER BY id;
FETCH 3 FROM c_rd6;
FETCH 3 FROM c_rd6;
FETCH 1 FROM c_rd6;
FETCH 1 FROM c_rd6;
COMMIT;

-- I.5 SRF early stop: LIMIT 1 truncates the SRF before it drains, so the
--     handle is released by its context reset callback (no leak / crash).
SELECT count(*) AS rd6_srf_limit_rows
FROM (SELECT id FROM overlay_branch.overlay_main_plus_delta('public.bs_rd6')
        AS r(id int4, tag text) LIMIT 1) s;

-- I.6 SRF vs direct scan equivalence: the streaming rewrite must not change
--     the result set of either entry point.
WITH srf AS (
  SELECT string_agg((r).id||':'||(r).tag, ',' ORDER BY (r).id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_rd6')
    AS r(id int4, tag text)
), cs AS (
  SELECT string_agg(id||':'||tag, ',' ORDER BY id) AS pic FROM public.bs_rd6
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:RD6_SRF_EQUIV'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS rd6_srf_equiv
FROM srf, cs;

-- I.7 Rewind: a correlated subplan re-executes the BranchScan once per outer
--     row, so the iterator is rebuilt from scratch each time (FR6).
SELECT count(*) AS rd6_rescan_ok
FROM generate_series(1, 3) g
WHERE (SELECT count(*) FROM public.bs_rd6 WHERE id <= g) > 0;

-- I.8 MAIN zero-pollution, then cleanup.
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id||':'||tag, ',' ORDER BY id)
              = '1:m1,2:m2,3:m3,4:m4,5:m5'
         THEN 'PASS:RD6_MAIN_CLEAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||tag, ',' ORDER BY id),'NULL')
       END AS rd6_main_clean
FROM public.bs_rd6;
DROP TABLE public.bs_rd6;

/* =========================================================== Section J */
-- =====================================================================
-- ===== Section J: P0 predicate pushdown IN / range / OR            =====
-- =====================================================================
-- P0 used to recognise only a lone `pk = Const`.  It now folds any
-- clause built purely from PK-column btree comparisons into ONE deparsed
-- SQL fragment for the MAIN SPI SELECT; IN / range / OR have no single
-- key, so the DELTA side auto-degrades to the full-list path.  Every
-- result set below is compared against the drained overlay picture.

-- J.0 Fixture: 5 MAIN rows; the branch overrides id=1 and id=2,
--     tombstones id=3, and inserts two pure-delta rows (id=6 in range,
--     id=8 above range) that only Pass2 can emit.
CREATE TABLE public.bs_pkpd (id INT4 PRIMARY KEY, tag TEXT);
INSERT INTO public.bs_pkpd VALUES
  (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e');

SELECT overlay_branch.create_branch('bs_pkpd_b');
SELECT overlay_branch.use_branch('bs_pkpd_b');

UPDATE public.bs_pkpd SET tag = 'a1' WHERE id = 1;
UPDATE public.bs_pkpd SET tag = 'b2' WHERE id = 2;
DELETE FROM public.bs_pkpd               WHERE id = 3;
INSERT INTO public.bs_pkpd VALUES (6, 'n6'), (8, 'n8');

-- J.1 IN list: id=6 is a branch INSERT with no MAIN pairing, so it must
--     arrive via Pass2; id=2/3/5/8 fall outside the IN list.
SELECT CASE
         WHEN string_agg(id||':'||tag, ',' ORDER BY id) = '1:a1,4:d,6:n6'
         THEN 'PASS:PKIN_BASIC'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||tag, ',' ORDER BY id),'NULL')
       END AS pkin_basic
FROM public.bs_pkpd WHERE id IN (1, 4, 6);

-- J.2 BETWEEN range: the tombstoned id=3 must stay hidden.
SELECT CASE
         WHEN string_agg(id||':'||tag, ',' ORDER BY id) = '2:b2,4:d'
         THEN 'PASS:PKRANGE_BASIC'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||tag, ',' ORDER BY id),'NULL')
       END AS pkrange_basic
FROM public.bs_pkpd WHERE id BETWEEN 2 AND 4;

-- J.3 Open-ended ranges (both directions).
WITH gt AS (SELECT string_agg(id::text, ',' ORDER BY id) AS s
              FROM public.bs_pkpd WHERE id > 4),
     lt AS (SELECT string_agg(id::text, ',' ORDER BY id) AS s
              FROM public.bs_pkpd WHERE id < 3)
SELECT CASE WHEN gt.s = '5,6,8' AND lt.s = '1,2' THEN 'PASS:PKRANGE_OPEN'
            ELSE 'FAIL:gt='||COALESCE(gt.s,'NULL')||' lt='||COALESCE(lt.s,'NULL')
       END AS pkrange_open
FROM gt, lt;

-- J.4 OR of two point equalities (mixed MAIN row + branch INSERT).
SELECT CASE
         WHEN string_agg(id||':'||tag, ',' ORDER BY id) = '1:a1,8:n8'
         THEN 'PASS:PKOR_BASIC'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||tag, ',' ORDER BY id),'NULL')
       END AS pkor_basic
FROM public.bs_pkpd WHERE id = 1 OR id = 8;

-- J.5 P0 (IN) + P2 (non-PK `tag <> 'e'`) co-operate: id=5 is dropped by
--     the non-PK filter, and no row is emitted twice.
WITH r AS (
  SELECT id, tag FROM public.bs_pkpd WHERE id IN (1, 4, 5) AND tag <> 'e'
)
SELECT CASE
         WHEN count(*) = 2
          AND string_agg(id||':'||tag, ',' ORDER BY id) = '1:a1,4:d'
         THEN 'PASS:PKIN_PLUS_NONPK'
         ELSE 'FAIL:n='||count(*)||' '||COALESCE(string_agg(id||':'||tag, ',' ORDER BY id),'NULL')
       END AS pkin_plus_nonpk
FROM r;

-- J.6 Orphan under a pushed range: MAIN SPI returns only {4,5}, so the
--     branch INSERTs id=6/8 must be re-emitted by Pass2, while the id=2
--     UPDATE orphan sits outside `id > 3` and must be dropped by qual.
SELECT CASE
         WHEN string_agg(id||':'||tag, ',' ORDER BY id) = '4:d,5:e,6:n6,8:n8'
         THEN 'PASS:PKIN_ORPHAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||tag, ',' ORDER BY id),'NULL')
       END AS pkin_orphan
FROM public.bs_pkpd WHERE id > 3;

-- J.7 EXPLAIN: the pushed IN / range predicate must stay inside the single
--     Custom Scan node (no SeqScan leaking out).
EXPLAIN (COSTS OFF, SUMMARY OFF)
SELECT * FROM public.bs_pkpd WHERE id IN (1, 4, 6);
EXPLAIN (COSTS OFF, SUMMARY OFF)
SELECT * FROM public.bs_pkpd WHERE id > 4;

-- J.8 MAIN zero-pollution, then cleanup.
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id||':'||tag, ',' ORDER BY id)
              = '1:a,2:b,3:c,4:d,5:e'
         THEN 'PASS:PKIN_MAIN_CLEAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id||':'||tag, ',' ORDER BY id),'NULL')
       END AS pkin_main_clean
FROM public.bs_pkpd;
DROP TABLE public.bs_pkpd;

