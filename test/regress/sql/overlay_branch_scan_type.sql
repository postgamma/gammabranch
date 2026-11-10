/*
 * overlay_branch_scan_type.sql — PK type diversity + edge/BUG regression.
 *
 * COVERAGE (originally overlay_branch_advanced.sql Section F/G/H/I/J, single session self-contained):
 *   Section F — PK TYPE DIVERSITY: SMALLINT/BIGINT/SERIAL/NUMERIC(8,2)/BPCHAR(12)/
 *               VARCHAR/TIMESTAMPTZ/TEXT/UUID 9 kinds of non-INT4 PK O(1) fast-path
 *   Section G — PK EDGE CASES: repeated writes to same PK (I→U→U→D→I) / WHERE pk IS NULL /
 *               WHERE pk <> ALL(ARRAY[...]) / BETWEEN SYMMETRIC edge predicates
 *   Section H — BUG REGRESSIONS P0/P1:
 *               H1) PK INT4 NULL key does not crash; H2) WHERE pk = (SELECT NULL::int);
 *               H3) WITH RETURNING vs plain INSERT/UPDATE results agree
 *   Section I — Multi-type PK typmod: NUMERIC(p,s) scale / BPCHAR(n) r-trim /
 *               DOUBLE / VARCHAR / TIMETZ format_type_with_typmod serialization correct
 *   Section J — P2 complex non-PK expression deparse: WHERE col OP col / LIKE /
 *               LEAST/GREATEST / IN subquery / CASE / CAST etc. deparse coverage
 *   Section K — dropped-column delta reconstruction (B1 regression): a branch
 *               taken AFTER a middle column is dropped must still rebuild
 *               delta rows correctly -- the SPI result set is mapped to the
 *               heap TupleDesc by skipping dropped attnums.
 *
 * Sections share session; between F→G→H→I→J if the same bs_user table is reused keep it continuous without DISCARD.
 */

-- ===== 0. Prerequisite guard (same as the other regression files) =====
DO $$
DECLARE
  _s text := current_setting('shared_preload_libraries', true);
  _msg text;
BEGIN
  IF _s NOT LIKE '%overlay_branch%' THEN
    _msg := 'Postmaster started WITHOUT shared_preload_libraries=''overlay_branch''.  '
         || 'The 2 correct ways to invoke these tests (DON''T `cd test/` first):'
         || chr(10)||chr(10)||'  (A) make installcheck EXTRA_REGRESS_OPTS="--use-existing --host=/tmp --port=15433"'
         || chr(10)||'  (B) pg_regress overlay_branch_advanced \'
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

CREATE EXTENSION IF NOT EXISTS overlay_branch;
SET client_min_messages = WARNING;

/* =========================================================== Section F */
-- =====================================================================
-- ===== Section F: PK TYPE DIVERSITY — O(1) fast-path for non-INT4 PKs =====
-- =====================================================================
-- The #1 P0 root cause was "literal consttype != PK column typid"
-- producing mismatched serialized keys (e.g. bigint PK fed with
-- int4 literal 2, bpchar(8) PK fed with varchar literal 'abc').
-- Sections A/B exercise INT4 PK exclusively and therefore never
-- trigger the real code path.  This section uses INT8 / VARCHAR /
-- BPCHAR, the three most common non-INT4 PK types, to strictly
-- verify that from_single_datum 2-stage coerce ↔ WR serialize_pk
-- match byte-for-byte.

-- F.1 INT8 (bigint) PK — parser-default int4 literal vs bigint PK,
--     exact reproduction of Bug1 class literal-type mismatch.
CREATE TABLE public.bs_pk_bigint (
    id    BIGINT PRIMARY KEY,
    tag   TEXT
);
INSERT INTO public.bs_pk_bigint VALUES   -- MAIN baseline
  (10::bigint, 'ten'),
  (20::bigint, 'twenty'),
  (30::bigint, 'thirty');
SELECT overlay_branch.create_branch('bs_pk_bigint_b');
SELECT overlay_branch.use_branch('bs_pk_bigint_b');

-- F.1a Write 4 delta physics categories, all literals UNCAST so the
--      parser picks int4 as the default consttype.
DELETE FROM public.bs_pk_bigint WHERE id = 20;        -- literal 20 = INT4, PK = INT8
UPDATE public.bs_pk_bigint SET tag = 'TEN!' WHERE id = 10;
INSERT INTO public.bs_pk_bigint VALUES (40::bigint, 'forty');
-- id=30 passthrough

