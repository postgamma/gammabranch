# ob_state_inval.spec — L1 pg_isolation_regress (T8)
# I8.3: FR4 kickout ONLY via pure SPI counter fallback; NOTIFY dispatch via
# step-switch LISTEN async socket is flaky under pg_isolation_regress so we
# use the _debug_invalidation_counter_throttle_override GUC (default 32, set to
# 1 in session setup).  Every throttled() call triggers heavy SPI ground-truth.
# After apply fires, the NEXT call (a DML INSERT — r3) will see state=applied
# through SPI re-check, combined with for_dml=true + ob_throttled_allow_kickout
# (write_redirect ExecutorRun opt-in) → raises HARD ERROR 55000 kickout.
# NOTE: pure DQL SELECT here would only fire soft NOTICE + MAIN fallback,
# so r3 MUST be real DML to hit the FR4 ERROR contract.
#
# I8.2: 2 permutations need 2 unique branch names: bs_inval_p1, bs_inval_p2.

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t8_inval(id int PRIMARY KEY, v text);
    INSERT INTO public.t8_inval SELECT g, 'm_'||g FROM generate_series(1, 100) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t8_inval;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_inval_p%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s_apply"
setup { SET client_min_messages = WARNING; }
step "s_apply_p1" { SELECT overlay_branch.apply_branch('bs_inval_p1'); }
step "s_apply_p2" { SELECT overlay_branch.apply_branch('bs_inval_p2'); }

# s_counter: session-level GUC override = 1.  Every throttled() call
#   immediately triggers heavy SPI ground-truth.  After apply sets the
#   catalog state to 'applied', the NEXT DML step (r3, real INSERT) runs
#   write_redirect ExecutorRun hook which:
#     (a) sets ob_throttled_allow_kickout=true (opt-in destructive side-effect)
#     (b) calls throttled(for_dml=true) → SPI detects state=applied → kickout
#     (c) raises FR4 ERROR 55000 "branch has been mutated".
#   NOTE: pure DQL SELECT r1/r2 after apply only fires soft NOTICE + MAIN
#   fallback (ob_throttled_allow_kickout stays false), so r3 MUST be real DML
#   to satisfy the FR4 HARD ERROR contract.
session "s_counter"
setup {
    SET client_min_messages = WARNING;
    SET overlay_branch._debug_invalidation_counter_throttle_override = 1;
}
step "s_counter_p1_use" { SELECT overlay_branch.use_branch('bs_inval_p1', 'live'); }
step "s_counter_p1_r1"  { SELECT count(*) AS c FROM public.t8_inval; }
step "s_counter_p1_r2"  { SELECT count(*) AS c FROM public.t8_inval; }
step "s_counter_p1_r3"  { INSERT INTO public.t8_inval VALUES (901, 'kick_p1'); }

step "s_counter_p2_use" { SELECT overlay_branch.use_branch('bs_inval_p2', 'live'); }
step "s_counter_p2_r1"  { SELECT count(*) AS c FROM public.t8_inval; }
step "s_counter_p2_r2"  { SELECT count(*) AS c FROM public.t8_inval; }
step "s_counter_p2_r3"  { INSERT INTO public.t8_inval VALUES (902, 'kick_p2'); }

session "ctrl"
setup { SET client_min_messages = WARNING; }
step "ctrl_p1_setup" {
    SELECT overlay_branch.create_branch('bs_inval_p1');
    SELECT overlay_branch.use_branch('bs_inval_p1', 'live');
    INSERT INTO public.t8_inval VALUES (101, 'd1'), (102, 'd2');
    COMMIT;
    RESET overlay_branch.current;
}
step "ctrl_p2_setup" {
    SELECT overlay_branch.create_branch('bs_inval_p2');
    SELECT overlay_branch.use_branch('bs_inval_p2', 'live');
    INSERT INTO public.t8_inval VALUES (103, 'd3'), (104, 'd4');
    COMMIT;
    RESET overlay_branch.current;
}

# P1 AC-B3: override=1 → r1 (ok, MAIN+WR rows), apply kicks in background,
#   r2 (ok SELECT: sees soft NOTICE + MAIN fallback after kickout),
#   r3 (DML INSERT → write_redirect opt-in kickout ERROR 55000 FR4).
# Expected: s_counter_p1_r3 ERROR "branch has been mutated" (FR4 ERROR 55000)
permutation "ctrl_p1_setup" "s_counter_p1_use" "s_counter_p1_r1" "s_apply_p1" "s_counter_p1_r2" "s_counter_p1_r3"

# P2 AC-B3 b: same semantics. r2 runs BEFORE apply (normal reads), apply kicks
#   after r2, then r3 (DML INSERT) triggers hard ERROR kickout.  Exercises
#   unique branch id reuse across permutations.
permutation "ctrl_p2_setup" "s_counter_p2_use" "s_counter_p2_r1" "s_counter_p2_r2" "s_apply_p2" "s_counter_p2_r3"
