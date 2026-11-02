# pure_delta_update.spec — L1 pg_isolation_regress
# B2 Pure Delta UPDATE MAIN-miss — 3 permutations

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t9_upd(id int PRIMARY KEY, v int, t text DEFAULT 'main');
    INSERT INTO public.t9_upd SELECT g, g*10, 'main' FROM generate_series(1,5) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t9_upd;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_upd_%' LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = ERROR; }
step "s1_enter_p1" { SELECT overlay_branch.create_branch('bs_upd_p1'); SELECT overlay_branch.use_branch('bs_upd_p1'); }
step "s1_pure_ins" { INSERT INTO public.t9_upd VALUES (11, 1, 'pure'); }
step "s1_upd_101"  { UPDATE public.t9_upd SET v = 101, t = 'u1' WHERE id = 11; }
step "s1_apply"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_upd_p1'); }
step "s1_check"    { SELECT v AS v11, t AS t11 FROM public.t9_upd WHERE id = 11; }

session "s2"
setup { SET client_min_messages = ERROR; }
step "s2_enter_p2" { SELECT overlay_branch.create_branch('bs_upd_p2'); SELECT overlay_branch.use_branch('bs_upd_p2'); }
step "s2_pure_ins" { INSERT INTO public.t9_upd VALUES (22, 2, 'pure'); }
step "s2_upd_22"   { UPDATE public.t9_upd SET v = 22, t = 'u1' WHERE id = 22; }
step "s2_upd_222"  { UPDATE public.t9_upd SET v = 222, t = 'u2' WHERE id = 22; }
step "s2_discard"  { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.discard_branch('bs_upd_p2'); }
step "s2_check"    { SELECT count(*) AS c22 FROM public.t9_upd WHERE id = 22; }

session "s3"
setup { SET client_min_messages = ERROR; }
step "s3_enter_p3" { SELECT overlay_branch.create_branch('bs_upd_p3'); SELECT overlay_branch.use_branch('bs_upd_p3'); }
step "s3_upd_base" { UPDATE public.t9_upd SET v = v + 100 WHERE id = 3; }
step "s3_apply"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_upd_p3'); }
step "s3_check"    { SELECT v AS v3 FROM public.t9_upd WHERE id = 3; }

session "s4"
setup { SET client_min_messages = ERROR; }
step "s4_enter_p3" { SELECT overlay_branch.create_branch('bs_upd_p3b'); SELECT overlay_branch.use_branch('bs_upd_p3b'); }
step "s4_pure_ins" { INSERT INTO public.t9_upd VALUES (44, 4, 'pure'); }
step "s4_upd_pure" { UPDATE public.t9_upd SET v = 444, t = 'u-pure' WHERE id = 44; }
step "s4_apply"    { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_upd_p3b'); }
step "s4_check"    { SELECT v AS v44, t AS t44 FROM public.t9_upd WHERE id = 44; }

permutation "s1_enter_p1" "s1_pure_ins" "s1_upd_101" "s1_apply" "s1_check"
permutation "s2_enter_p2" "s2_pure_ins" "s2_upd_22" "s2_upd_222" "s2_discard" "s2_check"
permutation
    "s3_enter_p3"  "s4_enter_p3"
    "s4_pure_ins"  "s3_upd_base"  "s4_upd_pure"
    "s3_apply"     "s4_apply"
    "s3_check"     "s4_check"
