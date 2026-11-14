/*
 * overlay_branch_index_rls.sql — index-shape, RLS, UPSERT-clause and float-key gates.
 *
 * Purpose:  the eager conflict probe must agree with the redo path on which
 *           indexes are key columns (INCLUDE columns are not; a not-ready
 *           UNIQUE index is not enforced), branch writes must honour RLS,
 *           UPSERT must evaluate BOTH DO UPDATE policy clauses, and a lowered
 *           extra_float_digits must not let a duplicate float key slip in.
 *
 * Coverage: R34 INCLUDE (non-key) columns are not key columns;
 *           R35 a not-ready UNIQUE index (CONCURRENTLY interrupted) is not
 *               eagerly enforced;
 *           R36 RLS WITH CHECK is enforced on branch writes;
 *           R37 UPSERT DO UPDATE enforces both policy clauses (USING + WHERE);
 *           R38 float key identity under extra_float_digits = 0.
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
DROP TABLE IF EXISTS public.t_r34pk CASCADE;
DROP TABLE IF EXISTS public.t_r34u CASCADE;
DROP TABLE IF EXISTS public.t_r34inc CASCADE;
DROP TABLE IF EXISTS public.t_r35 CASCADE;
DROP TABLE IF EXISTS public.t_r36 CASCADE;
DROP TABLE IF EXISTS public.t_r37 CASCADE;
DROP TABLE IF EXISTS public.t_r38u CASCADE;
DROP TABLE IF EXISTS public.t_r38p CASCADE;
DROP EXTENSION IF EXISTS overlay_branch CASCADE;
RESET client_min_messages;

CREATE EXTENSION IF NOT EXISTS overlay_branch;
SET client_min_messages = WARNING;
-- =====================================================================
-- ===== R34: INCLUDE (non-key) index columns are not key columns ======
-- ---------------------------------------------------------------------
--   An index's INCLUDE (non-key) columns are carried as payload and take
--   no part in its uniqueness: `PRIMARY KEY (a) INCLUDE (b)` has
--   indnatts = 2 but indnkeyatts = 1, and `UNIQUE (u) INCLUDE (w)` only
--   forbids duplicate u.  Several redirect paths read the *whole*
--   attribute list (indnatts / ii_NumIndexAttrs) instead of the key
--   prefix, silently folding the INCLUDE columns into the identity /
--   uniqueness key and causing four divergences: (a) an in-branch UPDATE
--   of a PK's INCLUDE column was rejected 0A000 as a "PK mutation"
--   although native permits it; (b) a key-only conflict under a UNIQUE
--   INCLUDE index was accepted into the delta and surfaced its 23505 only
--   at apply; (c) `ON CONFLICT (u)` on `UNIQUE (u) INCLUDE (w)` resolved
--   to (u,w), so the arbiter probe never matched and the upsert degraded
--   to a plain INSERT; (d) the G01 category-9 citext probe rejected a
--   table whose citext value was merely an INCLUDE column of the PK.  All
--   key-column probes now use indnkeyatts / ii_NumIndexKeyAttrs.
--
-- COVERAGE:
--   R34_A : a PK INCLUDE column may be updated in-branch while the key
--           column stays protected (0A000)
--   R34_B : a key-only conflict under a UNIQUE INCLUDE index raises 23505
--           eagerly inside the branch
--   R34_C : the same conflict from two rows of ONE statement still 23505
--   R34_D : ON CONFLICT (u) on UNIQUE (u) INCLUDE (w) matches the existing
--           row (updates it) instead of inserting a second row
--   R34_E : apply writes the branch result back to MAIN intact
--   R34_F : a citext value carried as an INCLUDE (non-key) PK column does
--           NOT trip the G01 category-9 citext probe (it reads only
--           indkey[0:indnkeyatts-1]), so create_branch succeeds
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r34pk (id int, inc int, PRIMARY KEY (id) INCLUDE (inc));
CREATE TABLE public.t_r34u (id int PRIMARY KEY, u int, w text, UNIQUE (u) INCLUDE (w));
INSERT INTO public.t_r34pk VALUES (1, 5);
INSERT INTO public.t_r34u VALUES (1, 10, 'x');
SELECT overlay_branch.create_branch('br_r34');
SELECT overlay_branch.use_branch('br_r34');

-- R34_A: updating the PK's INCLUDE column is legal in-branch (native allows
--        it); updating the key column itself is still refused 0A000.
UPDATE public.t_r34pk SET inc = 99 WHERE id = 1;
SELECT CASE WHEN inc = 99 THEN 'PASS:R34_A_PK_INCLUDE_UPDATE_ALLOWED'
            ELSE 'FAIL:R34_A inc=' || inc END AS r34_a
  FROM public.t_r34pk WHERE id = 1;
DO $$
BEGIN
    BEGIN
        UPDATE public.t_r34pk SET id = 2 WHERE id = 1;
        RAISE EXCEPTION 'FAIL_R34_A_NO_ERROR';
    EXCEPTION WHEN feature_not_supported THEN NULL;
    END;
END $$;
SELECT 'PASS:R34_A_PK_KEY_COLUMN_STILL_PROTECTED' AS r34_a2;

-- R34_B: a key-only conflict under UNIQUE (u) INCLUDE (w) -- same u, a
--        different w -- must raise 23505 eagerly, not silently land.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r34u VALUES (2, 10, 'different');
        RAISE EXCEPTION 'FAIL_R34_B_NO_ERROR';
    EXCEPTION WHEN unique_violation THEN NULL;
    END;
END $$;
SELECT 'PASS:R34_B_UNIQUE_INCLUDE_EAGER_23505' AS r34_b;

-- R34_C: the same key-only conflict from two rows of a single statement.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r34u VALUES (3, 20, 'p'), (4, 20, 'q');
        RAISE EXCEPTION 'FAIL_R34_C_NO_ERROR';
    EXCEPTION WHEN unique_violation THEN NULL;
    END;
END $$;
SELECT 'PASS:R34_C_UNIQUE_INCLUDE_SAME_COMMAND_23505' AS r34_c;

-- R34_D: ON CONFLICT (u) on UNIQUE (u) INCLUDE (w) must resolve to (u) and
--        update the existing row -- exactly one row with u = 10 afterwards.
INSERT INTO public.t_r34u VALUES (9, 10, 'z')
    ON CONFLICT (u) DO UPDATE SET w = EXCLUDED.w;
SELECT CASE WHEN count(*) = 1 AND min(w) = 'z'
            THEN 'PASS:R34_D_UPSERT_ARBITER_INCLUDE_RESOLVED'
            ELSE 'FAIL:R34_D rows=' || count(*) || ' w=' || min(w) END AS r34_d
  FROM public.t_r34u WHERE u = 10;

-- R34_E: MAIN pristine until apply; apply writes the branch result back.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 1 AND min(inc) = 5
            THEN 'PASS:R34_E_MAIN_PRISTINE'
            ELSE 'FAIL:R34_E rows=' || count(*) END AS r34_e
  FROM public.t_r34pk;
SELECT overlay_branch.apply_branch('br_r34');
SELECT CASE WHEN inc = 99 THEN 'PASS:R34_E_APPLY_PK_INCLUDE'
            ELSE 'FAIL:R34_E inc=' || inc END AS r34_e2
  FROM public.t_r34pk WHERE id = 1;
SELECT CASE WHEN count(*) = 1 AND min(w) = 'z'
            THEN 'PASS:R34_E_APPLY_UNIQUE_INCLUDE'
            ELSE 'FAIL:R34_E rows=' || count(*) END AS r34_e3
  FROM public.t_r34u WHERE u = 10;

-- R34_F: a citext value carried as an INCLUDE (non-key) PK column is NOT
--        part of the identity, so the G01 category-9 citext probe -- which
--        must read only indkey[0:indnkeyatts-1] -- lets the table through:
--        create_branch succeeds and the branch opens/discards normally.
CREATE TABLE public.t_r34inc (a int, c citext, PRIMARY KEY (a) INCLUDE (c));
SELECT CASE WHEN (SELECT overlay_branch.create_branch('br_r34inc') IS NOT NULL)
            THEN 'PASS:R34_F_PK_INCLUDE_CITEXT_ALLOWED'
            ELSE 'FAIL:R34_F' END AS r34_f;
SELECT overlay_branch.discard_branch('br_r34inc');
DROP TABLE public.t_r34inc;

-- R34 teardown (branch left in the terminal 'applied' state).
DROP TABLE public.t_r34pk;
DROP TABLE public.t_r34u;
RESET client_min_messages;

-- =====================================================================
-- ===== R35: a not-ready UNIQUE index is not eagerly enforced =========
-- ---------------------------------------------------------------------
--   Native ExecInsertIndexTuples skips every index whose
--   IndexInfo.ii_ReadyForInserts is false (pg_index.indisready = false),
--   so a UNIQUE index left behind by an ABORTED CREATE INDEX CONCURRENTLY
--   (duplicate data) enforces nothing.  The branch's eager UNIQUE probe
--   enumerates indexes with RelationGetIndexList, which returns such a
--   not-ready index, so it must apply the same gate -- otherwise it raises
--   a spurious 23505 for a write native accepts.
--
-- COVERAGE:
--   R35_A : a not-ready UNIQUE index does not block an in-branch INSERT
--           that native also accepts, and the row still reaches MAIN
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r35 (a int PRIMARY KEY, u int, v text);
INSERT INTO public.t_r35 VALUES (1, 1, 'x'), (2, 1, 'y');
/* Abort a concurrent UNIQUE build on duplicate data: the index is left
 * with indisvalid = false AND indisready = false. */
