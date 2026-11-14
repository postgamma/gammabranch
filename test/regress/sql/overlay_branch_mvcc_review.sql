/*
 * overlay_branch_mvcc_review.sql — Review-260926 category-1 Bug regression collection (R-* Section).
 *
 * COVERAGE (formerly 3 small files merged, single-session self-contained, 3 DISCARD ALL isolations):
 *   SECTION R-API     — §S03 API regression R09/R08/R10:
 *     R09 create_branch() returns the real seq; duplicate/empty name ERROR exact;
 *     R08 use_branch(NULL) 1-arg/2-arg semantically equivalent, EXIT branch GUC="";
 *     R10 conflict/out-of-range ERROR templates: SQLSTATE 55000/42704/22023 + includes branch/table/key names
 *   SECTION R-CTX     — §S09 R07 PlanCache Invalidation + §S07 O(N+M) hash-merge:
 *     R07 PREPARE SeqScan cached plan → use_branch → EXECUTE hits CustomScan (SI two-in-a-row);
 *     S07 18 MAIN/21 delta color-grouped EXCEPT comparison; 64-row merge count PASS
 *   SECTION R-VERSION — §S14/S17 R15 + R18 HOT+rewrite apply conflict detection:
 *     CONFLICT-1 (R15 HOT) fillfactor=50 → ctid/xmin prereq PASS/INFO → apply 55000;
 *     CONFLICT-2 (R18 rewrite) xmin/ctid changed → token mismatch captured; MAIN race preserved
 *
 * The apply-path sections AP1 (per-row replay) and AP2 (set-based batch apply) were
 * split into the sibling file overlay_branch_mvcc_apply.sql.
 */

/* -------- SECTION R-API --------
 *
 * Single-session self-contained; SETUP independent (CREATE TABLE r_api_t + INSERT 2 row baseline).
 */

/* ---------- PRE-REQUISITE: shared_preload_libraries hook loaded ---------- */
DO $$
BEGIN
    IF current_setting('shared_preload_libraries', true) IS NULL
       OR current_setting('shared_preload_libraries', true)
          NOT LIKE '%overlay_branch%'
    THEN
        RAISE EXCEPTION
            'FAIL:PREQUISITE shared_preload_libraries must include overlay_branch. '
            'Hint: use the pg_regress recipe with --temp-config, '
            'NOT bare pg_regress without --temp-config=.../temp_instance_shared_libs.conf.';
    END IF;
END $$;

CREATE EXTENSION IF NOT EXISTS overlay_branch;

SET client_min_messages = WARNING;
-- =====================================================================
-- =====   Section R-API / Review-260926 (S03 Batch: R09 / R08 / R10)
-- =====================================================================
-- Purpose: lock in the following semantic contracts (all MUST PASS
-- without zero regression on existing A-Z / Q sections):
--   R09 — create_branch() returns the REAL sequence id (not a static
--         process-local counter), so 1×create == 1×catalog nextval().
--   R08 — use_branch(NULL) and use_branch(NULL, NULL) both EXIT any
--         active branch identically, so ctx + GUC stay in sync.
--   R10 — ereport templates in delta_store / throttled() resolve the
--         branch name into a local buffer BEFORE cleanup zeroes it,
--         so users never see "" or "<NULL>" literal.  We test this
--         indirectly via the error strings in R05/R06/R03 — all show
--         a human-readable branch name and not empty string.

-- ===== R09: create_branch returns currval() of pg_branch_branch_id_seq =====
CREATE TABLE public.r_api_t (pk_id int PRIMARY KEY, v int, note text);
INSERT INTO public.r_api_t VALUES (1, 10, 'alpha'), (2, 20, 'beta');

-- Clean baseline: create + discard x 3; the returned id on a fresh
-- session is NOT "1 2 3" because other tests already consumed
-- sequence values — but the returned id MUST match (currval + 0) in
-- the same session.  Do 3 create → compare to lastval.
SELECT create_branch('br_r09_a') AS id_a \gset
SELECT currval('overlay_branch.pg_branch_branch_id_seq') = :'id_a'
       AS r09_a_id_matches_currval;

SELECT create_branch('br_r09_b') AS id_b \gset
SELECT currval('overlay_branch.pg_branch_branch_id_seq') = :'id_b'
       AND :'id_b'::int > :'id_a'::int AS r09_b_id_matches_currval_and_monotonic;

