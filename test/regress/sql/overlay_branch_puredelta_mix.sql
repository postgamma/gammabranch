/*
 * overlay_branch_puredelta_mix.sql — Pure delta (MAIN empty or MAIN row not overlaid) scenarios
 *                           mixed DML / boundary / apply-discard regression.
 *
 * COVERAGE (formerly overlay_branch_advanced.sql Section K~Q):
 *   Section K — Mixed DML lifecycle: UPDATE→DELETE→INSERT rebirth chain; WR multi-op row counts;
 *               same-tx multi-table zero-pollution MAIN (bs_mix + bs_mix2 two independent tables)
 *   Section L — Pure-delta UPDATE NOOP: SET projection not wired, WR SAFE NOOP;
 *               NOT silent data corruption (UPDATE WHERE not hit → OK, no error)
 *   Section M — BPCHAR/NUMERIC typmod PK pure-delta DML: I.1/I.2 MAIN-PK paths
 *               also work for pure delta rows; bpchar r-trim comparison / NUMERIC scale preserved
 *   Section N — Multi puredelta rows + non-PK WHERE: WR ExecQual non-PK true filtering,
 *               does not "NUKEm all"; bs_pd_multi WHERE col=? only filters pure delta rows matching the condition
 *   Section O — apply/discard boundary: pure delta INSERT → apply propagates to MAIN;
 *               pure delta INSERT → discard ROLLBACK; MAIN original rows unchanged
 *   Section P — EMPTY MAIN heap 100% pure delta: MAIN 0 rows, overlay composed entirely of pure delta
 *               INSERT; UPDATE( NOP ) / DELETE OK; BranchScan reads correctly; apply propagates
 *   Section Q — Composite PK pure-delta: serialize_pk / deparse both cover composite PK;
 *               WR WHERE (a,b)=(?,?) composite key serialized correctly
 *
 * Each section's SETUP is self-contained; DISCARD ALL + RESET current between sections.
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

/* =========================================================== Section K */
-- =====================================================================
-- =====================================================================
-- ===== Section K: Mixed DML lifecycle (same row UPDATE->DELETE->INSERT =====
-- =====            rebirth chain / WR multi-row counts / multi-table =====
-- =====            zero-pollution in same transaction)               =====
-- =====================================================================
-- =====================================================================
CREATE TABLE public.bs_k1 (id INT4 PRIMARY KEY, v TEXT);
CREATE TABLE public.bs_k2 (id INT4 PRIMARY KEY, n NUMERIC(12,4));
INSERT INTO public.bs_k1 VALUES (1,'a'),(2,'b'),(3,'c');
INSERT INTO public.bs_k2 VALUES (1,10.0000),(2,20.5000);
SELECT overlay_branch.create_branch('bs_k_b');
SELECT overlay_branch.use_branch('bs_k_b');

-- K.1 Same-row lifecycle: MAIN row id=1 -> UPDATE -> DELETE -> INSERT reborn
UPDATE public.bs_k1 SET v = 'A-U1' WHERE id = 1;
UPDATE public.bs_k1 SET v = 'A-U2' WHERE id = 1;
DELETE FROM public.bs_k1 WHERE id = 1;
INSERT INTO public.bs_k1 VALUES (1, 'A-REBORN');
SELECT CASE WHEN v = 'A-REBORN' THEN 'PASS:K1_ROW_LIFECYCLE' ELSE 'FAIL:'||COALESCE(v,'NULL') END AS k1_lc
FROM public.bs_k1 WHERE id = 1;

-- K.2 WR multi-row UPDATE/DELETE counts (Section B already covers this,
--      but here asserted more directly).
UPDATE public.bs_k1 SET v = v || '-X' WHERE id IN (2, 3, 9999);
SELECT CASE
         WHEN string_agg(id::text||':'||v, ',' ORDER BY id) = '1:A-REBORN,2:b-X,3:c-X'
         THEN 'PASS:K2_WR_2_OF_3_HIT'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||v, ',' ORDER BY id),'NULL')
       END AS k2_multi;

DELETE FROM public.bs_k1 WHERE id IN (2, 9998);
SELECT CASE
         WHEN string_agg(id::text||':'||v, ',' ORDER BY id) = '1:A-REBORN,3:c-X'
         THEN 'PASS:K2_WR_DELETE_1_OF_2_HIT'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||v, ',' ORDER BY id),'NULL')
       END AS k2_del;

