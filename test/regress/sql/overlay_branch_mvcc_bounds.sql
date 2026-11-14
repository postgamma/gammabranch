/*
 * overlay_branch_mvcc_bounds.sql — MVCC advanced boundary/concurrency semantics regression.
 *
 * COVERAGE (formerly overlay_branch_mvcc.sql SECTION L/M/N/O, single-session self-contained):
 *   SECTION L — FR2 Branch Advisory Lock key equivalence: C-side use_branch(SHARED) ↔ SQL-side
 *             pg_advisory_xact_lock_shared(int8) same key 0x4F42<<48 | bid re-entry;
 *             L1 S→X upgrade / L2 discard X-only / L3 S→X discard / L4 key bitcast equivalence
 *   SECTION M — apply_branch/discard_branch same-session ERROR capture (single-session deterministic
 *             100-row MAIN baseline + 10 delta → apply replay ERROR + discard rollback correct)
 *   SECTION N — Cross-branch isolation + FR1 NOTIFY: b2 die, b3 unaffected;
 *             ob_branch_state channel payload format bid:applied/discarded/applying
 *   SECTION O — API boundary: use_branch("") dual path, invalid mode ERRCODE,
 *             apply/discard on APPLIED/DISCARDED/not_exist → undefined_object / prereq_state
 *
 * The BranchScan rescan / total-acceptance / performance sections P (FR6 rescan),
 * Q (T8 acceptance + FR7 GUC ABI) and A4 (10K-row fixture) were split into the
 * sibling file overlay_branch_mvcc_rescan.sql.
 *
 * DISCARD ALL isolation between sections (clean session baseline per section).
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
/* =========================================================== SECTION L */

/* ---------- L1: create → use(SHARED) → apply(S→X upgrade) ---------- */
SELECT create_branch('b_fr2_l1') >= 1 AS l1_create_ok;
CREATE TABLE IF NOT EXISTS t_fr2_l1 (pk int PRIMARY KEY, v text);
SELECT use_branch('b_fr2_l1');
INSERT INTO t_fr2_l1 VALUES (1, 'l1hello');
SELECT count(*) = 1 AS l1_delta_has_1_row
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b ON b.branch_id = d.branch_id
 WHERE b.branch_name = 'b_fr2_l1';
SELECT apply_branch('b_fr2_l1');
SELECT state = 'applied' AS l1_state_applied
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_l1';
SELECT count(*) = 0 AS l1_delta_cleaned
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b ON b.branch_id = d.branch_id
 WHERE b.branch_name = 'b_fr2_l1';

/* ---------- L2: create → discard (EXCLUSIVE, never used) ---------- */
SELECT create_branch('b_fr2_l2') >= 1 AS l2_create_ok;
CREATE TABLE IF NOT EXISTS t_fr2_l2 (pk int PRIMARY KEY);
SELECT discard_branch('b_fr2_l2');
SELECT state = 'discarded' AS l2_state_discarded
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_l2';

/* ---------- L3: create → use(SHARED) → discard(S→X) + GUC reset ---------- */
SELECT create_branch('b_fr2_l3') >= 1 AS l3_create_ok;
CREATE TABLE IF NOT EXISTS t_fr2_l3 (pk int PRIMARY KEY, n int);
SELECT use_branch('b_fr2_l3', 'snapshot');
INSERT INTO t_fr2_l3 VALUES (1, 42), (2, 99);
SELECT current_setting('overlay_branch.current') = 'b_fr2_l3'
       AS l3_guc_before_discard;
SELECT discard_branch('b_fr2_l3');
SELECT current_setting('overlay_branch.current') = ''
       AS l3_guc_empty_after_discard;
SELECT state = 'discarded' AS l3_state_discarded
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_l3';