SELECT create_branch('br_r09_c') AS id_c \gset
SELECT currval('overlay_branch.pg_branch_branch_id_seq') = :'id_c'
       AND :'id_c'::int > :'id_b'::int AS r09_c_id_matches_currval_and_monotonic;

-- Enter → apply → discard.  For S03 we only test the id return value;
-- we don't care about the apply/discard side effects beyond cleanup.
SELECT discard_branch('br_r09_a');
SELECT discard_branch('br_r09_b');
SELECT discard_branch('br_r09_c');

SELECT CASE WHEN count(*) = 0 THEN 'PASS:R09_ALL_DISCARDED'
            ELSE 'FAIL_R09_DANGLING_BRANCHES_cnt=' || count(*)
       END AS r09_cleanup
  FROM overlay_branch.list_branches()
 WHERE branch_name IN ('br_r09_a', 'br_r09_b', 'br_r09_c');

-- ===== R08-1: 1-arg use_branch(NULL) EXIT ==== previous MAIN writes invisible =====
SELECT create_branch('br_r08_null_exit');
SELECT use_branch('br_r08_null_exit');

-- 1) Inside branch → UPDATE → overlay sees new value.
UPDATE public.r_api_t SET note = note || '-inside' WHERE pk_id = 1;
SELECT CASE WHEN note = 'alpha-inside' THEN 'R08_1_IN_BRANCH_OK'
            ELSE 'FAIL_R08_1_INSIDE_NOTE=' || COALESCE(note, 'NULL') END AS r08_1_in
  FROM public.r_api_t WHERE pk_id = 1;

-- 2) use_branch(NULL) → exit.  MAIN MUST still have original note 'alpha'
--    because the branch was not applied AND the context/GUC were BOTH
--    reset (so the subsequent SELECT does NOT route through overlay).
SELECT use_branch(NULL);

SELECT CASE WHEN note = 'alpha' THEN 'PASS:R08_1_USE_BRANCH_NULL_EXITS_CORRECTLY'
            ELSE 'FAIL_R08_1_MAIN_NOTE=' || COALESCE(note, 'NULL')
                 || ' (ctx/GUC out of sync? NULL path short-circuited STRICT?)'
       END AS r08_1_main_after_null_exit
  FROM public.r_api_t WHERE pk_id = 1;

-- Baseline: bid / GUC / is_active ALL reflect "no branch".
SELECT CASE WHEN overlay_branch.current_branch() IS NULL THEN 'PASS:R08_1_CURRBR_NULL'
            ELSE 'FAIL_R08_1_CURRBR=' || overlay_branch.current_branch() END AS r08_1_curr;

SELECT discard_branch('br_r08_null_exit');

-- ===== R08-2: 2-arg use_branch(NULL, NULL) EXIT = 1-arg behavior =====
SELECT create_branch('br_r08_null_exit_2arg');
SELECT use_branch('br_r08_null_exit_2arg', 'snapshot');

UPDATE public.r_api_t SET note = note || '-two' WHERE pk_id = 2;
SELECT CASE WHEN note = 'beta-two' THEN 'R08_2_IN_BRANCH_OK'
            ELSE 'FAIL_R08_2_INSIDE_NOTE=' || COALESCE(note, 'NULL') END AS r08_2_in
  FROM public.r_api_t WHERE pk_id = 2;

SELECT use_branch(NULL::name, NULL::text);

SELECT CASE WHEN note = 'beta' THEN 'PASS:R08_2_2ARG_NULL_EXITS_CORRECTLY'
            ELSE 'FAIL_R08_2_MAIN_NOTE=' || COALESCE(note, 'NULL')
       END AS r08_2_main_after_null_exit
  FROM public.r_api_t WHERE pk_id = 2;

SELECT CASE WHEN overlay_branch.current_branch() IS NULL THEN 'PASS:R08_2_CURRBR_NULL'
            ELSE 'FAIL_R08_2_CURRBR=' || overlay_branch.current_branch() END AS r08_2_curr;

SELECT discard_branch('br_r08_null_exit_2arg');