-- K.3 Multi-table mix (guarantees overlay_branch.current switching never
--      cross-contaminates tables).
INSERT INTO public.bs_k2 VALUES (99, 999.9999);
UPDATE public.bs_k2 SET n = n * 2 WHERE id = 2;
DELETE FROM public.bs_k2 WHERE id = 1;

SELECT CASE
         WHEN (SELECT string_agg(id::text||':'||v, ',' ORDER BY id) FROM public.bs_k1)
              = '1:A-REBORN,3:c-X'
          AND (SELECT string_agg(id::text||':'||n::text, ',' ORDER BY id) FROM public.bs_k2)
              = '2:41.0000,99:999.9999'
         THEN 'PASS:K3_MULTI_TABLE_MIX'
         ELSE 'FAIL'
       END AS k3_mix;

RESET overlay_branch.current;
SELECT CASE
         WHEN (SELECT string_agg(id::text||':'||v, ',' ORDER BY id) FROM public.bs_k1) = '1:a,2:b,3:c'
          AND (SELECT string_agg(id::text||':'||n::text, ',' ORDER BY id) FROM public.bs_k2) = '1:10.0000,2:20.5000'
         THEN 'PASS:K_MAIN_ZERO_POLLUTION'
         ELSE 'FAIL'
       END AS k_main_clean;
DROP TABLE public.bs_k1;
DROP TABLE public.bs_k2;


DISCARD ALL;
SET client_min_messages = WARNING;
RESET overlay_branch.current;

/* =========================================================== Section L */
-- =====================================================================
-- ===== Section L: pure delta UPDATE (ACC-1 wires SET projection over
-- =====            the reconstructed overlay row and applies an explicit
-- =====            WHERE to pure-delta rows)
-- =====================================================================
CREATE TABLE public.bs_L_upd (id INT4 PRIMARY KEY, payload TEXT, score INT4);
INSERT INTO public.bs_L_upd VALUES (1,'main-a', 10),(2,'main-b', 20);
SELECT overlay_branch.create_branch('bs_L_b');
SELECT overlay_branch.use_branch('bs_L_b');

-- L.1 Pure-delta INSERT 3 rows.
INSERT INTO public.bs_L_upd VALUES (11, 'pd-c', 30);
INSERT INTO public.bs_L_upd VALUES (12, 'pd-d', 40);
INSERT INTO public.bs_L_upd VALUES (13, 'pd-e', 50);

SELECT CASE WHEN count(*) = 3 THEN 'PASS:L1_PURE_DELTA_3_INSERTED'
            ELSE 'FAIL_COUNT:'||count(*) END AS l1_cnt
FROM public.bs_L_upd WHERE id IN (11,12,13);

-- L.2 Pure delta UPDATE by PK equality: WR CMD_UPDATE pure-delta MAIN-miss
--     implementation (approach B) properly materializes the reconstructed slot,
--     runs ModifyTable SET projection, and writes op=U back to delta_store.
--     The NOP guard is bypassed because PK-equality qual is
--     successfully decomposed.  Assert the row was updated correctly.
UPDATE public.bs_L_upd SET payload = 'pd-c-UPD', score = score + 100 WHERE id = 11;
SELECT CASE WHEN payload = 'pd-c-UPD' AND score = 130 THEN 'PASS:L2_UPD_PK_EQ_APPLIED'
            ELSE 'FAIL:'||COALESCE(payload,'NULL')||'@'||COALESCE(score::text,'NULL') END AS l2_upd
FROM public.bs_L_upd WHERE id = 11;

-- L.3 Pure delta UPDATE WHERE id IN (...): ACC-1 evaluates an explicit
--     WHERE against every reconstructed pure-delta row, so the matching
--     rows 12 and 13 are updated and the unmatched 9999 is a no-op.
--     Row 11 was already updated by L.2.
UPDATE public.bs_L_upd SET payload = 'WRONG' WHERE id IN (12, 13, 9999);
SELECT CASE WHEN string_agg(id::text||':'||payload, ',' ORDER BY id) = '11:pd-c-UPD,12:WRONG,13:WRONG'
            THEN 'PASS:L3_UPD_PK_IN_APPLIED'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||payload, ',' ORDER BY id),'NULL') END AS l3_nop
FROM public.bs_L_upd WHERE id IN (11,12,13);

