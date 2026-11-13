/*
 * overlay_branch_upsert_arbiter.sql — ON CONFLICT arbiter-selection semantics:
 *   a NON-primary UNIQUE arbiter, a targetless DO NOTHING, and the eager
 *   PK / other-unique conflict check once the arbiter does NOT match.
 *
 * Purpose:
 *   The write redirect must resolve the conflict target exactly like native
 *   PostgreSQL, and eagerly reject a candidate that the (unmatched) arbiter
 *   does not cover but a PRIMARY KEY or another UNIQUE index does.
 *
 * Coverage:
 *   AD.1  non-PK UNIQUE arbiter, arbiter MATCHES -> DO UPDATE the conflicted row
 *   AD.2  non-PK UNIQUE arbiter, arbiter MATCHES -> DO NOTHING skips
 *   AD.3  non-PK UNIQUE arbiter, arbiter MISSES + PK conflict   -> ERROR 23505
 *   AD.4  non-PK UNIQUE arbiter, arbiter MISSES + other UNIQUE  -> ERROR 23505
 *   AD.5  PK arbiter, other NON-PK UNIQUE conflict              -> ERROR 23505
 *   AD.6  composite PK + non-PK arbiter, PK conflict            -> ERROR 23505
 *   AD.7  targetless DO NOTHING: PK conflict skips, other UNIQUE
 *         conflict skips, no conflict inserts
 *   AD.8  overlay view + no phantom rows from the refused/skipped inserts
 *   AD.9  composite-PK non-PK arbiter DO UPDATE (arbiter matches) -> update
 *   AD.10 MAIN pristine after leaving the branch; apply materializes
 *   AE.1  KEYED partial-index arbiter           -> ERROR 0A000
 *   AE.2  KEYED expression-index arbiter        -> ERROR 0A000
 *   AE.3  the refused statements wrote no delta (overlay == MAIN)
 *   AE.4  a plain non-partial non-PK arbiter is still accepted
 *   AE.5  discard leaves MAIN pristine
 *   AF.1  arbiter path, branch DELETE frees the arbiter value -> the later
 *         UPSERT (u) must INSERT, not resurrect the stale MAIN row
 *   AF.2  arbiter path, branch UPDATE moved the arbiter value away -> the
 *         later UPSERT (u) on the old value must INSERT
 *   AF.3  arbiter path, branch DELETE + same-PK rebirth with a new arbiter
 *         value; a later UPSERT (u) on the freed value must INSERT
 *   AF.4  regression guard: a NON-arbiter UPDATE keeps the row in the
 *         arbiter path (DO UPDATE still applies)
 *   AF.5  targetless DO NOTHING: branch DELETE frees the value -> INSERT
 *   AF.6  targetless DO NOTHING: branch UPDATE moved the value; a live
 *         conflict still skips, the freed value inserts
 *   AF.7  MAIN pristine after leaving; apply materializes both tables
 *
 * Dependencies:
 *   none (standalone; creates and drops its own tables/branch).
 */

-- ===== 0. Prerequisite guard =====
DO $$
DECLARE
  _s text := current_setting('shared_preload_libraries', true);
BEGIN
  IF _s NOT LIKE '%overlay_branch%' THEN
    RAISE EXCEPTION 'Postmaster started WITHOUT shared_preload_libraries=''overlay_branch''.';
  END IF;
END $$;

-- ===== Idempotent cleanup =====
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.bs_AD_up;
DROP TABLE IF EXISTS public.bs_AD_cpk;
DROP TABLE IF EXISTS public.bs_AE_part;
DROP TABLE IF EXISTS public.bs_AE_expr;
DROP TABLE IF EXISTS public.bs_AE_plain;
DROP TABLE IF EXISTS public.bs_AF_up;
DROP TABLE IF EXISTS public.bs_AF_dn;
DROP EXTENSION IF EXISTS overlay_branch CASCADE;
RESET client_min_messages;

CREATE EXTENSION IF NOT EXISTS overlay_branch;
SET client_min_messages = warning;

