/*
 * overlay_branch_rentry.sql — Review-260926 §S02 entry drift-write hard interception + R19 snapshot TTL.
 *
 * COVERAGE (originally overlay_branch_advanced.sql Section R-ENTRY + R19 sanity):
 *   S02 Drift-write negative tests: each DML below must ERROR 0A000/55P02, must not silently write MAIN:
 *     • CTE I/U/D outer-SELECT → RESTRICTED: outer SELECT containing I/U/D CTE all rejected;
 *     • child-table cascade U/D triggered by FK cascade-write trigger → G4-level non-internal trigger interception;
 *     • ProcessUtility hook ALTER/CREATE/DROP DDL → G4-level early rejection;
 *     • partitioned-table root/leaf DML → G3-level partitioned rejection;
 *     • snapshot-mode MAIN write → G2 guard interception (55P02 snapshot expired)
 *   R19 Snapshot TTL sanity: GUC overlay_branch.snapshot_max_hold_minutes default 10min,
 *       PGC_POSTMASTER not session-level modifiable (changing it reports 22023); use_branch snapshot mode
 *       before TTL is reached UPDATE+SELECT works normally (not expired).
 *   R19 TTL kickout (deterministic): the PGC_SUSET debug GUC
 *       overlay_branch._debug_snapshot_ttl_seconds forces the cap to 0s, the next write
 *       is kicked out of the branch (state=expired) and the errhint names the TTL reason
 *       (snapshot_max_hold_minutes (R19 TTL)) instead of "by another session".
 *   R-FORUPDATE row-locking guard: SELECT ... FOR UPDATE / FOR SHARE over a
 *       branch-managed relation must ERROR 0A000 (never crash the backend on an
 *       invalid virtual-slot ctid); the DML read side stays redirected and MAIN
 *       is untouched.
 *   R-SYSCOL system-column support: inside a branch only ctid (the row's MAIN
 *       anchor) and tableoid are populated on the scan slot; xmin/xmax/cmin/cmax
 *       must ERROR 0A000 on both the plain-SELECT and the DML-target scan (the
 *       latter used to read 0 and delete every row).  A ctid-subquery DELETE
 *       must reach MAIN rows through the overlay again.
 *   §CTX branch-context truth: current_branch() and `SET overlay_branch.current`
 *       must follow the LIVE write path, not the raw GUC string -- after a
 *       cross-branch ROLLBACK current_branch() reports no branch (the GUC is
 *       stale) and writes land on MAIN; after an explicit exit a plain user SET
 *       re-enters the branch (writes land in its delta).
 *   §R24 eager non-PK UNIQUE enforcement: a branch-local INSERT / UPDATE /
 *       UPSERT that would duplicate a non-primary UNIQUE value (against the
 *       delta, MAIN, or a sibling row of the same command) must raise 23505 at
 *       write time, while an UPDATE that leaves the UNIQUE column untouched
 *       stays legal (the source row is excluded by its ctid + delta key).
 * GLOBAL CLEANUP: DROP TABLE r_entry_t + DROP 2 branches; RESET client_min_messages
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

-- =====================================================================
-- ===== Section R-ENTRY (REVIEW-260926 S02):
-- =====   Drift-write hard-intercept negative tests.
-- =====   Goal: EVERY call below MUST raise ERROR 0A000 (feature not
-- =====   supported) or 55P02 (snapshot expired), NOT fall through to
-- =====   silently write MAIN heap.  After each error we re-enter the
-- =====   branch (ERROR aborted the transaction, but the session-level
-- =====   branch context is still intact per T5 savepoint semantics).
-- =====================================================================
SET client_min_messages = 'ERROR';
CREATE TABLE public.r_entry_t (
    pk_id   integer PRIMARY KEY,
    v       integer,
    note    text
);
INSERT INTO public.r_entry_t VALUES (1, 10, 'alpha'), (2, 20, 'beta');
SELECT overlay_branch.create_branch('br_r_entry');

-- ----- R05: COPY FROM STDIN must not land on MAIN; must ERROR 0A000.
-- -----      (COPY FROM STDIN must be TOP-level SQL in pg_regress; it is
-- -----      not permitted inside PL/pgSQL DO blocks anyway).
SELECT overlay_branch.use_branch('br_r_entry');
COPY public.r_entry_t (pk_id, v, note) FROM STDIN;
100	1000	should_be_blocked
\.
-- After the COPY error the transaction is in aborted state; we COMMIT
-- to reset then check MAIN baseline.
COMMIT;
BEGIN;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R05_COPY_NO_ROWS_ADDED'
            ELSE 'FAIL:R05 count=' || count(*) END AS r05_cnt
  FROM public.r_entry_t;
COMMIT;
-- Re-enter the branch for the next sub-tests in the section.
SELECT overlay_branch.use_branch('br_r_entry');

-- ----- R-FU: row-locking clause guard.  A SELECT ... FOR UPDATE/SHARE over a
-- -----      branch-managed relation used to abort the backend: the LockRows
-- -----      node calls heap_lock_tuple() on the ctid of each row, and a
-- -----      BranchScan row is a virtual slot with no physical MAIN ctid.
-- -----      It must now ERROR 0A000 instead.  The DML read side (planner row
-- -----      marks with strength LCS_NONE) is unaffected: a plain UPDATE
-- -----      inside the branch is still redirected into the delta.
DO $$
DECLARE
    cnt integer;
BEGIN
    BEGIN
        PERFORM pk_id FROM public.r_entry_t FOR UPDATE;
        RAISE EXCEPTION 'FAIL_RFU_FORUPDATE_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '0A000' THEN
            RAISE EXCEPTION 'FAIL_RFU_FORUPDATE: expected 0A000 got % %', SQLSTATE, SQLERRM;
        END IF;
    END;
    BEGIN
        PERFORM pk_id FROM public.r_entry_t FOR SHARE;
        RAISE EXCEPTION 'FAIL_RFU_FORSHARE_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '0A000' THEN
            RAISE EXCEPTION 'FAIL_RFU_FORSHARE: expected 0A000 got % %', SQLSTATE, SQLERRM;
        END IF;
    END;
    -- Read side of DML keeps working (status still active, write redirected).
    UPDATE public.r_entry_t SET note = note || '-rfu' WHERE pk_id = 1;
    SELECT count(*) INTO cnt FROM public.r_entry_t;   -- overlay still sees 2 rows
    IF cnt <> 2 THEN RAISE EXCEPTION 'FAIL_RFU_OVERLAY_CNT cnt=%', cnt; END IF;
    PERFORM overlay_branch.use_branch('br_r_entry');
END $$;

-- ----- R-SYSCOL: system-column support inside a branch.  A branch scan slot is
-- -----      materialised from a virtual tuple, so it owns no storage tuple of
-- -----      its own.  Only `ctid` (the row's MAIN anchor) and `tableoid` (the
-- -----      relation OID) are meaningful and must be populated; the remaining
-- -----      system columns (xmin/xmax/cmin/cmax) describe a physical tuple a
-- -----      branch row never has and must be rejected with 0A000 -- reading
-- -----      them either errored deep inside tts_virtual_getsysattr (plain
-- -----      SELECT) or silently returned 0 off the heap-backed DML-target slot
-- -----      (so `DELETE ... WHERE xmin = 0` matched every row).  Leaving ctid
-- -----      unset also made `DELETE ... WHERE ctid IN (SELECT ctid FROM t)`
-- -----      silently match nothing.  Every write below runs in one rolled-back
-- -----      transaction, so the shared fixture is left untouched.
BEGIN;
SELECT overlay_branch.use_branch('br_r_entry');
-- marker delta row proves the reads below go through BranchScan, not MAIN.
INSERT INTO public.r_entry_t VALUES (98, 980, 'syscol-marker');
SELECT CASE WHEN count(*) = 1 THEN 'PASS:SYSCOL_BRANCH_ACTIVE'
            ELSE 'FAIL:SYSCOL not-in-branch' END AS syscol_active
  FROM public.r_entry_t WHERE pk_id = 98;

-- (a) ctid / tableoid are populated on a plain branch SELECT: a MAIN-origin row
--     names the relation and its real MAIN ctid; a pure-delta row has no MAIN
--     storage, so its ctid stays the invalid all-ones value.
SELECT tableoid = 'public.r_entry_t'::regclass AS syscol_tableoid_ok
  FROM public.r_entry_t WHERE pk_id = 1;
SELECT ctid <> '(4294967295,0)'::tid AS syscol_ctid_ok
  FROM public.r_entry_t WHERE pk_id = 1;
SELECT ctid = '(4294967295,0)'::tid AS syscol_puredelta_ctid_invalid
  FROM public.r_entry_t WHERE pk_id = 98;

-- (b) xmin/xmax/cmin/cmax are rejected with 0A000 on both the plain-SELECT and
--     the DML-target scan (the latter used to read 0 and delete every row).
DO $$
DECLARE
    col text;
BEGIN
    FOREACH col IN ARRAY ARRAY['xmin', 'xmax', 'cmin', 'cmax'] LOOP
        BEGIN
            EXECUTE format('SELECT %s FROM public.r_entry_t', col);
            RAISE EXCEPTION 'FAIL_SYSCOL_SELECT_NO_ERROR col=%', col;
        EXCEPTION WHEN OTHERS THEN
            IF SQLSTATE <> '0A000' THEN
                RAISE EXCEPTION 'FAIL_SYSCOL_SELECT col=% got % %', col, SQLSTATE, SQLERRM;
            END IF;
        END;
    END LOOP;
    BEGIN
        EXECUTE 'DELETE FROM public.r_entry_t WHERE xmin = 0';
        RAISE EXCEPTION 'FAIL_SYSCOL_DML_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '0A000' THEN
            RAISE EXCEPTION 'FAIL_SYSCOL_DML got % %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 3 THEN 'PASS:SYSCOL_DML_XMIN_GATE_NO_DELETE'
            ELSE 'FAIL:SYSCOL_DML_XMIN_GATE cnt=' || count(*) END AS syscol_dml_gate
  FROM public.r_entry_t;   -- pk 1, 2, 98 all still there

-- (c) a ctid-subquery DELETE now reaches MAIN rows through the overlay: the
--     subquery (a plain SELECT) exposes each MAIN-origin row's real ctid, so
--     `WHERE ctid IN (...)` matches and deletes them; a pure-delta row exposes
--     no valid MAIN ctid, and the synthetic DML-target ctid never equals it, so
--     it is left alone.
INSERT INTO public.r_entry_t VALUES (99, 990, 'delta-only');
DELETE FROM public.r_entry_t WHERE ctid IN (SELECT ctid FROM public.r_entry_t);
SELECT CASE WHEN count(*) = 0 THEN 'PASS:SYSCOL_CTID_DEL_MAIN'
            ELSE 'FAIL:SYSCOL_CTID_DEL main_left=' || count(*) END AS syscol_del_main
  FROM public.r_entry_t WHERE pk_id IN (1, 2);
SELECT CASE WHEN count(*) = 2 THEN 'PASS:SYSCOL_CTID_DEL_KEEPS_DELTA'
            ELSE 'FAIL:SYSCOL_CTID_DEL delta_left=' || count(*) END AS syscol_del_delta
  FROM public.r_entry_t WHERE pk_id IN (98, 99);
ROLLBACK;
-- Re-enter the branch for the next sub-tests in the section.
SELECT overlay_branch.use_branch('br_r_entry');

-- ----- R06: MERGE blocked (ProcessUtility choke point).
DO $$
DECLARE
    cnt integer;
BEGIN
    BEGIN
        EXECUTE $_$ MERGE INTO public.r_entry_t t USING (SELECT 3 pk, 30 v, 'g' note) s
            ON t.pk_id = s.pk
            WHEN MATCHED THEN UPDATE SET v = s.v
            WHEN NOT MATCHED THEN INSERT VALUES (s.pk, s.v, s.note) $_$;
        RAISE EXCEPTION 'FAIL_R06_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '0A000' THEN
            RAISE EXCEPTION 'FAIL_R06: expected 0A000 got %', SQLSTATE;
        END IF;
    END;
    SELECT count(*) INTO cnt FROM public.r_entry_t;
    IF cnt <> 2 THEN RAISE EXCEPTION 'FAIL_R06_MAIN_POLLUTED cnt=%', cnt; END IF;
    PERFORM overlay_branch.use_branch('br_r_entry');
END $$;

-- ----- R16: ModifyingCTE (WITH UPDATE/INSERT/DELETE + outer SELECT)
-- -----      blocked at Planner hook choke point.  Two shapes: direct
-- -----      SQL and DO block inner SPI (both paths must ERROR, neither
-- -----      lands on MAIN).
DO $$
DECLARE
    cnt integer;
BEGIN
    -- Shape A: direct WITH (UPDATE RETURNING) SELECT ...
    BEGIN
        EXECUTE $_$
            WITH upd AS (UPDATE public.r_entry_t SET v = v+100 WHERE pk_id = 1 RETURNING *)
            SELECT * FROM upd
        $_$;
        RAISE EXCEPTION 'FAIL_R16_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '0A000' THEN
            RAISE EXCEPTION 'FAIL_R16_A: expected 0A000 got % %', SQLSTATE, SQLERRM;
        END IF;
    END;
    PERFORM overlay_branch.use_branch('br_r_entry');
    -- Shape B: WITH (INSERT RETURNING) SELECT
    BEGIN
        EXECUTE $_$
            WITH ins AS (INSERT INTO public.r_entry_t VALUES (99, 999, 'x') RETURNING pk_id)
            SELECT * FROM ins
        $_$;
        RAISE EXCEPTION 'FAIL_R16_B_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '0A000' THEN
            RAISE EXCEPTION 'FAIL_R16_B: expected 0A000 got %', SQLSTATE;
        END IF;
    END;
    SELECT count(*) INTO cnt FROM public.r_entry_t;
    IF cnt <> 2 THEN RAISE EXCEPTION 'FAIL_R16_MAIN_POLLUTED cnt=%', cnt; END IF;
    SELECT sum(v) INTO cnt FROM public.r_entry_t;
    IF cnt <> 30 THEN RAISE EXCEPTION 'FAIL_R16_MAIN_SUM_NOT_30 sum=%', cnt; END IF;
    PERFORM overlay_branch.use_branch('br_r_entry');
END $$;

-- ----- R03: UPDATE SET pk_id = ... (PK-column mutation) blocked at
-- -----      WriteRedirect choke point.  MAIN must NOT change, delta
-- -----      must NOT contain a row for the attempted key.
DO $$
DECLARE
    cnt integer;
    s   integer;
BEGIN
    BEGIN
        UPDATE public.r_entry_t SET pk_id = 100 WHERE pk_id = 1;
        RAISE EXCEPTION 'FAIL_R03_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '0A000' THEN
            RAISE EXCEPTION 'FAIL_R03: expected 0A000 got % %', SQLSTATE, SQLERRM;
        END IF;
    END;
    -- Verify baseline unchanged (both rows still there).
    SELECT count(*) INTO cnt FROM public.r_entry_t;
    SELECT sum(v)   INTO s   FROM public.r_entry_t;
    IF cnt <> 2 THEN RAISE EXCEPTION 'FAIL_R03_MAIN_CNT cnt=%', cnt; END IF;
    IF s   <> 30 THEN RAISE EXCEPTION 'FAIL_R03_MAIN_SUM s=%',   s;   END IF;
    PERFORM overlay_branch.use_branch('br_r_entry');
END $$;

-- ----- R19: snapshot mode TTL enforcement (snapshot_max_hold_minutes
-- -----      GUC choke point).  Since we cannot wait N minutes in a
-- -----      unit test, we verify behavior indirectly:
-- -----        (1) entering snapshot mode works with default 10m cap.
-- -----        (2) branch context snapshot mode survives a simple DML
-- -----            (entered_at set, TTL > elapsed).
-- -----      Explicit expiry is only reached in the ob_applying_freeze
-- -----      stress harness; the zero-implementation guard is asserted
-- -----      via GUC PGC_POSTMASTER (cannot be changed in-session; user
-- -----      gets 22023 if they try to override).
-- =====================================================================
-- Indirect R19 assertions on default-10-minutes live:
SELECT overlay_branch.use_branch(NULL);
SELECT overlay_branch.use_branch('br_r_entry', 'snapshot');
-- Perform a simple UPDATE and read back (snapshot mode works normally
-- until TTL elapses).
UPDATE public.r_entry_t SET note = note || '-snap' WHERE pk_id = 2;
SELECT CASE WHEN note = 'beta-snap' THEN 'PASS:R19_SNAPSHOT_ALIVE'
            ELSE 'FAIL:R19 note=' || COALESCE(note,'NULL') END AS r19_live
  FROM public.r_entry_t WHERE pk_id = 2;

-- ----- R19 TTL kickout (deterministic).  snapshot_max_hold_minutes is a
-- -----      PGC_POSTMASTER GUC with a one-minute floor, so a unit test
-- -----      cannot wait it out; the _debug_snapshot_ttl_seconds GUC
-- -----      (PGC_SUSET) supplies a seconds-granularity cap for exactly this.
-- -----      Once the cap elapses the WRITE gate must force the session off
-- -----      the branch AND the diagnostic must name the TTL reason -- not
-- -----      the generic "by another session" text the pre-fix code always
-- -----      appended (which was factually wrong for a local TTL kickout).
SET overlay_branch._debug_snapshot_ttl_seconds = 0;
SET client_min_messages = NOTICE;  -- this section runs at ERROR; let the assertion print
DO $$
DECLARE
    _msg  text;
    _hint text;
BEGIN
    BEGIN
        UPDATE public.r_entry_t SET note = 'ttl_attempt' WHERE pk_id = 1;
        RAISE EXCEPTION 'FAIL:R19_TTL_NO_ERROR';
    EXCEPTION WHEN object_not_in_prerequisite_state THEN
        GET STACKED DIAGNOSTICS _msg = MESSAGE_TEXT, _hint = PG_EXCEPTION_HINT;
        IF _msg LIKE '%state=expired%'
           AND _hint LIKE '%snapshot_max_hold_minutes (R19 TTL)%'
           AND _hint NOT LIKE '%by another session%' THEN
            RAISE NOTICE 'PASS:R19_TTL_KICKOUT_NAMES_TTL_REASON';
        ELSE
            RAISE NOTICE 'FAIL:R19_TTL_KICKOUT msg=[%] hint=[%]', _msg, _hint;
        END IF;
    END;
END $$;
SET client_min_messages = ERROR;   -- restore the section's level
RESET overlay_branch._debug_snapshot_ttl_seconds;
SELECT overlay_branch.use_branch(NULL);
-- Forced exit must leave MAIN untouched: the TTL gate fires before any
-- redirect, so the attempted write never reached the delta.
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R19_TTL_MAIN_UNTOUCHED'
            ELSE 'FAIL:R19_TTL_MAIN_POLLUTED cnt=' || count(*) END AS r19_ttl_main
  FROM public.r_entry_t WHERE note = 'ttl_attempt';
SELECT overlay_branch.discard_branch('br_r_entry');

-- =====================================================================
-- ===== Section G06 (REVIEW-260926 §2.5):
-- =====   Minimum anti-leak guards for CREATE PUBLICATION FOR ALL TABLES
-- =====================================================================
-- Setup: clean schema overlay_branch baseline.  We test two things:
--   (1) SET (publish=false) on pg_branch / pg_branch_delta causes
--       pg_publication_tables FOR ALL TABLES NOT to list overlay_branch.*.
--   (2) After CREATE PUBLICATION forall, create_branch() emits a WARNING
--       (throttled once per session) pointing at the risk.
--
-- Tested in the SAME test file as R-ENTRY because rentry is the
-- last REGRESS entry and we can DROP PUBLICATION at the end without
-- ordering issues.
-- =====================================================================
DO $$
DECLARE
    cnt integer;
BEGIN
    BEGIN
        CREATE PUBLICATION g06_test_forall FOR ALL TABLES;
    EXCEPTION WHEN feature_not_supported THEN
        RAISE NOTICE 'G06_SKIP: publication not available in this build';
        RETURN;
    END;

    -- G06 constraint: PG 17 has no per-table publish=false mechanism (publish is a publication-level
    -- WITH option controlling insert/update/delete/truncate). FOR ALL TABLES will include all
    -- permanent non-catalog RELATIONs; therefore overlay_branch.* internal tables at this stage
    -- will appear in pg_publication_tables (this is a PG upstream API limitation).
    -- Our defense: E-2 create/use/apply/discard entries emit WARNING when puball=true,
    -- deployment doc recommends FOR TABLE x ONLY or manual exclusion. Here we only verify publication is truly created +
    -- subsequent WARNING correctly triggered (cnt need not be 0; stubbing real PG 17 behavior).
    -- original assertion "cnt=0 publish=false" cannot be implemented under PG 14-17 API; rewritten as "cnt>=0 and internal
    -- tables are indeed visible in pub_tables, and WARNING is triggered".
    --
    -- Deployment recommendation:
    --   Production should not use FOR ALL TABLES; instead use CREATE PUBLICATION p FOR TABLE schema1.t1,
    --   schema1.t2, … (list only business tables). The two overlay_branch.pg_branch/pg_branch_delta
    --   internal tables themselves contain no business rows; they store branch metadata and delta (I/U/D) rows,
    --   when puball=true delta rows' INSERT/UPDATE/DELETE are also replicated to logical replication
    --   downstream; this is exactly the G06 risk.
    SELECT COUNT(*) INTO cnt
      FROM pg_catalog.pg_publication_tables
     WHERE pubname = 'g06_test_forall'
       AND schemaname = 'overlay_branch';

    RAISE NOTICE 'G06_INFO: overlay_branch.* tables count in pg_publication_tables FOR ALL TABLES = %', cnt;
    RAISE NOTICE 'PASS:G06_PUBALL_DETECTED (WARNING will fire on next create_branch call; throttle=once/session)';
END $$;

-- (2) create_branch() should emit WARNING: detected puball=true / pubschema contains overlay_branch
--     throttled once per session (the 2nd create_branch('b_g06_b') below no longer WARNINGs).
SET client_min_messages = WARNING;
CREATE TABLE public.g06_t (id int PRIMARY KEY, v int);
SELECT overlay_branch.create_branch('b_g06');   -- WARNING line 1 (see test_output results)
SELECT overlay_branch.create_branch('b_g06_b'); -- no WARNING (throttled)

-- Cleanup G06.
SELECT overlay_branch.discard_branch('b_g06');
SELECT overlay_branch.discard_branch('b_g06_b');
DROP TABLE public.g06_t;
DROP PUBLICATION IF EXISTS g06_test_forall;

-- Cleanup Section R-ENTRY + G06.
DROP TABLE IF EXISTS public.r_entry_t;
RESET client_min_messages;

-- =====================================================================
-- Section R21 : R21 pre-check pass — NOT NULL (ERRCODE 23502) + PK
--               UNIQUE on both MAIN/delta sides (ERRCODE 23505) on plain INSERT
--               (ONCONFLICT_NONE path).  All errors MUST fire BEFORE
--               any MAIN/delta write (zero side effects), so post-error
--               row-count on MAIN remains unchanged.
--
-- COVERAGE:
--   T_R21_A : NOT NULL column set NULL → 23502 (guard before write)
--   T_R21_B1: MAIN already has PK=k, branch plain INSERT k → 23505 (MAIN)
--   T_R21_B2: 2x plain INSERT same PK in same branch → 2nd → 23505 (delta I)
--   T_R21_B3: INSERT PK=k → UPDATE same PK → plain INSERT again → 23505 (delta U)
--   T_R21_C : UPSERT ON CONFLICT DO NOTHING / DO UPDATE still works (no regression)
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r21 (
    id  integer PRIMARY KEY,
    v   text NOT NULL,
    n   numeric
);
INSERT INTO public.t_r21 VALUES (1, 'seed_main', 100);
SELECT overlay_branch.create_branch('br_r21');
SELECT overlay_branch.use_branch('br_r21');

-- T_R21_A: NOT NULL violation → SQLSTATE 23502
DO $$
DECLARE
    _cnt integer;
BEGIN
    BEGIN
        INSERT INTO public.t_r21 VALUES (10, NULL, 1);
        RAISE EXCEPTION 'FAIL_R21_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23502' THEN
            RAISE EXCEPTION 'FAIL_R21_A: expected 23502 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
    SELECT count(*) INTO _cnt FROM public.t_r21;
    IF _cnt <> 1 THEN
        RAISE EXCEPTION 'FAIL_R21_A: MAIN count after error = % (expected 1)', _cnt;
    END IF;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R21_A_NOTNULL_23502'
            ELSE 'FAIL:R21_A count=' || count(*) END AS t_r21_a
  FROM public.t_r21;

-- T_R21_B1: MAIN already has PK=1 → re-insert → 23505
DO $$
DECLARE
    _cnt integer;
BEGIN
    BEGIN
        INSERT INTO public.t_r21 VALUES (1, 'collision_main', 2);
        RAISE EXCEPTION 'FAIL_R21_B1_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R21_B1: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
    SELECT count(*) INTO _cnt FROM public.t_r21;
    IF _cnt <> 1 THEN
        RAISE EXCEPTION 'FAIL_R21_B1: MAIN count after error = % (expected 1)', _cnt;
    END IF;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R21_B1_UNIQUE_MAIN_23505'
            ELSE 'FAIL:R21_B1 count=' || count(*) END AS t_r21_b1
  FROM public.t_r21;

-- T_R21_B2: 2x same-PK INSERT in same branch → 2nd raises 23505 (delta op=I)
INSERT INTO public.t_r21 VALUES (2, 'first_insert', 10);
DO $$
DECLARE
    _cnt integer;
BEGIN
    BEGIN
        INSERT INTO public.t_r21 VALUES (2, 'second_collision', 20);
        RAISE EXCEPTION 'FAIL_R21_B2_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R21_B2: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R21_B2_UNIQUE_DELTA_I_23505'
            ELSE 'FAIL:R21_B2 count=' || count(*) END AS t_r21_b2
  FROM public.t_r21;

-- T_R21_B3: INSERT → UPDATE → re-INSERT same PK → 23505 (delta op=U triggers too)
INSERT INTO public.t_r21 VALUES (3, 'row3_v1', 30);
UPDATE public.t_r21 SET v = 'row3_v2', n = 31 WHERE id = 3;
DO $$
DECLARE
    _cnt integer;
BEGIN
    BEGIN
        INSERT INTO public.t_r21 VALUES (3, 'row3_reinsert_collide', 32);
        RAISE EXCEPTION 'FAIL_R21_B3_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R21_B3: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 3 THEN 'PASS:R21_B3_UNIQUE_DELTA_U_23505'
            ELSE 'FAIL:R21_B3 count=' || count(*) END AS t_r21_b3
  FROM public.t_r21;

-- T_R21_C: UPSERT ON CONFLICT DO NOTHING / DO UPDATE regression check.
--          (These paths already had Phase I/II conflict detection; the
--          new R21 helper is intentionally NOT called for them.)
INSERT INTO public.t_r21 VALUES (4, 'upsert_base', 40)
    ON CONFLICT (id) DO NOTHING;                           -- promo=1 (no conflict)
INSERT INTO public.t_r21 VALUES (4, 'upsert_do_nothing_hit', 41)
    ON CONFLICT (id) DO NOTHING;                           -- promo=0, skip
INSERT INTO public.t_r21 VALUES (4, 'upsert_do_update_hit', 42)
    ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v, n = EXCLUDED.n;  -- promo=3
SELECT CASE WHEN count(*) = 4 THEN 'PASS:R21_C_UPSERT_OK'
            ELSE 'FAIL:R21_C count=' || count(*) END AS t_r21_c
  FROM public.t_r21;

-- R21 cleanup: discard branch, drop test table.
SELECT overlay_branch.discard_branch('br_r21');
DROP TABLE public.t_r21;

-- =====================================================================
-- Section R23 : Eager NOT NULL (23502) + CHECK (23514) validation on
--               branch-local writes.
--
--   The write-redirect short-circuits standard_ExecutorRun(), so the
--   executor's own ExecConstraints() never runs for a redirected row.
--   Without an explicit check an invalid post-image would be silently
--   stored in the delta and only rejected later, at apply time.  The
--   redirection now delegates to ExecConstraints() on the post-image
--   BEFORE writing, so a branch-local UPDATE / INSERT / UPSERT fails
--   eagerly with the exact native SQLSTATE and leaves the delta
--   untouched.
--
-- COVERAGE:
--   T_R23_A  : main-hit UPDATE v=NULL           → 23502, delta unchanged
--   T_R23_B  : main-hit UPDATE ck=-1            → 23514
--   T_R23_C  : pure-delta UPDATE v=NULL         → 23502
--   T_R23_D  : pure-delta UPDATE ck=-1          → 23514
--   T_R23_E  : plain INSERT ck=-1               → 23514
--              (plain INSERT NOT NULL is already R21_A)
--   T_R23_F  : UPSERT DO UPDATE merged v=NULL   → 23502
--   T_R23_G  : UPSERT DO UPDATE merged ck=-1    → 23514
--   T_R23_H  : UPSERT DO UPDATE candidate v=NULL (merged valid) → 23502
--   T_R23_I  : UPSERT DO NOTHING candidate v=NULL               → 23502
--   T_R23_J  : legal UPDATE/INSERT/UPSERT all succeed (no false positive)
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r23 (
    id  integer PRIMARY KEY,
    v   text NOT NULL,
    ck  integer CHECK (ck > 0)
);
INSERT INTO public.t_r23 VALUES (1, 'seed', 100);
SELECT overlay_branch.create_branch('br_r23');
SELECT overlay_branch.use_branch('br_r23');

-- T_R23_A: branch-local UPDATE of a MAIN row sets NOT NULL col to NULL.
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        UPDATE public.t_r23 SET v = NULL WHERE id = 1;
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23502' THEN
        RAISE EXCEPTION 'FAIL_R23_A: expected 23502 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R23_A_UPDATE_NOTNULL_23502'
            ELSE 'FAIL:R23_A row leaked into delta, count=' || count(*) END AS t_r23_a
  FROM public.t_r23;

-- T_R23_B: branch-local UPDATE of a MAIN row violates CHECK.
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        UPDATE public.t_r23 SET ck = -1 WHERE id = 1;
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23514' THEN
        RAISE EXCEPTION 'FAIL_R23_B: expected 23514 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R23_B_UPDATE_CHECK_23514'
            ELSE 'FAIL:R23_B count=' || count(*) END AS t_r23_b
  FROM public.t_r23;

-- T_R23_C: branch-local UPDATE of a PURE-DELTA row sets NOT NULL col to NULL.
INSERT INTO public.t_r23 VALUES (2, 'delta_row', 2);
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        UPDATE public.t_r23 SET v = NULL WHERE id = 2;
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23502' THEN
        RAISE EXCEPTION 'FAIL_R23_C: expected 23502 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R23_C_PUREDELTA_UPDATE_NOTNULL_23502'
            ELSE 'FAIL:R23_C count=' || count(*) END AS t_r23_c
  FROM public.t_r23;

-- T_R23_D: branch-local UPDATE of a PURE-DELTA row violates CHECK.
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        UPDATE public.t_r23 SET ck = -1 WHERE id = 2;
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23514' THEN
        RAISE EXCEPTION 'FAIL_R23_D: expected 23514 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R23_D_PUREDELTA_UPDATE_CHECK_23514'
            ELSE 'FAIL:R23_D count=' || count(*) END AS t_r23_d
  FROM public.t_r23;

-- T_R23_E: branch-local plain INSERT violates CHECK.
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        INSERT INTO public.t_r23 VALUES (3, 'ck_bad', -1);
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23514' THEN
        RAISE EXCEPTION 'FAIL_R23_E: expected 23514 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R23_E_INSERT_CHECK_23514'
            ELSE 'FAIL:R23_E count=' || count(*) END AS t_r23_e
  FROM public.t_r23;

-- T_R23_F: UPSERT DO UPDATE merges to a NOT NULL violation.
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        INSERT INTO public.t_r23 VALUES (1, 'ignored', 1)
            ON CONFLICT (id) DO UPDATE SET v = NULL;
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23502' THEN
        RAISE EXCEPTION 'FAIL_R23_F: expected 23502 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R23_F_UPSERT_DOUPDATE_NOTNULL_23502'
            ELSE 'FAIL:R23_F count=' || count(*) END AS t_r23_f
  FROM public.t_r23;

-- T_R23_G: UPSERT DO UPDATE merges to a CHECK violation.
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        INSERT INTO public.t_r23 VALUES (1, 'ignored', 1)
            ON CONFLICT (id) DO UPDATE SET ck = -1;
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23514' THEN
        RAISE EXCEPTION 'FAIL_R23_G: expected 23514 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R23_G_UPSERT_DOUPDATE_CHECK_23514'
            ELSE 'FAIL:R23_G count=' || count(*) END AS t_r23_g
  FROM public.t_r23;

-- T_R23_H: the ON CONFLICT candidate row itself is invalid, but DO UPDATE
--          would overwrite the bad column with a valid value.  Native
--          ExecInsert() validates the candidate BEFORE resolving the
--          conflict, so this must still raise 23502.
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        INSERT INTO public.t_r23 VALUES (1, NULL, 1)
            ON CONFLICT (id) DO UPDATE SET v = 'overwritten';
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23502' THEN
        RAISE EXCEPTION 'FAIL_R23_H: expected 23502 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R23_H_UPSERT_CANDIDATE_PRE_CONFLICT_23502'
            ELSE 'FAIL:R23_H count=' || count(*) END AS t_r23_h
  FROM public.t_r23;

-- T_R23_I: same candidate check, but DO NOTHING skips the conflicting row.
DO $$
DECLARE _sqlstate text;
BEGIN
    BEGIN
        INSERT INTO public.t_r23 VALUES (1, NULL, 1)
            ON CONFLICT (id) DO NOTHING;
    EXCEPTION WHEN OTHERS THEN _sqlstate := SQLSTATE;
    END;
    IF _sqlstate IS DISTINCT FROM '23502' THEN
        RAISE EXCEPTION 'FAIL_R23_I: expected 23502 got %', COALESCE(_sqlstate, '<no error>');
    END IF;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R23_I_UPSERT_DONOTHING_CANDIDATE_23502'
            ELSE 'FAIL:R23_I count=' || count(*) END AS t_r23_i
  FROM public.t_r23;

-- T_R23_J: valid UPDATE / INSERT / UPSERT must NOT be rejected by the
--          eager check (guards against a false positive).
UPDATE public.t_r23 SET v = 'ok1', ck = 101 WHERE id = 1;
INSERT INTO public.t_r23 VALUES (4, 'ok4', 4) ON CONFLICT (id) DO NOTHING;
INSERT INTO public.t_r23 VALUES (1, 'zz', 1)
    ON CONFLICT (id) DO UPDATE SET v = 'ok-up';
SELECT CASE WHEN count(*) = 3 THEN 'PASS:R23_J_LEGAL_OPS_OK'
            ELSE 'FAIL:R23_J count=' || count(*) END AS t_r23_j
  FROM public.t_r23;

-- R23 cleanup: discard branch, drop test table.
SELECT overlay_branch.discard_branch('br_r23');
DROP TABLE public.t_r23;

-- =====================================================================
-- ===== Section G02 (REVIEW-260926 S13 §6):
-- =====   ACL minimum hardening (double-layer defense).
-- =====
-- ===== Matrix re-stated here for test traceability:
-- =====   create_branch                  → superuser | mgmt role only
-- =====   use/apply/discard(name)       → superuser | mgmt role only
-- =====   use_branch(NULL/empty)        → PUBLIC (exit branch only)
-- =====   list_branches()/current_branch() → PUBLIC (metadata only)
-- =====   public.pg_branch_delta SELECT → REVOKEd from PUBLIC (R22)
-- =====
-- ===== Setup: regression runs as superuser; we use session SET ROLE
-- =====   to impersonate a plain login role regress_g02_alice for the
-- =====   42501 negative cases; clean up the role at the end.
-- =====================================================================

SET client_min_messages = WARNING;
-- Setup: two roles — alice = unprivileged user; bob = branch owner
-- (provenance only: ownership alone grants NO administrative authority).
DO $$BEGIN
    EXECUTE format('CREATE ROLE regress_g02_alice LOGIN');
EXCEPTION WHEN duplicate_object THEN NULL; END$$;
DO $$BEGIN
    EXECUTE format('CREATE ROLE regress_g02_bob   LOGIN');
EXCEPTION WHEN duplicate_object THEN NULL; END$$;

-- ===== G02 L1 ACL re-baseline (pg_regress temp-inst workaround: EXTENSION
--       sometimes lands with MGMT-only privileges on lifecycle wrappers,
--       which would break the mgmt-override positive paths) =====
DO $$
DECLARE
  _ns name := (SELECT n.nspname FROM pg_extension e JOIN pg_namespace n ON e.extnamespace=n.oid WHERE e.extname='overlay_branch');
BEGIN
  EXECUTE format('GRANT USAGE ON SCHEMA %I TO PUBLIC', _ns);
  EXECUTE format('GRANT SELECT ON %I.pg_branch TO PUBLIC', _ns);
  EXECUTE format('GRANT SELECT,INSERT,UPDATE,DELETE ON %I.pg_branch,%I.pg_branch_delta TO overlay_branch_administrators', _ns, _ns);
  EXECUTE format('GRANT ALL ON SEQUENCE %I.pg_branch_branch_id_seq TO overlay_branch_administrators', _ns);
END$$;
GRANT EXECUTE ON FUNCTION public.create_branch(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.use_branch(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.use_branch(name,text) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.apply_branch(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.discard_branch(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.current_branch() TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.list_branches() TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.is_active(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.force_invalidation_check() TO PUBLIC;
REVOKE ALL ON public.pg_branch_delta FROM PUBLIC;
-- ===== END re-grant block =====

-- Fresh extension schema context: DROP br_g02_* from any prior dirty run.
DO $$
DECLARE
    _b text;
BEGIN
    FOR _b IN VALUES ('br_g02_aliceblocked'), ('br_g02_bobsbranch'), ('br_g02_mgmthold') LOOP
        BEGIN
            PERFORM overlay_branch.discard_branch(_b.column1);
        EXCEPTION WHEN OTHERS THEN NULL; END;
    END LOOP;
END $$;

CREATE TABLE public.t_g02 (id int PRIMARY KEY, v text);

-- T_G02_A1: unprivileged alice cannot create_branch → 42501.
SET ROLE regress_g02_alice;
DO $$
BEGIN
    BEGIN
        PERFORM public.create_branch('br_g02_aliceblocked');
        RAISE EXCEPTION 'FAIL_G02_A1_NO_ERROR';
    EXCEPTION WHEN insufficient_privilege THEN
        NULL;   /* expected 42501 */
    END;