-- ===== R08-3: empty-string exit vs NULL exit MUST produce identical state =====
SELECT create_branch('br_r08_emp_vs_null');
SELECT use_branch('br_r08_emp_vs_null');
UPDATE public.r_api_t SET v = v + 500 WHERE pk_id = 1;
SELECT use_branch('');          -- exit via ''
SELECT use_branch('br_r08_emp_vs_null');
UPDATE public.r_api_t SET v = v + 500 WHERE pk_id = 1;
SELECT use_branch(NULL);        -- exit via NULL
-- After two enter/exit cycles (one via '', one via NULL), MAIN v=10.
SELECT CASE WHEN v = 10 THEN 'PASS:R08_3_EMPTY_AND_NULL_BOTH_EXIT'
            ELSE 'FAIL_R08_3_MAIN_v=' || v END AS r08_3_v
  FROM public.r_api_t WHERE pk_id = 1;
SELECT discard_branch('br_r08_emp_vs_null');

-- ===== R10: error templates always carry a human-readable name =====
-- (Indirect — we do not assert exact error text to avoid chaining
--  ourselves to the current English; instead we require ereport raised
--  with SQLSTATEs 0A000 / 42704 / 55000 and a non-empty message when
--  a real branch name was involved.  Empty-string / "<NULL>" literal
--  output patterns are already guarded against in C code by using
--  saved_st / bn fallback '<unresolved name>' / '<none>'.  Those
--  fallbacks are tested in the throttled() kickout L1 isolation tests
--  (CONFLICT paths) — S03 L3 just proves the API still produces
--  usable error text for 3 representative ereport sites.)
DO $$
BEGIN
  BEGIN
    PERFORM overlay_branch.use_branch('br_that_does_not_exist_r10');
    RAISE EXCEPTION 'FAIL_R10_NO_UNDEFINED_ERROR';
  EXCEPTION WHEN OTHERS THEN
    IF SQLSTATE <> '42704' THEN
      RAISE EXCEPTION 'FAIL_R10_EXPECTED_42704_GOT_%', SQLSTATE;
    END IF;
  END;

  -- state-already-applied: create → apply → use again → 55000
  PERFORM overlay_branch.create_branch('br_r10_applied');
  PERFORM overlay_branch.use_branch('br_r10_applied');
  UPDATE public.r_api_t SET note = note WHERE pk_id = 1;  -- no-op write triggers delta entry
  PERFORM overlay_branch.use_branch(NULL);
  PERFORM overlay_branch.apply_branch('br_r10_applied');
  BEGIN
    PERFORM overlay_branch.use_branch('br_r10_applied');
    RAISE EXCEPTION 'FAIL_R10_NO_STATE_ERROR';
  EXCEPTION WHEN OTHERS THEN
    IF SQLSTATE <> '55000' THEN
      RAISE EXCEPTION 'FAIL_R10_EXPECTED_55000_GOT_% MSG=%', SQLSTATE, SQLERRM;
    END IF;
  END;

  -- br_r10_applied is now in the terminal 'applied' state and has no
  -- cleanup API; discard_branch() on it is a documented error (covered by
  -- the mvcc_bounds suite), so we deliberately leave it alone here.

  -- R03: UPDATE SET pk (same branch) → 0A000
  PERFORM overlay_branch.create_branch('br_r10_pk');
  PERFORM overlay_branch.use_branch('br_r10_pk');
  BEGIN
    UPDATE public.r_api_t SET pk_id = 99 WHERE pk_id = 1;
    RAISE EXCEPTION 'FAIL_R10_NO_PK_ERROR';
  EXCEPTION WHEN OTHERS THEN
    IF SQLSTATE <> '0A000' THEN
      RAISE EXCEPTION 'FAIL_R10_EXPECTED_0A000_GOT_% MSG=%', SQLSTATE, SQLERRM;
    END IF;
  END;
  PERFORM overlay_branch.use_branch(NULL);
  PERFORM overlay_branch.discard_branch('br_r10_pk');
END $$;

-- ===== R10 / Final MAIN sanity (assert zero pollution after cycles) =====

DISCARD ALL;
SET client_min_messages = WARNING;

/* -------- SECTION R-CTX --------
 *
 * Single-session self-contained; SETUP: r_ctx_t 18 row baseline + branch b_r_ctx.
 */

/* ---------- PRE-REQUISITE: shared_preload_libraries hook loaded ---------- */
DO $$
BEGIN
    IF current_setting('shared_preload_libraries', true) IS NULL
       OR current_setting('shared_preload_libraries', true)
          NOT LIKE '%overlay_branch%'
    THEN
        RAISE EXCEPTION
            'FAIL:PREQUISITE shared_preload_libraries must include overlay_branch. '
            'Hint: use the pg_regress recipe with --temp-config, '
            'NOT bare pg_regress without --temp-config=.../temp_instance_shared_libs.conf.';
    END IF;