-- L.4 Verify MAIN rows untouched.
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||payload||'@'||score, ',' ORDER BY id) = '1:main-a@10,2:main-b@20'
            THEN 'PASS:L_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||payload||'@'||score, ',' ORDER BY id),'NULL') END AS l_main
FROM public.bs_L_upd;
DROP TABLE public.bs_L_upd;


DISCARD ALL;
SET client_min_messages = WARNING;
RESET overlay_branch.current;

/* =========================================================== Section M */
-- =====================================================================
-- ===== Section M: BPCHAR / NUMERIC typmod PK — pure delta DML
-- =====            (verify I.1/I.2 MAIN-PK paths work for pure-delta rows)
-- =====================================================================
-- M.1 NUMERIC(10,2) PK: pure delta INSERT then DELETE WHERE pk=const
--     (IndexScan → indexqualorig must be picked up by WR pure-delta pass)
CREATE TABLE public.bs_M_num (pk NUMERIC(10,2) PRIMARY KEY, label TEXT);
INSERT INTO public.bs_M_num VALUES (10.50, 'orig-ten'),(20.25, 'orig-twenty');
SELECT overlay_branch.create_branch('bs_Mn_b');
SELECT overlay_branch.use_branch('bs_Mn_b');

INSERT INTO public.bs_M_num VALUES (30.33, 'pd-thirty');
INSERT INTO public.bs_M_num VALUES (40.44, 'pd-forty');
DELETE FROM public.bs_M_num WHERE pk = 30.33;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:M1_NUM_PK_PURE_DELTA_DELETE_1_OF_2'
            ELSE 'FAIL_COUNT:'||count(*) END AS m1_del
FROM public.bs_M_num WHERE pk IN (30.33, 40.44);
SELECT CASE WHEN label = 'pd-forty' THEN 'PASS:M1_SURVIVOR_CORRECT'
            ELSE 'FAIL:'||COALESCE(label,'NULL') END AS m1_surv
FROM public.bs_M_num WHERE pk = 40.44;
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk) = '10.50:orig-ten,20.25:orig-twenty'
            THEN 'PASS:M1_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL') END AS m1_main
FROM public.bs_M_num;
DROP TABLE public.bs_M_num;

-- M.2 BPCHAR(6) PK: pure delta INSERT → DELETE WHERE pk=const
--     (WR / P0 paths both rtrim bpchar PK on both sides; Confirmed fact 5)
CREATE TABLE public.bs_M_bp (code BPCHAR(6) PRIMARY KEY, flag INT4);
INSERT INTO public.bs_M_bp VALUES ('MAIN01', 1), ('MAIN02', 2);
SELECT overlay_branch.create_branch('bs_Mb_b');
SELECT overlay_branch.use_branch('bs_Mb_b');
INSERT INTO public.bs_M_bp VALUES ('PD001', 101);
INSERT INTO public.bs_M_bp VALUES ('PD002', 102);
INSERT INTO public.bs_M_bp VALUES ('PD003', 103);
DELETE FROM public.bs_M_bp WHERE code = 'PD002';
SELECT CASE WHEN string_agg(rtrim(code)||':'||flag::text, ',' ORDER BY code) = 'PD001:101,PD003:103'
            THEN 'PASS:M2_BPCHAR_PK_PURE_DELTA_DELETE_1_OF_3'
            ELSE 'FAIL:'||COALESCE(string_agg(rtrim(code)||':'||flag::text, ',' ORDER BY code),'NULL') END AS m2_del
FROM public.bs_M_bp WHERE code LIKE 'PD%';

DELETE FROM public.bs_M_bp WHERE code = 'PD001' RETURNING rtrim(code), flag;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:M2_RETURNING_DELETE_OK'
            ELSE 'FAIL_COUNT:'||count(*) END AS m2_ret
FROM public.bs_M_bp WHERE code = 'PD001';

RESET overlay_branch.current;
SELECT CASE WHEN string_agg(rtrim(code)||':'||flag::text, ',' ORDER BY code) = 'MAIN01:1,MAIN02:2'
            THEN 'PASS:M2_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(rtrim(code)||':'||flag::text, ',' ORDER BY code),'NULL') END AS m2_main
FROM public.bs_M_bp;
DROP TABLE public.bs_M_bp;


DISCARD ALL;
SET client_min_messages = WARNING;
RESET overlay_branch.current;

