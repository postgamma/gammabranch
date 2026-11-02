# ob_apply_mutex.spec — L1 pg_isolation_regress (T8)
# Covers:
#   P1(AC-A1): dual-apply mutex CAS; loser → kick ERROR "not active (state=applying)"
#   P2(AC-A3): reverse order apply; no deadlock (FR2 V1: no shared→exclusive upgrade)
# I8.2: 2 permutations → 2 unique branch names (setup runs only once per spec).

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t8_mutex(id int PRIMARY KEY, v text);
    INSERT INTO public.t8_mutex SELECT g, 'm_'||g FROM generate_series(1, 1000) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t8_mutex;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_mutex_p%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; }
step "s1_p1_begin"  { BEGIN; }
step "s1_p1_use"    { SELECT overlay_branch.use_branch('bs_mutex_p1', 'live'); }
step "s1_p1_apply"  { SELECT overlay_branch.apply_branch('bs_mutex_p1'); }
step "s1_p1_commit" { COMMIT; }
step "s1_p2_begin"  { BEGIN; }
step "s1_p2_use"    { SELECT overlay_branch.use_branch('bs_mutex_p2', 'live'); }
step "s1_p2_apply"  { SELECT overlay_branch.apply_branch('bs_mutex_p2'); }
step "s1_p2_commit" { COMMIT; }

session "s2"
setup { SET client_min_messages = WARNING; }
step "s2_p1_begin"  { BEGIN; }
step "s2_p1_use"    { SELECT overlay_branch.use_branch('bs_mutex_p1', 'live'); }
step "s2_p1_apply"  { SELECT overlay_branch.apply_branch('bs_mutex_p1'); }
step "s2_p1_commit" { COMMIT; }
step "s2_p2_begin"  { BEGIN; }
step "s2_p2_use"    { SELECT overlay_branch.use_branch('bs_mutex_p2', 'live'); }
step "s2_p2_apply"  { SELECT overlay_branch.apply_branch('bs_mutex_p2'); }
step "s2_p2_commit" { COMMIT; }

session "ctrl"
setup { SET client_min_messages = WARNING; }
step "ctrl_p1_setup" {
    SELECT overlay_branch.create_branch('bs_mutex_p1');
    SELECT overlay_branch.use_branch('bs_mutex_p1', 'live');
    INSERT INTO public.t8_mutex VALUES (1001, 'd1'), (1002, 'd2');
    COMMIT;
    RESET overlay_branch.current;
}
step "ctrl_p2_setup" {
    SELECT overlay_branch.create_branch('bs_mutex_p2');
    SELECT overlay_branch.use_branch('bs_mutex_p2', 'live');
    INSERT INTO public.t8_mutex VALUES (1003, 'd3'), (1004, 'd4');
    COMMIT;
    RESET overlay_branch.current;
}
step "ctrl_p1_state" { SELECT state FROM overlay_branch.pg_branch WHERE branch_name='bs_mutex_p1'; }
step "ctrl_p2_state" { SELECT state FROM overlay_branch.pg_branch WHERE branch_name='bs_mutex_p2'; }

# P1 (AC-A1 dual-apply mutex): both begin+use; s1 apply CAS wins; s2 apply CAS loses.
# s2 → ERROR "branch bs_mutex_p1 is not active (state=applying)".
# After s1 commits: ctrl_p1_state = applied.  NO deadlock in permutation anywhere.
permutation "ctrl_p1_setup" "s1_p1_begin" "s1_p1_use" "s2_p1_begin" "s2_p1_use" "s1_p1_apply" "s2_p1_apply" "s1_p1_commit" "s2_p1_commit" "ctrl_p1_state"

# P2 (AC-A3 reverse order): s1 locks and applies first BEFORE s2 ever use+apply.
# FR2 V1 protocol guarantees NO shared→exclusive upgrade → no deadlock possible.
permutation "ctrl_p2_setup" "s1_p2_begin" "s1_p2_use" "s1_p2_apply" "s2_p2_begin" "s2_p2_use" "s2_p2_apply" "s1_p2_commit" "s2_p2_commit" "ctrl_p2_state"