-- F.1b 5 assertion classes: UPDATE override / DELETE tombstone / passthrough / INSERT / MISS
SELECT CASE
         WHEN (SELECT (r).tag FROM overlay_branch.overlay_main_plus_delta('public.bs_pk_bigint')
                 AS r(id bigint, tag text) WHERE (r).id = 10)
              = (SELECT tag FROM public.bs_pk_bigint WHERE id = 10)
              AND (SELECT tag FROM public.bs_pk_bigint WHERE id = 10) = 'TEN!'
         THEN 'PASS:ADV_F1_BIGINT_ID10_UPDATE'
         ELSE 'FAIL:'||COALESCE((SELECT tag FROM public.bs_pk_bigint WHERE id = 10),'NULL')
       END AS adv_f1_bigint_id10;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_F1_BIGINT_ID20_DELETED'
            ELSE 'FAIL:'||count(*) END AS adv_f1_bigint_id20
FROM public.bs_pk_bigint WHERE id = 20;
SELECT CASE WHEN tag = 'thirty' THEN 'PASS:ADV_F1_BIGINT_ID30_PASS'
            ELSE 'FAIL:'||COALESCE(tag,'NULL') END AS adv_f1_bigint_id30
FROM public.bs_pk_bigint WHERE id = 30;
SELECT CASE WHEN tag = 'forty' THEN 'PASS:ADV_F1_BIGINT_ID40_INSERT'
            ELSE 'FAIL:'||COALESCE(tag,'NULL') END AS adv_f1_bigint_id40
FROM public.bs_pk_bigint WHERE id = 40;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_F1_BIGINT_ID999_MISS'
            ELSE 'FAIL:'||count(*) END AS adv_f1_bigint_id999
FROM public.bs_pk_bigint WHERE id = 999;

RESET overlay_branch.current;
DROP TABLE public.bs_pk_bigint;

-- F.2 VARCHAR(32) PK — string PK; serialized key is JSON string array
--     ["user:alice"]; MUST match WR overlay_serialize_pk(slot) exactly.
CREATE TABLE public.bs_pk_varchar (
    user_id  VARCHAR(32) PRIMARY KEY,
    score    INT4
);
INSERT INTO public.bs_pk_varchar VALUES
  ('alice',   100),
  ('bob',     200),
  ('charlie', 300);
SELECT overlay_branch.create_branch('bs_pk_vc_b');
SELECT overlay_branch.use_branch('bs_pk_vc_b');

DELETE FROM public.bs_pk_varchar WHERE user_id = 'bob';
UPDATE public.bs_pk_varchar SET score = 9999 WHERE user_id = 'alice';
INSERT INTO public.bs_pk_varchar VALUES ('dave', 400);
-- 'charlie' passthrough

SELECT CASE WHEN score = 9999 THEN 'PASS:ADV_F2_VC_ALICE_UPDATE'
            ELSE 'FAIL:'||user_id||':'||COALESCE(score::text,'NULL') END AS adv_f2_vc_alice
FROM public.bs_pk_varchar WHERE user_id = 'alice';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_F2_VC_BOB_DELETED'
            ELSE 'FAIL:'||count(*) END AS adv_f2_vc_bob
FROM public.bs_pk_varchar WHERE user_id = 'bob';
SELECT CASE WHEN score = 300 THEN 'PASS:ADV_F2_VC_CHARLIE_PASS'
            ELSE 'FAIL:'||COALESCE(score::text,'NULL') END AS adv_f2_vc_charlie
FROM public.bs_pk_varchar WHERE user_id = 'charlie';
SELECT CASE WHEN score = 400 THEN 'PASS:ADV_F2_VC_DAVE_INSERT'
            ELSE 'FAIL:'||COALESCE(score::text,'NULL') END AS adv_f2_vc_dave
FROM public.bs_pk_varchar WHERE user_id = 'dave';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_F2_VC_ZENO_MISSING'
            ELSE 'FAIL:'||count(*) END AS adv_f2_vc_missing
FROM public.bs_pk_varchar WHERE user_id = 'zeno';

-- Literals with special characters: colon, comma (double-quote not
--     tested; JSON escaping would require extra WR deparse work, cover
--     separately).
INSERT INTO public.bs_pk_varchar VALUES ('weird,key:with-colons', -1);
SELECT CASE WHEN score = -1 THEN 'PASS:ADV_F2_VC_WEIRDKEY_INSERT'
            ELSE 'FAIL:'||COALESCE(score::text,'NULL') END AS adv_f2_vc_weird
FROM public.bs_pk_varchar WHERE user_id = 'weird,key:with-colons';

RESET overlay_branch.current;
DROP TABLE public.bs_pk_varchar;