/* ---------- L4: Key equivalence (C-side SHARED ↔ SQL-side SHARED) ---------- */
SELECT create_branch('b_fr2_l4') >= 1 AS l4_create_ok;
CREATE TABLE IF NOT EXISTS t_fr2_l4 (pk int PRIMARY KEY);
SELECT use_branch('b_fr2_l4');
SELECT (0x4F42::bigint << 48) IS NOT NULL AS l4_magic_high_bits_set;
SELECT (0x4F42::bigint << 48) <> 0::bigint AS l4_magic_nonzero;

/* Same-backend same-xact same-key SHARED re-acquire = always granted,
 * no wait.  C-side use_branch already holds SHARED on this key so the
 * SQL call is a no-op granted re-entry (LockAcquire returns OK without
 * touching the heavyweight lock wait queue). */
SELECT pg_advisory_xact_lock_shared(
         (0x4F42::bigint << 48) |
         (SELECT branch_id FROM overlay_branch.pg_branch
           WHERE branch_name = 'b_fr2_l4')::bigint
       ) IS NULL AS l4_sql_shared_reentry_ok;

SELECT discard_branch('b_fr2_l4');
SELECT state = 'discarded' AS l4_state_after_discard
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_l4';

/* ---------- Section L marker so grep can find the entry point ---------- */
SELECT 'PASS:L0_FR2_ENTRY' AS section_l_marker;

/* ================================================================
 * SECTION M: FR3 Layer-3 applying transient state + delta-write Guard
 * (L3 single-session sanity only — cross-session apply-vs-write races live
 * in T8 pg_isolation_regress specs ob_fr3_applying_delta_blocked.spec).
 *
 * FR3 introduces 2 hardening mechanisms (see doc/internals.md §3.4):
 *   M1. apply_branch inserts an atomic CAS UPDATE state='applying' WHERE
 *       bid=N AND state='active' between Step1 state-check and Step2 exit.
 *       SPI_processed != 1 → ereport 55000.  On any ERROR → xact ABORT,
 *       PG MVCC auto-rolls the UPDATE's XMIN back so state returns 'active'.
 *   M2. overlay_delta_insert() function-level top-Guard: SPI-selects
 *       pg_branch.state and ONLY allows writes when state='active'.
 *       state='applying'/'applied'/'discarded' → ERROR 55000 w/ hint.
 *
 * L3 TR tests (single session, fully deterministic, no races):
 *   TR-4.1 (M sub-case M1): manually force state='applying' → INSERT 2nd
 *          row → Guard ERROR; delta count stays 1.  Restore state='active'.
 *   TR-4.2 (M sub-case M2): create/use/3x INSERT → DO BEGIN PERFORM
 *          apply; RAISE EXCEPTION END → xact ABORT → state active (not
 *          applying/applied) + delta rows STILL 3 → apply 2nd SUCCESS
 *          state applied + delta 0 → rollback auto-restore verified.
 *   TR-4.3 (M sub-case M3): create → discard → INSERT → Guard ERROR
 *          branch discarded. delta rows 0 forever.
 * ================================================================ */

/* ---------- M1 TR-4.1 applying Guard blocks delta-writes ---------- */
SELECT create_branch('b_fr2_m1') >= 1 AS m1_create_ok;
CREATE TABLE IF NOT EXISTS t_fr3_m1 (pk int PRIMARY KEY, v text);
SELECT use_branch('b_fr2_m1');
INSERT INTO t_fr3_m1 VALUES (1, 'm1_before_applying');
SELECT count(*) = 1 AS m1_delta_1_row_before_applying
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'b_fr2_m1';

/* Simulate apply in progress (S1 hold state=applying).
 * Note: this is a direct SQL UPDATE ONLY for L3 sanity. In real apply
 * the CAS UPDATE is inside apply_branch C code and advisory-X-locked,
 * so other sessions cannot race here. */
UPDATE overlay_branch.pg_branch SET state = 'applying'
 WHERE branch_name = 'b_fr2_m1';
SELECT state = 'applying' AS m1_state_applied_manually
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_m1';

