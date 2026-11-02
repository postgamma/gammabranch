# ob_applying_freeze.spec — L1 pg_isolation_regress (T8)
# FR3 applying-transient-state delta-write freeze.
# I8.4: apply split into TWO steps — s1_apply_cas (BEGIN+SET LOCAL sleep=2+apply; NO COMMIT)
# then s2_delta steps in WHILE apply is still mid-flight (pg_sleep for 2s inside
# apply after CAS to applying).  s2_delta tries use+INSERT.  Two valid outcomes:
#   (a) FR3 state='applying' seen by delta gate → ERROR "state is applying, cannot write"
#   (b) FR2 Exclusive advisory blocks (s2 step shows `<waiting ...>`); after s1 COMMITs
#       → state='applied' → ERROR "branch not active"  (also correct; no MAIN leak).
# In both cases MAIN final rows must be exactly 110 (s2's row never merged).

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
    SELECT 1 AS s2_ok;
}

session "s3"
setup { SET client_min_messages = WARNING; }
step "s3_verify" { SELECT count(*) AS main_rows FROM public.t8_apply; }

# Critical ordering: s1_apply_cas (sleeps 2s inside apply) then immediately s2_delta.
# permutation engine will dispatch s1 then immediately next-step s2 without
# waiting s1 (unless it sees heavyweight lock), but even if FR2 Exclusive blocks
# s2_delta is safe: outcome (b) still OK.
permutation "s1_ctrl_setup" "s1_apply_cas" "s2_delta" "s1_commit" "s3_verify"
