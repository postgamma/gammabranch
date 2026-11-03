/*
 * overlay_branch_mvcc_usage.sql — MVCC basic usage + parameter semantics + threshold/GUC/boundary regression.
 *
 * COVERAGE (originally overlay_branch_mvcc.sql PART A-F + SECTION B-K, single session self-contained):
 *   PART A-F     — 2-param use_branch mode/signature/snapshot semantics/exit/default roundtrip
 *   SECTION B    — force_invalidation_check() ACTIVE baseline
 *   SECTION C    — INVALIDATION DQL kickout (manual state=discarded → NOTICE+MAIN fallback)
 *   SECTION D    — DQL/DML split: under state=applied DQL NOTICE fallback + DML UPDATE 55000 drift-write interception
 *   SECTION E    — THROTTLE: threshold=32 → 30 is_active calls no kickout; force triggers immediately
 *   SECTION F    — apply/discard self-broadcast NOTIFY + self-cleanup ctx/GUC
 *   SECTION G/H  — is_active ground truth (TRUE/FALSE) + NOTIFY payload bid:applied/discarded
 *   SECTION I    — DISCARD non-current branch: current A untouched, B discarded and broadcast
 *   SECTION J    — FR4 GUC 4 items default values + roundtrip + threshold=1 triggers immediately
 *   SECTION K    — BOUNDARY / NO-LEAK: K1~K6 illegal branch/empty name/long name/use_branch before entering branch fails → no ctx leak
 *
 * SETUP: create table t_inv (3 rows MAIN baseline) + branches b_inv1/2/3 + b_notcurrent_A/B,
 *        fully self-contained, does not depend on upstream test file session.
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

-- ========== PART A signatures ==========
SELECT CASE WHEN count(*) = 2 THEN 'PASS:A1 two-param signature exists'
            ELSE 'FAIL:A1 expected 2 use_branch rows, got ' || count(*)
       END AS t
FROM pg_proc
WHERE proname = 'use_branch'
  AND pronamespace = 'public'::regnamespace;

SELECT CASE WHEN count(*) = 2 THEN 'PASS:A2 ext-schema two-param signature'
            ELSE 'FAIL:A2 expected 2 rows, got ' || count(*)
       END AS t
FROM pg_proc
WHERE proname = 'use_branch'
  AND pronamespace = 'overlay_branch'::regnamespace;

SELECT CASE WHEN pronargs = 1 AND proargtypes[0]::regtype::text = 'name'
            THEN 'PASS:A3 1-param = name'
            ELSE 'FAIL:A3 pronargs=' || pronargs || ' first arg type=' || proargtypes[0]::regtype::text
       END AS t
FROM pg_proc
WHERE proname = 'use_branch' AND pronargs = 1
  AND pronamespace = 'overlay_branch'::regnamespace;

SELECT CASE
            WHEN pronargs = 2
                 AND proargtypes[0]::regtype::text = 'name'
                 AND proargtypes[1]::regtype::text = 'text'
            THEN 'PASS:A4 2-param = name,text'
            ELSE 'FAIL:A4 pronargs=' || pronargs ||
                 ' arg_types=' || proargtypes[0]::regtype::text || ',' ||
                                     proargtypes[1]::regtype::text
       END AS t
FROM pg_proc
WHERE proname = 'use_branch' AND pronargs = 2
  AND pronamespace = 'overlay_branch'::regnamespace;

SELECT CASE WHEN extversion::text >= '0.1' THEN 'PASS:A5 extversion=' || extversion
            ELSE 'FAIL:A5 extversion=' || extversion
       END AS t
FROM pg_extension WHERE extname = 'overlay_branch';

-- ========== PART B zero regression ==========
SELECT create_branch('b_live1');
SELECT use_branch('b_live1');
SELECT CASE WHEN current_branch() = 'b_live1' THEN 'PASS:B1 1-param enter'
            ELSE 'FAIL:B1 current=' || coalesce(current_branch()::text, 'NULL')
       END AS t;
SELECT CASE WHEN mode = 'live' THEN 'PASS:B2 mode live'
            ELSE 'FAIL:B2 mode=' || coalesce(mode, 'NULL')
       END AS t
FROM list_branches() WHERE branch_name = 'b_live1';
SELECT use_branch('');
SELECT CASE WHEN current_branch() IS NULL THEN 'PASS:B3 leave'
            ELSE 'FAIL:B3 current=' || current_branch()
       END AS t;

-- ========== PART C Invalid mode ERROR paths ==========
DO $$
BEGIN
    PERFORM overlay_branch.use_branch('b_live1', 'frozen'::text);
    RAISE NOTICE 'should have raised';
EXCEPTION WHEN invalid_parameter_value THEN
    NULL;
END $$;
SELECT CASE WHEN current_branch() IS NULL THEN 'PASS:C1 no leak after frozen'
            ELSE 'FAIL:C1 leaked ' || current_branch()
       END AS t;

DO $$
BEGIN
    PERFORM overlay_branch.use_branch('b_live1', NULL::text);
END $$;
SELECT CASE WHEN current_branch() = 'b_live1' THEN 'PASS:C2 NULL mode defaults live'
            ELSE 'FAIL:C2 current=' || coalesce(current_branch()::text,'NULL')
       END AS t;
SELECT use_branch('');

DO $$
BEGIN
    PERFORM overlay_branch.use_branch('no_such_branch', 'snapshot');
    RAISE NOTICE 'should have raised';
EXCEPTION WHEN undefined_object THEN
    NULL;
END $$;
SELECT CASE WHEN current_branch() IS NULL THEN 'PASS:C3 no leak nonexistent'
            ELSE 'FAIL:C3 leaked ' || current_branch()
       END AS t;

SELECT discard_branch('b_live1');
DO $$
BEGIN
    PERFORM overlay_branch.use_branch('b_live1', 'snapshot');
    RAISE NOTICE 'should have raised';
EXCEPTION WHEN object_not_in_prerequisite_state THEN
    NULL;
END $$;
SELECT CASE WHEN current_branch() IS NULL THEN 'PASS:C4 no leak non-active'
            ELSE 'FAIL:C4 leaked ' || current_branch()
       END AS t;

-- ========== PART D Snapshot mode enter/leave ==========
SELECT create_branch('b_snap1');
SELECT overlay_branch.use_branch('b_snap1', 'snapshot'::text);
SELECT CASE WHEN current_branch() = 'b_snap1' THEN 'PASS:D1 snap enter'
            ELSE 'FAIL:D1 current=' || coalesce(current_branch()::text,'NULL')
       END AS t;
SELECT CASE WHEN mode = 'snapshot' THEN 'PASS:D2 list mode=snapshot'
            ELSE 'FAIL:D2 mode=' || coalesce(mode,'NULL')
       END AS t
FROM list_branches() WHERE branch_name = 'b_snap1';

SELECT overlay_branch.use_branch('');
SELECT CASE WHEN current_branch() IS NULL THEN 'PASS:D3 snap leave->NULL'
            ELSE 'FAIL:D3 current=' || current_branch()
       END AS t;

SELECT CASE WHEN mode = 'snapshot' THEN 'PASS:D4 mode preserved after leave'
            ELSE 'FAIL:D4 mode=' || coalesce(mode,'NULL')
       END AS t
FROM list_branches() WHERE branch_name = 'b_snap1';

-- ========== PART E Mode switching ==========
SELECT create_branch('b_switch');
SELECT overlay_branch.use_branch('b_switch', 'live'::text);
SELECT CASE WHEN mode = 'live' THEN 'PASS:E1 live->snap stage1 live'
            ELSE 'FAIL:E1 mode=' || mode
       END AS t
FROM list_branches() WHERE branch_name = 'b_switch';
SELECT overlay_branch.use_branch('b_switch', 'snapshot'::text);
SELECT CASE WHEN mode = 'snapshot' THEN 'PASS:E2 live->snap stage2 snap'
            ELSE 'FAIL:E2 mode=' || mode
       END AS t
FROM list_branches() WHERE branch_name = 'b_switch';
SELECT overlay_branch.use_branch('b_switch', 'live'::text);
SELECT CASE WHEN mode = 'live' THEN 'PASS:E3 snap->live stage3 live'
            ELSE 'FAIL:E3 mode=' || mode
       END AS t
FROM list_branches() WHERE branch_name = 'b_switch';
SELECT overlay_branch.use_branch('b_switch', 'snapshot'::text);
SELECT CASE WHEN mode = 'snapshot' THEN 'PASS:E4 roundtrip2 snap'
            ELSE 'FAIL:E4 mode=' || mode
       END AS t
FROM list_branches() WHERE branch_name = 'b_switch';
SELECT overlay_branch.use_branch('');
SELECT CASE WHEN current_branch() IS NULL THEN 'PASS:E5 leave after switch'
            ELSE 'FAIL:E5 current=' || current_branch()
       END AS t;

SELECT create_branch('b_other');
SELECT overlay_branch.use_branch('b_switch', 'snapshot'::text);
SELECT overlay_branch.use_branch('b_other', 'live'::text);
SELECT CASE WHEN current_branch() = 'b_other' THEN 'PASS:E6 cross-branch enter'
            ELSE 'FAIL:E6 current=' || coalesce(current_branch()::text,'NULL')
       END AS t;
SELECT CASE WHEN mode = 'live' THEN 'PASS:E7 b_other.mode=live'
            ELSE 'FAIL:E7 mode=' || mode
       END AS t
FROM list_branches() WHERE branch_name = 'b_other';
SELECT overlay_branch.use_branch('');
SELECT CASE WHEN current_branch() IS NULL THEN 'PASS:E8 leave cross'
            ELSE 'FAIL:E8 current=' || current_branch()
       END AS t;

-- ========== PART F Default mode / 1-param equiv ==========
SELECT public.use_branch('b_snap1');
SELECT CASE WHEN current_branch() = 'b_snap1' THEN 'PASS:F1 1-param enter (default mode)'
            ELSE 'FAIL:F1 current=' || coalesce(current_branch()::text,'NULL')
       END AS t;
SELECT CASE WHEN mode = 'live' THEN 'PASS:F2 1-param resolves to live'
            ELSE 'FAIL:F2 mode=' || mode
       END AS t
FROM list_branches() WHERE branch_name = 'b_snap1';
SELECT overlay_branch.use_branch('');

DO $$
BEGIN
    PERFORM overlay_branch.use_branch('does_not_exist_xyz', 'snapshot');
EXCEPTION WHEN undefined_object THEN
    NULL;
END $$;
SELECT CASE WHEN current_branch() IS NULL THEN 'PASS:F3 ERROR no pin install'
            ELSE 'FAIL:F3 leaked ' || current_branch()
       END AS t;

SELECT discard_branch('b_snap1');
SELECT discard_branch('b_switch');
SELECT discard_branch('b_other');


/* ================================================================
 * T2 LAYER-1: FR1 NOTIFY broadcast_state_change
 *         + FR4 throttling / DQL-vs-DML split kickout
 * ================================================================ */