/* =========================================================== Section AD */
CREATE TABLE public.bs_AD_up (id INT4 PRIMARY KEY, u INT4 UNIQUE, w INT4 UNIQUE, v TEXT);
INSERT INTO public.bs_AD_up VALUES (1,1,101,'m1'),(2,2,102,'m2'),(3,3,103,'m3');
CREATE TABLE public.bs_AD_cpk (a INT4, b INT4, u INT4 UNIQUE, v TEXT, PRIMARY KEY (a,b));
INSERT INTO public.bs_AD_cpk VALUES (1,1,1,'c11'),(2,2,2,'c22');
SELECT overlay_branch.create_branch('bs_AD_br');
SELECT overlay_branch.use_branch('bs_AD_br');

-- AD.1 non-PK UNIQUE arbiter MATCHES (u=1) -> DO UPDATE the MAIN row id=1
INSERT INTO public.bs_AD_up VALUES (9,1,999,'upd')
  ON CONFLICT (u) DO UPDATE SET v = EXCLUDED.v;

-- AD.2 non-PK UNIQUE arbiter MATCHES (u=2) -> DO NOTHING skips
INSERT INTO public.bs_AD_up VALUES (9,2,999,'nope')
  ON CONFLICT (u) DO NOTHING;

-- AD.3 arbiter MISSES (u=99) but PK id=3 conflicts -> ERROR 23505
DO $$
BEGIN
  BEGIN
    INSERT INTO public.bs_AD_up VALUES (3,99,999,'z')
      ON CONFLICT (u) DO NOTHING;
    RAISE EXCEPTION 'FAIL_AD3_NO_ERROR';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
END $$;

-- AD.4 arbiter MISSES (u=88) but another UNIQUE w=101 conflicts -> ERROR 23505
DO $$
BEGIN
  BEGIN
    INSERT INTO public.bs_AD_up VALUES (88,88,101,'z')
      ON CONFLICT (u) DO NOTHING;
    RAISE EXCEPTION 'FAIL_AD4_NO_ERROR';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
END $$;

-- AD.5 PK arbiter MISSES (id=88) but UNIQUE w=101 conflicts -> ERROR 23505
DO $$
BEGIN
  BEGIN
    INSERT INTO public.bs_AD_up VALUES (88,88,101,'z')
      ON CONFLICT (id) DO NOTHING;
    RAISE EXCEPTION 'FAIL_AD5_NO_ERROR';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
END $$;

-- AD.6 composite PK + non-PK arbiter MISSES (u=99) but PK (1,1) conflicts -> 23505
DO $$
BEGIN
  BEGIN
    INSERT INTO public.bs_AD_cpk VALUES (1,1,99,'z')
      ON CONFLICT (u) DO NOTHING;
    RAISE EXCEPTION 'FAIL_AD6_NO_ERROR';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
END $$;

-- AD.7 targetless DO NOTHING: every unique index is checked, conflicts skip
INSERT INTO public.bs_AD_up VALUES (1,11,111,'nope') ON CONFLICT DO NOTHING;   -- PK id=1 conflicts
INSERT INTO public.bs_AD_up VALUES (20,1,120,'nope') ON CONFLICT DO NOTHING;   -- u=1 conflicts
INSERT INTO public.bs_AD_up VALUES (30,30,130,'new') ON CONFLICT DO NOTHING;   -- no conflict -> insert

-- AD.8 overlay: only AD.1's update (id=1) and AD.7's insert (id=30) landed
SELECT CASE WHEN count(*) = 4
             AND max(v) FILTER (WHERE id=1) = 'upd'
             AND max(v) FILTER (WHERE id=30) = 'new'
       THEN 'PASS:AD8_OVERLAY'
       ELSE 'FAIL:'||coalesce(string_agg(id||'='||v, ',' ORDER BY id),'empty') END AS ad8
FROM overlay_branch.overlay_main_plus_delta('public.bs_AD_up') AS t(id int, u int, w int, v text);

SELECT CASE WHEN count(*) = 0 THEN 'PASS:AD8_NO_PHANTOM'
            ELSE 'FAIL_PHANTOM:'||coalesce(string_agg(id::text,',' ORDER BY id),'?') END AS ad8b
FROM overlay_branch.overlay_main_plus_delta('public.bs_AD_up') AS t(id int, u int, w int, v text)
WHERE id IN (9, 20, 88, 99);

-- AD.9 composite PK + non-PK arbiter MATCHES (u=1) -> DO UPDATE row (1,1)
INSERT INTO public.bs_AD_cpk VALUES (9,9,1,'c1upd')
  ON CONFLICT (u) DO UPDATE SET v = EXCLUDED.v;