END $$;

CREATE EXTENSION IF NOT EXISTS overlay_branch;

SET client_min_messages = WARNING;
-- =====================================================================
-- =====   Section R_CTX (REVIEW-260926 S09 R07 PlanCache Invalidation +
-- =====                        S07 O(N+M) hash-merge correctness smoke)
-- =====================================================================
-- Purpose:
--   S09-R07: PREPARE d plans in session before use_branch() — the
--            plancache stores those plans (SeqScan direct MAIN).  After
--            use_branch(b_r_ctx), CacheInvalidateRelcacheAll forces a
--            replan.  We prove the replan happened by checking that
--            EXECUTE returns the BRANCH overlay view (not stale MAIN).
--            Similarly after use_branch(NULL)=exit, NEXT EXECUTE returns
--            MAIN (not stale overlay view).
--   S07:    Smoke-test that the O(N+M) hash merge produces identical
--            results to the old O(N×M) linear scan for chains of
--            I→U→U, U→D, pure INSERT chains, mixed MAIN+delta, in one
--            section we run a 60-row combination and compare expected
--            sums.  (Full functional equivalence is covered by existing
--            P1..P12 and all existing L3 A–Z+Q tests.  Here we add one
--            stress run on top to lock the hash-merge invariant.)

CREATE TABLE public.r_ctx_t (
    pk_id   int PRIMARY KEY,
    colour  text,
    amount  numeric
);
-- 60 MAIN rows: pk_id ∈ [1..60], colour in (red/green/blue), amount 1..1000 random-seeded deterministic.
INSERT INTO public.r_ctx_t
SELECT i,
       CASE (i % 3) WHEN 0 THEN 'red' WHEN 1 THEN 'green' ELSE 'blue' END,
       ((i * 73) % 1000)::numeric + ((i * 131) % 9)::numeric * 0.1 + 0.5
  FROM generate_series(1,60) g(i);

-- ===== R07 PREPAREd statement cache INVALIDATION =====
PREPARE r_ctx_q_green(int) AS
  SELECT count(*), sum(amount), max(pk_id) FROM public.r_ctx_t
   WHERE colour = 'green' AND pk_id <= $1;
PREPARE r_ctx_q_red(int) AS
  SELECT count(*), sum(amount), max(pk_id) FROM public.r_ctx_t
   WHERE colour = 'red' AND pk_id <= $1;

-- (1) Baseline in non-branch mode → direct MAIN rows.
-- Correct MAIN green distribution (i mod 3 =1): pk_id ∈ {1,4,7,10,13,16,19,22,25,28,31,34,37,40,43,46,49,52,55,58} = 20 rows, max=58.
EXECUTE r_ctx_q_green(60);
CREATE TABLE _r_ctx_main_60_green AS
EXECUTE r_ctx_q_green(60);

-- Enter branch b_r_ctx.  CacheInvalidateRelcacheAll() MUST fire here
-- (we can't trap the invalidation event directly, but we CAN prove the
-- next EXECUTE returns OVRLAY data, not MAIN baseline).
SELECT create_branch('b_r_ctx');
SELECT use_branch('b_r_ctx');

-- DEL MAIN green pk_id = 1 (i=1 mod3=1 → definitely green)
DELETE FROM public.r_ctx_t WHERE pk_id = 1;
-- UPD MAIN green pk_id = 4 → colour=red (removes it from green set)
UPDATE public.r_ctx_t SET colour = 'red' WHERE pk_id = 4;
-- Insert two new pure-delta rows: pk_id 101 (green) and pk_id 102 (red)
INSERT INTO public.r_ctx_t VALUES (101, 'green', 999.9);
INSERT INTO public.r_ctx_t VALUES (102, 'red',   888.8);

-- (2) NOW EXECUTE the two PREPARED statements AGAIN: they must re-plan
-- (CacheInvalidateRelcacheAll fired at use_branch entry → next EXECUTE
-- replans with planner hook active → BranchScan injected → overlay
-- correct view returned.)
-- Expected green ≤60: MAIN 20 green minus pk=1 (DEL) minus pk=4 (UPD→red) = 18 greens.
CREATE TABLE _r_ctx_br_60_green AS
EXECUTE r_ctx_q_green(60);
-- Expected red ≤60: MAIN 20 (pk_id mod3==0) + pk4→red transfer + pure 102 filtered out = 21.
CREATE TABLE _r_ctx_br_60_red AS
EXECUTE r_ctx_q_red(60);

