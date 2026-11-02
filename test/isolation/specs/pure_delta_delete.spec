# pure_delta_delete.spec — L1 pg_isolation_regress
# B2 Pure Delta DELETE MAIN-miss — 3 permutations
#
# P1 I→D apply: s1 pure-insert id=55 → DELETE id=55 → apply.
#    Final MAIN: id=55 absent (tombstone on empty MAIN baseline has no row to
#    remove; apply must silently swallow — §D.13 pure-D semantics).
#
# P2 I→U→D discard: s2 pure-insert id=66 → UPDATE v=2 → DELETE id=66 → discard.
#    Final MAIN: id=66 absent (delta-store folded to tombstone; discard reclaims
#    everything — no MAIN pollution either way).
#
# P3 Parallel MAIN baseline DEL + pure DEL: s3 deletes MAIN-seed id=4 (v=40 on
#    baseline) WHILE s4 deletes unrelated pure row id=77.  Both branches apply;
#    final MAIN: id=4 absent (baseline tombstone applied → row removed), id=77
#    absent (pure tombstone on empty baseline → no-op at apply time).

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t10_del(id int PRIMARY KEY, v int, t text DEFAULT 'main');
    INSERT INTO public.t10_del SELECT g, g*10, 'main' FROM generate_series(1,5) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t10_del;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_del_%' LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = ERROR; }
step "s1_enter_p1" { SELECT overlay_branch.create_branch('bs_del_p1'); SELECT overlay_branch.use_branch('bs_del_p1'); }
step "s1_pure_ins" { INSERT INTO public.t10_del VALUES (55, 5, 'pure'); }
step "s1_del_55"    { DELETE FROM public.t10_del WHERE id = 55; }
step "s1_apply"     { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_del_p1'); }
step "s1_check"     { SELECT count(*) AS c55 FROM public.t10_del WHERE id = 55; }

session "s2"
setup { SET client_min_messages = ERROR; }
step "s2_enter_p2" { SELECT overlay_branch.create_branch('bs_del_p2'); SELECT overlay_branch.use_branch('bs_del_p2'); }
step "s2_pure_ins" { INSERT INTO public.t10_del VALUES (66, 1, 'pure'); }
step "s2_upd_2"    { UPDATE public.t10_del SET v = 2 WHERE id = 66; }
step "s2_del_66"   { DELETE FROM public.t10_del WHERE id = 66; }
step "s2_discard"  { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.discard_branch('bs_del_p2'); }
step "s2_check"    { SELECT count(*) AS c66 FROM public.t10_del WHERE id = 66; }

session "s3"
setup { SET client_min_messages = ERROR; }
step "s3_enter_p3" { SELECT overlay_branch.create_branch('bs_del_p3'); SELECT overlay_branch.use_branch('bs_del_p3'); }
step "s3_del_base" { DELETE FROM public.t10_del WHERE id = 4; }
step "s3_apply"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_del_p3'); }
step "s3_check"    { SELECT count(*) AS c4 FROM public.t10_del WHERE id = 4; }

session "s4"
setup { SET client_min_messages = ERROR; }
step "s4_enter_p3" { SELECT overlay_branch.create_branch('bs_del_p3b'); SELECT overlay_branch.use_branch('bs_del_p3b'); }
step "s4_pure_ins" { INSERT INTO public.t10_del VALUES (77, 7, 'pure'); }
step "s4_del_pure" { DELETE FROM public.t10_del WHERE id = 77; }
step "s4_apply"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_del_p3b'); }
step "s4_check"    { SELECT count(*) AS c77 FROM public.t10_del WHERE id = 77; }

permutation "s1_enter_p1" "s1_pure_ins" "s1_del_55" "s1_apply" "s1_check"
permutation "s2_enter_p2" "s2_pure_ins" "s2_upd_2" "s2_del_66" "s2_discard" "s2_check"
permutation
    "s3_enter_p3"  "s4_enter_p3"
    "s4_pure_ins"  "s3_del_base"  "s4_del_pure"
    "s3_apply"     "s4_apply"
    "s3_check"     "s4_check"