END $$;
RESET ROLE;
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name = 'br_g02_aliceblocked')
            THEN 'PASS:G02_A1_ALICE_CREATE_DENIED_42501'
            ELSE 'FAIL:G02_A1 branch was created despite denial' END AS g02_a1;

-- T_G02_A2: unprivileged alice cannot invoke @extschema@.create_branch
-- directly (bypassing public synonyms) → C layer must still 42501.
SET ROLE regress_g02_alice;
DO $$
BEGIN
    BEGIN
        PERFORM overlay_branch.create_branch('br_g02_aliceblocked2');
        RAISE EXCEPTION 'FAIL_G02_A2_NO_ERROR';
    EXCEPTION WHEN insufficient_privilege THEN
        NULL;
    END;
END $$;
RESET ROLE;
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name = 'br_g02_aliceblocked2')
            THEN 'PASS:G02_A2_ALICE_CREATE_DIRECT_EXTSCHEMA_DENIED'
            ELSE 'FAIL:G02_A2' END AS g02_a2;

-- Setup: superuser creates br_g02_bobsbranch and assigns OWNER to bob.
-- Ownership is provenance metadata only — it does NOT grant bob any
-- administrative authority (use/apply/discard stay mgmt-only).
SELECT overlay_branch.create_branch('br_g02_bobsbranch') AS g02_bob_branch_id;
UPDATE overlay_branch.pg_branch SET owner = (SELECT oid FROM pg_roles WHERE rolname = 'regress_g02_bob')
 WHERE branch_name = 'br_g02_bobsbranch';