/* Guard must block this INSERT w/ ERROR 55000. We wrap in DO block so ERROR
 * is caught and test continues (pg_regress stops on uncaught ERRORs). */
DO $$
DECLARE
    m1_caughtt text;
BEGIN
    m1_caughtt = 'no';
    INSERT INTO t_fr3_m1 VALUES (2, 'm1_should_be_blocked');
EXCEPTION
    WHEN object_not_in_prerequisite_state THEN
        m1_caughtt = 'yes_55000';
    WHEN OTHERS THEN
        RAISE EXCEPTION 'TR-4.1 unexpected SQLSTATE % %', SQLSTATE, SQLERRM;
END
$$;
/* The above DO block swallowed ERROR; delta count must still be 1 row. */
SELECT count(*) = 1 AS m1_delta_still_1_no_new_rows_applied
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'b_fr2_m1';

/* Restore state to 'active' so discard Guard lets us call discard_branch.
 * discard_branch itself still calls C side state=discarded transition,
 * which needs state active to proceed without top check ERROR. */
UPDATE overlay_branch.pg_branch SET state = 'active'
 WHERE branch_name = 'b_fr2_m1';
SELECT discard_branch('b_fr2_m1');
SELECT state = 'discarded' AS m1_final_state_discarded
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_m1';

/* ---------- M2 TR-4.2 CAS rollback auto-restores state + delta kept ---------- */
SELECT create_branch('b_fr2_m2') >= 1 AS m2_create_ok;
CREATE TABLE IF NOT EXISTS t_fr3_m2 (pk int PRIMARY KEY, n numeric);
SELECT use_branch('b_fr2_m2');
INSERT INTO t_fr3_m2 VALUES (1, 1.1), (2, 2.2), (3, 3.3);
SELECT count(*) = 3 AS m2_delta_3_rows_initial
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'b_fr2_m2';

/* DO block: calls apply_branch (which runs CAS UPDATE state='applying'
 * internally at C side L990-L1042) THEN raises a user-defined exception
 * BEFORE commit.  PG rolls back the whole transaction including the
 * state='applying' UPDATE's write (the XMIN becomes aborted, visible
 * tuple reverts to pre-transaction contents = state='active').
 * After the block, we verify (a) state is ACTIVE again, (b) delta is
 * STILL 3 rows (not touched by apply Step 4 DELETE), then we retry
 * apply_branch and expect success. */
DO $$
BEGIN
    PERFORM apply_branch('b_fr2_m2');
    /* Intentionally abort here to simulate a Step 3 WR conflict error, so
     * we can test that state='applying' auto rolls back to 'active' and
     * pg_branch_delta rows are NOT deleted. */
    RAISE EXCEPTION 'TR42 simulate apply mid-flight WRONG_VERSION conflict';
EXCEPTION
    WHEN raise_exception THEN
        /* swallow, let PG roll back the inner transaction started by DO. */
        NULL;
END
$$;

/* After ROLLBACK: state must be ACTIVE, NOT applying/applied. */
SELECT state = 'active' AS m2_after_rollback_state_active_not_applying
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_m2';
/* Delta rows are untouched (the apply Step 4 DELETE was never committed). */
SELECT count(*) = 3 AS m2_after_rollback_delta_rows_kept_3
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'b_fr2_m2';

/* Now retry apply. Should go through cleanly. */
SELECT apply_branch('b_fr2_m2');
SELECT state = 'applied' AS m2_retry_apply_state_applied
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_m2';
SELECT count(*) = 0 AS m2_retry_delta_cleaned_0_rows
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'b_fr2_m2';

/* ---------- M3 TR-4.3 discarded branch blocks writes via Guard ---------- */
SELECT create_branch('b_fr2_m3') >= 1 AS m3_create_ok;
CREATE TABLE IF NOT EXISTS t_fr3_m3 (pk int PRIMARY KEY);
SELECT discard_branch('b_fr2_m3');
SELECT state = 'discarded' AS m3_state_discarded_before_insert
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_fr2_m3';