CREATE UNIQUE INDEX CONCURRENTLY u_t_r35 ON public.t_r35 (u);
SELECT c.relname, i.indisunique, i.indisvalid, i.indisready
  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
 WHERE i.indrelid = 'public.t_r35'::regclass ORDER BY c.relname;
SELECT overlay_branch.create_branch('br_r35');
SELECT overlay_branch.use_branch('br_r35');

-- R35_A: the not-ready index must not fire; this insert is legal natively.
INSERT INTO public.t_r35 VALUES (3, 1, 'z');
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R35_A_NOTREADY_UNIQUE_NOT_ENFORCED'
            ELSE 'FAIL:R35_A rows=' || count(*) END AS r35_a
  FROM public.t_r35 WHERE a = 3;

SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT overlay_branch.apply_branch('br_r35');
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R35_A_APPLY_MATERIALIZED'
            ELSE 'FAIL:R35_A rows=' || count(*) END AS r35_a2
  FROM public.t_r35 WHERE a = 3;

-- R35 teardown (the invalid index is dropped with the table).
DROP TABLE public.t_r35;
RESET client_min_messages;

-- =====================================================================
-- ===== R36: RLS WITH CHECK is enforced on branch writes ==============
-- ---------------------------------------------------------------------
--   The write-redirect short-circuits standard_ExecutorRun(), so the
--   executor never reaches its own ExecWithCheckOptions() call.  Without
--   an explicit replay a row that violates a row-level-security WITH
--   CHECK policy would be silently stored in the branch delta even though
--   the identical statement against MAIN is rejected.  The redirect now
--   replays the native checks with the same WCOKind native uses for the
--   statement: WCO_RLS_INSERT_CHECK for an INSERT candidate, and for a
--   plain UPDATE or the new image of an ON CONFLICT DO UPDATE the UPDATE
--   WITH CHECK policy (WCO_RLS_UPDATE_CHECK).  The UPDATE USING policy of
--   an ON CONFLICT DO UPDATE (WCO_RLS_CONFLICT_CHECK against the
--   conflicting pre-image) is covered by R37, which separates the clauses.
--   (The RLS USING side of a plain scan is applied by the planner and
--   already covers the branch scan/DML-target visibility.)
--
-- COVERAGE (the non-owner role ob_rls_role is subject to RLS; the table
--   is created and seeded by the superuser, for which RLS is bypassed):
--   R36_A : INSERT violating WITH CHECK -> rejected in the branch (RLS)
--   R36_B : legal INSERT is still accepted
--   R36_C : plain UPDATE to a violating value -> rejected
--   R36_D : UPSERT DO UPDATE to a violating value -> rejected
--   R36_E : UPSERT DO NOTHING whose candidate violates WITH CHECK ->
--           rejected (native validates the candidate BEFORE resolving
--           the conflict)
-- =====================================================================
SET client_min_messages = WARNING;
DROP ROLE IF EXISTS ob_rls_role;
CREATE ROLE ob_rls_role;
GRANT overlay_branch_administrators TO ob_rls_role;
CREATE TABLE public.t_r36 (id int PRIMARY KEY, v int);
ALTER TABLE public.t_r36 ENABLE ROW LEVEL SECURITY;
CREATE POLICY p_r36 ON public.t_r36 FOR ALL USING (v > 0) WITH CHECK (v > 0);
GRANT SELECT, INSERT, UPDATE, DELETE ON public.t_r36 TO ob_rls_role;
INSERT INTO public.t_r36 VALUES (1, 5);
SELECT overlay_branch.create_branch('br_r36');
SELECT overlay_branch.use_branch('br_r36');
SET ROLE ob_rls_role;

