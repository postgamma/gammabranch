/*
 * overlay_branch_scan_read.sql — BranchScan read-path shortcuts, de-SPI heap scan,
 *                                streaming iterator and relaxed PK predicate pushdown.
 *
 * Purpose:  the read paths that skip the delta JSON serialiser (RD-8), read MAIN
 *           without SPI (RD-7), stream overlay rows on demand (RD-6) or relax the
 *           P0 PK predicate must all stay latest-wins-correct.  Sections F-J were
 *           split out of overlay_branch_scan_pk.sql to keep each file focused.
 *
 * Coverage: Section F  RD-8 read-path shortcut — empty (M=0) and all-typed delta skip
 *                      Pass1 JSON text key serialisation; result matches MAIN / SRF;
 *           Section G  RD-8 pure-C PK serialiser — PK values carrying JSON
 *                      metacharacters (" and \) round-trip through the pure-C
 *                      escape_json key so the typed-key parse stays correct;
 *           Section H  RD-7 MAIN read de-SPI — direct heap scan + baserestrictinfo
 *                      pre-filter; the Pass2 orphan pass restores rows the branch
 *                      later rewrites, and scan.plan.qual re-filters as a fallback
 *                      (full scan / P0 point lookup / SRF equivalence / LIMIT);
 *           Section I  RD-6 streaming iterator — overlay rows produced on demand
 *                      (LIMIT early stop / cursor across Exec reuse / Pass1→Pass2
 *                      boundary / SRF early stop / correlated subplan rewind);
 *           Section J  P0 predicate pushdown extension — PK predicate relaxed from
 *                      "single-column equality" to IN / range / OR, with the Pass2
 *                      orphan pass restoring rows a branch INSERT brings into range.
 *
 * Dependencies: none beyond overlay_branch.  Sections are isolated by
 *           RESET overlay_branch.current and each owns its fixture table
 *           (bs_rd8 / bs_rd8q / bs_rd7 / bs_rd6 / bs_pkpd) and its branch.
 */

-- ===== 0. Prerequisite guard (same as the other regression files) =====
DO $$
DECLARE
  _s text := current_setting('shared_preload_libraries', true);
BEGIN
  IF _s NOT LIKE '%overlay_branch%' THEN
    RAISE EXCEPTION 'Postmaster started WITHOUT shared_preload_libraries=''overlay_branch''';
  END IF;
END $$;

-- ===== Idempotent cleanup =====
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.bs_rd8 CASCADE;
DROP TABLE IF EXISTS public.bs_rd8q CASCADE;
DROP TABLE IF EXISTS public.bs_rd7 CASCADE;
DROP TABLE IF EXISTS public.bs_rd6 CASCADE;
DROP TABLE IF EXISTS public.bs_pkpd CASCADE;
DROP EXTENSION IF EXISTS overlay_branch CASCADE;
RESET client_min_messages;

CREATE EXTENSION IF NOT EXISTS overlay_branch;
SET client_min_messages = WARNING;
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