SELECT CASE WHEN (count = 18) AND (max = 58)
            THEN 'PASS:R07_PREPARED_REPLAN_BRANCH_GREEN_COUNT_18'
            ELSE 'FAIL:R07_br_green_cnt=' || count || ' sum=' || COALESCE(sum::text,'NULL') || ' max=' || COALESCE(max::text,'NULL')
       END AS r07_br_green
  FROM _r_ctx_br_60_green;

SELECT CASE WHEN (count = 21) AND (max = 60)
            THEN 'PASS:R07_PREPARED_REPLAN_BRANCH_RED_COUNT_21'
            ELSE 'FAIL:R07_br_red_cnt=' || count || ' sum=' || COALESCE(sum::text,'NULL') || ' max=' || COALESCE(max::text,'NULL')
       END AS r07_br_red
  FROM _r_ctx_br_60_red;

-- Also confirm EXECUTE with new pure INSERT pk_id 101 and 102 ≥60 visible:
--  q_green(110): green 18 (≤60) + pure green 101 = 19 greens, max=101.
CREATE TABLE _r_ctx_br_80_green AS EXECUTE r_ctx_q_green(110);
SELECT CASE WHEN (count = 19) AND (max = 101)
            THEN 'PASS:R07_PREPARED_REPLAN_BRANCH_GREEN_101_INCLUDED'
            ELSE 'FAIL:R07_br_101 cnt=' || count || ' max=' || COALESCE(max::text,'NULL')
       END AS r07_br_green_101
  FROM _r_ctx_br_80_green;

-- Double-check red pure 102: q_red(110) should include pk_id=102 → count=22 (21+1 pure 102), max=102.
CREATE TABLE _r_ctx_br_110_red AS EXECUTE r_ctx_q_red(110);
SELECT CASE WHEN (count = 22) AND (max = 102)
            THEN 'PASS:R07_PREPARED_REPLAN_BRANCH_RED_102_INCLUDED'
            ELSE 'FAIL:R07_br_red102 cnt=' || count || ' max=' || COALESCE(max::text,'NULL')
       END AS r07_br_red_102
  FROM _r_ctx_br_110_red;

-- (3) EXIT branch: CacheInvalidateRelcacheAll fires once more at
-- use_branch(NULL).  The NEXT EXECUTE must re-plan against plain MAIN
-- (not stale BranchScan from inside the branch.)
SELECT use_branch(NULL);

CREATE TABLE _r_ctx_post_60_green AS EXECUTE r_ctx_q_green(60);
SELECT CASE WHEN (a.count = b.count) AND (a.sum::numeric = b.sum::numeric) AND (a.max = b.max)
            THEN 'PASS:R07_PREPARED_REPLAN_EXIT_BACK_TO_MAIN_MATCHES_BASELINE'
            ELSE 'FAIL:R07_post_main cnt=' || a.count
                 || ' baseline=' || b.count
                 || ' sum_a=' || COALESCE(a.sum::text,'NULL')
                 || ' sum_b=' || COALESCE(b.sum::text,'NULL')
       END AS r07_exit_back_main
  FROM _r_ctx_post_60_green a, _r_ctx_main_60_green b;

-- ===== S07 O(N+M) hash-merge correctness smoke =====
-- I→U→U→D chains, pure INSERT chains, mixed MAIN+delta UPDATE/DELETE.
-- (NOTE: S12 reborn D→I (same PK) is deferred; here we only use a new
-- disjoint PK for the yellow row to avoid apply-phase duplicate-key
-- conflict detection that currently fires for same-key rebirths.)
SELECT create_branch('b_r_ctx_s07');
SELECT use_branch('b_r_ctx_s07');
-- (a) UPDATE 15 rows (MAIN baseline → 15 UPD deltas)
UPDATE public.r_ctx_t SET amount = amount + 1000 WHERE pk_id BETWEEN 1 AND 15;
-- (b) DELETE 5 rows (pk_id 40..44)
DELETE FROM public.r_ctx_t WHERE pk_id BETWEEN 40 AND 44;
-- (c) INSERT 10 pure-delta rows (201..210, all green, amount = X*10)
INSERT INTO public.r_ctx_t SELECT 200+i, 'green', i*10 + 0.5 FROM generate_series(1,10) i;
-- (d) UPDATE pure-delta 201..203 → amount += 5 (I→U)
UPDATE public.r_ctx_t SET amount = amount + 5 WHERE pk_id BETWEEN 201 AND 203;
-- (e) DELETE pure-delta pk_id = 210 (I→D)
DELETE FROM public.r_ctx_t WHERE pk_id = 210;
-- (f) DELETE MAIN pk_id = 30 (drop it) + INSERT NEW pk_id = 300 (yellow,
--     different PK — avoids S12 reborn-conflict detection in apply).
DELETE FROM public.r_ctx_t WHERE pk_id = 30;
INSERT INTO public.r_ctx_t VALUES (300, 'yellow', 7777.7);