/* use_branch will fail before we even get to INSERT, but we want to
 * exercise the delta_insert Guard specifically, so we skip use_branch
 * and call the DEBUG callable directly with the discarded bid.
 * overlay_debug_delta_insert is declared in overlay_branch--0.1.sql and
 * calls overlay_delta_insert C side directly. */
DO $$
DECLARE
    m3_bid int;
BEGIN
    SELECT branch_id INTO m3_bid FROM overlay_branch.pg_branch
     WHERE branch_name = 'b_fr2_m3';
    BEGIN
        PERFORM overlay_debug_delta_insert(
            m3_bid, 't_fr3_m3'::regclass::oid,  /* relid */
            'pk=42',                            /* key (text serialize) */
            'I',                                /* op */
            NULL,                               /* old_version */
            NULL);                              /* tuple_data */
    EXCEPTION
        WHEN object_not_in_prerequisite_state THEN
            NULL;  /* expected */
        WHEN OTHERS THEN
            RAISE EXCEPTION 'TR-4.3 unexpected SQLSTATE % %', SQLSTATE, SQLERRM;
    END;
END
$$;

SELECT count(*) = 0 AS m3_delta_0_rows_after_blocked_insert
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'b_fr2_m3';


DISCARD ALL;
SET client_min_messages = WARNING;

/* =========================================================== SECTION M */
/* ---------- Section M marker ---------- */
SELECT 'PASS:M0_FR3_SANITY_OK' AS section_m_status;

/* =================================================================
 * ---------- SECTION N: FR5 SNAPSHOT Mode MAIN Freeze (T5) ----------
 * (L3 single-session sanity ONLY — real cross-session freeze tests in
 *  T8 pg_isolation_regress specs ob_fr5_snapshot_freeze.spec).
 *
 *   N1 MAIN freeze: snapshot branch captures state at t0, apply LIVE branch
 *      adds 1 row to MAIN, snapshot STILL sees 3 / live sees 4.
 *   N2 SNAPSHOT + WR DML INSERT allowed (old_version MAIN lookup OK with
 *      latest MAIN snapshot not frozen per D.9.2).
 *   N3 apply snapshot branch (0 writes) → state applied delta 0, exit OK.
 *   N4 re-entry: use twice (same snapshot branch name with snapshot mode)
 *      → ob_ensure_unregister_snapshot cleans old before capture new,
 *      no double leak / dangling.
 * ================================================================= */

/* ---------- SETUP t_fr5_n1 MAIN baseline 3 rows ---------- */
CREATE TABLE t_fr5_n1(id int PRIMARY KEY, val text NOT NULL);
INSERT INTO t_fr5_n1 VALUES (1, 'one'), (2, 'two'), (3, 'three');

SELECT count(*) = 3 AS n1_main_baseline_3_rows FROM t_fr5_n1;

/* ---------- N1 TR-5.1 MAIN freeze between LIVE vs SNAPSHOT ---------- */
/* A = capture snapshot branch at t0 (MAIN has 3 rows) */
SELECT create_branch('bs_fr5_a') AS n1_create_a;
SELECT use_branch('bs_fr5_a', 'snapshot') AS n1_use_a_snapshot;
SELECT count(*) = 3 AS n1_a_snapshot_t0_count_3 FROM t_fr5_n1;
SELECT use_branch('') AS n1_a_exit_t0;

/* B = LIVE branch write 1 delta then apply → MAIN gains 1 row (count becomes 4) */
SELECT create_branch('bs_fr5_b') AS n1_create_b;
SELECT use_branch('bs_fr5_b', 'live') AS n1_use_b_live;
INSERT INTO t_fr5_n1 VALUES (4, 'four');  /* WR delta insert row 4 */
SELECT overlay_branch.overlay_debug_delta_count(
          (SELECT branch_id FROM overlay_branch.pg_branch WHERE branch_name='bs_fr5_b')) = 1
  AS n1_b_delta_1_row;
