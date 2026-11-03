# keyless_version_chain.spec — L1 pg_isolation_regress
# B2 keyless(ctid) — read-side version-chain resolution on a no-PK table:
#   * s1 anchors a MAIN row from inside a branch (its delta key becomes
#     "A:<blk>:<off>", the physical ctid of m1)
#   * s2 (a different session, not in any branch) concurrently rewrites that
#     same MAIN row; the write creates a NEW tuple version at a different ctid
#     and leaves a dangling t_ctid link at the old one
#   * s1 re-enters the branch and READS the overlay: the anchor is chased down
#     the version chain to the row's current ctid, so the branch edit still
#     shows over the moved row instead of being silently dropped
#   * read-only guarantee: apply still locates no MAIN row at the stale anchor
#     and raises 0A000 (conflict), preserving the independently-committed write

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.kl_move(v text);
    INSERT INTO public.kl_move VALUES ('m1'), ('m2');
}

teardown
{
    RESET overlay_branch.current;
    SET overlay_branch.keyless_tables = off;
    DROP TABLE IF EXISTS public.kl_move;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_kl_move%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; SET overlay_branch.keyless_tables = on; }
step "s1_enter"   { SELECT overlay_branch.create_branch('bs_kl_move_1'); SELECT overlay_branch.use_branch('bs_kl_move_1'); }
step "s1_anchor"  { UPDATE public.kl_move SET v = 'm1-branch' WHERE v = 'm1'; }
step "s1_exit"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; }
step "s1_reenter" { SELECT overlay_branch.use_branch('bs_kl_move_1'); }
step "s1_read"    { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'm1-branch,m2' THEN 'PASS:kl_move_reanchored' ELSE 'FAIL:kl_move_reanchored:'||string_agg(v, ',' ORDER BY v) END AS s1_read FROM public.kl_move; }
step "s1_apply"   {
    DO $$
    BEGIN
        PERFORM overlay_branch.apply_branch('bs_kl_move_1');
        RAISE WARNING 'FAIL:kl_move_apply_not_blocked';
    EXCEPTION
        WHEN SQLSTATE '0A000' THEN RAISE WARNING 'PASS:kl_move_apply_conflict_0A000';
        WHEN OTHERS           THEN RAISE WARNING 'FAIL:kl_move_apply_other_sqlstate=%', SQLSTATE;
    END $$;
}
step "s1_main"    { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'M1-MAIN,m2' THEN 'PASS:kl_move_main_preserved' ELSE 'FAIL:kl_move_main_preserved:'||string_agg(v, ',' ORDER BY v) END AS s1_main FROM public.kl_move; }
step "s1_discard" { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.discard_branch('bs_kl_move_1'); }

session "s2"
setup { SET client_min_messages = WARNING; }
step "s2_move" { UPDATE public.kl_move SET v = 'M1-MAIN' WHERE v = 'm1'; }

permutation
    "s1_enter"      "s1_anchor"     "s1_exit"
    "s2_move"
    "s1_reenter"    "s1_read"
    "s1_apply"      "s1_main"       "s1_discard"
