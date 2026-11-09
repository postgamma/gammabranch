# ob_applying_freeze.spec — L1 pg_isolation_regress (T8)
# FR2/FR3: a delta write racing an in-flight apply must be frozen.
# I8.4: apply is split into two steps so s2 can probe WHILE apply is mid-flight —
# s1_apply_cas runs BEGIN + SET LOCAL sleep=2 + apply and does NOT commit; the
# sleep widens the window after s1 has taken the branch ExclusiveLock.  s2's
# INSERT then blocks on the delta writer's ShareLock (s2_delta shows
# `<waiting ...>`).  When s1 COMMITs (state 'applied'), s2's state re-check reads
# the fresh committed state and rejects the write with ERROR 55000 — s2's row is
# neither merged into MAIN nor left as an orphan delta row.
# Assertions: MAIN stays at exactly 110 rows AND the branch delta is empty.

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t8_apply(id int PRIMARY KEY, v text);
    INSERT INTO public.t8_apply SELECT g, 'm_'||g FROM generate_series(1, 100) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t8_apply;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name = 'bs_freeze'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; }
step "s1_ctrl_setup" {
    SELECT overlay_branch.create_branch('bs_freeze');
    SELECT overlay_branch.use_branch('bs_freeze', 'live');
    INSERT INTO public.t8_apply VALUES (101, 'd1'), (102, 'd2'), (103, 'd3'),
       (104, 'd4'), (105, 'd5'), (106, 'd6'), (107, 'd7'), (108, 'd8'),
       (109, 'd9'), (110, 'd10');
    COMMIT;
    RESET overlay_branch.current;
}
step "s1_apply_cas" {
    BEGIN;
    SET LOCAL overlay_branch._debug_apply_sleep_sec = 2;
    SELECT overlay_branch.apply_branch('bs_freeze');
}
step "s1_commit" { COMMIT; }

session "s2"
setup { SET client_min_messages = WARNING; }
step "s2_delta" {
    SELECT overlay_branch.use_branch('bs_freeze', 'live');
    INSERT INTO public.t8_apply VALUES (200, 'WR_by_s2_should_freeze');
}

session "s3"
setup { SET client_min_messages = WARNING; }
step "s3_verify" { SELECT count(*) AS main_rows FROM public.t8_apply; }
step "s3_delta"  {
    SELECT count(*) AS delta_rows
      FROM overlay_branch.pg_branch_delta d
      JOIN overlay_branch.pg_branch b USING (branch_id)
     WHERE b.branch_name = 'bs_freeze';
}

# Critical ordering: s1_apply_cas (sleeps 2s inside apply) then immediately s2_delta.
# s2_delta blocks on the writer ShareLock until s1 COMMITs, then its write to the
# now-'applied' branch is rejected; MAIN stays at 110 and the delta stays empty.
permutation "s1_ctrl_setup" "s1_apply_cas" "s2_delta" "s1_commit" "s3_verify" "s3_delta"