-- R36_A: a WITH CHECK violation is rejected exactly like on MAIN.
INSERT INTO public.t_r36 VALUES (2, -1);

-- R36_B: a legal row is accepted.
INSERT INTO public.t_r36 VALUES (3, 7);
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R36_B_LEGAL_INSERT_ACCEPTED'
            ELSE 'FAIL:R36_B rows=' || count(*) END AS r36_b
  FROM public.t_r36 WHERE id = 3;

-- R36_C: UPDATE to a violating value is rejected.
UPDATE public.t_r36 SET v = -1 WHERE id = 1;

-- R36_D: UPSERT DO UPDATE to a violating value is rejected.
INSERT INTO public.t_r36 VALUES (1, -2) ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v;

-- R36_E: UPSERT DO NOTHING whose candidate violates WITH CHECK is rejected.
INSERT INTO public.t_r36 VALUES (1, -5) ON CONFLICT (id) DO NOTHING;

RESET ROLE;
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT overlay_branch.discard_branch('br_r36');
DROP TABLE public.t_r36;
DROP ROLE ob_rls_role;
RESET client_min_messages;

-- =====================================================================
-- ===== R37: UPSERT DO UPDATE enforces BOTH policy clauses ============
-- ---------------------------------------------------------------------
--   Native splits the auxiliary UPDATE of an INSERT ... ON CONFLICT DO
--   UPDATE into two RLS checks: ExecOnConflictUpdate() runs the UPDATE
--   USING policy (WCO_RLS_CONFLICT_CHECK) against the conflicting
--   pre-image BEFORE projecting the new row, and ExecUpdate() then runs
--   the UPDATE WITH CHECK policy (WCO_RLS_UPDATE_CHECK) against the new
--   image.  A redirect that replays only one kind, or the wrong kind,
--   lets a policy-violating branch UPSERT through while the identical
--   MAIN statement is rejected.  R37 gives the two clauses different
--   columns so each check can be exercised on its own (in R36 the USING
--   and WITH CHECK clauses are both v > 0, so it cannot tell them apart).
--
-- COVERAGE (role ob_rls_role37 is subject to RLS; rows are seeded by the
--   superuser, for which RLS is bypassed):
--   R37_A : DO UPDATE new image satisfies USING but violates WITH CHECK
--           -> rejected
--   R37_B : DO UPDATE pre-image violates USING -> rejected
--   R37_C : legal DO UPDATE still accepted
-- =====================================================================
SET client_min_messages = WARNING;
DROP ROLE IF EXISTS ob_rls_role37;
CREATE ROLE ob_rls_role37;
GRANT overlay_branch_administrators TO ob_rls_role37;
CREATE TABLE public.t_r37 (id int PRIMARY KEY, v int, w int);
ALTER TABLE public.t_r37 ENABLE ROW LEVEL SECURITY;
CREATE POLICY p37_ins ON public.t_r37 FOR INSERT WITH CHECK (true);
CREATE POLICY p37_sel ON public.t_r37 FOR SELECT USING (true);
CREATE POLICY p37_upd ON public.t_r37 FOR UPDATE USING (v >= 0) WITH CHECK (w >= 0);
GRANT SELECT, INSERT, UPDATE, DELETE ON public.t_r37 TO ob_rls_role37;
INSERT INTO public.t_r37 VALUES (1, 5, 5);
INSERT INTO public.t_r37 VALUES (2, -1, 5);
SELECT overlay_branch.create_branch('br_r37');
SELECT overlay_branch.use_branch('br_r37');
SET ROLE ob_rls_role37;