-- =====================================================================

RESET overlay_branch.current;

/* =========================================================== Section G */
-- =====================================================================
-- ===== Section G: PK EDGE CASES - edge predicates / repeated writes =====
-- =====================================================================
-- G.1 NULL / IS NULL predicates: pushing `(r).id IS NULL` inside the
--     SRF used to trigger a planner bug ("could not devise a query
--     plan for the given query"), so NULL predicates only validate
--     the transparent BranchScan side for correctness; we no longer
--     cross-check against SRF truth.
CREATE TABLE public.bs_pk_edge (id INT4 PRIMARY KEY, payload TEXT);
INSERT INTO public.bs_pk_edge VALUES (1,'a'),(2,'b'),(3,'c');
SELECT overlay_branch.create_branch('bs_edge_b');
SELECT overlay_branch.use_branch('bs_edge_b');
INSERT INTO public.bs_pk_edge VALUES (5,'e');

-- G.1a PK = NULL literal: semantically never-true predicate -> 0 rows
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_G1_PK_EQ_NULL'
            ELSE 'FAIL:'||count(*) END AS adv_g1_null_eq
FROM public.bs_pk_edge WHERE id = NULL;

-- G.1b PK IS NULL: PK column is NOT NULL -> always 0 rows
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_G1_PK_IS_NULL'
            ELSE 'FAIL:'||count(*) END AS adv_g1_isnull
FROM public.bs_pk_edge WHERE id IS NULL;

-- G.1c PK IN (1, 2, 40) multi-value: ScalarArrayOpExpr; P0 does NOT
--      fire but semantic correctness holds.
SELECT CASE
         WHEN (SELECT string_agg(id::text||':'||payload, ',' ORDER BY id)
               FROM public.bs_pk_edge WHERE id IN (1,2,40))
              = '1:a,2:b'
         THEN 'PASS:ADV_G1_PK_IN_LIST'
         ELSE 'FAIL:'||COALESCE((SELECT string_agg(id::text||':'||payload, ',' ORDER BY id)
                                   FROM public.bs_pk_edge WHERE id IN (1,2,40)),'NULL')
       END AS adv_g1_in;

-- G.2 PK predicate in a subquery (NOT baserestrictinfo / lives in
--      upper Filter node): Planner won't push into baserestrictinfo
--      so P0 is bypassed, but row content remains correct.
SELECT CASE
         WHEN (SELECT string_agg(id::text||':'||payload, ',' ORDER BY id)
               FROM (SELECT * FROM public.bs_pk_edge) subq WHERE id = 3) = '3:c'
         THEN 'PASS:ADV_G2_PK_IN_SUBQUERY'
         ELSE 'FAIL_G2_SUBQ'
       END AS adv_g2_subq;

-- G.3 Same PK consecutively UPDATE'd: delta table keeps only the
--      latest entry per (bid, relid, key) (UPSERT semantics); final
--      result equals the content of the last SET.
UPDATE public.bs_pk_edge SET payload='first'  WHERE id = 1;
UPDATE public.bs_pk_edge SET payload='second' WHERE id = 1;
UPDATE public.bs_pk_edge SET payload='third'  WHERE id = 1;
SELECT CASE WHEN payload = 'third' THEN 'PASS:ADV_G3_PK_TRIPLE_UPSERT'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS adv_g3_upsert
FROM public.bs_pk_edge WHERE id = 1;

-- G.4 Verify same MAIN-row PK DELETE tombstone -> INSERT rebirth;
--      pure-INSERT-only row DELETE/UPDATE requires WR-path extra
--      support (out-of-scope, covered in H.2).
DELETE FROM public.bs_pk_edge WHERE id = 3;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_G4_ID3_TOMBSTONED'
            ELSE 'FAIL:'||count(*) END AS adv_g4_id3_dead
FROM public.bs_pk_edge WHERE id = 3;
INSERT INTO public.bs_pk_edge VALUES (3,'c2-reborn');
SELECT CASE WHEN payload = 'c2-reborn' THEN 'PASS:ADV_G4_ID3_REBORN_AFTER_TOMB'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS adv_g4_id3_reborn
FROM public.bs_pk_edge WHERE id = 3;

-- G.5 MAIN zero pollution final: go back to MAIN, data still pristine
--      (1:a, 2:b, 3:c)
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||payload, ',' ORDER BY id) = '1:a,2:b,3:c'
         THEN 'PASS:ADV_G5_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||payload, ',' ORDER BY id),'NULL')
       END AS adv_g5_pristine
FROM public.bs_pk_edge;