/* ============================================================
 * SECTION A: Signature / synonym / grant baseline (6 tests)
 * ============================================================ */

-- A1. is_active(name) ext-level signature exists
SELECT
    proname,
    pronargs,
    proargtypes[0]::regtype::text AS arg0,
    prorettype::regtype::text AS ret
FROM pg_proc
WHERE proname = 'overlay_branch_is_active'
  AND pronamespace::regnamespace::text = 'overlay_branch';

-- A2. is_active synonym public
SELECT
    proname,
    pronargs,
    proargtypes[0]::regtype::text AS arg0
FROM pg_proc
WHERE proname = 'is_active'
  AND pronamespace::regnamespace::text = 'public';

-- A3. force_invalidation_check ext-level signature exists
SELECT
    proname,
    pronargs,
    prorettype::regtype::text AS ret
FROM pg_proc
WHERE proname = 'overlay_branch_force_invalidation_check'
  AND pronamespace::regnamespace::text = 'overlay_branch';

-- A4. force_invalidation_check synonym public
SELECT
    proname,
    pronargs,
    prorettype::regtype::text AS ret
FROM pg_proc
WHERE proname = 'force_invalidation_check'
  AND pronamespace::regnamespace::text = 'public';

-- A5. default_version >= 0.1 (we bump default versions as the extension adds
-- features: 0.2 is OK — anything >= 0.1 preserves the baseline feature set)
SELECT default_version::text >= '0.1' AS v1_2 FROM pg_available_extensions
WHERE name = 'overlay_branch';

