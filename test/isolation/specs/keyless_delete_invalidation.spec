# keyless_delete_invalidation.spec — L1 pg_isolation_regress
# keyless(ctid) — anchor invalidation by a concurrent DELETE (internals.md §4.5):
#   * s1 anchors a MAIN row from inside a branch (its delta key becomes
#     "A:<blk>:<off>", the physical ctid of d2)
#   * s2 (a different session, not in any branch) concurrently DELETEs that
#     same MAIN row
#   * read side: a deleted row has no t_ctid successor to chase, so the
#     anchored delta is silently dropped — the branch shows MAIN minus the
#     deleted row, and never revives the gone row (no misread)
#   * apply side: the anchor locates no MAIN row -> 0A000, the branch stays
#     active and the independently-committed DELETE survives

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.kl_del(v text);
    INSERT INTO public.kl_del VALUES ('d1'), ('d2'), ('d3');
}

teardown
{
    RESET overlay_branch.current;
    SET overlay_branch.keyless_tables = off;
    DROP TABLE IF EXISTS public.kl_del;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_kl_del%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; SET overlay_branch.keyless_tables = on; }
step "s1_enter"   { SELECT overlay_branch.create_branch('bs_kl_del_1'); SELECT overlay_branch.use_branch('bs_kl_del_1'); }
step "s1_anchor"  { UPDATE public.kl_del SET v = 'd2-branch' WHERE v = 'd2'; }
step "s1_exit"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; }
step "s1_reenter" { SELECT overlay_branch.use_branch('bs_kl_del_1'); }
step "s1_read"    { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'd1,d3' THEN 'PASS:kl_del_delta_dropped' ELSE 'FAIL:kl_del_delta_dropped:'||string_agg(v, ',' ORDER BY v) END AS s1_read FROM public.kl_del; }
step "s1_apply"   {
    DO $$
    BEGIN
        PERFORM overlay_branch.apply_branch('bs_kl_del_1');
        RAISE WARNING 'FAIL:kl_del_apply_not_blocked';
    EXCEPTION
        WHEN SQLSTATE '0A000' THEN RAISE WARNING 'PASS:kl_del_apply_conflict_0A000';
        WHEN OTHERS           THEN RAISE WARNING 'FAIL:kl_del_apply_other_sqlstate=%', SQLSTATE;
    END $$;
}
step "s1_state"   { SELECT state AS s1_state FROM overlay_branch.pg_branch WHERE branch_name = 'bs_kl_del_1'; }
step "s1_exit2"   { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; }
step "s1_main"    { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'd1,d3' THEN 'PASS:kl_del_main_preserved' ELSE 'FAIL:kl_del_main_preserved:'||string_agg(v, ',' ORDER BY v) END AS s1_main FROM public.kl_del; }
step "s1_discard" { SELECT overlay_branch.discard_branch('bs_kl_del_1'); }

session "s2"
setup { SET client_min_messages = WARNING; }
step "s2_delete" { DELETE FROM public.kl_del WHERE v = 'd2'; }
step "s2_main"   { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'd1,d3' THEN 'PASS:kl_del_s2_main' ELSE 'FAIL:kl_del_s2_main:'||string_agg(v, ',' ORDER BY v) END AS s2_main FROM public.kl_del; }

permutation
    "s1_enter"
    "s1_anchor"
    "s1_exit"
    "s2_delete"
    "s2_main"
    "s1_reenter"
    "s1_read"
    "s1_apply"
    "s1_state"
    "s1_exit2"
    "s1_main"
    "s1_discard"