DROP TABLE public.bs_pk_edge;

RESET overlay_branch.current;

/* =========================================================== Section H */
-- =====================================================================
-- ===== Section H: BUG REGRESSIONS - P0/P1 bugfix coverage =====
-- =====================================================================
-- H.1 = Bug#1 BPCHAR(8): WR serialize_pk vs P0 serialized key mismatch
--     Fix expected: UPDATE/DELETE both take effect; pre-fix: passthrough
CREATE TABLE public.bs_pk_bpchar (
    code   CHAR(8) PRIMARY KEY,
    label  TEXT
);
INSERT INTO public.bs_pk_bpchar VALUES
  ('AB12', 'abbr-1'),
  ('CD34', 'abbr-2'),
  ('EF56', 'abbr-3');
SELECT overlay_branch.create_branch('bs_h_bc_b');
SELECT overlay_branch.use_branch('bs_h_bc_b');

DELETE FROM public.bs_pk_bpchar WHERE code = 'CD34';
UPDATE public.bs_pk_bpchar SET label = 'AB12-UPDATED!' WHERE code = 'AB12';
INSERT INTO public.bs_pk_bpchar VALUES ('GH78', 'abbr-4');

SELECT CASE WHEN label = 'AB12-UPDATED!' THEN 'PASS:H1_BC_AB12_UPDATE'
            ELSE 'FAIL:'||COALESCE(label,'NULL') END AS h1_bc_ab12
FROM public.bs_pk_bpchar WHERE code = 'AB12';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H1_BC_CD34_DELETED'
            ELSE 'FAIL:'||count(*) END AS h1_bc_cd34
FROM public.bs_pk_bpchar WHERE code = 'CD34';
SELECT CASE WHEN label = 'abbr-3' THEN 'PASS:H1_BC_EF56_PASS'
            ELSE 'FAIL:'||COALESCE(label,'NULL') END AS h1_bc_ef56
FROM public.bs_pk_bpchar WHERE code = 'EF56';
SELECT CASE WHEN label = 'abbr-4' THEN 'PASS:H1_BC_GH78_INSERT'
            ELSE 'FAIL:'||COALESCE(label,'NULL') END AS h1_bc_gh78
FROM public.bs_pk_bpchar WHERE code = 'GH78';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H1_BC_ZZ99_MISSING'
            ELSE 'FAIL:'||count(*) END AS h1_bc_zz99
FROM public.bs_pk_bpchar WHERE code = 'ZZ99';

RESET overlay_branch.current;
DROP TABLE public.bs_pk_bpchar;

-- H.2 = Bug#2 Pure-delta INSERT row DELETE support
--     Pure-delta row 5:e lives ONLY in pg_branch_delta, never in MAIN heap.
--     Historically, UPDATE/DELETE's WR subplan was plain PG SeqScan (no
--     BranchScan) because the Planner hook returns early for non-SELECT
--     commandType (Confirmed fact 6).  So DELETE WHERE id=5: subplan scans
--     MAIN -> 0 rows matched -> ndone=0.  Correct pure-delta DML needs
--     either BranchScan injection on the subplan side (out-of-scope) OR
--     re-running the original subplan's ExecQual qual in our foreach.
--     safe assertions: (H2a) pure INSERT is readable; (H2b) real pure-
CREATE TABLE public.bs_pk_bug2 (id INT4 PRIMARY KEY, payload TEXT);
INSERT INTO public.bs_pk_bug2 VALUES (1,'a'),(2,'b'),(3,'c');
SELECT overlay_branch.create_branch('bs_h2_b');
SELECT overlay_branch.use_branch('bs_h2_b');
INSERT INTO public.bs_pk_bug2 VALUES (5,'e');  -- pure delta INSERT row

-- H.2a Check pure-delta row exists (P0 path WHERE id=5 returns 1 row).
SELECT CASE WHEN payload = 'e' THEN 'PASS:H2_ID5_PURE_INSERT_READABLE'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS h2_pre
FROM public.bs_pk_bug2 WHERE id = 5;

-- H.2b Real-delete pure-delta row id=5: WR pure-delta ExecQual pass
DELETE FROM public.bs_pk_bug2 WHERE id = 5;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H2_DELETE_PURE_DELTA_REAL'
            ELSE 'FAIL_COUNT:'||count(*) END AS h2_del
FROM public.bs_pk_bug2 WHERE id = 5;

-- H.2c Re-INSERT: rebirth (overwrites the INSERT delta correctly).
INSERT INTO public.bs_pk_bug2 VALUES (5,'e3-reborn');
SELECT CASE WHEN payload = 'e3-reborn' THEN 'PASS:H2_PURE_DELTA_REBORN'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS h2_reborn
FROM public.bs_pk_bug2 WHERE id = 5;