/* =========================================================== Section N */
-- =====================================================================
-- ===== Section N: Multi pure-delta rows + non-PK WHERE filters
-- =====            (verify WR ExecQual on non-PK quals really filters,
-- =====             not NUKEm all pure-delta rows)
-- =====================================================================
CREATE TABLE public.bs_N_mix (id INT4 PRIMARY KEY, name TEXT, grp TEXT, score INT4);
INSERT INTO public.bs_N_mix VALUES (1, 'main-one', 'alpha', 100);
SELECT overlay_branch.create_branch('bs_N_b');
SELECT overlay_branch.use_branch('bs_N_b');

-- N.1 Insert 6 pure-delta rows: 3 × group A score<100, 3 × group B score>=100
INSERT INTO public.bs_N_mix VALUES (10, 'pd-a1', 'A', 50);
INSERT INTO public.bs_N_mix VALUES (11, 'pd-a2', 'A', 70);
INSERT INTO public.bs_N_mix VALUES (12, 'pd-a3', 'A', 90);
INSERT INTO public.bs_N_mix VALUES (20, 'pd-b1', 'B', 150);
INSERT INTO public.bs_N_mix VALUES (21, 'pd-b2', 'B', 200);
INSERT INTO public.bs_N_mix VALUES (22, 'pd-b3', 'B', 250);

SELECT CASE WHEN count(*) = 6 THEN 'PASS:N1_PURE_DELTA_6_INSERTED'
            ELSE 'FAIL_COUNT:'||count(*) END AS n1_cnt
FROM public.bs_N_mix WHERE id >= 10 AND id <= 22;

-- N.2 DELETE WHERE grp='B' (non-PK, non-indexable filter): WR ExecQual
--     must run SeqScan qual (grp='B') per reconstructed pure-delta slot.
DELETE FROM public.bs_N_mix WHERE grp = 'B';
SELECT CASE
         WHEN (SELECT count(*) FROM public.bs_N_mix WHERE grp = 'A') = 3
          AND (SELECT count(*) FROM public.bs_N_mix WHERE grp = 'B') = 0
         THEN 'PASS:N2_NONPK_FILTER_DELETE_GRP_B_ONLY'
         ELSE 'FAIL A='||COALESCE((SELECT count(*) FROM public.bs_N_mix WHERE grp = 'A')::text,'?')||' B='||COALESCE((SELECT count(*) FROM public.bs_N_mix WHERE grp = 'B')::text,'?')
       END AS n2_filt;

-- N.3 Then DELETE WHERE score < 80 (another non-PK range filter): 2 rows
--     (a1 score=50 and a2 score=70 → gone; a3 score=90 → stays)
DELETE FROM public.bs_N_mix WHERE score < 80;
SELECT CASE WHEN string_agg(id::text||':'||name||'@'||score, ',' ORDER BY id) = '12:pd-a3@90'
            THEN 'PASS:N3_SCORE_RANGE_FILTER_2_DELETED'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||'@'||score, ',' ORDER BY id),'NULL') END AS n3_rng
FROM public.bs_N_mix WHERE id >= 10 AND id <= 22;

-- N.4 RETURNING non-PK filter: re-insert a couple, delete with RETURNING
INSERT INTO public.bs_N_mix VALUES (30, 'pd-c1', 'C', 333);
INSERT INTO public.bs_N_mix VALUES (31, 'pd-c2', 'C', 333);
INSERT INTO public.bs_N_mix VALUES (32, 'pd-c3', 'C', 999);
DELETE FROM public.bs_N_mix WHERE grp = 'C' AND score = 333
RETURNING id, name, score ORDER BY id;
SELECT CASE WHEN string_agg(id::text||':'||name, ',' ORDER BY id) = '32:pd-c3'
            THEN 'PASS:N4_RETURNING_FILTER_SURVIVOR_32'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name, ',' ORDER BY id),'NULL') END AS n4_surv
FROM public.bs_N_mix WHERE grp = 'C';

-- N.5 MAIN row id=1 plus pure delta N.1→N.4 combined view (aggregate)
SELECT CASE
         WHEN (SELECT count(*) FROM public.bs_N_mix) = 3
         THEN 'PASS:N5_TOTAL_3_ROWS'
         ELSE 'FAIL_COUNT:'||(SELECT count(*)::text FROM public.bs_N_mix)
       END AS n5_tot;

RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||name||'@'||grp, ',' ORDER BY id) = '1:main-one@alpha'
            THEN 'PASS:N_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||'@'||grp, ',' ORDER BY id),'NULL') END AS n_main
