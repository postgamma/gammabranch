# keyless_apply_conflict.spec — L1 pg_isolation_regress
# B6 keyless(ctid) — cross-session apply conflict on a no-PK table:
#   * s1 anchors a MAIN row from inside a branch (its delta key becomes
#     "A:<blk>:<off>", the physical ctid of r1)
#   * s2 (a different session, not in any branch) concurrently rewrites that
#     same MAIN row; the write creates a NEW tuple version at a different ctid
#   * s1 re-enters the branch and applies -> the anchor no longer locates a
#     MAIN row, so apply raises 0A000 (conflict) and the branch stays 'active'
#   * the MAIN write committed independently by s2 is preserved
#   * a second, non-conflicting branch then applies cleanly (positive contrast)

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.kl_race(v text);
    INSERT INTO public.kl_race VALUES ('r1'), ('r2'), ('r3');
}

teardown
{
    RESET overlay_branch.current;
    SET overlay_branch.keyless_tables = off;
    DROP TABLE IF EXISTS public.kl_race;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_kl_race%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; SET overlay_branch.keyless_tables = on; }
step "s1_enter"   { SELECT overlay_branch.create_branch('bs_kl_race_1'); SELECT overlay_branch.use_branch('bs_kl_race_1'); }
step "s1_anchor"  { UPDATE public.kl_race SET v = 'r1-branch' WHERE v = 'r1'; }
step "s1_exit"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; }
step "s1_reenter" { SELECT overlay_branch.use_branch('bs_kl_race_1'); }
step "s1_apply"   {
    DO $$
    BEGIN
        PERFORM overlay_branch.apply_branch('bs_kl_race_1');
        RAISE WARNING 'FAIL:kl_race_apply_not_blocked';
    EXCEPTION
        WHEN SQLSTATE '0A000' THEN RAISE WARNING 'PASS:kl_race_apply_conflict_0A000';
        WHEN OTHERS           THEN RAISE WARNING 'FAIL:kl_race_apply_other_sqlstate=%', SQLSTATE;
    END $$;
}
step "s1_state"   { SELECT state AS s1_state FROM overlay_branch.pg_branch WHERE branch_name = 'bs_kl_race_1'; }
step "s1_main"    { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'RACE-MAIN,r2,r3' THEN 'PASS:kl_race_main_preserved' ELSE 'FAIL:kl_race_main_preserved:'||string_agg(v, ',' ORDER BY v) END AS s1_main FROM public.kl_race; }
step "s1_discard" { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.discard_branch('bs_kl_race_1'); }
step "s1_b2_enter" { SELECT overlay_branch.create_branch('bs_kl_race_2'); SELECT overlay_branch.use_branch('bs_kl_race_2'); }
step "s1_b2_upd"   { UPDATE public.kl_race SET v = 'r2-branch' WHERE v = 'r2'; }
step "s1_b2_apply" { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_kl_race_2'); }
step "s1_b2_main"  { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'RACE-MAIN,r2-branch,r3' THEN 'PASS:kl_race_ok_apply' ELSE 'FAIL:kl_race_ok_apply:'||string_agg(v, ',' ORDER BY v) END AS s1_b2_main FROM public.kl_race; }

session "s2"
setup { SET client_min_messages = WARNING; }
step "s2_race"  { UPDATE public.kl_race SET v = 'RACE-MAIN' WHERE v = 'r1'; }
step "s2_main"  { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'RACE-MAIN,r2,r3' THEN 'PASS:kl_race_s2_main' ELSE 'FAIL:kl_race_s2_main:'||string_agg(v, ',' ORDER BY v) END AS s2_main FROM public.kl_race; }

permutation
    "s1_enter"      "s1_anchor"     "s1_exit"
    "s2_race"       "s2_main"
    "s1_reenter"    "s1_apply"      "s1_state"      "s1_main"
    "s1_discard"
    "s1_b2_enter"   "s1_b2_upd"     "s1_b2_apply"   "s1_b2_main"