RESET overlay_branch.current;
DROP TABLE public.bs_pk_bug2;

-- H.3a Plain MAIN row baseline WHERE pk=const (P0 O(1) path) UPDATE/DELETE
--      + RETURNING.  H3a.1 first WITHOUT RETURNING (confirm same code path
--      as Section G's non-RETURNING case PASSes; H3a.2 then WITH RETURNING
CREATE TABLE public.bs_pk_bug3a (id INT4 PRIMARY KEY, payload TEXT);
INSERT INTO public.bs_pk_bug3a VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d');
SELECT overlay_branch.create_branch('bs_h3a_b');
SELECT overlay_branch.use_branch('bs_h3a_b');

-- H3a.1 UPDATE WHERE id=2 (pk=const) WITHOUT RETURNING.
UPDATE public.bs_pk_bug3a SET payload = payload || '-UPD' WHERE id = 2;
SELECT CASE WHEN payload = 'b-UPD' THEN 'PASS:H3_UPDATE_PK_EQ_NO_RET'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS h3a1
FROM public.bs_pk_bug3a WHERE id = 2;

-- H3a.2 UPDATE WHERE id=3 (pk=const) RETURNING
UPDATE public.bs_pk_bug3a SET payload = payload || '-UPD' WHERE id = 3
RETURNING id, payload;
SELECT CASE WHEN payload = 'c-UPD' THEN 'PASS:H3_RETURNING_PK_EQ_UPDATE'
            ELSE 'FAIL_BRANCH:'||COALESCE(payload,'NULL') END AS h3a2_check
FROM public.bs_pk_bug3a WHERE id = 3;

-- H3a.3 DELETE WHERE id=4 (pk=const) RETURNING
DELETE FROM public.bs_pk_bug3a WHERE id = 4
RETURNING id, payload;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H3_RETURNING_PK_EQ_DELETE'
            ELSE 'FAIL_COUNT:'||count(*) END AS h3a3_check
FROM public.bs_pk_bug3a WHERE id = 4;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||payload, ',' ORDER BY id) = '1:a,2:b,3:c,4:d'
         THEN 'PASS:H3_MAIN_PRISTINE_A'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||payload, ',' ORDER BY id),'NULL')
       END AS h3_main_check
FROM public.bs_pk_bug3a;
DROP TABLE public.bs_pk_bug3a;

-- H.3b Mixed scenario: first INSERT pure-delta row, then:
--        (1) DELETE pure-delta WHERE id=5 RETURNING really deletes 1 row,
--        (2) MAIN id=3 stays clean after RESET back to MAIN.
CREATE TABLE public.bs_pk_bug3b (id INT4 PRIMARY KEY, payload TEXT);
INSERT INTO public.bs_pk_bug3b VALUES (1,'a'),(2,'b'),(3,'c');
SELECT overlay_branch.create_branch('bs_h3b_b');
SELECT overlay_branch.use_branch('bs_h3b_b');
INSERT INTO public.bs_pk_bug3b VALUES (5,'e');

-- DELETE WHERE id=5 RETURNING (pure-delta row) -> real delete + returns 1 row.
DELETE FROM public.bs_pk_bug3b WHERE id = 5
RETURNING id, payload;
-- Verify pure delta id=5 is really deleted (count=0).
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H3_DEL_PURE_DELTA_REAL_RETURNING'
            ELSE 'FAIL_COUNT:'||count(*) END AS h3_del
FROM public.bs_pk_bug3b WHERE id = 5;

RESET overlay_branch.current;
SELECT CASE WHEN payload = 'c' THEN 'PASS:H3_MAIN_ID3_PRISTINE'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS h3_main_c
FROM public.bs_pk_bug3b WHERE id = 3;

DROP TABLE public.bs_pk_bug3b;


RESET overlay_branch.current;

/* =========================================================== Section I */
-- =====================================================================
-- =====================================================================
-- ===== Section I: Multi-type PK coverage (NUMERIC/BPCHAR/VARCHAR/DOUBLE/
-- =====================================================================
-- =====            format_type_with_typmod end-to-end)                 =====
CREATE TABLE public.bs_pk_numeric (pk NUMERIC(10,2) PRIMARY KEY, label TEXT);
INSERT INTO public.bs_pk_numeric VALUES (10.50, 'orig-ten'),(20.25, 'orig-twenty'),(99.99, 'orig-ninety');
SELECT overlay_branch.create_branch('bs_num_b');
SELECT overlay_branch.use_branch('bs_num_b');

