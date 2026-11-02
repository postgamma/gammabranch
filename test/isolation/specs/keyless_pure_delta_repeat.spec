# keyless_pure_delta_repeat.spec — L1 pg_isolation_regress
# B6 keyless(ctid) — pure-delta (branch-only) row identity stability:
#   * INSERT 'p1' into a no-PK table inside a branch gets a synthetic "S:<seq>" key
#   * repeated UPDATEs of that row must keep the SAME synthetic key (key
#     propagation) -> the overlay never duplicates the row
#   * a DELETE of the synthetic row then removes it (no MAIN anchor, so it is
#     simply dropped from the branch delta)
#   * a concurrent second branch owns its own synthetic namespace; the two
#     branches never see each other's delta
#   * MAIN stays pristine until apply; apply materializes exactly the surviving
#     post-image (the deleted synthetic row never lands)

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.kl_pure(v text);
    INSERT INTO public.kl_pure VALUES ('m1'), ('m2');
}

teardown
{
    RESET overlay_branch.current;
    SET overlay_branch.keyless_tables = off;
    DROP TABLE IF EXISTS public.kl_pure;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_kl_pure%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; SET overlay_branch.keyless_tables = on; }
step "s1_enter"  { SELECT overlay_branch.create_branch('bs_kl_pure_1'); SELECT overlay_branch.use_branch('bs_kl_pure_1'); }
step "s1_ins"    { INSERT INTO public.kl_pure VALUES ('p1'); }
step "s1_upd1"   { UPDATE public.kl_pure SET v = 'p1a' WHERE v = 'p1'; }
step "s1_upd2"   { UPDATE public.kl_pure SET v = 'p1b' WHERE v = 'p1a'; }
step "s1_upd3"   { UPDATE public.kl_pure SET v = 'p1c' WHERE v = 'p1b'; }
step "s1_nodup"  { SELECT CASE WHEN count(*) = 3 AND string_agg(v, ',' ORDER BY v) = 'm1,m2,p1c' THEN 'PASS:kl_pure_no_dup' ELSE 'FAIL:kl_pure_no_dup:count='||count(*)||':'||string_agg(v, ',' ORDER BY v) END AS s1_nodup FROM public.kl_pure; }
step "s1_del"    { DELETE FROM public.kl_pure WHERE v = 'p1c'; }
step "s1_gone"   { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'm1,m2' THEN 'PASS:kl_pure_synth_gone' ELSE 'FAIL:kl_pure_synth_gone:'||string_agg(v, ',' ORDER BY v) END AS s1_gone FROM public.kl_pure; }
step "s1_apply"  { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_kl_pure_1'); }

session "s2"
setup { SET client_min_messages = WARNING; SET overlay_branch.keyless_tables = on; }
step "s2_enter"  { SELECT overlay_branch.create_branch('bs_kl_pure_2'); SELECT overlay_branch.use_branch('bs_kl_pure_2'); }
step "s2_ins"    { INSERT INTO public.kl_pure VALUES ('q1'); }
step "s2_upd"    { UPDATE public.kl_pure SET v = 'q1a' WHERE v = 'q1'; }
step "s2_check"  { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'm1,m2,q1a' THEN 'PASS:kl_pure_b2_isolated' ELSE 'FAIL:kl_pure_b2_isolated:'||string_agg(v, ',' ORDER BY v) END AS s2_check FROM public.kl_pure; }
step "s2_discard" { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.discard_branch('bs_kl_pure_2'); }

session "ctrl"
setup { SET client_min_messages = WARNING; }
step "ctrl_main"  { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'm1,m2' THEN 'PASS:kl_pure_main_pristine' ELSE 'FAIL:kl_pure_main_pristine:'||string_agg(v, ',' ORDER BY v) END AS ctrl_main FROM public.kl_pure; }
step "ctrl_final" { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'm1,m2' THEN 'PASS:kl_pure_main_final' ELSE 'FAIL:kl_pure_main_final:'||string_agg(v, ',' ORDER BY v) END AS ctrl_final FROM public.kl_pure; }

permutation
    "s1_enter"      "s2_enter"
    "s1_ins"        "s2_ins"
    "s1_upd1"       "s2_upd"
    "s1_upd2"       "s1_upd3"
    "s1_nodup"      "s2_check"
    "ctrl_main"
    "s1_del"        "s1_gone"
    "s1_apply"
    "s2_discard"
    "ctrl_final"