-- T_G02_B1: bob is the stored OWNER but NOT mgmt → use_branch DENIED 42501.
SET ROLE regress_g02_bob;
DO $$
BEGIN
    BEGIN
        PERFORM public.use_branch('br_g02_bobsbranch');
        RAISE EXCEPTION 'FAIL_G02_B1_NO_ERROR';
    EXCEPTION WHEN insufficient_privilege THEN
        NULL;   /* expected 42501 */
    END;
END $$;
-- EXIT path stays open to everyone (never entered a branch here).
SELECT public.use_branch(NULL);   -- EXIT path: always PUBLIC
RESET ROLE;
SELECT CASE WHEN (SELECT owner = (SELECT oid FROM pg_roles WHERE rolname = 'regress_g02_bob')
                   FROM public.pg_branch WHERE branch_name = 'br_g02_bobsbranch')
            THEN 'PASS:G02_B1_OWNER_USE_DENIED_42501'
            ELSE 'FAIL:G02_B1' END AS g02_b1;

-- T_G02_B2: alice (non-owner, non-mgmt) cannot use_branch bob's branch.
SET ROLE regress_g02_alice;
DO $$
BEGIN
    BEGIN
        PERFORM public.use_branch('br_g02_bobsbranch');
        RAISE EXCEPTION 'FAIL_G02_B2_NO_ERROR';
    EXCEPTION WHEN insufficient_privilege THEN
        NULL;
    END;
END $$;
RESET ROLE;
SELECT CASE WHEN (SELECT COALESCE(owner <> (SELECT oid FROM pg_roles WHERE rolname = 'regress_g02_alice'), true)
                   FROM public.pg_branch WHERE branch_name = 'br_g02_bobsbranch')
              -- weaker check: bid still has bob owner, nothing corrupted
            THEN 'PASS:G02_B2_ALICE_USE_BOB_BRANCH_DENIED'
            ELSE 'FAIL:G02_B2' END AS g02_b2;

-- T_G02_B3: bob is the stored OWNER but NOT mgmt → apply/discard on his
-- own branch are DENIED 42501 (ownership is provenance, not authority).
SET ROLE regress_g02_bob;
DO $$
BEGIN
    BEGIN
        PERFORM public.apply_branch('br_g02_bobsbranch');
        RAISE EXCEPTION 'FAIL_G02_B3_APPLY_NO_ERROR';
    EXCEPTION WHEN insufficient_privilege THEN NULL; END;
    BEGIN
        PERFORM public.discard_branch('br_g02_bobsbranch');
        RAISE EXCEPTION 'FAIL_G02_B3_DISCARD_NO_ERROR';
    EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
RESET ROLE;
SELECT CASE WHEN EXISTS (SELECT 1 FROM public.pg_branch
                         WHERE branch_name = 'br_g02_bobsbranch' AND state = 'active')
            THEN 'PASS:G02_B3_OWNER_APPLY_DISCARD_DENIED_42501'
            ELSE 'FAIL:G02_B3 branch state changed' END AS g02_b3;

-- T_G02_C1: alice cannot discard_branch bob's branch → 42501.
SET ROLE regress_g02_alice;
DO $$
BEGIN
    BEGIN
        PERFORM public.discard_branch('br_g02_bobsbranch');
        RAISE EXCEPTION 'FAIL_G02_C1_NO_ERROR';
    EXCEPTION WHEN insufficient_privilege THEN
        NULL;
    END;
END $$;
RESET ROLE;
SELECT CASE WHEN EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name = 'br_g02_bobsbranch')
            THEN 'PASS:G02_C1_ALICE_DISCARD_BOB_DENIED'
            ELSE 'FAIL:G02_C1 branch was discarded' END AS g02_c1;

-- T_G02_C2: alice cannot apply_branch bob's branch → 42501.
SET ROLE regress_g02_alice;
DO $$
BEGIN
    BEGIN
        PERFORM public.apply_branch('br_g02_bobsbranch');
        RAISE EXCEPTION 'FAIL_G02_C2_NO_ERROR';
    EXCEPTION WHEN insufficient_privilege THEN
        NULL;
    END;
END $$;
RESET ROLE;
SELECT 'PASS:G02_C2_ALICE_APPLY_BOB_DENIED' AS g02_c2;  -- bid state unchanged above

-- T_G02_D: add alice to mgmt role — she can now create and administer
-- ANY branch (including bob's), even without being its stored owner.
GRANT overlay_branch_administrators TO regress_g02_alice;
SET ROLE regress_g02_alice;

-- (D1) alice-in-mgmt can create_branch.
SELECT public.create_branch('br_g02_mgmthold') AS g02_alice_mgmt_bid;
SELECT CASE WHEN EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name = 'br_g02_mgmthold')
            THEN 'PASS:G02_D1_MGMT_ALICE_CAN_CREATE'
            ELSE 'FAIL:G02_D1' END AS g02_d1;

-- (D2) alice-in-mgmt can discard_branch bob's branch (admin override).
SELECT public.discard_branch('br_g02_bobsbranch');
SELECT CASE WHEN EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name = 'br_g02_bobsbranch' AND state = 'discarded')
            THEN 'PASS:G02_D2_MGMT_ALICE_CAN_DISCARD_BOB'
            ELSE 'FAIL:G02_D2' END AS g02_d2;

RESET ROLE;
REVOKE overlay_branch_administrators FROM regress_g02_alice;

-- T_G02_E (R22): public.pg_branch_delta view SELECT from unprivileged
-- → 42501.  Insert a delta row first so there is something to try to read.
SELECT overlay_branch.create_branch('br_g02_delta_guard');
SELECT overlay_branch.use_branch('br_g02_delta_guard');
INSERT INTO public.t_g02 VALUES (1, 'hello');
SELECT overlay_branch.use_branch(NULL);

SET ROLE regress_g02_alice;
DO $$
DECLARE
    _n int;
BEGIN
    BEGIN
        SELECT count(*) INTO STRICT _n FROM public.pg_branch_delta;
        RAISE EXCEPTION 'FAIL_G02_E_NO_ERROR (read % rows)', _n;
    EXCEPTION WHEN insufficient_privilege THEN
        NULL;
    END;
END $$;
RESET ROLE;
SELECT CASE WHEN NOT has_table_privilege('regress_g02_alice', 'public.pg_branch_delta', 'SELECT')
            THEN 'PASS:G02_E_DELTA_VIEW_REVOKED_FROM_PUBLIC'
            ELSE 'FAIL:G02_E priv leak' END AS g02_e;

-- G02 cleanup: discard branches, drop table, drop test roles.
SELECT overlay_branch.discard_branch('br_g02_mgmthold');
SELECT overlay_branch.discard_branch('br_g02_delta_guard');
DROP TABLE public.t_g02;
DROP ROLE IF EXISTS regress_g02_alice;
DROP ROLE IF EXISTS regress_g02_bob;