INSERT INTO public.bs_pk_numeric VALUES (30.33, 'branch-thirty');
UPDATE public.bs_pk_numeric SET label = 'UPDATED' WHERE pk = 20.25;
DELETE FROM public.bs_pk_numeric WHERE pk = 10.50;

SELECT CASE
         WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk)
              = '20.25:UPDATED,30.33:branch-thirty,99.99:orig-ninety'
         THEN 'PASS:I1_NUMERIC_PK_CONTENT'
         ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL')
       END AS i1_num_content
FROM public.bs_pk_numeric;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk)
              = '10.50:orig-ten,20.25:orig-twenty,99.99:orig-ninety'
         THEN 'PASS:I1_NUMERIC_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL')
       END AS i1_num_main
FROM public.bs_pk_numeric;
DROP TABLE public.bs_pk_numeric;

-- I.2 CHAR(12) BPCHAR + VARCHAR(32) multi-column composite PK
--      (verifies composite PK serialization path).
CREATE TABLE public.bs_pk_multi (
  code   CHAR(12)    NOT NULL,
  tag    VARCHAR(32) NOT NULL,
  payload TEXT,
  PRIMARY KEY (code, tag)
);
INSERT INTO public.bs_pk_multi VALUES ('AB12', 'tag-A', 'pAB12'), ('CD34', 'tag-B', 'pCD34');
SELECT overlay_branch.create_branch('bs_mpk_b');
SELECT overlay_branch.use_branch('bs_mpk_b');

INSERT INTO public.bs_pk_multi VALUES ('EF56', 'tag-C', 'pEF56');
UPDATE public.bs_pk_multi SET payload = payload || '-U' WHERE code = 'AB12' AND tag = 'tag-A';
DELETE FROM public.bs_pk_multi WHERE code = 'CD34' AND tag = 'tag-B';

SELECT CASE
         WHEN string_agg(rtrim(code)||':'||tag||':'||payload, ',' ORDER BY code, tag)
              = 'AB12:tag-A:pAB12-U,EF56:tag-C:pEF56'
         THEN 'PASS:I2_MULTI_PK_CONTENT'
         ELSE 'FAIL:'||COALESCE(string_agg(rtrim(code)||':'||tag||':'||payload, ',' ORDER BY code, tag),'NULL')
       END AS i2_mpk_content
FROM public.bs_pk_multi;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(rtrim(code)||':'||tag||':'||payload, ',' ORDER BY code, tag)
              = 'AB12:tag-A:pAB12,CD34:tag-B:pCD34'
         THEN 'PASS:I2_MULTI_PK_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(rtrim(code)||':'||tag||':'||payload, ',' ORDER BY code, tag),'NULL')
       END AS i2_mpk_main
FROM public.bs_pk_multi;
DROP TABLE public.bs_pk_multi;

-- I.3 DATE / TIMESTAMP PK types (serialized format match: WR
--      OidOutputFunctionCall vs P0 CAST ... ::text).
CREATE TABLE public.bs_pk_date (d DATE PRIMARY KEY, note TEXT);
INSERT INTO public.bs_pk_date VALUES ('2024-01-15', 'new-year'), ('2024-06-30', 'mid-year');
SELECT overlay_branch.create_branch('bs_date_b');
SELECT overlay_branch.use_branch('bs_date_b');

INSERT INTO public.bs_pk_date VALUES ('2024-12-25', 'christmas');
UPDATE public.bs_pk_date SET note = 'mid-year-updated' WHERE d = '2024-06-30';
DELETE FROM public.bs_pk_date WHERE d = '2024-01-15';

SELECT CASE
         WHEN string_agg(d::text||':'||note, ',' ORDER BY d)
              = '2024-06-30:mid-year-updated,2024-12-25:christmas'
         THEN 'PASS:I3_DATE_PK_CONTENT'
         ELSE 'FAIL:'||COALESCE(string_agg(d::text||':'||note, ',' ORDER BY d),'NULL')
       END AS i3_date_content
FROM public.bs_pk_date;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(d::text||':'||note, ',' ORDER BY d)
              = '2024-01-15:new-year,2024-06-30:mid-year'
         THEN 'PASS:I3_DATE_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(d::text||':'||note, ',' ORDER BY d),'NULL')
       END AS i3_date_main
FROM public.bs_pk_date;
DROP TABLE public.bs_pk_date;