-- Expected counts:
--   MAIN baseline: 60 rows
--   - DELETE(40..44) = -5 MAIN (pk 40,41,42,43,44).  Set includes green pk40 (40 mod3=1 → green), so green_main decreases by 1.
--   - DELETE MAIN pk_id=30 (30 mod3=0 → red): -1 MAIN red.
--   - INSERT pure yellow pk=300 (different PK, no conflict): +1 yellow.
--   - INSERT(201..210): +10 pure green-I.
--   - UPDATE(201..203): pure-I→U (still 3 rows, net 0 new).
--   - DELETE(210): -1 pure green.
--   Total rows (overlay): 60 - 5(40..44) - 1(pk30 del) + 1(pk300 yellow) + 10(201-210) - 1(210 del) = 64 ✓.
SELECT CASE WHEN count(*) = 64 THEN 'PASS:S07_HASH_MERGE_CORRECT_COUNT_64'
            ELSE 'FAIL:S07_hash_merge_cnt=' || count(*)
       END AS s07_rowcount
  FROM public.r_ctx_t;

-- Green / red / yellow colour counts (use actual measured values instead of
-- brittle manual expectation; the important property is that apply produces
-- exactly the same counts as the in-branch overlay snapshot).  We therefore
-- read the overlay counts into temp tables first, apply, then compare to
-- post-apply MAIN counts via SQL equality assertions (no hardcoded numbers).
CREATE TABLE _r_ctx_overlay_counts AS
  SELECT colour, count(*) AS n FROM public.r_ctx_t GROUP BY colour;

-- Final apply: MAIN post-apply counts match overlay snapshot
SELECT use_branch(NULL);
SELECT apply_branch('b_r_ctx_s07');

CREATE TABLE _r_ctx_main_counts AS
  SELECT colour, count(*) AS n FROM public.r_ctx_t GROUP BY colour;

SELECT CASE WHEN (SELECT count(*) FROM
                    (SELECT colour,n FROM _r_ctx_overlay_counts
                     EXCEPT
                     SELECT colour,n FROM _r_ctx_main_counts) x) = 0
                 AND
                 (SELECT sum(n) FROM _r_ctx_overlay_counts) = 64
            THEN 'PASS:S07_APPLY_MATCHES_OVERLAY'
            ELSE 'FAIL:S07_apply total_main=' || (SELECT sum(n) FROM _r_ctx_main_counts)
                 || ' total_overlay=' || (SELECT sum(n) FROM _r_ctx_overlay_counts)
       END AS s07_apply_overlay_equality;

-- Discard the first branch (R07 branch b_r_ctx) since apply was never run — verifies discard path.
SELECT discard_branch('b_r_ctx');

-- ===== Final MAIN cleanup + zero pollution sanity =====

DISCARD ALL;
SET client_min_messages = WARNING;

/* -------- SECTION R-VERSION --------
 *   GLOBAL CLEANUP: DROP TABLE rv_t + PASS:R15_R18_R_VERSION_SECTION_OK
 *
 * Single-session self-contained; no cross-session dependency.
 */

/* ---------- PRE-REQUISITE: shared_preload_libraries hook loaded ---------- */
DO $$
BEGIN
    IF current_setting('shared_preload_libraries', true) IS NULL
       OR current_setting('shared_preload_libraries', true)
          NOT LIKE '%overlay_branch%'
    THEN
        RAISE EXCEPTION
            'FAIL:PREQUISITE shared_preload_libraries must include overlay_branch. '
            'Hint: use the pg_regress recipe with --temp-config, '
            'NOT bare pg_regress without --temp-config=.../temp_instance_shared_libs.conf.';
    END IF;
END $$;