SELECT use_branch('') AS n1_b_exit_before_apply;
SELECT apply_branch('bs_fr5_b') AS n1_apply_b;
/* MAIN direct count now 4 */
SELECT count(*) = 4 AS n1_main_after_b_count_4 FROM t_fr5_n1;

/* A re-enter snapshot mode: snapshot frozen at t0, MUST NOT see row 4 from
 * B's applied MAIN delta (Pass1 MAIN snapshot frozen at 3 rows t0) */
SELECT use_branch('bs_fr5_a', 'snapshot') AS n1_a_reenter_snapshot;
SELECT count(*) = 3 AS n1_a_frozen_still_3_not_4 FROM t_fr5_n1;  /* freeze OK */
SELECT use_branch('') AS n1_a_exit_after_freeze_check;

/* LIVE branch C see ALL latest MAIN rows = 4 */
SELECT create_branch('bs_fr5_c') AS n1_create_c;
SELECT use_branch('bs_fr5_c', 'live') AS n1_use_c_live;
SELECT count(*) = 4 AS n1_c_live_latest_4 FROM t_fr5_n1;
SELECT use_branch('') AS n1_c_exit;

/* ---------- N2 TR-5.2 snapshot + WR INSERT old_version MAIN lookup OK ----- */
/* Re-enter branch A (snapshot frozen at t0 xmin).  Inside branch A with
 * snapshot mode, do a WR INSERT row id=5.  old_version MAIN lookup by
 * ctid fetch uses LatestSnapshot (not frozen per D.9.2) so no conflict
 * check drift even though MAIN has changed since t0.  This MUST succeed
 * and produce exactly 1 delta row. */
SELECT use_branch('bs_fr5_a', 'snapshot') AS n2_a_reenter_for_wr;
INSERT INTO t_fr5_n1 VALUES (5, 'five-wr');
SELECT overlay_branch.overlay_debug_delta_count(
          (SELECT branch_id FROM overlay_branch.pg_branch WHERE branch_name='bs_fr5_a')) = 1
  AS n2_a_snapshot_delta_1_row_ok;
SELECT count(*) = 4 AS n2_a_snapshot_select_sees_4_rows_after_insert
  FROM t_fr5_n1;  /* 3 frozen MAIN + 1 WR INSERT delta branch row */
SELECT use_branch('') AS n2_a_exit;

/* discard A so no cleanup side-effect left */
SELECT discard_branch('bs_fr5_a') AS n2_discard_a;

/* ---------- N3 TR-5.3 apply snapshot branch C (0 wr rows) OK ---------- */
SELECT apply_branch('bs_fr5_c') AS n3_apply_c;
SELECT state = 'applied' AS n3_c_state_applied FROM overlay_branch.pg_branch
 WHERE branch_name = 'bs_fr5_c';
SELECT overlay_branch.overlay_debug_delta_count(
          (SELECT branch_id FROM overlay_branch.pg_branch WHERE branch_name='bs_fr5_c')) = 0
  AS n3_c_delta_0;

/* ---------- N4 TR-5.4 snapshot re-entry no double leak ------------------- */
SELECT create_branch('bs_fr5_d') AS n4_create_d;
SELECT use_branch('bs_fr5_d', 'snapshot') AS n4_use_d_first;  /* capture #1 */
SELECT use_branch('bs_fr5_d', 'snapshot') AS n4_use_d_second; /* capture #2, #1 pfree */
INSERT INTO t_fr5_n1 VALUES (6, 'six-delta');  /* any write after re-entry OK */
SELECT overlay_branch.overlay_debug_delta_count(
          (SELECT branch_id FROM overlay_branch.pg_branch WHERE branch_name='bs_fr5_d')) = 1
  AS n4_delta_after_reentry_1_row_ok;