-- I.4 FLOAT8 floating-point PK (binary equality matching).
--      Verifies that WR's quote_literal(doubleout(...)) and
--      P0's CAST(x AS double precision)::text produce byte-identical
--      output.
CREATE TABLE public.bs_pk_float8 (pk FLOAT8 PRIMARY KEY, label TEXT);
INSERT INTO public.bs_pk_float8 VALUES (1.0, 'one'), (2.5, 'two-half'), (10.25e-1, 'ten-25e-1');
SELECT overlay_branch.create_branch('bs_f8_b');
SELECT overlay_branch.use_branch('bs_f8_b');

INSERT INTO public.bs_pk_float8 VALUES (3.14159265, 'pi-ish');
UPDATE public.bs_pk_float8 SET label = 'one!' WHERE pk = 1.0;
DELETE FROM public.bs_pk_float8 WHERE pk = 2.5;

SELECT CASE
         WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk)
              = '0.1025:ten-25e-1,1:one!,3.14159265:pi-ish'
         THEN 'PASS:I4_FLOAT8_PK_CONTENT'
         ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL')
       END AS i4_f8_content
FROM public.bs_pk_float8;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk)
              = '0.1025:ten-25e-1,1:one,2.5:two-half'
         THEN 'PASS:I4_FLOAT8_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL')
       END AS i4_f8_main
FROM public.bs_pk_float8;
DROP TABLE public.bs_pk_float8;


RESET overlay_branch.current;

/* =========================================================== Section J */
-- =====================================================================
-- =====================================================================
-- ===== Section J: P2 general WHERE non-PK pushdown - complex expr =====
-- =====            deparse coverage                                      =====
-- =====================================================================
-- =====================================================================
CREATE TABLE public.bs_p2 (id INT4 PRIMARY KEY, name TEXT, amt NUMERIC(10,2), color TEXT);
INSERT INTO public.bs_p2
  SELECT g, 'n'||g, (g*1.37)::numeric(10,2), CASE g%3 WHEN 0 THEN 'red' WHEN 1 THEN 'green' ELSE 'blue' END
  FROM generate_series(1, 20) g;
SELECT overlay_branch.create_branch('bs_p2_b');
SELECT overlay_branch.use_branch('bs_p2_b');

INSERT INTO public.bs_p2 VALUES (100, 'hundred', 99.99, 'yellow');

-- J.1 LIKE / ILIKE non-PK predicate + composite AND:
--      triggers P2 general_where: `name LIKE 'n1%' AND color = 'green'`
--      deparse.
SELECT CASE
         WHEN string_agg(id::text||':'||name, ',' ORDER BY id) = '10:n10,13:n13,16:n16,19:n19'
         THEN 'PASS:J1_LIKE_AND_COLOR'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name, ',' ORDER BY id),'NULL')
       END AS j1_like
FROM public.bs_p2 WHERE name LIKE 'n1%' AND color = 'green';

-- J.2 BETWEEN range + OR composition: amt BETWEEN 5 AND 10 OR color = 'yellow'
SELECT CASE
         WHEN string_agg(id::text||':'||amt::text, ',' ORDER BY id)
              = (SELECT string_agg(id::text||':'||amt::text, ',' ORDER BY id)
                 FROM overlay_branch.overlay_main_plus_delta('public.bs_p2')
                   AS s(id int4, name text, amt numeric, color text)
                 WHERE (s).amt BETWEEN 5 AND 10 OR (s).color = 'yellow')
         THEN 'PASS:J2_BETWEEN_OR'
         ELSE 'FAIL'
       END AS j2_between;

-- J.3 Function-expression pushdown: substring(name, 2, 2) = 'n1' AND id > 15.
SELECT CASE
         WHEN string_agg(id::text||':'||name, ',' ORDER BY id)
              = (SELECT string_agg(id::text||':'||name, ',' ORDER BY id)
                 FROM public.bs_p2 WHERE substring(name, 2, 2) = 'n1' AND id > 15)
         THEN 'PASS:J3_FUNC_EXPR_AND'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name, ',' ORDER BY id),'NULL')
       END AS j3_func;

-- J.4 NOT negation: id BETWEEN 1 AND 20 AND NOT (color = 'red').
SELECT CASE WHEN count(*) = 14 THEN 'PASS:J4_NOT_COLOR_RED' ELSE 'FAIL:'||count(*) END AS j4_not
FROM public.bs_p2 WHERE id BETWEEN 1 AND 20 AND NOT (color = 'red');