-- R37_A: the new image keeps USING (v >= 0) true but sets w < 0, so only
-- the UPDATE WITH CHECK clause is violated.
INSERT INTO public.t_r37 VALUES (1, 3, 9) ON CONFLICT (id) DO UPDATE SET w = -1;

-- R37_B: the conflicting pre-image (id = 2, v = -1) violates the UPDATE
-- USING clause.
INSERT INTO public.t_r37 VALUES (2, 0, 0) ON CONFLICT (id) DO UPDATE SET w = 1;

-- R37_C: a legal DO UPDATE is still accepted.
INSERT INTO public.t_r37 VALUES (1, 3, 9) ON CONFLICT (id) DO UPDATE SET w = 7;
SELECT CASE WHEN w = 7 THEN 'PASS:R37_C_LEGAL_UPSERT_ACCEPTED'
            ELSE 'FAIL:R37_C w=' || w END AS r37_c
  FROM public.t_r37 WHERE id = 1;

RESET ROLE;
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT overlay_branch.discard_branch('br_r37');
DROP TABLE public.t_r37;
DROP ROLE ob_rls_role37;
RESET client_min_messages;

-- =====================================================================
-- ===== R38: float key identity under a lowered extra_float_digits ====
-- ---------------------------------------------------------------------
--   Every write-side conflict probe renders a live candidate column as
--   "col = $n::text::<type>" and lets the input function rebuild it, so
--   the rendered text must round-trip.  float4/float8 output honours
--   extra_float_digits: at the canonical value (1) it prints the shortest
--   text that re-parses to the SAME float, but a session that lowered it
--   to 0 gets a SHORT text -- 0.1 + 0.2 prints "0.3" instead of
--   "0.30000000000000004" -- that re-parses to a DIFFERENT float8.  The
--   probe used to render the live candidate under the session GUC, so it
--   mismatched the exact stored value and SILENTLY accepted a duplicate
--   that MAIN rejects with 23505.  R38 drives the whole scenario with
--   extra_float_digits = 0 and asserts the branch rejects it like MAIN.
--
-- COVERAGE:
--   R38_A : duplicate non-PK UNIQUE float value -> 23505 (was accepted)
--   R38_B : a genuinely distinct float is still accepted
--   R38_C : duplicate float PRIMARY KEY value -> 23505 (was accepted)
-- =====================================================================
SET client_min_messages = WARNING;
SET extra_float_digits = 0;