SELECT CASE WHEN count(*) = 2
             AND max(v) FILTER (WHERE a=1 AND b=1) = 'c1upd'
             AND max(v) FILTER (WHERE a=2 AND b=2) = 'c22'
       THEN 'PASS:AD9_CPK_OVERLAY'
       ELSE 'FAIL:'||coalesce(string_agg(a||'.'||b||'='||v, ',' ORDER BY a,b),'empty') END AS ad9
FROM overlay_branch.overlay_main_plus_delta('public.bs_AD_cpk') AS t(a int, b int, u int, v text);

-- AD.10 MAIN pristine after leaving the branch
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 3
             AND max(v) FILTER (WHERE id=1) = 'm1'
             AND count(*) FILTER (WHERE id IN (30)) = 0
       THEN 'PASS:AD10_MAIN_PRISTINE'
       ELSE 'FAIL:'||coalesce(string_agg(id||'='||v, ',' ORDER BY id),'empty') END AS ad10
FROM public.bs_AD_up;
SELECT CASE WHEN count(*) = 2
             AND max(v) FILTER (WHERE a=1 AND b=1) = 'c11'
       THEN 'PASS:AD10_CPK_PRISTINE'
       ELSE 'FAIL:'||coalesce(string_agg(a||'.'||b||'='||v, ',' ORDER BY a,b),'empty') END AS ad10b
FROM public.bs_AD_cpk;

-- AD.11 apply materializes the two UPSERTs onto MAIN
SELECT overlay_branch.apply_branch('bs_AD_br');
SELECT CASE WHEN count(*) = 4
             AND max(v) FILTER (WHERE id=1) = 'upd'
             AND max(v) FILTER (WHERE id=30) = 'new'
       THEN 'PASS:AD11_APPLIED'
       ELSE 'FAIL:'||coalesce(string_agg(id||'='||v, ',' ORDER BY id),'empty') END AS ad11
FROM public.bs_AD_up;
SELECT CASE WHEN count(*) = 2
             AND max(v) FILTER (WHERE a=1 AND b=1) = 'c1upd'
       THEN 'PASS:AD11_CPK_APPLIED'
       ELSE 'FAIL:'||coalesce(string_agg(a||'.'||b||'='||v, ',' ORDER BY a,b),'empty') END AS ad11b
FROM public.bs_AD_cpk;

RESET client_min_messages;
DROP TABLE public.bs_AD_up;
DROP TABLE public.bs_AD_cpk;

/* =========================================================== Section AE */
-- AE.* An UPSERT whose arbiter is NOT exactly one non-partial UNIQUE index
--      over plain columns is refused (0A000) for a KEYED table too.  The
--      plain column probe cannot honour an index predicate (partial index)
--      or an expression: proceeding would silently match the wrong row (a
--      spurious DO NOTHING skip, or a DO UPDATE against a row the statement
--      never named).
SET client_min_messages = warning;

CREATE TABLE public.bs_AE_part (id INT4 PRIMARY KEY, u INT4, v TEXT);
CREATE UNIQUE INDEX bs_AE_part_u ON public.bs_AE_part (u) WHERE u > 0;
INSERT INTO public.bs_AE_part VALUES (1,10,'m1'),(2,-5,'m2');
CREATE TABLE public.bs_AE_expr (id INT4 PRIMARY KEY, v TEXT);
CREATE UNIQUE INDEX bs_AE_expr_v ON public.bs_AE_expr (lower(v));
INSERT INTO public.bs_AE_expr VALUES (1,'a');
CREATE TABLE public.bs_AE_plain (id INT4 PRIMARY KEY, u INT4 UNIQUE, v TEXT);
INSERT INTO public.bs_AE_plain VALUES (1,1,'p1');

SELECT overlay_branch.create_branch('bs_AE_br');
SELECT overlay_branch.use_branch('bs_AE_br');

-- AE.1 partial-index arbiter (keyed) -> 0A000.  The candidate u=-5 is OUTSIDE
--      the predicate domain; the old rebind matched MAIN's (2,-5) and would
--      have updated the wrong row instead of inserting.
DO $$
BEGIN
  BEGIN
    INSERT INTO public.bs_AE_part VALUES (3,-5,'z')
      ON CONFLICT (u) WHERE u > 0 DO UPDATE SET v = 'upd';
    RAISE EXCEPTION 'FAIL_AE1_PARTIAL_UPSERT_WAS_ALLOWED';
  EXCEPTION WHEN feature_not_supported THEN NULL;
  END;
