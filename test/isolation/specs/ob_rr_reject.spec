# ob_rr_reject.spec — L1 pg_isolation_regress
# Branch operations require READ COMMITTED (doc/internals.md §3.1).  Under a
# transaction snapshot (REPEATABLE READ / SERIALIZABLE) the cross-session
# freeze cannot observe a concurrent apply/discard commit, so both entering a
# branch and writing to an active branch must fail fast with 0A000 instead of
# silently appending an orphan delta row.
#   P1: use_branch() inside REPEATABLE READ            -> 0A000
#   P2: READ COMMITTED delta write succeeds, then the
#       same branch write inside REPEATABLE READ       -> 0A000
# Assertions: the RC write lands in the delta; the RR writes raise
# feature_not_supported and leave no orphan delta row.

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t_rr(id int PRIMARY KEY, v text);
    INSERT INTO public.t_rr SELECT g, 'm_'||g FROM generate_series(1, 100) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t_rr;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_rr%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; }
step "s1_p1_create"    { SELECT overlay_branch.create_branch('bs_rr_use') > 0 AS created; }
step "s1_p1_rr_begin"  { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step "s1_p1_rr_use"    { SELECT overlay_branch.use_branch('bs_rr_use', 'live'); }
step "s1_p1_rollback"  { ROLLBACK; }

step "s1_p2_enter"     { SELECT overlay_branch.create_branch('bs_rr_write') > 0 AS created; SELECT overlay_branch.use_branch('bs_rr_write', 'live'); }
step "s1_p2_rc_write"  { INSERT INTO public.t_rr VALUES (1001, 'rc_ok'); }
step "s1_p2_rr_begin"  { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step "s1_p2_rr_write"  { INSERT INTO public.t_rr VALUES (1002, 'rr_should_fail'); }
step "s1_p2_rollback"  { ROLLBACK; }

session "ctrl"
setup { SET client_min_messages = WARNING; }
step "ctrl_p2_delta" {
    SELECT count(*) AS delta_rows
      FROM overlay_branch.pg_branch_delta d
      JOIN overlay_branch.pg_branch b USING (branch_id)
     WHERE b.branch_name = 'bs_rr_write';
}
step "ctrl_p2_main" { SELECT count(*) AS main_rows FROM public.t_rr; }

# P1: RR transaction -> use_branch() rejected before any branch state installs.
permutation "s1_p1_create" "s1_p1_rr_begin" "s1_p1_rr_use" "s1_p1_rollback"

# P2: enter the branch in READ COMMITTED (write lands in the delta), then
# switch to REPEATABLE READ and write again -> rejected, no orphan row.
permutation "s1_p2_enter" "s1_p2_rc_write" "s1_p2_rr_begin" "s1_p2_rr_write" "s1_p2_rollback" "ctrl_p2_delta" "ctrl_p2_main"