CREATE TABLE public.t_r38u (id int PRIMARY KEY, f float8);
CREATE UNIQUE INDEX t_r38u_f ON public.t_r38u (f);
INSERT INTO public.t_r38u VALUES (1, 0.1::float8 + 0.2::float8);
SELECT overlay_branch.create_branch('br_r38u');
SELECT overlay_branch.use_branch('br_r38u');
SHOW extra_float_digits;

-- R38_A: the 0.1 + 0.2 candidate collides with the stored 0.3+epsilon row.
INSERT INTO public.t_r38u VALUES (2, 0.1::float8 + 0.2::float8);

-- R38_B: 0.7 is a different float8 and is accepted.
INSERT INTO public.t_r38u VALUES (3, 0.7::float8);
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R38_B_DISTINCT_FLOAT_ACCEPTED'
            ELSE 'FAIL:R38_B rows=' || count(*) END AS r38_b
  FROM public.t_r38u;

SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT overlay_branch.discard_branch('br_r38u');
DROP TABLE public.t_r38u;

-- R38_C: the same precision loss on a float PRIMARY KEY (R21 MAIN probe).
CREATE TABLE public.t_r38p (id float8 PRIMARY KEY, v int);
INSERT INTO public.t_r38p VALUES (0.1::float8 + 0.2::float8, 1);
SELECT overlay_branch.create_branch('br_r38p');
SELECT overlay_branch.use_branch('br_r38p');
INSERT INTO public.t_r38p VALUES (0.1::float8 + 0.2::float8, 2);

SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT overlay_branch.discard_branch('br_r38p');
DROP TABLE public.t_r38p;

RESET extra_float_digits;
RESET client_min_messages;