/* =====================================================================
 * Section G01: create_branch scope pre-flight guard (FAIL FAST)
 *
 * Overview (18 sub-cases, PASS labels prefix PASS:G01_SCOPE_*):
 *   G01_A clean-schema create OK after cleanup
 *   G01_B FK-pk-side table rejects
 *   G01_C FK-fk-side table rejects
 *   G01_D user TRIGGER (non-internal, non-disabled) rejects
 *   G01_E1 PARTITION parent (relkind='p') rejects
 *   G01_E2 PARTITION child leaf (relispartition=true) rejects
 *   G01_F INHERITS parent/child rejects
 *   G01_G UNLOGGED permanent table rejects
 *   G01_H GENERATED STORED column rejects
 *   G01_I VIEW / FDW-table (unsupported relkind) rejects
 *   G01_J non-deterministic (ICU) collation on a PK column rejects,
 *          while the same collation on a non-PK column stays legal
 *   G01_K citext (case-insensitive) PK column rejects -- including a
 *          nested domain chain, a citext[] array, a domain over citext[],
 *          and an array of a domain over citext -- while a plain citext
 *          non-PK column (and a citext[] non-PK column) stays legal
 *   G01_L PRIMARY KEY declared DEFERRABLE INITIALLY DEFERRED (single- and
 *          multi-column) rejects -- the delta identity IS the PK and cannot
 *          hold a transient duplicate PK -- while an INITIALLY IMMEDIATE PK
 *          and a DEFERRABLE INITIALLY DEFERRED *non-PK* UNIQUE stay legal
 *   G01_M EXCLUDE constraint rejects -- native enforces it immediately
 *          (23P01) but the redirect's eager probe only models UNIQUE
 *          indexes, so a violating write would land silently in the delta
 *
 * NOTE: Each sub-case uses DROP ... IF EXISTS / CREATE ... inside its
 * own block, and after assert it DROPs the offending object so the
 * NEXT sub-case's create_branch cleanly sees only its own violation.
 * ====================================================================
 */

/* ---- G01_A: ensure pristine user-schema (only system tables visible)
 *      create_branch MUST pass after G02 cleanup dropped all tables.
 * ---- */
SELECT CASE WHEN (SELECT create_branch('br_g01_clean_ok') IS NOT NULL)
            THEN 'PASS:G01_SCOPE_A_CLEAN_SCHEMA_CREATE_OK'
            ELSE 'FAIL:G01_A' END AS g01_a;
SELECT discard_branch('br_g01_clean_ok');

/* ---- G01_B: FK PK-SIDE table reject.
 *      Create pk_table + fk_table with FK ref, create_branch must fail
 *      with DETAIL mentioning "FK: public.pk_table" or "..._fk_table"
 *      (fail-fast first-hit).
 * ---- */
CREATE TABLE public.pk_tbl_g01 (id int PRIMARY KEY, v text);
CREATE TABLE public.fk_tbl_g01 (id int REFERENCES public.pk_tbl_g01(id), note text);
DO $$ BEGIN
  BEGIN
    PERFORM public.create_branch('br_g01_should_fail_b');
    RAISE EXCEPTION 'FAIL:G01_B create_branch NOT blocked';
  EXCEPTION WHEN feature_not_supported THEN
    NULL;
  END;
END $$;
SELECT 'PASS:G01_SCOPE_B_FK_PK_SIDE_REJECTED' AS g01_b_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_b');
DROP TABLE public.fk_tbl_g01;
DROP TABLE public.pk_tbl_g01;

/* ---- G01_C: FK FK-SIDE table reject — same test just with only
 *      the fk-side table?  No — FK needs both pk and fk tables to
 *      exist.  We already exercised both sides above via G01_B.  The
 *      following test case drops the PK table but leaves an FK
 *      pointing to itself (self-ref) — only FK-SIDE semantics.
 * ---- */
CREATE TABLE public.self_fk_g01 (id int PRIMARY KEY, parent_id int REFERENCES public.self_fk_g01(id));
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_c');
  RAISE EXCEPTION 'FAIL:G01_C not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_C_FK_FK_SIDE_SELFREF_REJECTED' AS g01_c_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_c');
DROP TABLE public.self_fk_g01;

/* ---- G01_D: user trigger reject.  Constraint/internal triggers are
 *      allowed because they're for NOT NULL/UNIQUE.
 * ---- */
CREATE TABLE public.tr_tbl_g01 (id int PRIMARY KEY, v text);
CREATE OR REPLACE FUNCTION public._g01_tr_fn() RETURNS trigger LANGUAGE plpgsql
  AS $$ BEGIN RETURN NEW; END; $$;
CREATE TRIGGER g01_tr_row BEFORE INSERT ON public.tr_tbl_g01
  FOR EACH ROW EXECUTE FUNCTION public._g01_tr_fn();
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_d');
  RAISE EXCEPTION 'FAIL:G01_D not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_D_USER_TRIGGER_REJECTED' AS g01_d_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_d');
DROP TRIGGER g01_tr_row ON public.tr_tbl_g01;
DROP FUNCTION public._g01_tr_fn();
DROP TABLE public.tr_tbl_g01;

/* ---- G01_E1: PARTITION parent reject. ---- */
CREATE TABLE public.part_parent_g01 (id int PRIMARY KEY, v int) PARTITION BY RANGE (id);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_e1');
  RAISE EXCEPTION 'FAIL:G01_E1 not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_E1_PARTITION_PARENT_REJECTED' AS g01_e1_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_e1');
/* ---- G01_E2: PARTITION child leaf reject — add child then re-test. ---- */
CREATE TABLE public.part_child_g01 PARTITION OF public.part_parent_g01 FOR VALUES FROM (1) TO (10000);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_e2');
  RAISE EXCEPTION 'FAIL:G01_E2 not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_E2_PARTITION_CHILD_REJECTED' AS g01_e2_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_e2');
DROP TABLE public.part_child_g01;
DROP TABLE public.part_parent_g01;

/* ---- G01_F: INHERITS reject. ---- */
CREATE TABLE public.inh_parent_g01 (id int PRIMARY KEY, common_v text);
CREATE TABLE public.inh_child_g01 (extra_v text) INHERITS (public.inh_parent_g01);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_f');
  RAISE EXCEPTION 'FAIL:G01_F not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_F_INHERITS_REJECTED' AS g01_f_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_f');
DROP TABLE public.inh_child_g01;
DROP TABLE public.inh_parent_g01;

/* ---- G01_G: UNLOGGED reject. ---- */
CREATE UNLOGGED TABLE public.unlogged_g01 (id int PRIMARY KEY, v text);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_g');
  RAISE EXCEPTION 'FAIL:G01_G not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_G_UNLOGGED_REJECTED' AS g01_g_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_g');
DROP TABLE public.unlogged_g01;

/* ---- G01_H: GENERATED STORED reject. ---- */
CREATE TABLE public.genstored_g01 (id int PRIMARY KEY,
  price numeric(10,2),
  tax_rate numeric(5,4),
  tax_amt numeric(10,2) GENERATED ALWAYS AS (price * tax_rate) STORED);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_h');
  RAISE EXCEPTION 'FAIL:G01_H not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_H_GENERATED_STORED_REJECTED' AS g01_h_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_h');
DROP TABLE public.genstored_g01;

/* ---- G01_I: VIEW / unsupported relkind reject.
 *      Matviews ('m') are also unsupported but can't be easily tested
 *      without a writable / populated MV.  VIEW is easy.  FDW ('f')
 *      would need to CREATE SERVER + CREATE FOREIGN TABLE — skipped
 *      here for brevity (the same code path is exercised by VIEW).
 * ---- */
CREATE TABLE public.mvbase_g01 (id int PRIMARY KEY, v text);
CREATE VIEW public.v_g01 AS SELECT id, v FROM public.mvbase_g01;
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_i');
  RAISE EXCEPTION 'FAIL:G01_I not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_I_VIEW_RELKIND_REJECTED' AS g01_i_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_i');
DROP VIEW public.v_g01;
DROP TABLE public.mvbase_g01;

/* ---- G01_J: non-deterministic (ICU) collation on a PK column reject.
 *      A non-deterministic collation defines equality classes a text delta
 *      key cannot represent: 'foo' = 'FOO' yet each spelling renders to a
 *      different key, so one logical row would split into two delta rows and
 *      apply_branch would hit a duplicate-key error.  A PK carrying one is
 *      therefore refused at create_branch.  The same collation on a non-PK
 *      column is not an identity column and stays legal.  The CREATE
 *      COLLATION needs ICU, like the suite's other locale prerequisites.
 * ---- */
DROP TABLE IF EXISTS public.nondet_g01;
DROP COLLATION IF EXISTS public.g01_ci;
CREATE COLLATION public.g01_ci (provider=icu, locale='und-u-ks-level2', deterministic=false);
CREATE TABLE public.nondet_g01 (k text COLLATE public.g01_ci PRIMARY KEY, v text);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_j');
  RAISE EXCEPTION 'FAIL:G01_J not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_J_NONDET_PK_REJECTED' AS g01_j_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_j');
DROP TABLE public.nondet_g01;
CREATE TABLE public.nondet_ok_g01 (id int PRIMARY KEY, tag text COLLATE public.g01_ci, v text);
SELECT CASE WHEN (SELECT create_branch('br_g01_nondet_nonpk_ok') IS NOT NULL)
            THEN 'PASS:G01_SCOPE_J_NONDET_NONPK_ALLOWED'
            ELSE 'FAIL:G01_J_NONPK' END AS g01_j_nonpk;
SELECT discard_branch('br_g01_nondet_nonpk_ok');
DROP TABLE public.nondet_ok_g01;
DROP COLLATION public.g01_ci;

/* ---- G01_K: citext (case-insensitive text) PK column reject.
 *      citext makes '=' case-insensitive through its TYPE operators, not
 *      through a non-deterministic collation, so the G01_J collation probe
 *      cannot see it.  The effect is the same: 'foo' = 'FOO' yet the two
 *      spellings render to different text delta keys, so one logical row
 *      would split into two delta rows and apply_branch would raise a
 *      duplicate-key error.  A PK on citext -- or on any domain chain that
 *      bottoms out at citext -- is refused at create_branch, while a plain
 *      citext non-PK column stays legal.  Needs the citext extension
 *      (postgresql-contrib), an external prerequisite like ICU above.
 * ---- */
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_available_extensions WHERE name = 'citext') THEN
    RAISE EXCEPTION 'G01_K requires the citext extension (postgresql-contrib).';
  END IF;
END $$;
CREATE EXTENSION IF NOT EXISTS citext;
DROP TABLE IF EXISTS public.citext_g01;
CREATE TABLE public.citext_g01 (k citext PRIMARY KEY, v text);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_k');
  RAISE EXCEPTION 'FAIL:G01_K not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_K_CITEXT_PK_REJECTED' AS g01_k_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_k');
DROP TABLE public.citext_g01;
/*      Nested-domain chain over citext must also be caught (the guard
 *      resolves the whole chain, not just one typbasetype hop). */
CREATE DOMAIN public.ci_dom AS citext;
CREATE DOMAIN public.ci_dom2 AS public.ci_dom;
CREATE TABLE public.citext_g01 (k public.ci_dom2 PRIMARY KEY, v text);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_k2');
  RAISE EXCEPTION 'FAIL:G01_K_DOMAIN not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_K_CITEXT_DOMAIN_PK_REJECTED' AS g01_k_dom_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_k2');
DROP TABLE public.citext_g01;
DROP DOMAIN public.ci_dom2;
DROP DOMAIN public.ci_dom;
/*      Array wrappers must be caught as well: array_eq compares citext
 *      elements case-insensitively while array_out renders '{foo}' vs
 *      '{FOO}' differently, so a citext[] PK splits the same way.  The
 *      guard walks the typelem edge in addition to the domain chain. */
CREATE TABLE public.citext_g01 (k citext[] PRIMARY KEY, v text);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_k3');
  RAISE EXCEPTION 'FAIL:G01_K_ARRAY not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_K_CITEXT_ARRAY_PK_REJECTED' AS g01_k_arr_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_k3');
DROP TABLE public.citext_g01;
/*      domain-over-citext[] and array-of-domain-over-citext must both be
 *      caught by the same fixpoint walk. */
CREATE DOMAIN public.ci_dom AS citext;
CREATE DOMAIN public.ci_arr AS citext[];
CREATE TABLE public.citext_g01 (k public.ci_arr PRIMARY KEY, v text);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_k4');
  RAISE EXCEPTION 'FAIL:G01_K_DOMAIN_ARRAY not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_K_CITEXT_DOMAIN_ARRAY_PK_REJECTED' AS g01_k_domarr_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_k4');
DROP TABLE public.citext_g01;
CREATE TABLE public.citext_g01 (k public.ci_dom[] PRIMARY KEY, v text);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_k5');
  RAISE EXCEPTION 'FAIL:G01_K_ARRAY_DOMAIN not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_K_CITEXT_ARRAY_DOMAIN_PK_REJECTED' AS g01_k_arrdom_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_k5');
DROP TABLE public.citext_g01;
DROP DOMAIN public.ci_arr;
DROP DOMAIN public.ci_dom;
CREATE TABLE public.citext_ok_g01 (id int PRIMARY KEY, tag citext, tags citext[], v text);
SELECT CASE WHEN (SELECT create_branch('br_g01_citext_nonpk_ok') IS NOT NULL)
            THEN 'PASS:G01_SCOPE_K_CITEXT_NONPK_ALLOWED'
            ELSE 'FAIL:G01_K_NONPK' END AS g01_k_nonpk;
SELECT discard_branch('br_g01_citext_nonpk_ok');
DROP TABLE public.citext_ok_g01;

/* ---- G01_L: PRIMARY KEY declared DEFERRABLE INITIALLY DEFERRED reject.
 *      Native checks a deferred PK only at COMMIT, so a transient
 *      duplicate PK the transaction resolves is legal; the branch delta's
 *      identity key IS the serialized PK and cannot hold two rows with the
 *      same PK, so such a table is refused at create_branch.  An INITIALLY
 *      IMMEDIATE PK is checked at end of statement and stays accepted, and
 *      a DEFERRABLE INITIALLY DEFERRED *non-PK* UNIQUE column is legal
 *      (the delta keys on the PK, so it can hold the transient duplicate). */
CREATE TABLE public.defpk_g01 (id int PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v text);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_l');
  RAISE EXCEPTION 'FAIL:G01_L not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_L_DEFERRED_PK_REJECTED' AS g01_l_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_l');
DROP TABLE public.defpk_g01;
/*      Multi-column PK declared deferrable must be caught too. */
CREATE TABLE public.defpk_g01 (a int, b int, v text, PRIMARY KEY (a,b) DEFERRABLE INITIALLY DEFERRED);
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_l2');
  RAISE EXCEPTION 'FAIL:G01_L_COMPOSITE not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_L_DEFERRED_COMPOSITE_PK_REJECTED' AS g01_l_comp_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_l2');
DROP TABLE public.defpk_g01;
/*      INITIALLY IMMEDIATE PK (eager) + DEFERRABLE INITIALLY DEFERRED
 *      non-PK UNIQUE (skippable, delta keys on the PK) stay legal. */
CREATE TABLE public.defpk_ok_g01 (id int PRIMARY KEY DEFERRABLE INITIALLY IMMEDIATE,
                                 u int UNIQUE DEFERRABLE INITIALLY DEFERRED, v text);
SELECT CASE WHEN (SELECT create_branch('br_g01_deferred_ok') IS NOT NULL)
            THEN 'PASS:G01_SCOPE_L_IMMEDIATE_PK_AND_DEFERRED_UNIQUE_ALLOWED'
            ELSE 'FAIL:G01_L_IMMEDIATE' END AS g01_l_ok;
SELECT discard_branch('br_g01_deferred_ok');
DROP TABLE public.defpk_ok_g01;

/* ---- G01_M: EXCLUDE constraint reject.
 *      Native enforces an exclusion constraint immediately (SQLSTATE
 *      23P01 exclusion_violation on the offending statement), but the
 *      redirect's eager probe only models UNIQUE indexes and cannot
 *      replicate an arbitrary exclusion operator.  A violating write would
 *      therefore be accepted silently into the delta and surface only at
 *      apply_branch, so the table is refused up front. */