FROM public.bs_N_mix;
DROP TABLE public.bs_N_mix;


DISCARD ALL;
SET client_min_messages = WARNING;
RESET overlay_branch.current;

/* =========================================================== Section O */
-- =====================================================================
-- ===== Section O: apply_branch / discard_branch boundary on pure-delta
-- =====            (pure delta INSERT → then apply or discard correctly
-- =====             propagates or rolls back)
-- =====================================================================
-- O.1 apply: pure delta INSERT id=5 + DELETE id=5 reborn id=5 twice
CREATE TABLE public.bs_O1 (id INT4 PRIMARY KEY, s TEXT);
INSERT INTO public.bs_O1 VALUES (1, 'm1'),(2, 'm2');
SELECT overlay_branch.create_branch('bs_O1b');
SELECT overlay_branch.use_branch('bs_O1b');
INSERT INTO public.bs_O1 VALUES (5, 'first');
DELETE FROM public.bs_O1 WHERE id = 5;
INSERT INTO public.bs_O1 VALUES (5, 'second');
DELETE FROM public.bs_O1 WHERE id = 5;
INSERT INTO public.bs_O1 VALUES (5, 'FINAL');
SELECT CASE WHEN s = 'FINAL' THEN 'PASS:O1_PURE_REBIRTH_CHAIN'
            ELSE 'FAIL:'||COALESCE(s,'NULL') END AS o1_br
FROM public.bs_O1 WHERE id = 5;
SELECT overlay_branch.apply_branch('bs_O1b');
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||s, ',' ORDER BY id) = '1:m1,2:m2,5:FINAL'
            THEN 'PASS:O1_APPLY_PURE_DELTA_PROPAGATED'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||s, ',' ORDER BY id),'NULL') END AS o1_post
FROM public.bs_O1;
SELECT state FROM public.pg_branch WHERE branch_name = 'bs_O1b';
DROP TABLE public.bs_O1;

-- O.2 discard: pure delta INSERT id=77 + UPDATE MAIN id=1 → discard rolls back
CREATE TABLE public.bs_O2 (id INT4 PRIMARY KEY, s TEXT);
INSERT INTO public.bs_O2 VALUES (1, 'm1'),(2, 'm2');
SELECT overlay_branch.create_branch('bs_O2b');
SELECT overlay_branch.use_branch('bs_O2b');
INSERT INTO public.bs_O2 VALUES (77, 'will_discard');
UPDATE public.bs_O2 SET s = 'BRANCH_UPD' WHERE id = 1;
DELETE FROM public.bs_O2 WHERE id = 2;
SELECT overlay_branch.discard_branch('bs_O2b');
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||s, ',' ORDER BY id) = '1:m1,2:m2'
            THEN 'PASS:O2_DISCARD_ROLLS_BACK_EVERYTHING'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||s, ',' ORDER BY id),'NULL') END AS o2_disc
FROM public.bs_O2;
SELECT state FROM public.pg_branch WHERE branch_name = 'bs_O2b';
DROP TABLE public.bs_O2;

-- O.3 apply_branch with pure-delta DELETE only (INSERT then DELETE → NOP):
--     after apply MAIN baseline rows should be exactly untouched.
CREATE TABLE public.bs_O3 (id INT4 PRIMARY KEY, s TEXT);
INSERT INTO public.bs_O3 VALUES (1, 'm1'),(2, 'm2');
SELECT overlay_branch.create_branch('bs_O3b');
SELECT overlay_branch.use_branch('bs_O3b');
INSERT INTO public.bs_O3 VALUES (99, 'live_then_die');
DELETE FROM public.bs_O3 WHERE id = 99;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:O3_BRANCH_0_PURE_ROWS'
            ELSE 'FAIL_COUNT:'||count(*) END AS o3_br
FROM public.bs_O3 WHERE id = 99;
SELECT overlay_branch.apply_branch('bs_O3b');
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||s, ',' ORDER BY id) = '1:m1,2:m2'
            THEN 'PASS:O3_APPLY_PURE_NOP_NO_CONFLICT'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||s, ',' ORDER BY id),'NULL') END AS o3_main
FROM public.bs_O3;
DROP TABLE public.bs_O3;


DISCARD ALL;
SET client_min_messages = WARNING;
RESET overlay_branch.current;