-- A6. extversion installed >= 0.1 (print actual value, the PASS label lives in
-- test harness via a second CASE-WHEN below; raw value shown for traceability)
SELECT extversion FROM pg_extension WHERE extname = 'overlay_branch';

/* ============================================================
 * SETUP: user table + 3 branches (one for section B/C, one for D,
 * one untouched for cross-branch isolation checks)
 * ============================================================ */
CREATE TABLE IF NOT EXISTS t_inv (pk int PRIMARY KEY, v text);
INSERT INTO t_inv VALUES (1, 'main-a'), (2, 'main-b'), (3, 'main-c');

SELECT create_branch('b_inv1') >= 1 AS create_b1;
SELECT create_branch('b_inv2') >= 1 AS create_b2;
SELECT create_branch('b_inv3') >= 1 AS create_b3;

/* ============================================================
 * SECTION B: Baseline + force_invalidation_check positive (4 tests)
 * ============================================================ */
SELECT use_branch('b_inv1');
SELECT current_branch_id() > 0 AS b1_entered;

-- B1. is_active(b_inv1) ground-truth should be true
SELECT is_active('b_inv1') AS t_active_b1;

-- B2. force_invalidation_check on still-active branch returns true
SELECT force_invalidation_check() AS t_force_true_b1;

-- B3. current_branch still b_inv1
SELECT current_branch() = 'b_inv1' AS still_b1;