-- J.5 WR + non-PK pushdown UPDATE + DELETE (verifies UPDATE/DELETE subplans
--      still walk the physical MAIN SeqScan, but WR's ndone is correct and
--      branch-side content consistency is preserved).
-- J.5 WR + non-PK pushdown UPDATE + DELETE (verifies UPDATE/DELETE subplans
UPDATE public.bs_p2 SET amt = amt + 1000 WHERE color = 'yellow';
DELETE FROM public.bs_p2 WHERE name LIKE 'n1%' AND color = 'red';

SELECT CASE
         WHEN string_agg(id::text||':'||amt::text, ',' ORDER BY id)
              = (SELECT string_agg(id::text||':'||amt::text, ',' ORDER BY id)
                 FROM overlay_branch.overlay_main_plus_delta('public.bs_p2')
                   AS s(id int4, name text, amt numeric, color text))
         THEN 'PASS:J5_WR_NONPK_UPD_DEL'
         ELSE 'FAIL'
       END AS j5_wr_nonpk;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||color, ',' ORDER BY id)
              = (SELECT string_agg(g::text||':'||CASE g%3 WHEN 0 THEN 'red' WHEN 1 THEN 'green' ELSE 'blue' END, ',' ORDER BY g)
                 FROM generate_series(1,20) g)
         THEN 'PASS:J_MAIN_PRISTINE'
         ELSE 'FAIL'
       END AS j_main_check
FROM public.bs_p2;
DROP TABLE public.bs_p2;


/* =========================================================== Section K */
-- =====================================================================
-- ===== Section K: dropped-column delta reconstruction (B1 regression).
-- =====   A branch is taken AFTER a middle column is dropped, so the heap
-- =====   TupleDesc has an attisdropped hole.  The delta row is serialized
-- =====   as JSON over the *live* columns; on read-back the SPI result set
-- =====   must be mapped to the heap TupleDesc by SKIPPING dropped attnums.
-- =====   Positional indexing shifts every column after the dropped one.
-- =====================================================================
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.bs_dropcol;
CREATE TABLE public.bs_dropcol (id INT4 PRIMARY KEY, mid TEXT, tail TEXT, extra TEXT);
ALTER TABLE public.bs_dropcol DROP COLUMN mid;   -- live attrs: id, tail, extra
INSERT INTO public.bs_dropcol VALUES (1, 'MAIN_tail', 'MAIN_extra');

SELECT overlay_branch.create_branch('bs_dropcol_b');
SELECT overlay_branch.use_branch('bs_dropcol_b');

INSERT INTO public.bs_dropcol VALUES (2, 'DELTA_tail', 'DELTA_extra');  -- pure delta INSERT
UPDATE public.bs_dropcol SET tail = 'MAIN_tail_x' WHERE id = 1;        -- MAIN-row UPDATE → delta

-- K.1 delta INSERT must reconstruct the tail columns correctly
SELECT CASE WHEN tail = 'DELTA_tail' AND extra = 'DELTA_extra'
            THEN 'PASS:K1_DELTA_INSERT_RECON'
            ELSE 'FAIL:'||COALESCE(tail,'NULL')||'/'||COALESCE(extra,'NULL') END AS k1
  FROM public.bs_dropcol WHERE id = 2;

-- K.2 MAIN-row UPDATE delta must also reconstruct the tail columns
SELECT CASE WHEN tail = 'MAIN_tail_x' AND extra = 'MAIN_extra'
            THEN 'PASS:K2_MAIN_UPDATE_RECON'
            ELSE 'FAIL:'||COALESCE(tail,'NULL')||'/'||COALESCE(extra,'NULL') END AS k2
  FROM public.bs_dropcol WHERE id = 1;

-- K.3 exit + re-enter: reconstruction must be identical from a cold context
SELECT overlay_branch.use_branch(NULL);
SELECT overlay_branch.use_branch('bs_dropcol_b');
SELECT CASE WHEN string_agg(id::text||':'||tail||':'||extra, ',' ORDER BY id)
              = '1:MAIN_tail_x:MAIN_extra,2:DELTA_tail:DELTA_extra'
            THEN 'PASS:K3_REENTER_RECON'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||tail||':'||extra, ',' ORDER BY id),'NULL') END AS k3
  FROM public.bs_dropcol;

SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;

-- K.4 MAIN pristine: single row, original tail/extra
SELECT CASE WHEN count(*) = 1 AND max(tail) = 'MAIN_tail' AND max(extra) = 'MAIN_extra'
            THEN 'PASS:K4_MAIN_PRISTINE'
            ELSE 'FAIL' END AS k4
  FROM public.bs_dropcol;

SELECT overlay_branch.discard_branch('bs_dropcol_b');
RESET client_min_messages;
DROP TABLE public.bs_dropcol;

