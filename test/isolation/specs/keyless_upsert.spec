# keyless_upsert.spec — L1 pg_isolation_regress
# keyless(ctid) — UPSERT arbitration across sessions on a no-PK table:
#   * s1 enters a branch and runs INSERT ... ON CONFLICT DO UPDATE over a
#     UNIQUE arbiter that hits a MAIN row: the delta is re-anchored to that
#     row's physical ctid with its version token (promotion = MAIN hit)
#   * s2 (a different session, outside any branch) concurrently rewrites that
#     same row, moving it to a new ctid
#   * read side: the stale anchor is chased down the version chain, so the
#     branch still shows its UPSERTed image over the moved row; the arbiter
#     hold is not duplicated (one overlay row per key)
#   * apply side: the stale anchor no longer locates a MAIN row -> 0A000, the
#     branch stays active and the independently-committed MAIN write survives
#   * a contrast branch covers the other two promotions: a delta-only hit
#     (UPSERT of a branch-born row keeps its synthetic key) and a no-hit
#     INSERT, both applying cleanly to MAIN

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.kl_ups(id int UNIQUE, v text);
    INSERT INTO public.kl_ups VALUES (1, 'a'), (2, 'b');
}

teardown
{
    RESET overlay_branch.current;
    SET overlay_branch.keyless_tables = off;
    DROP TABLE IF EXISTS public.kl_ups;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_kl_ups%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; SET overlay_branch.keyless_tables = on; }
step "s1_enter"   { SELECT overlay_branch.create_branch('bs_kl_ups_1'); SELECT overlay_branch.use_branch('bs_kl_ups_1'); }
step "s1_ups"     { INSERT INTO public.kl_ups VALUES (2, 'b-branch') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v; }
step "s1_dup"     { INSERT INTO public.kl_ups VALUES (2, 'b-branch2') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v; }
step "s1_check"   { SELECT CASE WHEN count(*) = 2 AND string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b-branch2' THEN 'PASS:kl_ups_overlay' ELSE 'FAIL:kl_ups_overlay:'||count(*)||':'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS s1_check FROM public.kl_ups; }
step "s1_exit"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; }
step "s1_reenter" { SELECT overlay_branch.use_branch('bs_kl_ups_1'); }
step "s1_read"    { SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=b-branch2' THEN 'PASS:kl_ups_reanchored' ELSE 'FAIL:kl_ups_reanchored:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS s1_read FROM public.kl_ups; }
step "s1_apply"   {
    DO $$
    BEGIN
        PERFORM overlay_branch.apply_branch('bs_kl_ups_1');
        RAISE WARNING 'FAIL:kl_ups_apply_not_blocked';
    EXCEPTION
        WHEN SQLSTATE '0A000' THEN RAISE WARNING 'PASS:kl_ups_apply_conflict_0A000';
        WHEN OTHERS           THEN RAISE WARNING 'FAIL:kl_ups_apply_other_sqlstate=%', SQLSTATE;
    END $$;
}
step "s1_state"   { SELECT state AS s1_state FROM overlay_branch.pg_branch WHERE branch_name = 'bs_kl_ups_1'; }
step "s1_exit2"   { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; }
step "s1_main"    { SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=B-MAIN' THEN 'PASS:kl_ups_main_preserved' ELSE 'FAIL:kl_ups_main_preserved:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS s1_main FROM public.kl_ups; }
step "s1_discard" { SELECT overlay_branch.discard_branch('bs_kl_ups_1'); }
step "s1_b2_enter" { SELECT overlay_branch.create_branch('bs_kl_ups_2'); SELECT overlay_branch.use_branch('bs_kl_ups_2'); }
step "s1_b2_born"  { INSERT INTO public.kl_ups VALUES (5, 'e'); }
step "s1_b2_ups"   { INSERT INTO public.kl_ups VALUES (5, 'e2') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v; }
step "s1_b2_new"   { INSERT INTO public.kl_ups VALUES (3, 'c') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v; }
step "s1_b2_check" { SELECT CASE WHEN count(*) = 4 AND string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=B-MAIN,3=c,5=e2' THEN 'PASS:kl_ups_b2_overlay' ELSE 'FAIL:kl_ups_b2_overlay:'||count(*)||':'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS s1_b2_check FROM public.kl_ups; }
step "s1_b2_apply" { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_kl_ups_2'); }
step "s1_b2_main"  { SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=B-MAIN,3=c,5=e2' THEN 'PASS:kl_ups_b2_applied' ELSE 'FAIL:kl_ups_b2_applied:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS s1_b2_main FROM public.kl_ups; }

session "s2"
setup { SET client_min_messages = WARNING; }
step "s2_move" { UPDATE public.kl_ups SET v = 'B-MAIN' WHERE id = 2; }
step "s2_main" { SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=a,2=B-MAIN' THEN 'PASS:kl_ups_s2_main' ELSE 'FAIL:kl_ups_s2_main:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS s2_main FROM public.kl_ups; }

permutation
    "s1_enter"
    "s1_ups"
    "s1_dup"
    "s1_check"
    "s1_exit"
    "s2_move"
    "s2_main"
    "s1_reenter"
    "s1_read"
    "s1_apply"
    "s1_state"
    "s1_exit2"
    "s1_main"
    "s1_discard"
    "s1_b2_enter"
    "s1_b2_born"
    "s1_b2_ups"
    "s1_b2_new"
    "s1_b2_check"
    "s1_b2_apply"
    "s1_b2_main"