END $$;

-- AE.2 expression-index arbiter (keyed) -> 0A000
DO $$
BEGIN
  BEGIN
    INSERT INTO public.bs_AE_expr VALUES (1,'A')
      ON CONFLICT (lower(v)) DO NOTHING;
    RAISE EXCEPTION 'FAIL_AE2_EXPR_UPSERT_WAS_ALLOWED';
  EXCEPTION WHEN feature_not_supported THEN NULL;
  END;
END $$;

-- AE.3 neither refused statement wrote a delta row (overlay == MAIN)
SELECT CASE WHEN string_agg(id||':'||u||':'||v, ',' ORDER BY id) = '1:10:m1,2:-5:m2'
            THEN 'PASS:AE3_PARTIAL_NO_DELTA'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||u||':'||v, ',' ORDER BY id),'empty') END AS ae3
FROM public.bs_AE_part;
SELECT CASE WHEN string_agg(id||':'||v, ',' ORDER BY id) = '1:a'
            THEN 'PASS:AE3_EXPR_NO_DELTA'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||v, ',' ORDER BY id),'empty') END AS ae3b
FROM public.bs_AE_expr;

-- AE.4 a plain non-partial non-PK UNIQUE arbiter is still accepted
INSERT INTO public.bs_AE_plain VALUES (2,1,'p2')
  ON CONFLICT (u) DO UPDATE SET v = 'p3';
SELECT CASE WHEN string_agg(id||':'||u||':'||v, ',' ORDER BY id) = '1:1:p3'
            THEN 'PASS:AE4_PLAIN_ARBITER_OK'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||u||':'||v, ',' ORDER BY id),'empty') END AS ae4
FROM public.bs_AE_plain;

-- AE.5 discard leaves MAIN pristine (the refused statements never applied)
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SELECT overlay_branch.discard_branch('bs_AE_br');
SELECT CASE WHEN string_agg(id||':'||v, ',' ORDER BY id) = '1:p1'
            THEN 'PASS:AE5_MAIN_PRISTINE'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||v, ',' ORDER BY id),'empty') END AS ae5
FROM public.bs_AE_plain;

RESET client_min_messages;
DROP TABLE public.bs_AE_part;
DROP TABLE public.bs_AE_expr;
DROP TABLE public.bs_AE_plain;

/* =========================================================== Section AF */
-- AF.* DELTA-FIRST SHADOW.  Once the branch has DELETEd a row, or UPDATEd it
--      so its arbiter value moved away, the MAIN row it shadowed must no
--      longer be found by the arbiter probe: the candidate must INSERT, not
--      DO UPDATE a row the branch already logically removed / rewrote.
--      Phase I cannot see that -- it matches the delta row's *stored image*,
--      which a tombstone (op='D') does not have and an op='U' rewrites -- so
--      the MAIN probe must itself exclude any row the delta shadows (its
--      version token equals the delta row's old_version).
SET client_min_messages = warning;

CREATE TABLE public.bs_AF_up (id INT4 PRIMARY KEY, u INT4 UNIQUE, v TEXT);
INSERT INTO public.bs_AF_up VALUES (1,10,'m1'),(2,20,'m2'),(3,30,'m3'),(4,40,'m4');
CREATE TABLE public.bs_AF_dn (id INT4 PRIMARY KEY, u INT4 UNIQUE, v TEXT);
INSERT INTO public.bs_AF_dn VALUES (1,10,'m1'),(2,20,'m2'),(3,30,'m3');

SELECT overlay_branch.create_branch('bs_AF_br');
SELECT overlay_branch.use_branch('bs_AF_br');

-- AF.1 branch DELETE frees u=10; the later UPSERT (u) must INSERT (11,10)
DELETE FROM public.bs_AF_up WHERE id = 1;
INSERT INTO public.bs_AF_up VALUES (11,10,'a1')
  ON CONFLICT (u) DO UPDATE SET v = EXCLUDED.v;

-- AF.2 branch UPDATE moved u away from 20; the later UPSERT (u) on the old
--      value must INSERT (12,20)
UPDATE public.bs_AF_up SET u = 200 WHERE id = 2;
INSERT INTO public.bs_AF_up VALUES (12,20,'a2')
  ON CONFLICT (u) DO UPDATE SET v = EXCLUDED.v;

-- AF.3 branch DELETE then same-PK rebirth with a new arbiter (u=300); the
--      later UPSERT (u) on the freed value must INSERT (13,30)
DELETE FROM public.bs_AF_up WHERE id = 3;
INSERT INTO public.bs_AF_up VALUES (3,300,'a3b');
INSERT INTO public.bs_AF_up VALUES (13,30,'a3')
  ON CONFLICT (u) DO UPDATE SET v = EXCLUDED.v;

-- AF.4 regression guard: a NON-arbiter UPDATE keeps the row in the arbiter
--      path; the UPSERT (u=40) must DO UPDATE id=4 (never insert id=14)
UPDATE public.bs_AF_up SET v = 'm4u' WHERE id = 4;
INSERT INTO public.bs_AF_up VALUES (14,40,'a4')
  ON CONFLICT (u) DO UPDATE SET v = EXCLUDED.v;

SELECT CASE WHEN string_agg(id||':'||u||':'||v, ',' ORDER BY id)
                 = '2:200:m2,3:300:a3b,4:40:a4,11:10:a1,12:20:a2,13:30:a3'
            THEN 'PASS:AF1_4_ARBITER_SHADOW'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||u||':'||v, ',' ORDER BY id),'empty') END AS af14