/* =========================================================== Section P */
-- =====================================================================
-- ===== Section P: EMPTY MAIN heap — pure delta 100% of rows
-- =====            (MAIN has zero rows; overlay consists entirely of
-- =====             pure delta INSERTs, then UPDATE / DELETE,
-- =====             BranchScan reads correctly, apply propagates)
-- =====================================================================
CREATE TABLE public.bs_P (id INT4 PRIMARY KEY, t TEXT);
SELECT overlay_branch.create_branch('bs_Pb');
SELECT overlay_branch.use_branch('bs_Pb');
-- P.1 Insert 4 rows on totally empty MAIN
INSERT INTO public.bs_P VALUES (1, 'one'), (2, 'two'), (3, 'three'), (4, 'four');
SELECT CASE WHEN string_agg(id::text||':'||t, ',' ORDER BY id) = '1:one,2:two,3:three,4:four'
            THEN 'PASS:P1_READ_EMPTY_MAIN_4_PURE'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||t, ',' ORDER BY id),'NULL') END AS p1_rd
FROM public.bs_P;
-- P.2 DELETE WHERE id IN (2,4): 2 rows removed → 2 remain
DELETE FROM public.bs_P WHERE id IN (2, 4);
SELECT CASE WHEN string_agg(id::text||':'||t, ',' ORDER BY id) = '1:one,3:three'
            THEN 'PASS:P2_DELETE_2_OF_4_ON_EMPTY_MAIN'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||t, ',' ORDER BY id),'NULL') END AS p2_del
FROM public.bs_P;
-- P.3 RETURNING delete id=3
DELETE FROM public.bs_P WHERE id = 3 RETURNING id, t;
SELECT CASE WHEN string_agg(id::text||':'||t, ',' ORDER BY id) = '1:one'
            THEN 'PASS:P3_RETURNING_DELETE_3'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||t, ',' ORDER BY id),'NULL') END AS p3_ret
FROM public.bs_P;
-- P.4 apply: MAIN should now have id=1 only
SELECT overlay_branch.apply_branch('bs_Pb');
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||t, ',' ORDER BY id) = '1:one'
            THEN 'PASS:P4_APPLY_PROPAGATED_1_ROW'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||t, ',' ORDER BY id),'NULL') END AS p4_app
FROM public.bs_P;
DROP TABLE public.bs_P;


DISCARD ALL;
SET client_min_messages = WARNING;
RESET overlay_branch.current;

/* =========================================================== Section Q */
-- =====================================================================
-- ===== Section Q: Composite PK — pure delta DML
-- =====            (WR serialize_pk / P0 deparse both cover composite PK)
-- =====================================================================
CREATE TABLE public.bs_Q (a INT4, b TEXT, v TEXT, PRIMARY KEY (a, b));
INSERT INTO public.bs_Q VALUES (1, 'm-x', 'vx'), (2, 'm-y', 'vy');
SELECT overlay_branch.create_branch('bs_Qb');
SELECT overlay_branch.use_branch('bs_Qb');
INSERT INTO public.bs_Q VALUES (10, 'pd-a', 'A');
INSERT INTO public.bs_Q VALUES (10, 'pd-b', 'B');
INSERT INTO public.bs_Q VALUES (20, 'pd-c', 'C');
-- Q.1 Composite WHERE a=10 AND b='pd-b' — multi col IndexScan → indexqualorig.
DELETE FROM public.bs_Q WHERE a = 10 AND b = 'pd-b';
SELECT CASE WHEN string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b)
                 = '10:pd-a=A,20:pd-c=C'
            THEN 'PASS:Q1_COMPOSITE_PK_DELETE_1_OF_3'
            ELSE 'FAIL:'||COALESCE(string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b),'NULL') END AS q1_del
FROM public.bs_Q WHERE a >= 10;
-- Q.2 WHERE a=20 (partial PK prefix filter, non-exact composite match)
--     (still should hit the survivor)
DELETE FROM public.bs_Q WHERE a = 20 RETURNING a, b, v;
SELECT CASE WHEN string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b) = '10:pd-a=A'
            THEN 'PASS:Q2_PREFIX_PK_DELETE_20'
            ELSE 'FAIL:'||COALESCE(string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b),'NULL') END AS q2_pfx
FROM public.bs_Q WHERE a >= 10;
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b) = '1:m-x=vx,2:m-y=vy'
            THEN 'PASS:Q_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b),'NULL') END AS q_main
FROM public.bs_Q;
DROP TABLE public.bs_Q;