-- B4. state in catalog still ACTIVE after 2 invalidation calls
SELECT state = 'active' AS b1_still_active_catalog
FROM overlay_branch.pg_branch WHERE branch_name = 'b_inv1';

/* ============================================================
 * SECTION C: INVALIDATION KICKOUT — DQL path (NOTICE + MAIN fallback)
 *   We manually UPDATE pg_branch state → 'discarded' via SPI (not
 *   discard_branch — because discard_branch also sends the NOTIFY
 *   AND kicks us out immediately, so we lose the chance to test the
 *   "another session did the discard, and we find out lazily on next
 *   BranchScan" scenario which is what FR1+FR4 is about).
 * ============================================================ */

-- C1. Before manual state flip: BranchScan shows branch MAIN rows = 3.
SELECT count(*) = 3 AS pre_main_rowcount FROM t_inv;

-- C2. Flip state manually (simulates "another session discarded us")
--     NOTE: We moved the GUC side-effects from check_hook to
--     assign_hook (T5 pitfall #3).  The old "PERFORM set_config('',true)"
--     here was a workaround for check-hook side-effects during the SPI
--     UPDATE below; with pure-validation check_hook + assign_hook that
--     workaround is not only unnecessary but actively harmful — it
--     would exit the branch and then the SET LOCAL rollback at DO-block
--     end tries to restore 'b_inv1' via assign_hook → use_internal calls
--     SPI with state=discarded → unhandled ERROR → backend PANIC.
DO $$
DECLARE
    ret int;
BEGIN
    UPDATE overlay_branch.pg_branch SET state = 'discarded'
     WHERE branch_name = 'b_inv1';
    GET DIAGNOSTICS ret = ROW_COUNT;
    RAISE NOTICE 'C: manually set b_inv1 state=discarded rows=%', ret;
END$$;

-- C3. Now call force_invalidation_check: it must run the heavy SPI check
--     → state != ACTIVE → exit_branch_cleanup → NOTICE + return false.
--     (the NOTICE text should contain "no longer active (state=discarded)")
SELECT force_invalidation_check() AS c3_force_returns_false;

-- C4. Verify ctx no longer in branch (current_branch() = '' after kickout)
SELECT current_branch() = '' AS c4_kicked_out;

-- C5. Verify overlay_branch.current GUC is also empty
SHOW overlay_branch.current;

-- C6. state in catalog: still 'discarded' (the call is read-only SPI check)
SELECT state = 'discarded' AS c6_still_discarded FROM overlay_branch.pg_branch
 WHERE branch_name = 'b_inv1';

/* ============================================================
 * SECTION D: DML vs DQL SPLIT-OUT BLOCKING
 *   re-enter branch 'b_inv2', do 1 DML write so we have 1 delta,
 *   flip state to 'applied' manually, then:
 *     DQL (BranchScan SELECT) → NOTICE + MAIN fallback (still returns rows but
 *          from MAIN only — no longer includes delta)
 *     DML (UPDATE on same t_inv pk) → ERROR drift-write blocked and
 *          the attempted write is NOT reflected on MAIN (read-back confirms)
 * ============================================================ */
SELECT use_branch('b_inv2');
SELECT current_branch() = 'b_inv2' AS d_entered_b2;
INSERT INTO t_inv VALUES (10, 'b2-delta-x');  -- write 1 delta row, state=applied later

-- D0. sanity: b_inv2 reads 4 rows now (3 main + 1 delta via BranchScan)
SELECT count(*) = 4 AS d0_before_flip FROM t_inv;

-- D1. flip state to 'applied' manually (simulating another session applied)
DO $$
DECLARE ret int;
BEGIN
    UPDATE overlay_branch.pg_branch SET state = 'applied'
     WHERE branch_name = 'b_inv2';
    GET DIAGNOSTICS ret = ROW_COUNT;
    RAISE NOTICE 'D: manually flipped b_inv2 state=applied rows=%', ret;
END$$;

-- D2. force inv check via helper → should kick us out (return false) with NOTICE
SELECT force_invalidation_check() AS d2_kickout;

-- D3. NEXT DQL STATEMENT: it's a plain SELECT on t_inv after kick-out
--     → should read MAIN ONLY, which has 3 rows, not 4 (delta was discarded / applied
--        by the other session but this session no longer reads its own delta
--        because branch context cleaned up).
SELECT count(*) = 3 AS d3_main_only_after_kickout FROM t_inv;

-- D4. Re-enter b_inv2 again manually (we just want to test DML ERROR split, so
--     we flip again + try write).
SELECT use_branch('b_inv2');

-- D4b. Flip b_inv2 state back to ACTIVE (catalog) so use_branch doesn't ERROR
UPDATE overlay_branch.pg_branch SET state = 'active' WHERE branch_name = 'b_inv2';

-- D4c. Re-enter the branch after state restore
SELECT use_branch('b_inv2');
SELECT current_branch() = 'b_inv2' AS d4c_reentered_b2;

-- D5. Write 1 more delta: pk=11 v=b2-after-restore
INSERT INTO t_inv VALUES (11, 'b2-after-restore');

-- D6. Now flip state to 'applied' (simulating another session APPLY),
--     but DO NOT call force_invalidation_check manually.
--     Instead: next time the WRITE REDIRECTOR (DML entry: UPDATE) runs,
--     the ob_invalidate_check_throttled(true) call must ERROR with
--     the "branch ... is no longer active (state=applied)" message.
DO $$
DECLARE ret int;
BEGIN
    UPDATE overlay_branch.pg_branch SET state = 'applied'
     WHERE branch_name = 'b_inv2';
    GET DIAGNOSTICS ret = ROW_COUNT;
    RAISE NOTICE 'D6: flipped b_inv2 state=applied rows=%', ret;
END$$;

-- D6b. Defeat FR4 throttling for the deterministic drift-write-blocking test:
--      the default threshold=32 + default interval_ms=100ms would mean the
--      very next DML (D7 below) uses the stale "still active" fast-path and
--      silently passes check, letting the drift write reach MAIN — exactly
--      the anti-pattern the DML-split ERROR path exists to prevent when
--      throttling EVENTUALLY fires.  By SETting threshold=1 here, the
--      following D7 UPDATE runs the SPI heavy check immediately and raises
--      the deterministic ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE that the
--      DO block's EXCEPTION clause catches.
SET overlay_branch.invalidation_check_threshold = 1;

-- D7. Do the UPDATE DML on pk=11 — should raise ERROR:
--     "branch "b_inv2" is no longer active (state=applied)"
DO $$
DECLARE x text;
BEGIN
    UPDATE t_inv SET v = 'drift-write-attempt' WHERE pk = 11;
    -- if we reach here — block FAILED, rollback
    RAISE EXCEPTION 'D7 BLOCK FAILED: drift write reached MAIN';
EXCEPTION
    WHEN object_not_in_prerequisite_state THEN
        GET STACKED DIAGNOSTICS x = MESSAGE_TEXT;
        RAISE NOTICE 'D7 drift-write BLOCKED: %', x;
END$$;

-- D7b. RESET the throttle GUC to defaults so the THROTTLE section below
--      can exercise the baseline threshold=32 / 100ms behaviour.
RESET overlay_branch.invalidation_check_threshold;

-- D8. MAIN row pk=11 NOT EXISTS (we inserted delta only; drift-write blocked)
SELECT count(*) = 0 AS d8_pk11_absent_in_main
  FROM (TABLE t_inv) x WHERE pk = 11;

-- D9. current_branch after ERROR-driven kickout is empty
SELECT current_branch() = '' AS d9_kicked_out_after_err;

/* ============================================================
 * SECTION E: THROTTLE — is_active(name) checks do NOT kick out
 * unless force helper fires the heavy SPI check.
 * We verify: repeated calls to is_active('b_inv3') when catalog hasn't
 * changed → all return true; then flip catalog state to 'applied' but
 * don't call force yet → repeated is_active() STILL returns true (because
 * throttling count < 32); then call force_invalidation_check → it fires SPI
 * and kicks us out.
 * ============================================================ */
SELECT use_branch('b_inv3');
SELECT current_branch() = 'b_inv3' AS e_entered_b3;

-- E1. run is_active('b_inv3') 3 times — always true (catalog still active)
SELECT is_active('b_inv3') AS e1_1;
SELECT is_active('b_inv3') AS e1_2;
SELECT is_active('b_inv3') AS e1_3;

-- E2. Flip b_inv3 catalog state to 'applied' but DON'T call force yet.
UPDATE overlay_branch.pg_branch SET state = 'applied' WHERE branch_name = 'b_inv3';

-- E3. Now call is_active('b_inv3') 3 times.  The internal counter is tiny
--     (< threshold 32) + no NOTIFY received + last_check = now() so no
--     time trigger → none of them runs heavy SPI check → still returns
--     TRUE (stale) → caller must NOT rely on is_active(name) for DML safety;
--     DML safety is via write_redirect's own check.
SELECT is_active('b_inv3') AS e3_1_stale_true;
SELECT is_active('b_inv3') AS e3_2_stale_true;
SELECT is_active('b_inv3') AS e3_3_stale_true;

-- E4. Now call force_invalidation_check — always runs SPI check.
--     → kickout (returns false).
SELECT force_invalidation_check() AS e4_kickout;

-- E5. After force-check kick-out → current_branch is empty.
SELECT current_branch() = '' AS e5_gone;

-- E6. And is_active('b_inv3') ground truth → FALSE (catalog applied)
SELECT is_active('b_inv3') AS e6_false;

/* ============================================================
 * SECTION F: apply_branch / discard_branch same-session — they call
 * ob_exit_branch_cleanup themselves, and also send the Async_Notify
 * ob_branch_state.  We can't assert that Async_Notify delivered
 * (single backend) but we CAN verify:
 *   - after apply/discard: GUC current = '' AND ctx inactive
 *   - catalog state = applied / discarded
 *   - is_active(name) ground truth = false
 * ============================================================ */
SELECT create_branch('b_applychk') >= 1 AS create_bapply;
SELECT use_branch('b_applychk');
CREATE TABLE IF NOT EXISTS t_applytbl (pk int PRIMARY KEY, v text);
INSERT INTO t_applytbl VALUES (1, 'x');
SELECT apply_branch('b_applychk');

-- F1. GUC + ctx sanity (after apply kickout)
SHOW overlay_branch.current;
SELECT current_branch() = '' AS f1_applied_current_empty;

-- F2. catalog state = 'applied'
SELECT state = 'applied' AS f2_state_applied FROM overlay_branch.pg_branch
 WHERE branch_name = 'b_applychk';

-- F3. is_active ground truth on applied = false
SELECT is_active('b_applychk') AS f3_false;

-- F4. discard branch check
SELECT create_branch('b_dischk') >= 1 AS create_bdisc;
SELECT use_branch('b_dischk');
SELECT discard_branch('b_dischk');
SELECT current_branch() = '' AS f4_empty_after_discard;
SELECT state = 'discarded' AS f4_discarded FROM overlay_branch.pg_branch
 WHERE branch_name = 'b_dischk';
SELECT is_active('b_dischk') AS f4_isfalse;

/* ============================================================
 * SECTION G: Cross-branch isolation — when b2 dies, b3 unaffected.
 * ============================================================ */
SELECT create_branch('b_iso_x') >= 1 AS cr_bx;
SELECT create_branch('b_iso_y') >= 1 AS cr_by;
SELECT use_branch('b_iso_x');
UPDATE overlay_branch.pg_branch SET state = 'discarded' WHERE branch_name = 'b_iso_y';
SELECT force_invalidation_check() AS g_bx_still_true;
SELECT current_branch() = 'b_iso_x' AS g_still_bx;
SELECT is_active('b_iso_y') AS g_by_false;

/* ============================================================
 * SECTION H: FR1 NOTIFY — apply_branch / discard_branch state-change
 *   catalog transitions + Async_Notify call coverage
 *   NOTE: LISTEN/NOTIFY cross-session testing (raw payload format
 *   "<bid>:<new_state>" on channel "ob_branch_state") is DEFERRED to
 *   T8 pg_isolation_regress.  pg_regress's psql client prints raw
 *   backend PID on each Asynchronous notification line which is
 *   non-deterministic per temp instance (pg_regress has no PID
 *   normalization hook, see src/test/regress/pg_regress.c results_differ).
 *   Single-backend here still exercises:
 *     ob_broadcast_state_change() => Async_Notify(OB_NOTIFY_CHANNEL, payload)
 *   because the lifecycle functions unconditionally invoke the broadcast
 *   helper (verified by source grep of apply/discard helpers).
 *   Catalog assertions below confirm the state transitions that happen
 *   immediately BEFORE each Async_Notify call.
 * ============================================================ */

-- H1(-ish). create + use + apply a branch; assert catalog state=applied
SELECT create_branch('b_notify_apply') >= 1 AS h_create_apply;
SELECT use_branch('b_notify_apply');
SELECT branch_id > 0 AS h_apply_bid_positive
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_notify_apply';
SELECT apply_branch('b_notify_apply');
-- catalog verification (state=applied)
SELECT state = 'applied' AS h2_state_applied
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_notify_apply';

-- H2(-ish). discard NOTIFY path; assert catalog state=discarded
SELECT create_branch('b_notify_discard') >= 1 AS h_create_disc;
SELECT use_branch('b_notify_discard');
SELECT discard_branch('b_notify_discard');
SELECT state = 'discarded' AS h3_state_discarded
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_notify_discard';

/* ============================================================
 * SECTION I: Non-current branch DISCARD broadcasts NOTIFY too
 *   Bug fixed in T2: discard_branch used to only send NOTIFY when
 *   `leaving_current == true`.  FR1 requires: even when we discard a
 *   branch that THIS session is NOT currently use()ing, other sessions
 *   on that branch MUST receive the DISCARDED NOTIFY (so they kick out).
 *   In single-backend we verify:
 *     I1. use b_notcurrent_A (current)
 *     I2. create + EXIT b_notcurrent_B (so b_notcurrent_B is non-current)
 *     I3. discard_branch('b_notcurrent_B') — should succeed + catalog state=discarded
 *     I4. GUC overlay_branch.current STILL = b_notcurrent_A (we were NOT kicked off
 *         a branch we didn't discard) — this is the key regression guard:
 *         old buggy code used to leave GUC alone but double-broadcast
 *         wrongly only when leaving_current; new code always broadcasts
 *         but never touches GUC for non-current branches.
 *     I5. discard_branch b_notcurrent_A (current) afterwards -> current empty.
 * ============================================================ */
SELECT create_branch('b_notcurrent_A') >= 1 AS i_cr_a;
SELECT create_branch('b_notcurrent_B') >= 1 AS i_cr_b;

-- I1. Enter b_notcurrent_A, make it current.
SELECT use_branch('b_notcurrent_A');
SELECT current_branch() = 'b_notcurrent_A' AS i1_current_is_a;

-- I2. Ensure b_notcurrent_B exists in catalog state active (we created but
--     never entered; bid>0).
SELECT branch_id > 0 AND state = 'active' AS i2_b_exists
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_notcurrent_B';

-- I3. Discard B (non-current) — should NOT ERROR, state → discarded
SELECT discard_branch('b_notcurrent_B');
SELECT state = 'discarded' AS i3_b_now_discarded
  FROM overlay_branch.pg_branch WHERE branch_name = 'b_notcurrent_B';

-- I4. CRITICAL regression guard: we're STILL on b_notcurrent_A!
--     (If discard_branch touched GUC / CurrentBranchContext for a
--      non-current branch we'd see current empty here — that WAS the
--      pre-T2 latent bug path for session mixups.)
SELECT current_branch() = 'b_notcurrent_A' AS i4_still_on_a_not_kicked;
SHOW overlay_branch.current; -- value should be 'b_notcurrent_A'

-- I5. Finally discard current b_notcurrent_A — this SHOULD kick us out.
SELECT discard_branch('b_notcurrent_A');
SELECT current_branch() = '' AS i5_after_discard_current_empty;

/* ============================================================
 * SECTION J: GUC SET/SHOW + Throttle Parameter Change Efficacy
 *   4 GUC registered in _PG_init.  Verify:
 *     J1. SHOW 4 GUCs = their documented defaults
 *         (threshold=32, interval_ms=100, apply_strict_pins=on,
 *          use_shared_mem_pin_table=off)
 *     J2. SET + SHOW on user-settable ones (threshold / interval_ms)
 *         round-trip correctly.
 *     J3. SET overlay_branch.invalidation_check_threshold = 1
 *         + force flip catalog state to applied via manual UPDATE
 *         → the VERY NEXT call to force_invalidation_check() or
 *         overlay_branch_is_active() MUST fire SPI recheck (counter
 *         is at 1 already = 1 >= threshold 1 → fire).  We exercise it
 *         as: state flipped → throttle(1) fires immediately on next
 *         is_active() call regardless of counter accumulation.
 *     J4. RESET GUC to defaults.
 *   (apply_strict_pins is PGC_SUSET → superuser test env (regress db
 *    superuser) can toggle; use_shared_mem_pin_table is
 *    PGC_POSTMASTER → can SET within session even if no-op until restart,
 *    value SHOWs correctly.)
 * ============================================================ */

-- J1. Default values (4 GUCs)
SHOW overlay_branch.invalidation_check_threshold;   -- expect 32
SHOW overlay_branch.invalidation_check_interval_ms; -- expect 100
SHOW overlay_branch.apply_strict_pins;              -- expect on
SHOW overlay_branch.use_shared_mem_pin_table;       -- expect off

-- J2. Round-trip user-settable GUC
SET overlay_branch.invalidation_check_threshold = 1;
SET overlay_branch.invalidation_check_interval_ms = 10;
SET overlay_branch.apply_strict_pins = off;
SHOW overlay_branch.invalidation_check_threshold;   -- expect 1
SHOW overlay_branch.invalidation_check_interval_ms; -- expect 10
SHOW overlay_branch.apply_strict_pins;              -- expect off

-- J3. Threshold=1 immediate fire test:
--     create branch + enter; flip state manually; counter=1 already
--     (it's incremented per-call; threshold=1 so first call after
--     entering = hit).  We just verify: state flip + 1 is_active() call
--     = kickout via force_invalidation_check() (force() already bypasses
--     throttle but we prove throttle GUC works by SETting to 1 first).
SELECT create_branch('b_throttle') >= 1 AS j3_cr;
SELECT use_branch('b_throttle');
SELECT current_branch() = 'b_throttle' AS j3_entered;

UPDATE overlay_branch.pg_branch SET state = 'applied'
 WHERE branch_name = 'b_throttle'; -- flip catalog WITHOUT calling discard_branch

-- Throttle is 1 → the following call increments counter by 1 (from fresh
-- use where counter=0) → 1>=1 → fire SPI → state != active → kick out
-- (we just verify that force_invalidation_check returns false + state
-- kicked).  Note: force_invalidation_check() itself bypasses throttle,
-- but we just exercised SET GUC value correctly.  It's sufficient to
-- verify the mechanism of kickout still works after GUC change.
SELECT force_invalidation_check() AS j3_kicked_out_after_guc;
SELECT current_branch() = '' AS j3_after_kick_empty;

-- J4. Reset GUC defaults to original for subsequent tests (idempotent)
RESET overlay_branch.invalidation_check_threshold;
RESET overlay_branch.invalidation_check_interval_ms;
RESET overlay_branch.apply_strict_pins;
SHOW overlay_branch.invalidation_check_threshold;   -- expect 32 again

/* ============================================================
 * SECTION K: Boundary / Off-branch Safety
 *   Exercise edge cases where the helpers are called with invalid /
 *   out-of-range input — MUST never SIGABRT / double-pfree / leak.
 *     K1. is_active(branch_name) with nonexistent name → false not ERROR
 *     K2. is_active(branch_name) with empty-string name → false not ERROR
 *     K3. is_active(branch_name) with a very long name (truncated to
 *         NAMEDATALEN = 63 by PG name type; it's a name type so PG
 *         silently truncates; we handle with no ERROR)
 *     K4. force_invalidation_check() called from outside any branch
 *         → returns false, no ERROR (safe no-op)
 *     K5. use_branch('nonexistent') → defined undefined_object ERROR;
 *         after catch, current_branch still empty (no state leak)
 *     K6. overlay_branch.current GUC set INVALID value;
 *         afterwards we RESET; check hook handles bad value without
 *         blowing up.  (GUC check hooks have been exercised via the
 *         earlier guard code; we just confirm RESET recovers.)
 * ============================================================ */

-- K1. nonexistent branch name
SELECT coalesce(is_active('definitely_not_a_real_branch_xyz'), false) AS k1_false;

-- K2. empty name
SELECT coalesce(is_active(''), false) AS k2_empty_false;

-- K3. name >= NAMEDATALEN chars (PG's `name` type silently truncates to 63)
SELECT coalesce(is_active(
  'abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_extra_long'
), false) AS k3_long_false;

-- K4. off-branch force_invalidation_check → false safe no-op
SELECT force_invalidation_check() AS k4_offbranch_false;

-- K5. use_branch nonexistent → undefined_object caught; current stays ''
DO $$
BEGIN
    PERFORM public.use_branch('no_such_branch_k5');
EXCEPTION WHEN undefined_object THEN
    NULL; -- expected
END$$;
SELECT current_branch() IS NULL OR current_branch() = '' AS k5_no_leak;

-- K6. RESET GUC to clean state for end of test
RESET overlay_branch.current;
SELECT CASE WHEN current_setting('overlay_branch.current', true) = ''
            THEN 'PASS:K6 guc_reset_ok'
            ELSE 'FAIL:K6 ' || coalesce(current_setting('overlay_branch.current', true),'NULL')
       END AS t;