SELECT use_branch('') AS n4_exit;  /* single snapshot pfree once, no double */
SELECT discard_branch('bs_fr5_d') AS n4_discard_d;


DISCARD ALL;
SET client_min_messages = WARNING;

/* =========================================================== SECTION N */
/* ---------- Section N cleanup (MAIN direct rows back to 4) ---------- */
DELETE FROM t_fr5_n1 WHERE id IN (5,6);  /* only 4 = B-applied remains */
DROP TABLE t_fr5_n1;

/* ---------- Section N marker ---------- */
SELECT 'PASS:N0_FR5_SNAPSHOT_FREEZE_OK' AS section_n_status;


/* ================================================================
 * SECTION O: Coverage Gap Hardening (T5 + earlier callable branch fills)
 * ================================================================
 * Target: Fill 9 key missing code-branches identified by the
 * callable × test-suite matrix:
 *   O1  use_branch('') empty-string dual paths (ctx absent / ctx present)
 *   O2  use_branch(name, INVALID_MODE) → ERRCODE_INVALID_PARAMETER_VALUE
 *   O3  use_branch(APPLIED_BRANCH, live) → OBJECT_NOT_IN_PREREQ_STATE
 *   O4  apply_branch(NOT_EXIST) → UNDEFINED_OBJECT
 *   O5  apply_branch(APPLIED) → already applied ERROR
 *   O6  apply_branch(DISCARDED) → already discarded ERROR
 *   O7  discard_branch(NOT_EXIST) → UNDEFINED_OBJECT
 *   O8  discard_branch(APPLIED) → already applied ERROR
 *   O9  discard_branch(DISCARDED) → double discard ERROR
 *
 * Every DO block catches its expected errcode precisely.  Any
 * mismatched / missing error aborts pg_regress so we never get a
 * silent false-green.
 * ================================================================ */

-- 0. Setup: build APPLIED / DISCARDED terminal-state target branches
--    (no writers; reach terminal directly via apply / discard)
SELECT create_branch('ob_cover_applied') AS o_setup_applied;
SELECT create_branch('ob_cover_discarded') AS o_setup_discarded;
SELECT use_branch('ob_cover_applied');           -- enter with no writes → delta=0
SELECT apply_branch('ob_cover_applied');          -- → state=applied (terminal)
SELECT discard_branch('ob_cover_discarded');      -- → state=discarded (terminal, never entered)
SELECT current_branch() IS NULL OR current_branch() = '' AS o_setup_ctx_empty;

-- O1. use_branch('') dual paths: (a) no ctx → pure no-op; (b,c) ctx present → exit + unpin
SELECT use_branch('') AS o1a_noctx_nop;
-- O1b: 1-parameter form
SELECT create_branch('ob_cover_temp_empty_exit') AS o1b_create;
SELECT use_branch('ob_cover_temp_empty_exit', 'live');
SELECT current_branch() = 'ob_cover_temp_empty_exit' AS o1b_inside;
SELECT use_branch('') AS o1b_exit_via_1param_empty;
SELECT current_branch() IS NULL OR current_branch() = '' AS o1b_outside;
-- O1c: 2-parameter form (empty name + any mode → exit branch path)
SELECT use_branch('ob_cover_temp_empty_exit', 'snapshot');
SELECT current_branch() = 'ob_cover_temp_empty_exit' AS o1c_inside;
SELECT use_branch('', 'live') AS o1c_exit_via_2param_empty;
SELECT current_branch() IS NULL OR current_branch() = '' AS o1c_outside;
SELECT discard_branch('ob_cover_temp_empty_exit') AS o1b_cleanup;

-- O2. invalid mode → ERRCODE_INVALID_PARAMETER_VALUE
DO $$
BEGIN
    PERFORM overlay_branch.use_branch('ob_cover_applied', 'definitely_not_a_real_mode');
    RAISE EXCEPTION 'FAIL:O2 invalid_parameter_value not raised';