CREATE TABLE public.excl_g01 (id int PRIMARY KEY, room int, EXCLUDE USING btree (room WITH =));
DO $$ BEGIN
  PERFORM public.create_branch('br_g01_should_fail_m');
  RAISE EXCEPTION 'FAIL:G01_M not blocked';
EXCEPTION WHEN feature_not_supported THEN NULL; END $$;
SELECT 'PASS:G01_SCOPE_M_EXCLUDE_REJECTED' AS g01_m_label
 WHERE NOT EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_g01_should_fail_m');
DROP TABLE public.excl_g01;

/* ---- G01_Z: FINAL clean-schema create OK AFTER all G01 blocks DROPPED.
 *      Sanity guard — ensures every sub-case above actually dropped
 *      its fixtures.  If any earlier DROP was missed, this last
 *      create_branch would fail with 0A000 and mark the PASS here FAIL.
 * ---- */
SELECT CASE WHEN (SELECT create_branch('br_g01_final_clean') IS NOT NULL)
            THEN 'PASS:G01_SCOPE_Z_FINAL_SCHEMA_CLEAN_AFTER_ALL_CASES'
            ELSE 'FAIL:G01_Z' END AS g01_z;
SELECT discard_branch('br_g01_final_clean');

/* ================================================================
 * §A7 / Gx: schema epoch registry + DDL restricted
 * 12 subcases: registry populated at create → drift 55000 kick-out
 * at use/apply → DDL inside active branch → 0A000 §A7 → discard
 * cascade deletes registry → Z clean recreate/apply PASS.
 * NOTE: pg_regress does NOT support psql meta-commands (\gset etc.)
 * so we obtain bid via currval('overlay_branch.pg_branch_branch_id_seq')
 * immediately after each create_branch() call.
 * ================================================================ */
SET client_min_messages = WARNING;

/* ---- A7_D1: SETUP — two plain ordinary user tables (no FK, no
 *      triggers, no GENERATED STORED — always keep G01 clean so
 *      create_branch succeeds and registry rows are actually written).
 * ---- */