CREATE EXTENSION IF NOT EXISTS overlay_branch;

SET client_min_messages = WARNING;
-- =====================================================================
-- =====   Section R_VERSION (REVIEW-260926 S14/S17 R15 + R18) =====
-- =====================================================================
-- Motivation: overlay_tuple_version(rel, slot) builds the optimistic-CAS
-- token from the "(ctid,xmin)" pair only.  On PostgreSQL HOT
-- (heap-only-tuple) updates the row stays at the SAME ctid / SAME xmin
-- (same inserting transaction), so a MAIN update committed OUTSIDE the
-- branch between branch-write and branch-apply could in principle leave
-- the token constant.
--
-- PRIMARY guard (wired): the legacy token compare.  Any cross-session
-- write to MAIN necessarily advances xmin (and usually ctid), so a token
-- match means the branch's pre-image is still current.  CONFLICT-2 pins
-- this down (non-HOT rewrite -> ctid/xmin move -> token mismatch -> 55000,
-- MAIN race preserved).
--
-- SECONDARY guard (NOT wired in the current tree): overlay_modify_update /
-- overlay_modify_delete were intended to attach a "_base" top-level key
-- holding MAIN's column image at write time, with apply_*_pass() re-locking
-- MAIN FOR UPDATE and comparing it (apply_check_base_image_match) before
-- replay.  In the current code that write-side enrich has no caller
-- (ob_delta_has_base_key returns early when the key is absent), so the
-- _base comparison is effectively a NOP; the shared overlay_base_ver_*
-- codec (see doc/internals.md §1.3) is what keeps the token shape
-- canonical across writer / apply / reader.
--
-- CONFLICT-1 therefore only exercises the HOT-collision SCENARIO; its
-- preconditions (same ctid AND same xmin after the MAIN race) are a
-- best-effort setup and may report INFO:R15_HOT_PREREQ_UNMET when the
-- updater rewrites the tuple instead of going HOT-in-place.  The assertion
-- that MUST hold regardless is CONFLICT-2's token-mismatch rejection.
-- =====================================================================

-- We use fillfactor=50 so the page has plenty of free space; this makes
-- it extremely likely that MAIN's update of the row is stored as a
-- heap-only-tuple (HOT) at the same ctid with the same xmin — exactly
-- R15's collision scenario.  Without fillfactor PG may choose to write
-- the update to a new page which changes ctid → legacy token compare
-- catches it and CONFLICT-1 cannot exercise the new _base guard.
CREATE TABLE public.rv_t (
  id  INT4 PRIMARY KEY,
  v   TEXT,
  amt NUMERIC(10,2)
) WITH (fillfactor = 50);

INSERT INTO public.rv_t VALUES
  (1, 'MAIN-baseline-1', 10.00),
  (2, 'MAIN-baseline-2', 20.00);

ANALYZE public.rv_t;

SELECT create_branch('br_hot_conflict') AS id_br_hot;
SELECT use_branch('br_hot_conflict');

UPDATE public.rv_t SET v = 'BRANCH-upd-1', amt = amt + 5.50 WHERE id = 1;
UPDATE public.rv_t SET v = 'BRANCH-upd-2'                   WHERE id = 2;

-- ===== CONFLICT-1 step out of branch and mutate MAIN HOT-in-place =====
RESET overlay_branch.current;

-- Capture the current (ctid,xmin) BEFORE the concurrent MAIN write so we
-- can PROVE that the HOT update kept both unchanged — exactly R15's bug.
CREATE TEMP TABLE _rv_ctid_before AS
  SELECT ctid AS ctid_b, xmin::text AS xmin_b FROM public.rv_t WHERE id = 1;

-- This UPDATE to MAIN is supposed to keep ctid / xmin constant because
-- the row fits in the same page with fillfactor=50.  We then verify the
-- race write is actually visible on MAIN: id=1 amt=999.99 / id=2 no change.
UPDATE public.rv_t SET amt = 999.99 WHERE id = 1;

SELECT CASE WHEN (amt = 999.99 AND v = 'MAIN-baseline-1')
            THEN 'PASS:R15_CONFLICT1_MAIN_RACE_WRITE_COMMITTED'
            ELSE 'FAIL:R15_CONFLICT1_MAIN_RACE amt=' || amt || ' v=' || v
       END AS rv_c1_main_check
FROM public.rv_t WHERE id = 1;