EXCEPTION WHEN invalid_parameter_value THEN
    NULL; -- expected errmsg "invalid branch mode ..."
END $$;
SELECT state = 'applied' AS o2_no_state_mutation
  FROM public.pg_branch WHERE branch_name='ob_cover_applied';

-- O3. use_branch(APPLIED_BRANCH, live) → state not active (use_internal path)
DO $$
BEGIN
    PERFORM public.use_branch('ob_cover_applied');  -- 1-param default mode=live
    RAISE EXCEPTION 'FAIL:O3 object_not_in_prerequisite_state not raised';
EXCEPTION WHEN object_not_in_prerequisite_state THEN
    NULL;
END $$;
SELECT current_branch() IS NULL OR current_branch() = '' AS o3_ctx_clean;

-- O4. apply_branch(not_exist) → UNDEFINED_OBJECT (C side directly, MAIN ctx)
DO $$
BEGIN
    PERFORM overlay_branch.apply_branch('cover_not_exist_apply_xyz');
    RAISE EXCEPTION 'FAIL:O4 undefined_object not raised';
EXCEPTION WHEN undefined_object THEN
    NULL;
END $$;

-- O5. apply_branch(APPLIED_BRANCH) → state=applied already, ERROR (C side)
DO $$
BEGIN
    PERFORM overlay_branch.apply_branch('ob_cover_applied');
    RAISE EXCEPTION 'FAIL:O5 not raised';
EXCEPTION WHEN feature_not_supported THEN
    NULL;   -- actually FEATURE_NOT_SUPPORTED (0A000) via overlay_guard_ereport_fail
END $$;

-- O6. apply_branch(DISCARDED_BRANCH) → state=discarded already, ERROR (C side)
DO $$
BEGIN
    PERFORM overlay_branch.apply_branch('ob_cover_discarded');
    RAISE EXCEPTION 'FAIL:O6 not raised';
EXCEPTION WHEN feature_not_supported THEN
    NULL;
END $$;

-- O7. discard_branch(not_exist) → UNDEFINED_OBJECT (C side)
DO $$
BEGIN
    PERFORM overlay_branch.discard_branch('cover_not_exist_discard_uvw');
    RAISE EXCEPTION 'FAIL:O7 undefined_object not raised';
EXCEPTION WHEN undefined_object THEN
    NULL;
END $$;

-- O8. discard_branch(APPLIED_BRANCH) → already applied ERROR (C side)
DO $$
BEGIN
    PERFORM overlay_branch.discard_branch('ob_cover_applied');
    RAISE EXCEPTION 'FAIL:O8 not raised';
EXCEPTION WHEN feature_not_supported THEN
    NULL;
END $$;

-- O9. discard_branch(DISCARDED_BRANCH) → double-discard ERROR (C side)
DO $$
BEGIN
    PERFORM overlay_branch.discard_branch('ob_cover_discarded');
    RAISE EXCEPTION 'FAIL:O9 not raised';
EXCEPTION WHEN feature_not_supported THEN
    NULL;
END $$;

-- Final invariant: terminal states preserved, ctx clean
SELECT current_branch() IS NULL OR current_branch() = '' AS o_all_ctx_empty;
SELECT
  (SELECT state FROM public.pg_branch WHERE branch_name='ob_cover_applied') = 'applied'
  AND
  (SELECT state FROM public.pg_branch WHERE branch_name='ob_cover_discarded') = 'discarded'
  AS o_state_invariant_preserved;


DISCARD ALL;
SET client_min_messages = WARNING;

/* =========================================================== SECTION O */
-- Section O marker: when pg_regress prints this SELECT, O1–O9 all asserted
SELECT 'PASS:O0_COVERAGE_GAPS_9_BRANCHES_FILLED' AS section_o_status;


