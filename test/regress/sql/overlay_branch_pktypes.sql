/*
 * overlay_branch_pktypes.sql — PK identity across container, range and JSON types.
 *
 * Purpose:  the delta identity key is the canonical serialized PK, so every
 *           supported PK type must render to one stable key and round-trip its
 *           payload: ARRAY elements, INTERVAL, RANGE / multirange, money,
 *           jsonb PK, and json/jsonb payload structure.
 *
 * Coverage: R28 ARRAY PK (float8[] / numeric[] / bpchar[]);
 *           R29 INTERVAL PK (bare, array, DOMAIN over interval);
 *           R30 RANGE / multirange PK (numrange / tsrange / tstzrange /
 *               daterange + nummultirange / tsmultirange and their arrays);
 *           R31 money PK and non-key column canonicalization;
 *           R32 jsonb (JSONBOID) PK identity canonicalization;
 *           R33 json/jsonb payload structure round-trip.
 *
 * Dependencies: none beyond overlay_branch.
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
DROP TABLE IF EXISTS public.t_r28a CASCADE;
DROP TABLE IF EXISTS public.t_r28b CASCADE;
DROP TABLE IF EXISTS public.t_r28c CASCADE;
DROP TABLE IF EXISTS public.t_r29a CASCADE;
DROP TABLE IF EXISTS public.t_r29b CASCADE;
DROP TABLE IF EXISTS public.t_r29c CASCADE;
DROP TABLE IF EXISTS public.t_r30a CASCADE;
DROP TABLE IF EXISTS public.t_r30b CASCADE;
DROP TABLE IF EXISTS public.t_r30t CASCADE;
DROP TABLE IF EXISTS public.t_r30z CASCADE;
DROP TABLE IF EXISTS public.t_r30d CASCADE;
DROP TABLE IF EXISTS public.t_r30m CASCADE;
DROP TABLE IF EXISTS public.t_r30mb CASCADE;
DROP TABLE IF EXISTS public.t_r30tm CASCADE;
DROP TABLE IF EXISTS public.t_r31p CASCADE;
DROP TABLE IF EXISTS public.t_r31e CASCADE;
DROP TABLE IF EXISTS public.t_r32p CASCADE;
DROP TABLE IF EXISTS public.t_r33p CASCADE;
DROP TABLE IF EXISTS public.t_r33u CASCADE;
DROP DOMAIN IF EXISTS public.d_r29_iv CASCADE;
DROP DOMAIN IF EXISTS public.jb_r33 CASCADE;
DROP EXTENSION IF EXISTS overlay_branch CASCADE;
RESET client_min_messages;

CREATE EXTENSION IF NOT EXISTS overlay_branch;
SET client_min_messages = WARNING;
-- =====================================================================
-- Section R28 : ARRAY PKs of a canonicalizable element type.
--
--   array_eq compares element-wise, so ARRAY[1.0] and ARRAY[1.00] on a
--   NUMERIC[] key (ARRAY[-0.0] / ARRAY[0.0] on a FLOAT8[] key, and
--   ARRAY['a'] / ARRAY['a '] on a BPCHAR[] key) are '='-equal, yet array_out
--   prints "{1.00}"/"{1.0}", "{-}"/"{0}", and "{a}"/"{"a "}".  The scalar
--   identity path folds numeric scale, float signed-zero, and bpchar trailing
--   blanks; the array path must apply the SAME fold element-wise, or the two
--   spellings land on two identity keys and a duplicate branch INSERT
--   silently succeeds.  The rebuild keeps dimensionality and lower bounds:
--   array_eq treats a different shape/bound as a DIFFERENT value, so
--   flattening them would instead invent a false 23505.
--
-- COVERAGE:
--   R28_A : float8[] PK, ARRAY[-0.0] vs ARRAY[0.0]                -> 23505
--   R28_B : numeric[] PK, ARRAY[1.0] vs ARRAY[1.00]               -> 23505
--   R28_C : different shape / lower bound -> distinct; exact dup  -> 23505
--   R28_D : const probe ARRAY[1.0] matches the normalized stored key
--   R28_E : MAIN pristine; apply materializes the canonical rows
--   R28_F : bpchar[] PK, ARRAY['a'] vs ARRAY['a ']                -> 23505
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r28a (k float8[] PRIMARY KEY, v text);
CREATE TABLE public.t_r28b (k numeric[] PRIMARY KEY, v text);
CREATE TABLE public.t_r28c (k bpchar[] PRIMARY KEY, v text);
SELECT overlay_branch.create_branch('br_r28');
SELECT overlay_branch.use_branch('br_r28');

-- R28_A: float8[] PK — ARRAY[-0.0] and ARRAY[0.0] are '='-equal -> one key.
INSERT INTO public.t_r28a VALUES (ARRAY[-0.0::float8], 'neg');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r28a VALUES (ARRAY[0.0::float8], 'pos');
        RAISE EXCEPTION 'FAIL_R28_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R28_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'neg'
            THEN 'PASS:R28_A_FLOAT_ARRAY_NEGZERO_23505'
            ELSE 'FAIL:R28_A rows=' || count(*) || ' v=' || min(v) END AS r28_a
  FROM public.t_r28a WHERE k = ARRAY[0.0::float8];

-- R28_B: numeric[] PK — ARRAY[1.0] and ARRAY[1.00] are '='-equal -> one key.
INSERT INTO public.t_r28b VALUES (ARRAY[1.0::numeric], 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r28b VALUES (ARRAY[1.00::numeric], 'b');
        RAISE EXCEPTION 'FAIL_R28_B_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R28_B: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'a'
            THEN 'PASS:R28_B_NUMERIC_ARRAY_IDENTITY_23505'
            ELSE 'FAIL:R28_B rows=' || count(*) || ' v=' || min(v) END AS r28_b
  FROM public.t_r28b WHERE k = ARRAY[1.0::numeric];

-- R28_C: a different shape / lower bound is a DIFFERENT value (array_eq), so
--        it must NOT collapse to a flattened key; an exact duplicate still
--        raises 23505.
INSERT INTO public.t_r28b VALUES ('{{1,2},{3,4}}'::numeric[], 'mdim');
INSERT INTO public.t_r28b VALUES ('{1,2,3,4}'::numeric[], 'flat');
INSERT INTO public.t_r28b VALUES ('[2:3]={1,2}'::numeric[], 'lb2');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r28b VALUES ('{1,2,3,4}'::numeric[], 'dup');
        RAISE EXCEPTION 'FAIL_R28_C_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R28_C: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 4 THEN 'PASS:R28_C_SHAPE_LBOUND_DISTINCT'
            ELSE 'FAIL:R28_C rows=' || count(*) END AS r28_c
  FROM public.t_r28b;

-- R28_D: const probe with an '='-equal but differently-scaled array must
--        match the normalized stored key (drives the O(1) delta lookup).
UPDATE public.t_r28b SET v = 'z' WHERE k = ARRAY[1.0::numeric];
SELECT CASE WHEN count(*) = 1 AND min(v) = 'z'
            THEN 'PASS:R28_D_CONST_PROBE_MATCHES'
            ELSE 'FAIL:R28_D rows=' || count(*) || ' v=' || min(v) END AS r28_d
  FROM public.t_r28b WHERE k = ARRAY[1.00::numeric];

-- R28_F: bpchar[] PK — array_eq ignores each element's trailing blanks while
--        array_out keeps them, so ARRAY['a'] and ARRAY['a '] are '='-equal
--        -> one key.  A genuinely different value stays distinct, and a const
--        probe with a differently-padded array matches the normalized key.
INSERT INTO public.t_r28c VALUES (ARRAY['a'::bpchar], 'p');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r28c VALUES (ARRAY['a '::bpchar], 'p2');
        RAISE EXCEPTION 'FAIL_R28_F_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R28_F: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
INSERT INTO public.t_r28c VALUES (ARRAY['aa'::bpchar], 'q');
UPDATE public.t_r28c SET v = 'z' WHERE k = ARRAY['a '::bpchar];
SELECT CASE WHEN count(*) = 2
            THEN 'PASS:R28_F_BPCHAR_ARRAY_RTRIM_23505'
            ELSE 'FAIL:R28_F rows=' || count(*) END AS r28_f
  FROM public.t_r28c;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'z'
            THEN 'PASS:R28_F_BPCHAR_CONST_PROBE_MATCHES'
            ELSE 'FAIL:R28_F_PROBE rows=' || count(*) || ' v=' || min(v) END AS r28_f2
  FROM public.t_r28c WHERE k = ARRAY['a '::bpchar];

-- R28_E: MAIN pristine until apply; apply materializes the canonical rows.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R28_E_MAIN_PRISTINE'
            ELSE 'FAIL:R28_E rows=' || count(*) END AS r28_e
  FROM public.t_r28a;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R28_E_BPCHAR_MAIN_PRISTINE'
            ELSE 'FAIL:R28_E_BPCHAR rows=' || count(*) END AS r28_e3
  FROM public.t_r28c;
SELECT overlay_branch.apply_branch('br_r28');
SELECT CASE WHEN count(*) = 1 AND min(v) = 'neg'
            THEN 'PASS:R28_E_APPLY_MATERIALIZED'
            ELSE 'FAIL:R28_E rows=' || count(*) END AS r28_e2
  FROM public.t_r28a;
SELECT CASE WHEN count(*) = 2 AND min(v) = 'q'
            THEN 'PASS:R28_E_BPCHAR_APPLY_MATERIALIZED'
            ELSE 'FAIL:R28_E_BPCHAR rows=' || count(*) || ' v=' || min(v) END AS r28_e4
  FROM public.t_r28c;

-- R28 teardown (branch left in the terminal 'applied' state).
DROP TABLE public.t_r28a;
DROP TABLE public.t_r28b;
DROP TABLE public.t_r28c;
RESET client_min_messages;

-- =====================================================================
-- Section R29 : INTERVAL PKs fold to their '='-equality class.
--
--   interval equality (interval_cmp) folds months into 30 days and days
--   into 24 hours and compares the resulting microsecond span, so
--   '1 mon' = '30 days' and '1 day' = '24 hours' are both true -- yet
--   interval_out prints them all differently ("1 mon" / "30 days",
--   "1 day" / "24:00:00").  IntervalStyle pinning cannot close this: the
--   split lives in the VALUE, not the rendering.  Unless the serializer
--   folds the value to a bare time span (month = day = 0) the two spellings
--   land on two identity keys and a duplicate branch INSERT silently
--   succeeds.  The fold is element-wise for an interval[] key and applies
--   through a domain over interval too.
--
-- COVERAGE:
--   R29_A : interval PK, '1 mon'/'30 days' and '1 day'/'24 hours'   -> 23505
--   R29_B : const probe with the other spelling matches the stored key
--   R29_C : genuinely distinct intervals stay distinct; a domain folds
--   R29_D : interval[] PK element fold -> 23505; a different shape stays
--   R29_E : MAIN pristine; apply materializes the folded rows
-- =====================================================================
SET client_min_messages = WARNING;
CREATE DOMAIN public.d_r29_iv AS interval;
CREATE TABLE public.t_r29a (k interval PRIMARY KEY, v text);
CREATE TABLE public.t_r29b (k interval[] PRIMARY KEY, v text);
CREATE TABLE public.t_r29c (k public.d_r29_iv PRIMARY KEY, v text);
SELECT overlay_branch.create_branch('br_r29');
SELECT overlay_branch.use_branch('br_r29');

-- R29_A: '1 mon' and '30 days' (and '1 day' / '24 hours') are one value.
INSERT INTO public.t_r29a VALUES ('1 mon'::interval, 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r29a VALUES ('30 days'::interval, 'b');
        RAISE EXCEPTION 'FAIL_R29_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R29_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
INSERT INTO public.t_r29a VALUES ('1 day'::interval, 'c');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r29a VALUES ('24 hours'::interval, 'd');
        RAISE EXCEPTION 'FAIL_R29_A2_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R29_A2: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R29_A_INTERVAL_IDENTITY_23505'
            ELSE 'FAIL:R29_A rows=' || count(*) END AS r29_a
  FROM public.t_r29a;

-- R29_B: a const probe using the OTHER spelling must still reach the stored
--        key (drives the O(1) delta lookup / const serializer).
UPDATE public.t_r29a SET v = 'z' WHERE k = '30 days'::interval;
UPDATE public.t_r29a SET v = 'y' WHERE k = '24 hours'::interval;
SELECT CASE WHEN count(*) = 2 AND min(v) = 'y'
            THEN 'PASS:R29_B_CONST_PROBE_MATCHES'
            ELSE 'FAIL:R29_B rows=' || count(*) || ' v=' || min(v) END AS r29_b
  FROM public.t_r29a;

-- R29_C: genuinely distinct intervals must NOT collide; a domain over
--        interval folds identically.
INSERT INTO public.t_r29a VALUES ('2 mons'::interval, 'm2');
INSERT INTO public.t_r29a VALUES ('25 hours'::interval, 'h25');
SELECT CASE WHEN count(*) = 4 THEN 'PASS:R29_C_DISTINCT_DO_NOT_COLLIDE'
            ELSE 'FAIL:R29_C rows=' || count(*) END AS r29_c
  FROM public.t_r29a;
INSERT INTO public.t_r29c VALUES ('1 mon'::interval, 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r29c VALUES ('30 days'::interval, 'b');
        RAISE EXCEPTION 'FAIL_R29_C_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R29_C: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R29_C_DOMAIN_FOLDS'
            ELSE 'FAIL:R29_C_DOMAIN rows=' || count(*) END AS r29_c2
  FROM public.t_r29c;

-- R29_D: interval[] PK folds element-wise; a different shape is distinct.
INSERT INTO public.t_r29b VALUES (ARRAY['1 day'::interval], 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r29b VALUES (ARRAY['24 hours'::interval], 'b');
        RAISE EXCEPTION 'FAIL_R29_D_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R29_D: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
INSERT INTO public.t_r29b VALUES ('{{1 day},{2 days}}'::interval[], 'mdim');
INSERT INTO public.t_r29b VALUES ('{1 day,2 days}'::interval[], 'flat');
SELECT CASE WHEN count(*) = 3 THEN 'PASS:R29_D_ARRAY_FOLD_SHAPE_DISTINCT'
            ELSE 'FAIL:R29_D rows=' || count(*) END AS r29_d
  FROM public.t_r29b;

-- R29_E: MAIN pristine until apply; apply materializes the folded rows.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R29_E_MAIN_PRISTINE'
            ELSE 'FAIL:R29_E rows=' || count(*) END AS r29_e
  FROM public.t_r29a;
SELECT overlay_branch.apply_branch('br_r29');
SELECT CASE WHEN count(*) = 4 THEN 'PASS:R29_E_APPLY_MATERIALIZED'
            ELSE 'FAIL:R29_E rows=' || count(*) END AS r29_e2
  FROM public.t_r29a;

-- R29 teardown (branch left in the terminal 'applied' state).
DROP TABLE public.t_r29a;
DROP TABLE public.t_r29b;
DROP TABLE public.t_r29c;
DROP DOMAIN public.d_r29_iv;
RESET client_min_messages;

-- =====================================================================
-- Section R30 : RANGE PKs fold through their bounds.
--
--   A range renders each bound with its SUBTYPE's output function and
--   range_eq compares bounds with the subtype's '=' operator, so the
--   scalar identity splits reappear one level down: a numrange holds
--   '[1.0,2.0)' and '[1,2)' as ONE '='-equal value that prints two ways,
--   exactly like a bare numeric.  Unless the serializer folds each finite
--   bound to its '='-class representative the two spellings land on two
--   identity keys and a duplicate branch INSERT silently succeeds.  A range
--   whose subtype's output reads a session GUC (tsrange / daterange /
--   tstzrange) is pinned through the subtype so it renders canonically
--   regardless of the session.
--
-- COVERAGE:
--   R30_A : numrange PK '[1.0,2.0)' / '[1,2)'                         -> 23505
--   R30_B : const probe with the folded spelling reaches the stored key
--   R30_C : genuinely distinct ranges (endpoints / inclusivity / empty) stay
--   R30_D : numrange[] PK element fold -> 23505; a distinct value stays
--   R30_E : tsrange (DateStyle) / tstzrange (TimeZone) / daterange
--           (DateStyle) -- same value under another GUC -> 23505
--   R30_G : nummultirange PK fold -> 23505; a distinct value stays; const
--           probe with the folded spelling reaches the stored key
--   R30_H : nummultirange[] PK element fold -> 23505
--   R30_I : tsmultirange (DateStyle) -- same value under another GUC -> 23505
--   R30_F : MAIN pristine; apply materializes the folded rows
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r30a (k numrange PRIMARY KEY, v text);
CREATE TABLE public.t_r30b (k numrange[] PRIMARY KEY, v text);
CREATE TABLE public.t_r30t (k tsrange PRIMARY KEY, v text);
CREATE TABLE public.t_r30z (k tstzrange PRIMARY KEY, v text);
CREATE TABLE public.t_r30d (k daterange PRIMARY KEY, v text);
CREATE TABLE public.t_r30m (k nummultirange PRIMARY KEY, v text);
CREATE TABLE public.t_r30mb (k nummultirange[] PRIMARY KEY, v text);
CREATE TABLE public.t_r30tm (k tsmultirange PRIMARY KEY, v text);
SELECT overlay_branch.create_branch('br_r30');
SELECT overlay_branch.use_branch('br_r30');

-- R30_A: '[1.0,2.0)' and '[1,2)' are '='-equal numrange values.
INSERT INTO public.t_r30a VALUES ('[1.0,2.0)'::numrange, 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r30a VALUES ('[1,2)'::numrange, 'b');
        RAISE EXCEPTION 'FAIL_R30_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R30_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R30_A_NUMRANGE_IDENTITY_23505'
            ELSE 'FAIL:R30_A rows=' || count(*) END AS r30_a
  FROM public.t_r30a;

-- R30_B: a const probe with the folded spelling must still reach the stored
--        key (drives the O(1) delta lookup / const serializer).
UPDATE public.t_r30a SET v = 'z' WHERE k = '[1,2)'::numrange;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'z'
            THEN 'PASS:R30_B_CONST_PROBE_MATCHES'
            ELSE 'FAIL:R30_B rows=' || count(*) || ' v=' || min(v) END AS r30_b
  FROM public.t_r30a;

-- R30_C: genuinely distinct ranges must NOT collide.
INSERT INTO public.t_r30a VALUES ('[1,3)'::numrange, 'c');
INSERT INTO public.t_r30a VALUES ('(1,2]'::numrange, 'd');
INSERT INTO public.t_r30a VALUES ('empty'::numrange, 'e');
SELECT CASE WHEN count(*) = 4 THEN 'PASS:R30_C_DISTINCT_DO_NOT_COLLIDE'
            ELSE 'FAIL:R30_C rows=' || count(*) END AS r30_c
  FROM public.t_r30a;

-- R30_D: numrange[] PK folds element-wise; a distinct value stays distinct.
INSERT INTO public.t_r30b VALUES (ARRAY['[1.0,2.0)'::numrange], 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r30b VALUES (ARRAY['[1,2)'::numrange], 'b');
        RAISE EXCEPTION 'FAIL_R30_D_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R30_D: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
INSERT INTO public.t_r30b VALUES (ARRAY['[5,6)'::numrange], 'c');
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R30_D_ARRAY_FOLD_DISTINCT'
            ELSE 'FAIL:R30_D rows=' || count(*) END AS r30_d
  FROM public.t_r30b;

-- R30_E: a range over a GUC-sensitive subtype must serialize canonically, so
--        the SAME value written under another DateStyle / TimeZone collides.
SET DateStyle = 'ISO, MDY';
INSERT INTO public.t_r30t VALUES ('[2020-01-01 00:00:00,2020-01-02 00:00:00)'::tsrange, 'iso');
SET DateStyle = 'SQL, DMY';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r30t VALUES ('[2020-01-01 00:00:00,2020-01-02 00:00:00)'::tsrange, 'sql');
        RAISE EXCEPTION 'FAIL_R30_E_TS_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R30_E_TS: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
RESET DateStyle;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R30_E_TSRANGE_DATESTYLE_23505'
            ELSE 'FAIL:R30_E_TS rows=' || count(*) END AS r30_e
  FROM public.t_r30t;

SET TimeZone = 'UTC';
INSERT INTO public.t_r30z VALUES ('[2020-01-01 00:00:00+00,2020-01-02 00:00:00+00)'::tstzrange, 'utc');
SET TimeZone = 'America/New_York';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r30z VALUES ('[2020-01-01 00:00:00+00,2020-01-02 00:00:00+00)'::tstzrange, 'ny');
        RAISE EXCEPTION 'FAIL_R30_E_TSTZ_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R30_E_TSTZ: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
RESET TimeZone;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R30_E_TSTZRANGE_TIMEZONE_23505'
            ELSE 'FAIL:R30_E_TSTZ rows=' || count(*) END AS r30_e2
  FROM public.t_r30z;

SET DateStyle = 'ISO, MDY';
INSERT INTO public.t_r30d VALUES ('[2020-01-01,2020-01-02)'::daterange, 'iso');
SET DateStyle = 'SQL, DMY';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r30d VALUES ('[2020-01-01,2020-01-02)'::daterange, 'sql');
        RAISE EXCEPTION 'FAIL_R30_E_DR_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R30_E_DR: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
RESET DateStyle;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R30_E_DATERANGE_DATESTYLE_23505'
            ELSE 'FAIL:R30_E_DR rows=' || count(*) END AS r30_e3
  FROM public.t_r30d;

-- R30_G: a multirange renders each element range through range_out, and
--        multirange_eq compares element ranges bound-wise, so '{[1.0,2.0)}'
--        and '{[1,2)}' are '='-equal yet print differently.  A distinct value
--        must stay distinct and a folded-spelling const probe must match.
INSERT INTO public.t_r30m VALUES ('{[1.0,2.0)}'::nummultirange, 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r30m VALUES ('{[1,2)}'::nummultirange, 'b');
        RAISE EXCEPTION 'FAIL_R30_G_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R30_G: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
INSERT INTO public.t_r30m VALUES ('{[5,6)}'::nummultirange, 'c');
UPDATE public.t_r30m SET v = 'z' WHERE k = '{[1,2)}'::nummultirange;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R30_G_NUMMULTIRANGE_IDENTITY_23505'
            ELSE 'FAIL:R30_G rows=' || count(*) END AS r30_g
  FROM public.t_r30m;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'z'
            THEN 'PASS:R30_G_CONST_PROBE_MATCHES'
            ELSE 'FAIL:R30_G_PROBE rows=' || count(*) || ' v=' || min(v) END AS r30_g2
  FROM public.t_r30m WHERE k = '{[1.0,2.0)}'::nummultirange;

-- R30_H: nummultirange[] PK folds element-wise; a distinct value stays distinct.
INSERT INTO public.t_r30mb VALUES (ARRAY['{[1.0,2.0)}'::nummultirange], 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r30mb VALUES (ARRAY['{[1,2)}'::nummultirange], 'b');
        RAISE EXCEPTION 'FAIL_R30_H_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R30_H: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
INSERT INTO public.t_r30mb VALUES (ARRAY['{[5,6)}'::nummultirange], 'c');
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R30_H_ARRAY_FOLD_DISTINCT'
            ELSE 'FAIL:R30_H rows=' || count(*) END AS r30_h
  FROM public.t_r30mb;

-- R30_I: a multirange over a GUC-sensitive subtype must serialize canonically,
--        so the SAME value written under another DateStyle collides.
SET DateStyle = 'ISO, MDY';
INSERT INTO public.t_r30tm VALUES ('{[2020-01-01 00:00:00,2020-01-02 00:00:00)}'::tsmultirange, 'iso');
SET DateStyle = 'SQL, DMY';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r30tm VALUES ('{[2020-01-01 00:00:00,2020-01-02 00:00:00)}'::tsmultirange, 'sql');
        RAISE EXCEPTION 'FAIL_R30_I_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R30_I: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
RESET DateStyle;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R30_I_TSMULTIRANGE_DATESTYLE_23505'
            ELSE 'FAIL:R30_I rows=' || count(*) END AS r30_i
  FROM public.t_r30tm;

-- R30_F: MAIN pristine until apply; apply materializes the folded rows.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R30_F_MAIN_PRISTINE'
            ELSE 'FAIL:R30_F rows=' || count(*) END AS r30_f
  FROM public.t_r30a;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R30_F_MULTIRANGE_MAIN_PRISTINE'
            ELSE 'FAIL:R30_F_MR rows=' || count(*) END AS r30_f3
  FROM public.t_r30m;
SELECT overlay_branch.apply_branch('br_r30');
SELECT CASE WHEN count(*) = 4 THEN 'PASS:R30_F_APPLY_MATERIALIZED'
            ELSE 'FAIL:R30_F rows=' || count(*) END AS r30_f2
  FROM public.t_r30a;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R30_F_MULTIRANGE_APPLY_MATERIALIZED'
            ELSE 'FAIL:R30_F_MR rows=' || count(*) END AS r30_f4
  FROM public.t_r30m;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'z'
            THEN 'PASS:R30_F_MULTIRANGE_APPLY_UPDATED'
            ELSE 'FAIL:R30_F_MR_UPD rows=' || count(*) || ' v=' || min(v) END AS r30_f5
  FROM public.t_r30m WHERE k = '{[1,2)}'::nummultirange;

-- R30 teardown (branch left in the terminal 'applied' state).
DROP TABLE public.t_r30a;
DROP TABLE public.t_r30b;
DROP TABLE public.t_r30t;
DROP TABLE public.t_r30z;
DROP TABLE public.t_r30d;
DROP TABLE public.t_r30m;
DROP TABLE public.t_r30mb;
DROP TABLE public.t_r30tm;
RESET client_min_messages;

-- =====================================================================
-- ===== R31: money (MONEYOID) identity + payload canonicalization ======
-- ---------------------------------------------------------------------
--   money is the one type whose output AND input both read lc_monetary,
--   and cash_in accepts ONLY the reading session's own currency symbol /
--   separators.  A money value written under a foreign locale therefore
--   used to (a) land on a locale-specific identity key ("£1,234.56" vs
--   "$1,234.56"), silently defeating the duplicate-key check, and (b)
--   store a payload the C-configured reader could not re-parse ("invalid
--   input syntax for type money").  The write path now pins lc_monetary
--   to C so the stored key/payload are canonical, and every re-parse site
--   (BranchScan reconstruction, the delta probes, the apply replay) pins
--   the reader likewise.
--
--   The locale switch below is best-effort: en_GB.utf8 exercises the bug
--   where installed, and every assertion still holds on a C-only host (the
--   canonical form then coincides), so the output stays portable.
--
-- COVERAGE:
--   R31_A : same money value written under another lc_monetary -> 23505
--   R31_B : money payload written under another locale round-trips
--   R31_C : stored identity keys are the canonical C form
--   R31_D : genuinely distinct money values stay distinct
--   R31_E : MAIN pristine until apply; apply materializes the rows
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r31p (k money PRIMARY KEY, v text);
CREATE TABLE public.t_r31e (id int PRIMARY KEY, m money);
SELECT overlay_branch.create_branch('br_r31');
SELECT overlay_branch.use_branch('br_r31');

DO $$
BEGIN
    PERFORM set_config('lc_monetary', 'en_GB.utf8', false);
EXCEPTION WHEN OTHERS THEN
    NULL;
END $$;

-- R31_A / R31_D source rows, written before the reader switches to C.
INSERT INTO public.t_r31p VALUES (1234.56::money, 'a');
INSERT INTO public.t_r31p VALUES (7.77::money, 'x');
INSERT INTO public.t_r31p VALUES (8.88::money, 'y');
INSERT INTO public.t_r31e VALUES (1, 9999.99::money);

-- Switch the reader to the canonical locale: a foreign-written payload would
-- no longer re-parse (R31_B) and a foreign identity key would no longer match
-- (R31_A) without the fix.
SET lc_monetary = 'C';

-- R31_A: the same money value must collide across locales.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r31p VALUES (1234.56::money, 'b');
        RAISE EXCEPTION 'FAIL_R31_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R31_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 3 THEN 'PASS:R31_A_MONEY_IDENTITY_23505'
            ELSE 'FAIL:R31_A rows=' || count(*) END AS r31_a
  FROM public.t_r31p;

-- R31_B: a money payload written under another locale must round-trip.
SELECT CASE WHEN count(*) = 1 AND min(m) = 9999.99::money
            THEN 'PASS:R31_B_MONEY_PAYLOAD_ROUNDTRIP'
            ELSE 'FAIL:R31_B rows=' || count(*) END AS r31_b
  FROM public.t_r31e;

-- R31_C: the stored identity keys are the canonical C form.
SELECT CASE WHEN array_agg(key ORDER BY key)
                 = ARRAY['["$1,234.56"]','["$7.77"]','["$8.88"]']::text[]
            THEN 'PASS:R31_C_CANONICAL_KEYS'
            ELSE 'FAIL_R31_C keys=' || array_to_string(array_agg(key ORDER BY key), ',')
       END AS r31_c
  FROM overlay_branch.pg_branch_delta WHERE relid = 'public.t_r31p'::regclass;

-- R31_D: genuinely distinct money values stay distinct.
SELECT CASE WHEN count(DISTINCT k) = 3 THEN 'PASS:R31_D_DISTINCT_PRESERVED'
            ELSE 'FAIL:R31_D distinct=' || count(DISTINCT k) END AS r31_d
  FROM public.t_r31p;

-- R31_E: MAIN pristine until apply; apply materializes the rows.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R31_E_MAIN_PRISTINE'
            ELSE 'FAIL:R31_E rows=' || count(*) END AS r31_e
  FROM public.t_r31p;
SELECT overlay_branch.apply_branch('br_r31');
SELECT CASE WHEN count(*) = 3 THEN 'PASS:R31_E_APPLY_MATERIALIZED'
            ELSE 'FAIL:R31_E rows=' || count(*) END AS r31_e2
  FROM public.t_r31p;

-- R31 teardown (branch left in the terminal 'applied' state).
RESET lc_monetary;
DROP TABLE public.t_r31p;
DROP TABLE public.t_r31e;
RESET client_min_messages;

-- =====================================================================
-- ===== R32: jsonb (JSONBOID) PK identity canonicalization ============
-- ---------------------------------------------------------------------
--   jsonb_eq compares numeric values with numeric_eq, but a jsonb datum
--   keeps the dscale it was parsed with, so '{"a":1.0}' and '{"a":1.00}'
--   are '='-equal yet print differently.  Keying the delta on a raw
--   jsonb_out string would therefore split one logical key into two delta
--   rows and silently defeat the duplicate-key check.  The key serializer
--   now folds every numeric -- at any depth, at the scalar root, and inside
--   arrays -- with numeric_trim_scale, so the stored identity key is the
--   '='-equality canonical form.
--
-- COVERAGE:
--   R32_A : same numeric value at a different dscale   -> 23505
--   R32_B : scalar-root jsonb folds the same way        -> 23505
--   R32_C : stored identity keys are the canonical form
--   R32_D : a genuinely distinct jsonb value stays distinct
--   R32_E : MAIN pristine until apply; apply materializes the rows
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r32p (k jsonb PRIMARY KEY, v text);
SELECT overlay_branch.create_branch('br_r32');
SELECT overlay_branch.use_branch('br_r32');
INSERT INTO public.t_r32p VALUES ('{"a":1.0}'::jsonb, 'a');
INSERT INTO public.t_r32p VALUES ('{"b":2,"a":1}'::jsonb, 'b');
INSERT INTO public.t_r32p VALUES ('1.0'::jsonb, 'scalar');

-- R32_A: the same numeric value at a different dscale must collide.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r32p VALUES ('{"a":1.00}'::jsonb, 'dup');
        RAISE EXCEPTION 'FAIL_R32_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R32_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 3 THEN 'PASS:R32_A_JSONB_IDENTITY_23505'
            ELSE 'FAIL:R32_A rows=' || count(*) END AS r32_a
  FROM public.t_r32p;

-- R32_B: a scalar-root jsonb folds the same way.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r32p VALUES ('1.00'::jsonb, 'sdup');
        RAISE EXCEPTION 'FAIL_R32_B_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R32_B: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 3 THEN 'PASS:R32_B_SCALAR_ROOT_FOLD'
            ELSE 'FAIL:R32_B rows=' || count(*) END AS r32_b
  FROM public.t_r32p;

-- R32_C: the stored identity keys are the folded canonical form.
SELECT CASE WHEN (SELECT count(*) FROM overlay_branch.pg_branch_delta
                   WHERE relid = 'public.t_r32p'::regclass
                     AND key IN ('["1"]', '["{\"a\": 1}"]',
                                 '["{\"a\": 1, \"b\": 2}"]')) = 3
            THEN 'PASS:R32_C_CANONICAL_KEYS'
            ELSE 'FAIL:R32_C keys=' || (SELECT string_agg(key, ' | ' ORDER BY key)
                                          FROM overlay_branch.pg_branch_delta
                                         WHERE relid = 'public.t_r32p'::regclass)
       END AS r32_c;

-- R32_D: a genuinely distinct jsonb value is accepted.
INSERT INTO public.t_r32p VALUES ('{"a":1.5}'::jsonb, 'distinct');
SELECT CASE WHEN count(*) = 4 THEN 'PASS:R32_D_DISTINCT_PRESERVED'
            ELSE 'FAIL:R32_D rows=' || count(*) END AS r32_d
  FROM public.t_r32p;

-- R32_E: MAIN pristine until apply; apply materializes the rows.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R32_E_MAIN_PRISTINE'
            ELSE 'FAIL:R32_E rows=' || count(*) END AS r32_e
  FROM public.t_r32p;
SELECT overlay_branch.apply_branch('br_r32');
SELECT CASE WHEN count(*) = 4 THEN 'PASS:R32_E_APPLY_MATERIALIZED'
            ELSE 'FAIL:R32_E rows=' || count(*) END AS r32_e2
  FROM public.t_r32p;

-- R32 teardown (branch left in the terminal 'applied' state).
DROP TABLE public.t_r32p;
RESET client_min_messages;

-- =====================================================================
-- ===== R33: json/jsonb payload structure round-trip ==================
-- ---------------------------------------------------------------------
--   ob_serialize_tuple() bakes each column into a JSON string and relies
--   on the reader's input function to re-parse it.  jsonb_to_record() and
--   jsonb_populate_record() are the exception: for a json/jsonb target
--   column (or a domain over them) they splice the JSON value straight
--   through instead of calling the input function (jsonfuncs.c
--   populate_scalar()).  A jsonb object wrapped in a JSON string therefore
--   came back as a jsonb STRING scalar ("{\"a\": 1}"), silently corrupting
--   every BranchScan read, delta probe and apply write-back.  The
--   serializer now emits json/jsonb values verbatim (their output is
--   already valid JSON), so the reader's direct copy reproduces them;
--   jsonb[] keeps the string encoding because the reader runs array_in() on
--   it.  A jsonb JSON-null still maps to SQL NULL, matching
--   row_to_json()'s own round trip (documented limitation).
--
-- COVERAGE:
--   R33_A : jsonb object survives the BranchScan read (object, not string)
--   R33_B : branch UPDATE preserves a nested object / array
--   R33_C : json column, domain-over-jsonb, and jsonb[] element survive
--   R33_D : the jsonb UNIQUE delta probe still detects a colliding value
--   R33_E : apply writes the structure back to MAIN intact
-- =====================================================================
SET client_min_messages = WARNING;
CREATE DOMAIN public.jb_r33 AS jsonb;
CREATE TABLE public.t_r33p (id int PRIMARY KEY, j jsonb, jc json,
                            jd public.jb_r33, ja jsonb[], jt text);
CREATE TABLE public.t_r33u (id int PRIMARY KEY, ju jsonb UNIQUE);
SELECT overlay_branch.create_branch('br_r33');
SELECT overlay_branch.use_branch('br_r33');
INSERT INTO public.t_r33p
    VALUES (1, '{"a":1,"nested":{"b":2}}'::jsonb, '{"c":3}'::json,
            '{"d":4}'::public.jb_r33, ARRAY['{"x":1}'::jsonb], 'plain');
INSERT INTO public.t_r33u VALUES (1, '{"u":1}'::jsonb);

-- R33_A: a jsonb object must read back as an object, not a string.
SELECT CASE WHEN j = '{"a":1,"nested":{"b":2}}'::jsonb
                 AND jsonb_typeof(j) = 'object'
                 AND jsonb_typeof(j -> 'nested') = 'object'
            THEN 'PASS:R33_A_JSONB_OBJECT_SURVIVES'
            ELSE 'FAIL:R33_A j=' || j::text || ' ty=' || jsonb_typeof(j) END AS r33_a
  FROM public.t_r33p WHERE id = 1;

-- R33_B: a branch UPDATE preserves a nested object and an array.
UPDATE public.t_r33p SET j = '{"a":2,"b":[1,2]}'::jsonb, jt = 'edited' WHERE id = 1;
SELECT CASE WHEN j = '{"a":2,"b":[1,2]}'::jsonb
                 AND jsonb_typeof(j -> 'b') = 'array'
            THEN 'PASS:R33_B_BRANCH_UPDATE_STRUCTURE'
            ELSE 'FAIL:R33_B j=' || j::text END AS r33_b
  FROM public.t_r33p WHERE id = 1;

-- R33_C: a json column, a domain-over-jsonb column, and a jsonb[] element.
SELECT CASE WHEN jc::text = '{"c": 3}'
                 AND jd::text = '{"d": 4}'
                 AND ja = ARRAY['{"x":1}'::jsonb]
            THEN 'PASS:R33_C_JSON_DOMAIN_ARRAY_SURVIVE'
            ELSE 'FAIL:R33_C jc=' || jc::text || ' jd=' || jd::text
                 || ' ja=' || ja::text END AS r33_c
  FROM public.t_r33p WHERE id = 1;

-- R33_D: the jsonb UNIQUE delta probe still finds a colliding value.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r33u VALUES (2, '{"u":1}'::jsonb);
        RAISE EXCEPTION 'FAIL_R33_D_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R33_D: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R33_D_UNIQUE_PROBE_23505'
            ELSE 'FAIL:R33_D rows=' || count(*) END AS r33_d
  FROM public.t_r33u;

-- R33_E: MAIN pristine until apply; apply writes the structure back intact.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R33_E_MAIN_PRISTINE'
            ELSE 'FAIL:R33_E rows=' || count(*) END AS r33_e
  FROM public.t_r33p;
SELECT overlay_branch.apply_branch('br_r33');
SELECT CASE WHEN j = '{"a":2,"b":[1,2]}'::jsonb
                 AND jsonb_typeof(j) = 'object'
                 AND jc::text = '{"c": 3}'
                 AND jd::text = '{"d": 4}'
                 AND ja = ARRAY['{"x":1}'::jsonb]
            THEN 'PASS:R33_E_APPLY_STRUCTURE_PRESERVED'
            ELSE 'FAIL:R33_E j=' || j::text || ' jd=' || jd::text END AS r33_e2
  FROM public.t_r33p WHERE id = 1;

-- R33 teardown (branch left in the terminal 'applied' state).
DROP TABLE public.t_r33p;
DROP TABLE public.t_r33u;
DROP DOMAIN public.jb_r33;
RESET client_min_messages;