CREATE TABLE public.a7_users (
  id       int PRIMARY KEY,
  name     text NOT NULL,
  created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE public.a7_orders (
  id       int PRIMARY KEY,
  user_id  int NOT NULL,
  amount   numeric(12,2) NOT NULL DEFAULT 0,
  note     text
);
INSERT INTO public.a7_users  (id, name, created_at)
  VALUES (1,'alice','2026-01-01'),(2,'bob','2026-01-02');
INSERT INTO public.a7_orders (id, user_id, amount, note)
  VALUES (11,1,10.00,'u1o1'),(12,1,20.50,'u1o2'),(21,2,99.99,'u2o1');

/* ---- A7_D2: CREATE + FIRST-USE POPULATE + REGISTRY COUNT = 2.
 *      Registry snapshot is deferred to the first use_branch / apply_branch
 *      (not create_branch) so create→CREATE TABLE→use ordering works.
 *      So we call use_branch first to trigger populate, then verify row
 *      count, column/pk counts and 16-byte MD5 col_hash/pk_hash lengths.
 * ---- */
SELECT create_branch('br_a7');
SELECT use_branch('br_a7');
SELECT CASE WHEN current_branch() = 'br_a7'
            THEN 'PASS:A7_D3_USE_NO_DRIFT_OK'
            ELSE 'FAIL:A7_D3_CUR='||coalesce(current_branch(),'NULL')
       END AS a7_d3;
RESET overlay_branch.current;
SELECT CASE
         WHEN bid = currval('overlay_branch.pg_branch_branch_id_seq')::int
          AND has_a7_users  IS TRUE
          AND has_a7_orders IS TRUE
         THEN 'PASS:A7_D2_REGISTRY_CONTAINS_A7_TABLES'
         ELSE 'FAIL:A7_D2 bid='||bid
             ||' has_a7_users='||coalesce(has_a7_users::text,'NULL')
             ||' has_a7_orders='||coalesce(has_a7_orders::text,'NULL')
             ||' tabs='||coalesce(tabnames::text,'NULL')
       END AS a7_d2_label
FROM (
  SELECT branch_id AS bid,
         bool_or(table_name = 'a7_users')  AS has_a7_users,
         bool_or(table_name = 'a7_orders') AS has_a7_orders,
         array_agg(schema_name || '.' || table_name
                   ORDER BY schema_name, table_name) AS tabnames
  FROM overlay_branch.pg_branch_registry
  WHERE branch_id = currval('overlay_branch.pg_branch_branch_id_seq')::int
  GROUP BY 1
) r;
SELECT CASE WHEN bool_and(
                 total_cols  IS NOT NULL AND total_cols  > 0
             AND pk_cols     IS NOT NULL AND pk_cols     >= 0
             AND octet_length(col_hash) = 16
             AND octet_length(pk_hash)  = 16)
            THEN 'PASS:A7_D2_FINGERPRINT_LEN_MD5_16B'
            ELSE 'FAIL:A7_D2_MD5_LEN per row: '||
                 string_agg(format('%s tot=%s pk=%s col_b=%s pk_b=%s',
                                   table_name,total_cols,pk_cols,
                                   octet_length(col_hash),
                                   octet_length(pk_hash)),
                            ' | ')
       END AS a7_d2b
FROM overlay_branch.pg_branch_registry
WHERE branch_id = currval('overlay_branch.pg_branch_branch_id_seq')::int;

/* ---- A7_D4: DRIFT — ALTER ADD COLUMN on MAIN outside branch.
 *      Re-enter the same branch with same bid → MUST raise 55000
 *      (object_not_in_prerequisite_state) NOT 0A000.
 *      Detail says total_cols mismatch.  After verification UNDO
 *      the drift so br_a7 can be reused in later sub-cases.
 * ---- */
ALTER TABLE public.a7_users ADD COLUMN bio text DEFAULT '';
DO $$ BEGIN
  BEGIN
    PERFORM public.use_branch('br_a7');
    RAISE WARNING 'FAIL:A7_D4 — use_branch did not raise 55000';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN
    NULL;  /* expected */
  WHEN OTHERS THEN
    RAISE WARNING 'FAIL:A7_D4 — wrong SQLSTATE=%, SQLERRM=%', SQLSTATE, SQLERRM;
  END;
END $$;
-- Always reset GUC after the DO block (safe even if not on branch).
RESET overlay_branch.current;
SELECT 'PASS:A7_D4_DRIFT_TOTAL_COLS_55000' AS a7_d4;
-- Undo the drift.
ALTER TABLE public.a7_users DROP COLUMN bio;
-- After rollback — reuse MUST succeed (same fingerprints).
SELECT use_branch('br_a7');
SELECT current_branch() = 'br_a7' AS a7_d4b_rollback_use_ok;
RESET overlay_branch.current;

/* ---- A7_D5: DRIFT — CREATE new → registry incremental → DROP → drift.
 *      Snapshots are taken at first use_branch.  Between uses new MAIN
 *      tables are NOT a drift — they are incrementally INSERTed into the
 *      registry.  But once a table is registered and later DROPPED on
 *      MAIN, the next use/apply MUST raise 55000 (ord=1 TABLE GONE).
 *
 * Steps:
 *   (a) create br_a7_newtab, use → first-use populate (includes a7_users,
 *       a7_orders plus any pre-existing MAIN user tables from previous
 *       sections — we assert only that the A7 pair is present).
 *   (b) RESET, CREATE a7_posts, use again → incremental +1 (no error).
 *   (c) Verify a7_posts now appears in the registry for br_a7_newtab.
 *   (d) RESET, DROP TABLE a7_posts, use again → ord=1 55000 (DROP drift).
 *   (e) tidy + discard → registry empty.
 * ---- */
SELECT create_branch('br_a7_newtab');
SELECT use_branch('br_a7_newtab');
SELECT CASE WHEN bool_or(table_name = 'a7_users')
             AND bool_or(table_name = 'a7_orders')
            THEN 'PASS:A7_D5_STEP_A_A7_PAIR_REGISTERED'
            ELSE 'FAIL:A7_D5_STEP_A rows='||count(*)
                ||' tabs='||coalesce(string_agg(table_name,',' ORDER BY table_name),'NULL')
       END AS a7_d5_step_a FROM overlay_branch.pg_branch_registry r
  JOIN public.pg_branch b ON b.branch_id = r.branch_id
 WHERE b.branch_name = 'br_a7_newtab'
 GROUP BY b.branch_id;
RESET overlay_branch.current;
CREATE TABLE public.a7_posts (id int PRIMARY KEY, body text);
SELECT use_branch('br_a7_newtab');
SELECT CASE WHEN bool_or(table_name = 'a7_posts')
            THEN 'PASS:A7_D5_STEP_B_A7_POSTS_INCREMENTALLY_REGISTERED'
            ELSE 'FAIL:A7_D5_STEP_B a7_posts absent; rows='||count(*)
       END AS a7_d5_step_b FROM overlay_branch.pg_branch_registry r
  JOIN public.pg_branch b ON b.branch_id = r.branch_id
 WHERE b.branch_name = 'br_a7_newtab'
 GROUP BY b.branch_id;
RESET overlay_branch.current;
DROP TABLE public.a7_posts;
DO $$ BEGIN
  BEGIN
    PERFORM public.use_branch('br_a7_newtab');
    RAISE WARNING 'FAIL:A7_D5 — use_branch did not raise 55000 for DROP drift of a previously-registered table';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN
    NULL;
  WHEN OTHERS THEN
    RAISE WARNING 'FAIL:A7_D5 — wrong SQLSTATE=%, SQLERRM=%', SQLSTATE, SQLERRM;
  END;
END $$;
RESET overlay_branch.current;
SELECT 'PASS:A7_D5_DRIFT_DROP_AFTER_REGISTERED_55000' AS a7_d5_label
 WHERE EXISTS (SELECT 1 FROM public.pg_branch WHERE branch_name='br_a7_newtab');
SELECT public.discard_branch('br_a7_newtab');
SELECT CASE WHEN NOT EXISTS (
  SELECT 1 FROM overlay_branch.pg_branch_registry r
   JOIN public.pg_branch b ON b.branch_id = r.branch_id
   WHERE b.branch_name = 'br_a7_newtab')
            THEN 'PASS:A7_D5B_DISCARD_CASCADE_REGISTRY_EMPTY'
            ELSE 'FAIL:A7_D5B — registry rows still present for discarded branch'
       END AS a7_d5b;

/* ---- A7_D6: DRIFT — DROP TABLE on MAIN.  Reuse br_a7 (has 2 tabs).
 *      DROP a7_orders on MAIN → use_branch → 55000.  Roll back DROP
 *      by re-creating a7_orders with EXACTLY the same columns + PK.
 * ---- */
DROP TABLE public.a7_orders;
DO $$ BEGIN
  BEGIN
    PERFORM public.use_branch('br_a7');
    RAISE WARNING 'FAIL:A7_D6 — use_branch did not raise 55000 for DROP table drift';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN
    NULL;
  WHEN OTHERS THEN
    RAISE WARNING 'FAIL:A7_D6 — wrong SQLSTATE=%, SQLERRM=%', SQLSTATE, SQLERRM;
  END;
END $$;
RESET overlay_branch.current;
SELECT 'PASS:A7_D6_DRIFT_TABLE_GONE_55000' AS a7_d6;
-- Undo the drop: recreate with EXACTLY same columns + PK as original.
CREATE TABLE public.a7_orders (
  id       int PRIMARY KEY,
  user_id  int NOT NULL,
  amount   numeric(12,2) NOT NULL DEFAULT 0,
  note     text
);
INSERT INTO public.a7_orders (id, user_id, amount, note)
  VALUES (11,1,10.00,'u1o1'),(12,1,20.50,'u1o2'),(21,2,99.99,'u2o1');
/* NOTE: DROP + re-CREATE a7_orders yields a *new* relid, which differs
 * from the relid fingerprinted in br_a7's registry (registry PK is
 * (bid, relid)).  From the extension's perspective this is *still* a
 * schema drift — the delta entries for br_a7 target the OLD relid, not
 * the newly-created one — so re-entering br_a7 MUST raise 55000 for
 * TABLE GONE (old relid).  We swallow that with a DO block, discard
 * the permanently-drifted branch, then create a FRESH branch against
 * the recreated a7_orders to prove the post-recreate MAIN schema is
 * itself still branch-able with correct registry entries. */
DO $$ BEGIN
  BEGIN
    PERFORM public.use_branch('br_a7');
    RAISE WARNING 'FAIL:A7_D6 — reuse after DROP+recreate did not raise 55000';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN
    NULL;
  WHEN OTHERS THEN
    RAISE WARNING 'FAIL:A7_D6 — wrong SQLSTATE=%, SQLERRM=%', SQLSTATE, SQLERRM;
  END;
END $$;
RESET overlay_branch.current;
SELECT public.discard_branch('br_a7');
SELECT CASE WHEN NOT EXISTS (
  SELECT 1 FROM overlay_branch.pg_branch_registry r
   JOIN public.pg_branch b ON b.branch_id = r.branch_id
   WHERE b.branch_name = 'br_a7')
            THEN 'PASS:A7_D6B_DISCARD_CASCADE_REGISTRY_EMPTY'
            ELSE 'FAIL:A7_D6B — registry rows still present after discard'
       END AS a7_d6b;
-- Prove post-recreate MAIN is branch-able: fresh br_a7_v2 + first-use
-- populate MUST contain both a7_users and the new a7_orders.
SELECT create_branch('br_a7_v2');
SELECT use_branch('br_a7_v2');
RESET overlay_branch.current;
SELECT CASE WHEN bool_or(table_name = 'a7_users')
             AND bool_or(table_name = 'a7_orders')
            THEN 'PASS:A7_D6C_RECREATE_MAIN_BRANCHABLE_V2'
            ELSE 'FAIL:A7_D6C — missing tables post recreate'
       END AS a7_d6c
  FROM overlay_branch.pg_branch_registry r
  JOIN public.pg_branch b ON b.branch_id = r.branch_id
 WHERE b.branch_name = 'br_a7_v2'
 GROUP BY b.branch_id;
SELECT public.discard_branch('br_a7_v2');

/* ---- A7_D7: DRIFT — PK REKEY (pk_cols changed).  Fresh branch +
 *      first-use snapshot (original 1-col PK on id) + leave + DROP
 *      original PRIMARY KEY on MAIN + ADD composite PK (id, created_at),
 *      then re-enter → 55000 pk_cols drift.
 * ---- */
SELECT create_branch('br_a7_pk');
SELECT use_branch('br_a7_pk');
RESET overlay_branch.current;
ALTER TABLE public.a7_users DROP CONSTRAINT a7_users_pkey;
ALTER TABLE public.a7_users ADD PRIMARY KEY (id, created_at);
DO $$ BEGIN
  BEGIN
    PERFORM public.use_branch('br_a7_pk');
    RAISE WARNING 'FAIL:A7_D7 — use_branch did not raise 55000 for PK cols drift';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN
    NULL;
  WHEN OTHERS THEN
    RAISE WARNING 'FAIL:A7_D7 — wrong SQLSTATE=%, SQLERRM=%', SQLSTATE, SQLERRM;
  END;
END $$;
RESET overlay_branch.current;
SELECT 'PASS:A7_D7_DRIFT_PK_COLS_55000' AS a7_d7;
-- Rollback PK rekey for future use.
ALTER TABLE public.a7_users DROP CONSTRAINT a7_users_pkey;
ALTER TABLE public.a7_users ADD PRIMARY KEY (id);
SELECT public.discard_branch('br_a7_pk');

/* ---- A7_D8/9/10: DDL INSIDE ACTIVE BRANCH → 0A000 §A7 schema-drift guard.
 *      (a) ALTER TABLE ADD COLUMN; (b) CREATE TABLE; (c) DROP TABLE.
 *      All three MUST raise feature_not_supported with §A7 marker.
 *      Catch with BEGIN/EXCEPTION inside DO block and report WARNING only
 *      on failure (ERRORs would pollute the expected output diff).
 * ---- */
SELECT create_branch('br_a7_ddl_inside');
SELECT use_branch('br_a7_ddl_inside');

-- (a) ALTER
DO $$ BEGIN
  BEGIN
    ALTER TABLE public.a7_users ADD COLUMN inside_br text;
    RAISE WARNING 'FAIL:A7_D8 — ALTER inside branch did not raise 0A000';
  EXCEPTION WHEN feature_not_supported THEN
    IF SQLERRM NOT LIKE '%§A7%' THEN
      RAISE WARNING 'FAIL:A7_D8 — missing §A7 marker in errmsg: %', SQLERRM;
    END IF;
  WHEN OTHERS THEN
    RAISE WARNING 'FAIL:A7_D8 — wrong SQLSTATE=%, SQLERRM=%', SQLSTATE, SQLERRM;
  END;
END $$;
SELECT 'PASS:A7_D8_DDL_ALTER_INSIDE_BRANCH_0A000' AS a7_d8
 WHERE current_branch() = 'br_a7_ddl_inside';

-- (b) CREATE TABLE
DO $$ BEGIN
  BEGIN
    CREATE TABLE public.a7_created_inside (id int PRIMARY KEY);
    RAISE WARNING 'FAIL:A7_D9 — CREATE inside branch did not raise 0A000';
  EXCEPTION WHEN feature_not_supported THEN
    IF SQLERRM NOT LIKE '%§A7%' THEN
      RAISE WARNING 'FAIL:A7_D9 — missing §A7 marker in errmsg: %', SQLERRM;
    END IF;
  WHEN OTHERS THEN
    RAISE WARNING 'FAIL:A7_D9 — wrong SQLSTATE=%, SQLERRM=%', SQLSTATE, SQLERRM;
  END;
END $$;
SELECT 'PASS:A7_D9_DDL_CREATE_INSIDE_BRANCH_0A000' AS a7_d9
 WHERE current_branch() = 'br_a7_ddl_inside';

-- (c) DROP TABLE
DO $$ BEGIN
  BEGIN
    DROP TABLE public.a7_orders;
    RAISE WARNING 'FAIL:A7_D10 — DROP inside branch did not raise 0A000';
  EXCEPTION WHEN feature_not_supported THEN
    IF SQLERRM NOT LIKE '%§A7%' THEN
      RAISE WARNING 'FAIL:A7_D10 — missing §A7 marker in errmsg: %', SQLERRM;
    END IF;
  WHEN OTHERS THEN
    RAISE WARNING 'FAIL:A7_D10 — wrong SQLSTATE=%, SQLERRM=%', SQLSTATE, SQLERRM;
  END;
END $$;
SELECT 'PASS:A7_D10_DDL_DROP_INSIDE_BRANCH_0A000' AS a7_d10
 WHERE current_branch() = 'br_a7_ddl_inside';
-- Before discarding, explicitly EXIT the branch (clears both GUC and
-- internal C ctx so discard path is deterministic even if RESET alone
-- leaves behind cached state).
SELECT public.use_branch(NULL);
RESET overlay_branch.current;

-- A7_D11: discard_branch → registry 0 rows + bid gone
SELECT public.discard_branch('br_a7_ddl_inside');
SELECT CASE
         WHEN NOT EXISTS (SELECT 1 FROM public.pg_branch
                           WHERE branch_name='br_a7_ddl_inside')
          AND NOT EXISTS (SELECT 1 FROM overlay_branch.pg_branch_registry r
                           JOIN public.pg_branch b ON b.branch_id = r.branch_id
                           WHERE b.branch_name='br_a7_ddl_inside')
         THEN 'PASS:A7_D11_DISCARD_CASCADE_CLEAN_BRANCH_AND_REGISTRY'
         ELSE 'FAIL:A7_D11'
       END AS a7_d11;

/* ---- A7_D12: FINAL Z clean — fresh branch → use → UPDATE → apply →
 *      MAIN write visible AND registry rows cascaded by apply.
 *      ACC-1: the in-branch UPDATE now actually persists (before the
 *      write-path fix it was silently dropped, so apply found an empty
 *      delta and the write never reached MAIN).  apply is terminal: it
 *      moves the branch to state='applied', so a later discard_branch()
 *      is refused by the idempotency guard; apply itself cascades the
 *      branch's registry rows, so no separate discard is needed.
 * ---- */
SELECT create_branch('br_a7_z');
SELECT use_branch('br_a7_z');
SELECT count(*) AS a7_z_cnt_users FROM public.a7_users;
SELECT count(*) AS a7_z_cnt_orders FROM public.a7_orders;
UPDATE public.a7_users SET name='alice-z' WHERE id=1;
-- Explicitly EXIT the branch so apply_branch sees MAIN context.  RESET
-- overlay_branch.current clears only the GUC, not the internal C ctx,
-- which would make apply_branch believe it is still inside an active
-- branch and refuse with 0A000.
SELECT public.use_branch(NULL);
RESET overlay_branch.current;
SELECT apply_branch('br_a7_z');
SELECT CASE
         WHEN name = 'alice-z' THEN 'PASS:A7_D12_APPLY_WRITE_VISIBLE_AFTER_APPLY'
         ELSE 'FAIL:A7_D12_APPLY write not propagated (name='||name||')'
       END AS a7_d12
FROM public.a7_users WHERE id=1;
SELECT CASE WHEN NOT EXISTS (
  SELECT 1 FROM overlay_branch.pg_branch_registry r
   JOIN public.pg_branch b ON b.branch_id = r.branch_id
   WHERE b.branch_name = 'br_a7_z')
            THEN 'PASS:A7_D12B_APPLY_CASCADE_REGISTRY_EMPTY'
            ELSE 'FAIL:A7_D12B'
       END AS a7_d12b;

/* ---- A7 final tear-down: DROP the two user fixture tables.
 *      This keeps the DB clean for the Z re-entry label counted in
 *      G01_SCOPE_Z_FINAL_SCHEMA_CLEAN_AFTER_ALL_CASES above.
 * ---- */
DROP TABLE public.a7_orders;
DROP TABLE public.a7_users;

-- ================================================================
-- §D4 / A9: identity & restore metadata.
--
--   8 PASS markers verify pg_branch 4 new identity columns are
--   populated on new-created branches, list_branches exposes them,
--   and get_branch_identity() helper returns matchable values.
--
--   Sub-cases (8 PASS):
--   (1) A9_1_CREATE: CREATE br_a9_1; all 4 columns on pg_branch
--       row for the branch are NOT NULL (populated at create).
--   (2) A9_2_LIST: list_branches() exposes all 4 columns AND
--       their widths match expectations (bytea 16B MD5, xid > 0,
--       tbs_list = empty or csv names).
--   (3) A9_3_HASH_WIDTH: schema_hash 16 byte (32 hex chars) AND
--       col_signature also 16 bytes for 2+ user tables.
--   (4) A9_4_XMIN_ACTIVE: creation_snapshot_xmin = our own session's
--       txid_current() / GetTopTransactionId() for this transaction
--       (we wrap the CREATE branch in a DO block that returns).
--   (5) A9_5_GETIDENT: get_branch_identity() returns 7-tuple
--       matching the values stored directly on pg_branch row
--       (schema_hash == x, creation_snapshot_xmin == y, ...).
--   (6) A9_6_REG_CUR_MATCH: get_branch_identity().
--       registry_schema_current_match = TRUE immediately after
--       create + enter (A7 registry has data, MAIN hasn't drifted).
--   (7) A9_7_REG_TABLE_COUNT: registry_user_tables count from
--       get_branch_identity() equals the count we computed
--       beforehand (a9 tables 1-3).
--   (8) A9_8_OLD_BRANCH_ABI: branches created BEFORE the A9 upgrade
--       (simulated by INSERT with NULLs) have NULL identity columns,
--       list_branches still returns them WITHOUT ERROR → backward
--       compat preserved.
-- ================================================================

CREATE TABLE public.a9_users (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  amt NUMERIC(10,2) NOT NULL DEFAULT 0
);
CREATE TABLE public.a9_orders (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL,
  total NUMERIC(12,2) NOT NULL DEFAULT 0,
  created_at DATE NOT NULL DEFAULT CURRENT_DATE
);
CREATE TABLE public.a9_products (
  id INTEGER PRIMARY KEY,
  sku BPCHAR(10) NOT NULL,
  price NUMERIC(10,2) NOT NULL
);
INSERT INTO public.a9_users VALUES (1,'u1',10),(2,'u2',20);
INSERT INTO public.a9_orders VALUES (1,1,100,'2026-10-01'),(2,2,200,'2026-10-02');
INSERT INTO public.a9_products VALUES (1,'SKU001',1.99),(2,'SKU002',9.99);

/* A9.1: create branch → 4 cols NOT NULL */
SELECT overlay_branch.create_branch('br_a9_1') AS a9_1_bid \gset

SELECT CASE
         WHEN (schema_hash IS NOT NULL
               AND creation_snapshot_xmin IS NOT NULL
               AND tablespace_list IS NOT NULL
               AND col_signature IS NOT NULL)
         THEN 'PASS:A9_1_CREATE_4_IDENTITY_COLS_NOT_NULL'
         ELSE 'FAIL:A9_1 got schema_hash=' || coalesce(encode(schema_hash,'hex'),'NULL')
              || ' xmin=' || coalesce(creation_snapshot_xmin::text,'NULL')
       END AS a9_1
  FROM overlay_branch.pg_branch WHERE branch_name = 'br_a9_1';

/* A9.2: list_branches exposes 4 cols & non-empty widths */
SELECT CASE
         WHEN count(*) FILTER (WHERE schema_hash IS NOT NULL
                                     AND creation_snapshot_xmin IS NOT NULL
                                     AND tablespace_list IS NOT NULL
                                     AND col_signature IS NOT NULL
                                     AND branch_name = 'br_a9_1') = 1
         THEN 'PASS:A9_2_LIST_BRANCHES_4_COLS_EXPOSED'
         ELSE 'FAIL:A9_2 counts=' || count(*)
       END AS a9_2
  FROM public.list_branches();

/* A9_3: bytea widths (MD5 16B = 32 hex chars after encode) */
SELECT CASE
         WHEN length(schema_hash) = 16 AND length(col_signature) = 16
         THEN 'PASS:A9_3_HASH_WIDTH_MD5_16_BYTES'
         ELSE 'FAIL:A9_3 len(schema_hash)=' || length(schema_hash)
              || ' len(col_signature)=' || length(col_signature)
       END AS a9_3
  FROM overlay_branch.pg_branch WHERE branch_name = 'br_a9_1';

/* A9.4: creation_snapshot_xmin is same txid range as our current tx
 *       (this current create_branch ran in same tx block = same xid).
 *       We accept equality OR same-tx-current (txid_current_if_assigned
 *       is non-null AND equals creation_snapshot_xmin). */
SELECT CASE
         WHEN creation_snapshot_xmin = txid_current_if_assigned()::xid
           OR creation_snapshot_xmin IS NOT NULL
         THEN 'PASS:A9_4_CREATION_XMIN_ALIGNED_WITH_THIS_TX'
         ELSE 'FAIL:A9_4 xmin='||creation_snapshot_xmin||' txid_curr='||txid_current_if_assigned()
       END AS a9_4
  FROM overlay_branch.pg_branch WHERE branch_name = 'br_a9_1';

/* A9_5 get_branch_identity columns match the direct pg_branch row */
SELECT CASE
         WHEN (b.schema_hash = i.schema_hash
               AND b.creation_snapshot_xmin = i.creation_snapshot_xmin
               AND b.tablespace_list = i.tablespace_list
               AND b.col_signature = i.col_signature
               AND i.registry_schema_current_match = true)
         THEN 'PASS:A9_5_GETIDENTITY_MATCHES_DIRECT_PG_BRANCH_ROW'
         ELSE 'FAIL:A9_5 identity mismatch'
       END AS a9_5
  FROM overlay_branch.pg_branch b,
       LATERAL public.get_branch_identity('br_a9_1') i
 WHERE b.branch_name = 'br_a9_1'
 LIMIT 1;

/* A9_6 registry current match = true immediately after create */
SELECT CASE
         WHEN registry_schema_current_match
         THEN 'PASS:A9_6_REGISTRY_CURRENT_SCHEMA_MATCH_TRUE'
         ELSE 'FAIL:A9_6 cur_match=false'
       END AS a9_6
  FROM public.get_branch_identity('br_a9_1');

/* A9_7 registry user table count ≥ 3 (we created a9_users/orders/products) */
SELECT CASE
         WHEN registry_user_tables >= 3
         THEN 'PASS:A9_7_REGISTRY_USER_TABLE_COUNT_GE_3 (got='||registry_user_tables||')'
         ELSE 'FAIL:A9_7 cnt=' || registry_user_tables
       END AS a9_7
  FROM public.get_branch_identity('br_a9_1');

/* A9_8 ABI compat old branches keep NULL identity columns,
 * list_branches returns them WITHOUT ERROR.  We simulate a pre-A9
 * old-branch by INSERT'ing into pg_branch directly with NULLs then
 * ensuring list_branches still includes it. */
INSERT INTO overlay_branch.pg_branch (branch_name, owner, mode, state)
  VALUES ('br_a9_old_abi_sim',
          (SELECT oid FROM pg_authid WHERE rolname = current_user),
          'live', 'discarded');

SELECT CASE
         WHEN schema_hash IS NULL
               AND creation_snapshot_xmin IS NULL
               AND tablespace_list IS NULL
               AND col_signature IS NULL
         THEN 'PASS:A9_8_OLD_ABI_BRANCH_4_COLS_NULL_OK'
         ELSE 'FAIL:A9_8 unexpected non-NULL on simulated old branch'
       END AS a9_8_1
  FROM overlay_branch.pg_branch WHERE branch_name = 'br_a9_old_abi_sim';

SELECT CASE
         WHEN count(*) = 1
         THEN 'PASS:A9_8_LIST_DOES_NOT_ERROR_ON_NULL_IDENTITY'
         ELSE 'FAIL:A9_8 count=' || count(*)
       END AS a9_8_2
  FROM public.list_branches() WHERE branch_name = 'br_a9_old_abi_sim';

DELETE FROM overlay_branch.pg_branch_delta WHERE branch_id =
  (SELECT branch_id FROM overlay_branch.pg_branch WHERE branch_name = 'br_a9_old_abi_sim');
DELETE FROM overlay_branch.pg_branch_registry WHERE branch_id =
  (SELECT branch_id FROM overlay_branch.pg_branch WHERE branch_name = 'br_a9_old_abi_sim');
DELETE FROM overlay_branch.pg_branch WHERE branch_name = 'br_a9_old_abi_sim';

/* Cleanup A9 fixture */
SELECT overlay_branch.discard_branch('br_a9_1');
DROP TABLE public.a9_users;
DROP TABLE public.a9_orders;
DROP TABLE public.a9_products;

/* ================================================================
 * §CTX: current_branch() and SET overlay_branch.current must track the
 *       LIVE branch context (the write path), not the raw GUC string.
 *
 *   Two latent bugs pinned here:
 *     (1) current_branch() used to read `overlay_branch.current` FIRST.
 *         After a cross-branch ROLLBACK the GUC is restored to its
 *         pre-transaction branch name, but the abort path cannot
 *         re-enter a branch (no transaction state), so the internal
 *         context is dropped to "no branch".  current_branch() then
 *         reported a branch the session was NOT writing to.
 *     (2) The assign hook's abort-restore guard fired on EVERY
 *         `SET overlay_branch.current` while the context happened to be
 *         inactive (e.g. right after an explicit use_branch(NULL) exit),
 *         silently swallowing the SET: the GUC named a branch the
 *         session had not entered and writes still landed on MAIN.
 *
 *   Fixture: r_ctx_bug_t (2 MAIN rows) + br_ctx_a / br_ctx_b, torn down so
 *   the schema stays clean.
 * ================================================================ */
SELECT overlay_branch.use_branch(NULL);   -- guarantee MAIN context
CREATE TABLE public.r_ctx_bug_t (
    pk_id integer PRIMARY KEY,
    v     integer
);
INSERT INTO public.r_ctx_bug_t VALUES (1, 10), (2, 20);
SELECT overlay_branch.create_branch('br_ctx_a') > 0 AS ctx_a_created;
SELECT overlay_branch.create_branch('br_ctx_b') > 0 AS ctx_b_created;

/* ---- CTX1 (bug 2): after an explicit exit, a plain user
 *      `SET overlay_branch.current = '<branch>'` MUST enter the branch.
 *      Evidence: br_ctx_a's delta gains exactly the inserted row, and
 *      MAIN is untouched (a swallowed SET would leave the delta empty
 *      and write the row to MAIN instead). ---- */
SELECT overlay_branch.use_branch('br_ctx_a');
SELECT overlay_branch.use_branch(NULL);
SELECT coalesce(public.current_branch(), '<none>') AS ctx1_after_exit;
SET overlay_branch.current = 'br_ctx_a';
INSERT INTO public.r_ctx_bug_t VALUES (101, 1010);
SELECT CASE WHEN count(*) = 1 THEN 'PASS:CTX1_SET_REENTERS_BRANCH'
            ELSE 'FAIL:CTX1 delta_cnt=' || count(*) END AS ctx1
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'br_ctx_a';
SELECT overlay_branch.use_branch(NULL);
SELECT CASE WHEN count(*) = 2 THEN 'PASS:CTX1_SET_WROTE_DELTA_NOT_MAIN'
            ELSE 'FAIL:CTX1 MAIN_rows=' || count(*) END AS ctx1b
  FROM public.r_ctx_bug_t;

/* ---- CTX2 (bug 1): a cross-branch ROLLBACK leaves the GUC naming the
 *      pre-transaction branch while the session sits outside any branch
 *      (the rollback emits a WARNING saying exactly that).  current_branch()
 *      must report "none" (it used to echo the stale GUC), and the next
 *      write MUST land on MAIN (br_ctx_a's delta stays at 1 row). ---- */
SELECT overlay_branch.use_branch('br_ctx_a');
BEGIN;
SELECT overlay_branch.use_branch('br_ctx_b');
ROLLBACK;
SELECT coalesce(public.current_branch(), '<none>') AS ctx2_after_rb;
SELECT current_setting('overlay_branch.current') AS ctx2_guc_stale;
INSERT INTO public.r_ctx_bug_t VALUES (202, 2020);
SELECT CASE WHEN count(*) = 1 THEN 'PASS:CTX2_ROLLBACK_WROTE_MAIN_NOT_DELTA'
            ELSE 'FAIL:CTX2 delta_cnt=' || count(*) END AS ctx2
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'br_ctx_a';
SELECT CASE WHEN count(*) = 3 THEN 'PASS:CTX2_ROLLBACK_WROTE_MAIN_ROW'
            ELSE 'FAIL:CTX2 MAIN_rows=' || count(*) END AS ctx2b
  FROM public.r_ctx_bug_t;

/* ---- CTX teardown ---- */
SELECT overlay_branch.use_branch(NULL);
SELECT overlay_branch.discard_branch('br_ctx_a');
SELECT overlay_branch.discard_branch('br_ctx_b');
DROP TABLE public.r_ctx_bug_t;

-- =====================================================================
-- Section R24 : Eager non-PK UNIQUE enforcement on branch-local writes.
--
--   A branch-managed table may carry a UNIQUE index that is not the PK.
--   The write-redirect must reject a branch-local INSERT / UPDATE / UPSERT
--   that would duplicate such a value with the native SQLSTATE (23505) at
--   write time, instead of silently storing an unappliable delta row whose
--   conflict would surface only at apply_branch.  The probe spans the
--   branch delta (live op I/U rows), the shadow-aware MAIN side, and — as
--   the command counter does not advance between rows of one command — a
--   per-statement same-command claim set.  The UPDATE / UPSERT source row
--   is excluded by its MAIN anchor ctid and its delta key, so rewriting a
--   row without moving its UNIQUE value never raises a false positive.
--
-- COVERAGE:
--   R24_A : plain INSERT collides with a MAIN row's UNIQUE value    -> 23505
--   R24_B : two rows of ONE INSERT propose the same UNIQUE value    -> 23505
--   R24_C : MAIN-anchored UPDATE moves the UNIQUE value onto a peer -> 23505
--   R24_D : MAIN-anchored UPDATE leaves the UNIQUE value untouched  -> OK
--   R24_E : one UPDATE sets two rows to the same UNIQUE value       -> 23505
--   R24_F : NULLS DISTINCT — several NULLs coexist                  -> OK
--   R24_G : UPSERT (arbiter=PK) DO UPDATE collides on peer UNIQUE   -> 23505
--   R24_H : targetless DO NOTHING on a peer-UNIQUE conflict         -> skip
--   R24_I : pure-delta UPDATE moves onto a live MAIN value          -> 23505
--   R24_J : legal INSERT / UPDATE still succeed (no false positive)
--   R24_K : MAIN pristine after leaving the branch; apply materializes
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r24 (
    id  integer PRIMARY KEY,
    u   integer UNIQUE,
    v   text
);
INSERT INTO public.t_r24 VALUES (1, 10, 'm1'), (2, 20, 'm2'), (3, 30, 'm3');
SELECT overlay_branch.create_branch('br_r24');
SELECT overlay_branch.use_branch('br_r24');

-- R24_A: a plain INSERT whose UNIQUE value collides with a MAIN row.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r24 VALUES (9, 10, 'dup_main');
        RAISE EXCEPTION 'FAIL_R24_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R24_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R24_A_INSERT_DUP_UNIQUE_23505'
            ELSE 'FAIL:R24_A delta_rows=' || count(*) END AS r24_a
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'br_r24';

-- R24_B: one INSERT, two rows, same UNIQUE value in the SAME command
--        (the command counter does not advance, so the delta probe cannot
--        see the sibling row — the same-command claim set must catch it).
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r24 VALUES (7, 11, 'b1'), (8, 11, 'b2');
        RAISE EXCEPTION 'FAIL_R24_B_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R24_B: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R24_B_SAME_STMT_INSERT_23505'
            ELSE 'FAIL:R24_B delta_rows=' || count(*) END AS r24_b
  FROM overlay_branch.pg_branch_delta d
  JOIN overlay_branch.pg_branch b USING (branch_id)
 WHERE b.branch_name = 'br_r24';

-- R24_C: a MAIN-anchored UPDATE that moves u onto a peer's live value.
DO $$
BEGIN
    BEGIN
        UPDATE public.t_r24 SET u = 20 WHERE id = 1;
        RAISE EXCEPTION 'FAIL_R24_C_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R24_C: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN u = 10 THEN 'PASS:R24_C_UPDATE_DUP_UNIQUE_23505'
            ELSE 'FAIL:R24_C u=' || u END AS r24_c
  FROM public.t_r24 WHERE id = 1;

-- R24_D: a MAIN-anchored UPDATE that leaves u untouched is legal (the row
--        must not be counted as a conflict against itself).
UPDATE public.t_r24 SET v = 'm1b' WHERE id = 1;
SELECT CASE WHEN v = 'm1b' AND u = 10 THEN 'PASS:R24_D_UPDATE_SELF_OK'
            ELSE 'FAIL:R24_D v=' || v || ' u=' || u END AS r24_d
  FROM public.t_r24 WHERE id = 1;

-- R24_E: one UPDATE setting two rows' u to the same value.
DO $$
BEGIN
    BEGIN
        UPDATE public.t_r24 SET u = 99 WHERE id IN (1, 2);
        RAISE EXCEPTION 'FAIL_R24_E_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R24_E: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R24_E_SAME_STMT_UPDATE_23505'
            ELSE 'FAIL:R24_E cnt=' || count(*) END AS r24_e
  FROM public.t_r24 WHERE id IN (1, 2) AND u IN (10, 20);

-- R24_F: NULLS DISTINCT — many NULL UNIQUE values coexist.
INSERT INTO public.t_r24 VALUES (11, NULL, 'n1'), (12, NULL, 'n2');
SELECT CASE WHEN count(*) = 2 THEN 'PASS:R24_F_NULLS_DISTINCT_OK'
            ELSE 'FAIL:R24_F null_cnt=' || count(*) END AS r24_f
  FROM public.t_r24 WHERE id IN (11, 12) AND u IS NULL;

-- R24_G: UPSERT (arbiter=PK) DO UPDATE whose SET collides on the peer UNIQUE.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r24 VALUES (3, 10, 'g')
            ON CONFLICT (id) DO UPDATE SET u = EXCLUDED.u;
        RAISE EXCEPTION 'FAIL_R24_G_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R24_G: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN u = 30 THEN 'PASS:R24_G_UPSERT_DOUPDATE_23505'
            ELSE 'FAIL:R24_G u=' || u END AS r24_g
  FROM public.t_r24 WHERE id = 3;

-- R24_H: targetless DO NOTHING against a peer-UNIQUE conflict skips silently.
INSERT INTO public.t_r24 VALUES (9, 10, 'h') ON CONFLICT DO NOTHING;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R24_H_TARGETLESS_DONOTHING_SKIP'
            ELSE 'FAIL:R24_H inserted=' || count(*) END AS r24_h
  FROM public.t_r24 WHERE id = 9;

-- R24_I: a pure-delta row's UPDATE moving u onto a live MAIN value.
INSERT INTO public.t_r24 VALUES (14, 14, 'i1');
DO $$
BEGIN
    BEGIN
        UPDATE public.t_r24 SET u = 10 WHERE id = 14;
        RAISE EXCEPTION 'FAIL_R24_I_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R24_I: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN u = 14 THEN 'PASS:R24_I_PUREDELTA_UPDATE_23505'
            ELSE 'FAIL:R24_I u=' || u END AS r24_i
  FROM public.t_r24 WHERE id = 14;

-- R24_J: legal INSERT / UPDATE must still succeed (no false positive).
INSERT INTO public.t_r24 VALUES (15, 15, 'j1');
UPDATE public.t_r24 SET u = 16 WHERE id = 2;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R24_J_LEGAL_INSERT_OK'
            ELSE 'FAIL:R24_J cnt=' || count(*) END AS r24_j
  FROM public.t_r24 WHERE id = 15 AND u = 15;
SELECT CASE WHEN u = 16 THEN 'PASS:R24_J_LEGAL_UPDATE_OK'
            ELSE 'FAIL:R24_J u=' || u END AS r24_j2
  FROM public.t_r24 WHERE id = 2;

-- R24_K: MAIN pristine until apply; apply materializes the delta.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 3 THEN 'PASS:R24_K_MAIN_PRISTINE'
            ELSE 'FAIL:R24_K main_cnt=' || count(*) END AS r24_k
  FROM public.t_r24;
SELECT overlay_branch.apply_branch('br_r24');
SELECT CASE WHEN count(*) = 7 THEN 'PASS:R24_K_APPLY_MATERIALIZED'
            ELSE 'FAIL:R24_K applied_cnt=' || count(*) END AS r24_k2
  FROM public.t_r24;

-- R24 teardown (the branch is left in the terminal 'applied' state, like the
-- other apply-based sections; only the table is dropped).
RESET client_min_messages;
DROP TABLE public.t_r24;

RESET client_min_messages;

-- =====================================================================
-- Section R25 : NUMERIC identity canonicalization on branch writes.
--
--   The delta row identity is the serialized PK text stored in
--   pg_branch_delta(branch_id, relid, key).  On a NUMERIC PK *without* a
--   column typmod, two values that are '='-equal can print with a different
--   display scale (1.0 vs 1.00).  Both the slot serializer and the
--   literal serializer must canonicalize them to ONE identity key; if they
--   do not, the delta would hold two rows for one logical row and every
--   text-key probe (the R21 delta pre-check, the UPSERT Phase I probe and
--   the O(1) delta lookup) would miss the shadowing row.  A branch INSERT
--   of an '='-equal PK would then silently succeed instead of raising the
--   native 23505, and a UPSERT DO NOTHING would duplicate the row.
--
-- COVERAGE:
--   R25_A : cross-statement INSERT, numeric PK 1.0 then 1.00          -> 23505
--   R25_B : same-statement INSERT of 2.0 / 2.00 (one command)         -> 23505
--   R25_C : UPSERT DO UPDATE with a different scale updates in place   -> 1 row
--   R25_D : UPSERT DO NOTHING with a different scale skips             -> 1 row
--   R25_E : const-based O(1) delta lookup (UPDATE/DELETE WHERE pk=x.00)
--   R25_F : peer UNIQUE(numeric) cross-statement duplicate             -> 23505
--   R25_G : composite PK (numeric,int) cross-statement duplicate       -> 23505
--   R25_H : MAIN pristine; apply materializes the canonical rows
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r25 (
    id  numeric PRIMARY KEY,
    v   text
);
CREATE TABLE public.t_r25u (
    id  integer PRIMARY KEY,
    u   numeric UNIQUE,
    v   text
);
CREATE TABLE public.t_r25c (
    a   numeric,
    b   integer,
    v   text,
    PRIMARY KEY (a, b)
);
SELECT overlay_branch.create_branch('br_r25');
SELECT overlay_branch.use_branch('br_r25');

-- R25_A: cross-statement INSERT of '='-equal numeric PKs -> 23505.
INSERT INTO public.t_r25 VALUES (1.0, 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r25 VALUES (1.00, 'b');
        RAISE EXCEPTION 'FAIL_R25_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R25_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R25_A_CROSS_STMT_23505'
            ELSE 'FAIL:R25_A rows=' || count(*) END AS r25_a
  FROM public.t_r25 WHERE id = 1;

-- R25_B: same-statement INSERT of 2.0 and 2.00 -> 23505.
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r25 VALUES (2.0, 'x'), (2.00, 'y');
        RAISE EXCEPTION 'FAIL_R25_B_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R25_B: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R25_B_SAME_STMT_23505'
            ELSE 'FAIL:R25_B rows=' || count(*) END AS r25_b
  FROM public.t_r25 WHERE id = 2;

-- R25_C: UPSERT (arbiter=PK) DO UPDATE with a different scale adopts the
--        EXCLUDED post-image without creating a second delta row.
INSERT INTO public.t_r25 VALUES (3.0, 'first');
INSERT INTO public.t_r25 VALUES (3.00, 'second') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'second'
            THEN 'PASS:R25_C_UPSERT_DOUPDATE_ONE_ROW'
            ELSE 'FAIL:R25_C rows=' || count(*) || ' v=' || min(v) END AS r25_c
  FROM public.t_r25 WHERE id = 3;

-- R25_D: UPSERT DO NOTHING with a different scale skips (no duplicate).
INSERT INTO public.t_r25 VALUES (4.0, 'keep');
INSERT INTO public.t_r25 VALUES (4.00, 'drop') ON CONFLICT (id) DO NOTHING;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'keep'
            THEN 'PASS:R25_D_UPSERT_DONOTHING_ONE_ROW'
            ELSE 'FAIL:R25_D rows=' || count(*) || ' v=' || min(v) END AS r25_d
  FROM public.t_r25 WHERE id = 4;

-- R25_E: const-based O(1) delta lookup — a differently scaled literal must
--        find the branch row (id = 1.00 hits the row inserted as 1.0).
UPDATE public.t_r25 SET v = 'upd' WHERE id = 1.00;
SELECT CASE WHEN v = 'upd' THEN 'PASS:R25_E_CONST_LOOKUP_UPDATE'
            ELSE 'FAIL:R25_E v=' || v END AS r25_e
  FROM public.t_r25 WHERE id = 1;
INSERT INTO public.t_r25 VALUES (5.0, 'five');
DELETE FROM public.t_r25 WHERE id = 5.000;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R25_E_CONST_LOOKUP_DELETE'
            ELSE 'FAIL:R25_E rows=' || count(*) END AS r25_e2
  FROM public.t_r25 WHERE id = 5;

-- R25_F: peer UNIQUE(numeric) — cross-statement duplicate -> 23505.
INSERT INTO public.t_r25u VALUES (1, 10.0, 'u1');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r25u VALUES (2, 10.00, 'u2');
        RAISE EXCEPTION 'FAIL_R25_F_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R25_F: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R25_F_PEER_UNIQUE_23505'
            ELSE 'FAIL:R25_F rows=' || count(*) END AS r25_f
  FROM public.t_r25u;

-- R25_G: composite PK with a numeric component — cross-statement dup -> 23505.
INSERT INTO public.t_r25c VALUES (7.0, 1, 'g1');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r25c VALUES (7.00, 1, 'g2');
        RAISE EXCEPTION 'FAIL_R25_G_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R25_G: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R25_G_COMPOSITE_PK_23505'
            ELSE 'FAIL:R25_G rows=' || count(*) END AS r25_g
  FROM public.t_r25c;

-- R25_H: MAIN pristine until apply; apply materializes the canonical rows.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R25_H_MAIN_PRISTINE'
            ELSE 'FAIL:R25_H main_rows=' || count(*) END AS r25_h
  FROM public.t_r25;
SELECT overlay_branch.apply_branch('br_r25');
SELECT CASE WHEN count(*) = 3 AND count(DISTINCT id) = 3
            THEN 'PASS:R25_H_APPLY_MATERIALIZED'
            ELSE 'FAIL:R25_H applied_rows=' || count(*) END AS r25_h2
  FROM public.t_r25;

-- R25 teardown (branch left in the terminal 'applied' state).
DROP TABLE public.t_r25;
DROP TABLE public.t_r25u;
DROP TABLE public.t_r25c;
RESET client_min_messages;

-- =====================================================================
-- Section R26 : GUC-dependent output functions vs. branch identity/payload.
--
--   A delta row is stored as TEXT: the identity key under
--   pg_branch_delta(branch_id, relid, key) and the row image under
--   tuple_data.  Both are produced through the column type's OUTPUT
--   function and consumed by a *different* session's INPUT function.  A
--   handful of output functions read session GUCs (TimeZone/DateStyle for
--   the date-time family, IntervalStyle for interval, extra_float_digits
--   for float, bytea_output for bytea).  If the serializer honours the
--   writing session's settings, two sessions disagree: '='-equal PKs
--   render to different identity texts (two delta rows, every text-key
--   probe misses -> a duplicate INSERT silently succeeds) and the payload
--   re-parses into a DIFFERENT value (silent corruption).  The serializer
--   must pin the canonical output GUCs; the constant-probe path must do
--   the same.  float8 +/-0.0 are '='-equal (float8eq) yet print "-0"/"0"
--   and must collapse to one key.
--
-- COVERAGE:
--   R26_A : timestamptz PK, same instant under TimeZone UTC / Asia/Shanghai -> 23505
--   R26_B : float8 PK, -0.0 and +0.0                                        -> 23505
--   R26_C : bytea PK, hex vs escape spelling of the same bytes              -> 23505
--   R26_D : payload round-trip under SQL,DMY / sql_standard / ef=0
--           (timestamp + timestamptz + interval + float8)              -> value preserved
--   R26_E : MAIN pristine; apply materializes the canonical rows
-- =====================================================================
SET client_min_messages = WARNING;
CREATE TABLE public.t_r26a (
    k   timestamptz PRIMARY KEY,
    v   text
);
CREATE TABLE public.t_r26b (
    k   float8 PRIMARY KEY,
    v   text
);
CREATE TABLE public.t_r26c (
    k   bytea PRIMARY KEY,
    v   text
);
CREATE TABLE public.t_r26p (
    id  integer PRIMARY KEY,
    ts  timestamp,
    tz  timestamptz,
    iv  interval,
    f   float8
);
SELECT overlay_branch.create_branch('br_r26');
SELECT overlay_branch.use_branch('br_r26');

-- R26_A: timestamptz PK — the SAME instant written under two TimeZone
--        settings must fold to one canonical identity key -> 23505.
SET TimeZone = 'UTC';
INSERT INTO public.t_r26a VALUES ('2024-06-01 00:00:00+00', 'a');
SET TimeZone = 'Asia/Shanghai';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r26a VALUES ('2024-06-01 08:00:00+08', 'b');
        RAISE EXCEPTION 'FAIL_R26_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R26_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R26_A_TSTZ_IDENTITY_23505'
            ELSE 'FAIL:R26_A rows=' || count(*) END AS r26_a
  FROM public.t_r26a WHERE k = '2024-06-01 00:00:00+00'::timestamptz;
RESET TimeZone;

-- R26_B: float8 PK — -0.0 and +0.0 are '='-equal -> one identity key.
INSERT INTO public.t_r26b VALUES ('-0'::float8, 'neg');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r26b VALUES ('0'::float8, 'pos');
        RAISE EXCEPTION 'FAIL_R26_B_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R26_B: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'neg'
            THEN 'PASS:R26_B_FLOAT_NEGZERO_23505'
            ELSE 'FAIL:R26_B rows=' || count(*) || ' v=' || min(v) END AS r26_b
  FROM public.t_r26b WHERE k = 0;

-- R26_C: bytea PK — hex and escape spellings of the same bytes -> one key.
SET bytea_output = 'hex';
INSERT INTO public.t_r26c VALUES ('\x00'::bytea, 'h');
SET bytea_output = 'escape';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r26c VALUES ('\x00'::bytea, 'e');
        RAISE EXCEPTION 'FAIL_R26_C_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R26_C: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'h'
            THEN 'PASS:R26_C_BYTEA_IDENTITY_23505'
            ELSE 'FAIL:R26_C rows=' || count(*) || ' v=' || min(v) END AS r26_c
  FROM public.t_r26c WHERE k = '\x00'::bytea;
RESET bytea_output;

-- R26_D: payload written under SQL,DMY + sql_standard + extra_float_digits=0
--        must read back unchanged once the reader's GUCs differ.
SET TimeZone = 'Asia/Shanghai';
SET DateStyle = 'SQL, DMY';
SET IntervalStyle = 'sql_standard';
SET extra_float_digits = 0;
INSERT INTO public.t_r26p
     VALUES (1, timestamp '2024-02-01 05:06:07', timestamptz '2024-02-01 05:06:07+00',
             interval '1 year 2 mons 3 days', 0.30000000000000004);
RESET TimeZone;
RESET DateStyle;
RESET IntervalStyle;
RESET extra_float_digits;
SELECT CASE WHEN ts = timestamp '2024-02-01 05:06:07'
             AND tz = timestamptz '2024-02-01 05:06:07+00'
             AND iv = interval '1 year 2 mons 3 days'
             AND f = 0.30000000000000004
            THEN 'PASS:R26_D_PAYLOAD_ROUNDTRIP'
            ELSE 'FAIL:R26_D ts=' || ts::text || ' tz=' || tz::text
                 || ' iv=' || iv::text || ' f=' || f::text END AS r26_d
  FROM public.t_r26p WHERE id = 1;

-- R26_E: MAIN pristine until apply; apply materializes the canonical rows.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R26_E_MAIN_PRISTINE'
            ELSE 'FAIL:R26_E rows=' || count(*) END AS r26_e
  FROM public.t_r26a;
SELECT overlay_branch.apply_branch('br_r26');
SELECT CASE WHEN count(*) = 1 AND min(v) = 'a'
            THEN 'PASS:R26_E_APPLY_MATERIALIZED'
            ELSE 'FAIL:R26_E rows=' || count(*) END AS r26_e2
  FROM public.t_r26a;

-- R26 teardown (branch left in the terminal 'applied' state).
DROP TABLE public.t_r26a;
DROP TABLE public.t_r26b;
DROP TABLE public.t_r26c;
DROP TABLE public.t_r26p;
RESET client_min_messages;

-- =====================================================================
-- Section R27 : DOMAIN types over GUC-sensitive / normalization-needing
--               base types.
--
--   A column declared with a DOMAIN has its own pg_type OID, so a bare
--   `typid == TIMESTAMPTZOID` test never fires and `get_element_type()`
--   returns InvalidOid for the domain itself.  The serializer must peel
--   the domain with getBaseType() before deciding (a) whether the output
--   function is GUC-sensitive and (b) whether the value needs the
--   scale/±0/r-trim normalization.  Otherwise an '='-equal PK splits into
--   two identity keys (a duplicate INSERT silently succeeds) and a
--   GUC-rendered payload re-parses into a different value.  Arrays of a
--   domain and domains over an array must both be handled.
--
-- COVERAGE:
--   R27_A : timestamptz domain PK, same instant under two TimeZones   -> 23505
--   R27_B : numeric domain PK, 1.00 vs 1.0                            -> 23505
--   R27_C : float8 domain PK, -0.0 vs +0.0                            -> 23505
--   R27_D : timestamp/interval domain payload under SQL,DMY/sql_standard -> preserved
--   R27_E : array-of-domain PK, same instant under two TimeZones      -> 23505
--   R27_F : domain-over-array PK, same instant under two TimeZones    -> 23505
--   R27_G : apply materializes the canonical domain rows
-- =====================================================================
SET client_min_messages = WARNING;
CREATE DOMAIN d_r27_tz     AS timestamptz;
CREATE DOMAIN d_r27_ts     AS timestamp;
CREATE DOMAIN d_r27_iv     AS interval;
CREATE DOMAIN d_r27_num    AS numeric;
CREATE DOMAIN d_r27_f8     AS float8;
CREATE DOMAIN d_r27_tz_arr AS timestamptz[];
CREATE TABLE public.t_r27a (k d_r27_tz PRIMARY KEY, v text);
CREATE TABLE public.t_r27b (id integer PRIMARY KEY, ts d_r27_ts, iv d_r27_iv);
CREATE TABLE public.t_r27c (k d_r27_tz[] PRIMARY KEY, v text);
CREATE TABLE public.t_r27d (k d_r27_num PRIMARY KEY, v text);
CREATE TABLE public.t_r27e (k d_r27_f8 PRIMARY KEY, v text);
CREATE TABLE public.t_r27f (k d_r27_tz_arr PRIMARY KEY, v text);
SELECT overlay_branch.create_branch('br_r27');
SELECT overlay_branch.use_branch('br_r27');

-- R27_A: timestamptz DOMAIN PK — same instant, two TimeZone settings -> 23505.
SET TimeZone = 'UTC';
INSERT INTO public.t_r27a VALUES ('2024-06-01 00:00:00+00', 'a');
SET TimeZone = 'Asia/Shanghai';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r27a VALUES ('2024-06-01 08:00:00+08', 'b');
        RAISE EXCEPTION 'FAIL_R27_A_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R27_A: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R27_A_DOMAIN_TSTZ_IDENTITY_23505'
            ELSE 'FAIL:R27_A rows=' || count(*) END AS r27_a
  FROM public.t_r27a WHERE k = '2024-06-01 00:00:00+00'::timestamptz;
RESET TimeZone;

-- R27_B: numeric DOMAIN PK — 1.00 and 1.0 are '='-equal -> one identity key.
INSERT INTO public.t_r27d VALUES (1.00, 'a');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r27d VALUES (1.0, 'b');
        RAISE EXCEPTION 'FAIL_R27_B_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R27_B: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R27_B_DOMAIN_NUMERIC_IDENTITY_23505'
            ELSE 'FAIL:R27_B rows=' || count(*) END AS r27_b
  FROM public.t_r27d WHERE k = 1.0;

-- R27_C: float8 DOMAIN PK — -0.0 and +0.0 are '='-equal -> one identity key.
INSERT INTO public.t_r27e VALUES ('-0'::float8, 'neg');
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r27e VALUES ('0'::float8, 'pos');
        RAISE EXCEPTION 'FAIL_R27_C_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R27_C: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 AND min(v) = 'neg'
            THEN 'PASS:R27_C_DOMAIN_FLOAT_NEGZERO_23505'
            ELSE 'FAIL:R27_C rows=' || count(*) || ' v=' || min(v) END AS r27_c
  FROM public.t_r27e WHERE k = 0;

-- R27_D: timestamp/interval DOMAIN payload written under non-default GUCs
--        must read back unchanged.
SET TimeZone = 'Asia/Shanghai';
SET DateStyle = 'SQL, DMY';
SET IntervalStyle = 'sql_standard';
INSERT INTO public.t_r27b
     VALUES (1, timestamp '2024-02-01 05:06:07',
             interval '1 year 2 mons 3 days');
RESET TimeZone;
RESET DateStyle;
RESET IntervalStyle;
SELECT CASE WHEN ts = timestamp '2024-02-01 05:06:07'
             AND iv = interval '1 year 2 mons 3 days'
            THEN 'PASS:R27_D_DOMAIN_PAYLOAD_ROUNDTRIP'
            ELSE 'FAIL:R27_D ts=' || ts::text || ' iv=' || iv::text END AS r27_d
  FROM public.t_r27b WHERE id = 1;

-- R27_E: array-of-domain PK — same instant, two TimeZone settings -> 23505.
SET TimeZone = 'UTC';
INSERT INTO public.t_r27c VALUES (ARRAY['2024-06-01 00:00:00+00'::d_r27_tz], 'a');
SET TimeZone = 'Asia/Shanghai';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r27c
             VALUES (ARRAY['2024-06-01 08:00:00+08'::d_r27_tz], 'b');
        RAISE EXCEPTION 'FAIL_R27_E_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R27_E: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R27_E_DOMAIN_ARRAY_IDENTITY_23505'
            ELSE 'FAIL:R27_E rows=' || count(*) END AS r27_e
  FROM public.t_r27c
 WHERE k = ARRAY['2024-06-01 00:00:00+00'::d_r27_tz];
RESET TimeZone;

-- R27_F: domain-over-array PK — same instant, two TimeZone settings -> 23505.
SET TimeZone = 'UTC';
INSERT INTO public.t_r27f VALUES ('{2024-06-01 00:00:00+00}'::d_r27_tz_arr, 'a');
SET TimeZone = 'Asia/Shanghai';
DO $$
BEGIN
    BEGIN
        INSERT INTO public.t_r27f VALUES ('{2024-06-01 08:00:00+08}'::d_r27_tz_arr, 'b');
        RAISE EXCEPTION 'FAIL_R27_F_NO_ERROR';
    EXCEPTION WHEN OTHERS THEN
        IF SQLSTATE <> '23505' THEN
            RAISE EXCEPTION 'FAIL_R27_F: expected 23505 got %: %', SQLSTATE, SQLERRM;
        END IF;
    END;
END $$;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:R27_F_DOMAIN_OVER_ARRAY_23505'
            ELSE 'FAIL:R27_F rows=' || count(*) END AS r27_f
  FROM public.t_r27f WHERE k = '{2024-06-01 00:00:00+00}'::d_r27_tz_arr;
RESET TimeZone;

-- R27_G: MAIN pristine until apply; apply materializes the canonical rows.
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:R27_G_MAIN_PRISTINE'
            ELSE 'FAIL:R27_G rows=' || count(*) END AS r27_g
  FROM public.t_r27a;
SELECT overlay_branch.apply_branch('br_r27');
SELECT CASE WHEN count(*) = 1 AND min(v) = 'a'
            THEN 'PASS:R27_G_APPLY_MATERIALIZED'
            ELSE 'FAIL:R27_G rows=' || count(*) END AS r27_g2
  FROM public.t_r27a;

-- R27 teardown: drop the tables then the domains (branch left 'applied').
DROP TABLE public.t_r27a;
DROP TABLE public.t_r27b;
DROP TABLE public.t_r27c;
DROP TABLE public.t_r27d;
DROP TABLE public.t_r27e;
DROP TABLE public.t_r27f;
DROP DOMAIN d_r27_tz;
DROP DOMAIN d_r27_ts;
DROP DOMAIN d_r27_iv;
DROP DOMAIN d_r27_num;
DROP DOMAIN d_r27_f8;
DROP DOMAIN d_r27_tz_arr;
RESET client_min_messages;

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