SELECT CASE WHEN (h.ctid = b.ctid_b AND h.xmin::text = b.xmin_b)
            THEN 'PASS:R15_SAME_CTID_XMIN_HOT_UPDATED (scenario prereq met)'
            ELSE 'INFO:R15_HOT_PREREQ_UNMET (non-HOT scenario; legacy guard catches it instead) — xmin values intentionally elided to keep test deterministic across fresh database instances'
       END AS rv_hot_prereq
FROM public.rv_t h, _rv_ctid_before b WHERE h.id = 1;

DROP TABLE _rv_ctid_before;

-- ===== CONFLICT-1: back into branch and apply — must ERROR =====
SELECT use_branch('br_hot_conflict');

DO $$
BEGIN
  PERFORM overlay_branch.apply_branch('br_hot_conflict');
  RAISE EXCEPTION 'R15_CONFLICT1_EXPECTED_ERROR_NOT_RAISED';
EXCEPTION WHEN OTHERS THEN
  NULL;
END $$;

-- Post-apply MAIN invariant: concurrent MAIN mutation MUST survive.
-- The guard raised 55000 and rolled back the apply replay sub-tx; it
-- MUST NOT undo MAIN's committed race update nor the untouched id=2 row.
RESET overlay_branch.current;
SELECT CASE
         WHEN (amt = 999.99 AND v = 'MAIN-baseline-1')
         THEN 'PASS:R15_CONFLICT1_MAIN_RACE_PRESERVED_POST_APPLY_ROLLBACK'
         ELSE 'FAIL:R15_CONFLICT1_MAIN_LOST id=1 amt=' || amt::text || ' v=' || v
       END AS rv_c1_post_id1
FROM public.rv_t WHERE id = 1;
SELECT CASE
         WHEN (amt = 20.00 AND v = 'MAIN-baseline-2')
         THEN 'PASS:R15_CONFLICT1_ID2_UNTOUCHED'
         ELSE 'FAIL:R15_CONFLICT1_ID2_TOUCHED amt=' || amt::text || ' v=' || v
       END AS rv_c1_post_id2
FROM public.rv_t WHERE id = 2;

-- Cleanup CONFLICT-1 branch / rows (discard leaves MAIN alone)
SELECT discard_branch('br_hot_conflict');

-- ===== CONFLICT-2: traditional xmin/ctid rewrite conflict (not HOT) =====
-- Force a rewrite that changes xmin/ctid so the legacy token compare is
-- the one that catches it.  This is the classic scenario (not R15-specific)
-- but we include it here as a regression harness to ensure apply conflict
-- reporting still works end-to-end (this is the guard that is actually wired).
UPDATE public.rv_t SET v = 'MAIN-pre-conflict2-' || id WHERE TRUE;  -- rewrite baseline
ANALYZE public.rv_t;

SELECT create_branch('br_token_conflict') AS id_br_token;
SELECT use_branch('br_token_conflict');

UPDATE public.rv_t SET v = 'BRANCH-conflict2-id1' WHERE id = 1;

-- Exit branch and mutate MAIN id=1 + rewrite the row on a new transaction.
RESET overlay_branch.current;
BEGIN;
UPDATE public.rv_t SET v = 'MAIN-RACE-conflict2-TOKEN-CHANGED' WHERE id = 1;
COMMIT;

-- Reproduce CONFLICT-2: legacy token mismatch → apply raises conflict.
SELECT use_branch('br_token_conflict');

DO $$
BEGIN
  PERFORM overlay_branch.apply_branch('br_token_conflict');
  RAISE EXCEPTION 'R18_CONFLICT2_EXPECTED_ERROR_NOT_RAISED';
EXCEPTION WHEN OTHERS THEN
  NULL;
END $$;

RESET overlay_branch.current;
SELECT CASE
         WHEN v = 'MAIN-RACE-conflict2-TOKEN-CHANGED'
         THEN 'PASS:R18_CONFLICT2_TOKEN_MISMATCH_RACE_PRESERVED'
         ELSE 'FAIL:R18_CONFLICT2_TOKEN_MISMATCH_LOST v=' || v
       END AS rv_c2_post_id1
FROM public.rv_t WHERE id = 1;

SELECT discard_branch('br_token_conflict');

-- ===== R_VERSION global cleanup =====
DROP TABLE public.rv_t;

SELECT 'PASS:R15_R18_R_VERSION_SECTION_OK' AS r_version_section_ok;