FROM overlay_branch.overlay_main_plus_delta('public.bs_AF_up') AS t(id int, u int, v text);

-- AF.5 targetless DO NOTHING: branch DELETE frees u=10 -> the candidate INSERTs
DELETE FROM public.bs_AF_dn WHERE id = 1;
INSERT INTO public.bs_AF_dn VALUES (11,10,'n1') ON CONFLICT DO NOTHING;

-- AF.6 branch UPDATE moved u away from 20 -> INSERT (12,20); a still-live
--      conflict (u=30) must still skip
UPDATE public.bs_AF_dn SET u = 200 WHERE id = 2;
INSERT INTO public.bs_AF_dn VALUES (12,20,'n2') ON CONFLICT DO NOTHING;
INSERT INTO public.bs_AF_dn VALUES (99,30,'dup') ON CONFLICT DO NOTHING;

SELECT CASE WHEN string_agg(id||':'||u||':'||v, ',' ORDER BY id)
                 = '2:200:m2,3:30:m3,11:10:n1,12:20:n2'
            THEN 'PASS:AF5_6_TARGETLESS_SHADOW'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||u||':'||v, ',' ORDER BY id),'empty') END AS af56
FROM overlay_branch.overlay_main_plus_delta('public.bs_AF_dn') AS t(id int, u int, v text);

-- AF.7 MAIN pristine after leaving the branch; apply materializes both tables
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN string_agg(id||':'||u||':'||v, ',' ORDER BY id) = '1:10:m1,2:20:m2,3:30:m3,4:40:m4'
            THEN 'PASS:AF7_MAIN_PRISTINE'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||u||':'||v, ',' ORDER BY id),'empty') END AS af7
FROM public.bs_AF_up;

SELECT overlay_branch.apply_branch('bs_AF_br');
SELECT CASE WHEN string_agg(id||':'||u||':'||v, ',' ORDER BY id)
                 = '2:200:m2,3:300:a3b,4:40:a4,11:10:a1,12:20:a2,13:30:a3'
            THEN 'PASS:AF7_APPLIED'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||u||':'||v, ',' ORDER BY id),'empty') END AS af7b
FROM public.bs_AF_up;
SELECT CASE WHEN string_agg(id||':'||u||':'||v, ',' ORDER BY id)
                 = '2:200:m2,3:30:m3,11:10:n1,12:20:n2'
            THEN 'PASS:AF7_DN_APPLIED'
            ELSE 'FAIL:'||coalesce(string_agg(id||':'||u||':'||v, ',' ORDER BY id),'empty') END AS af7c
FROM public.bs_AF_dn;

RESET client_min_messages;
DROP TABLE public.bs_AF_up;
DROP TABLE public.bs_AF_dn;
