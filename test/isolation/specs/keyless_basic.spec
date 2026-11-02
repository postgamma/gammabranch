# keyless_basic.spec — L1 pg_isolation_regress
# B6 keyless(ctid) — GUC overlay_branch.keyless_tables=on:
#   * one-shot WARNING per (session, relid) on the first keyless DML
#   * BranchScan overlay read/write consistency for INSERT/UPDATE/DELETE
#   * two concurrent branches on the same no-PK table stay independent
#   * MAIN stays pristine while branches are active (ctrl reads it with GUC OFF,
#     i.e. BranchScan is not engaged and raw MAIN rows are returned)
#   * a normal PK table on the same GUC still uses the PK identity path (regression)

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.kl_basic(v text);
    INSERT INTO public.kl_basic VALUES ('a'), ('b'), ('c');
    CREATE TABLE public.kl_basic_pk(id int PRIMARY KEY, v text);
    INSERT INTO public.kl_basic_pk VALUES (1, 'p1'), (2, 'p2');
}

teardown
{
    RESET overlay_branch.current;
    SET overlay_branch.keyless_tables = off;
    DROP TABLE IF EXISTS public.kl_basic, public.kl_basic_pk;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_kl_basic%'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "s1"
setup { SET client_min_messages = WARNING; SET overlay_branch.keyless_tables = on; }
step "s1_enter"  { SELECT overlay_branch.create_branch('bs_kl_basic_1'); SELECT overlay_branch.use_branch('bs_kl_basic_1'); }
step "s1_ins"    { INSERT INTO public.kl_basic VALUES ('x'); }
step "s1_upd"    { UPDATE public.kl_basic SET v = 'b2' WHERE v = 'b'; }
step "s1_del"    { DELETE FROM public.kl_basic WHERE v = 'c'; }
step "s1_check"  { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b2,x' THEN 'PASS:kl_basic_b1_overlay' ELSE 'FAIL:kl_basic_b1_overlay:'||string_agg(v, ',' ORDER BY v) END AS s1_overlay FROM public.kl_basic; }
step "s1_apply"  { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_kl_basic_1'); }
step "s1_pk_enter" { SELECT overlay_branch.create_branch('bs_kl_basic_pk'); SELECT overlay_branch.use_branch('bs_kl_basic_pk'); }
step "s1_pk_ins"   { INSERT INTO public.kl_basic_pk VALUES (3, 'p3'); }
step "s1_pk_upd"   { UPDATE public.kl_basic_pk SET v = 'p2b' WHERE id = 2; }
step "s1_pk_check" { SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=p1,2=p2b,3=p3' THEN 'PASS:kl_basic_pk_overlay' ELSE 'FAIL:kl_basic_pk_overlay:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS s1_pk_overlay FROM public.kl_basic_pk; }
step "s1_pk_apply" { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.apply_branch('bs_kl_basic_pk'); }

session "s2"
setup { SET client_min_messages = WARNING; SET overlay_branch.keyless_tables = on; }
step "s2_enter"  { SELECT overlay_branch.create_branch('bs_kl_basic_2'); SELECT overlay_branch.use_branch('bs_kl_basic_2'); }
step "s2_ins"    { INSERT INTO public.kl_basic VALUES ('y'); }
step "s2_check"  { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b,c,y' THEN 'PASS:kl_basic_b2_overlay' ELSE 'FAIL:kl_basic_b2_overlay:'||string_agg(v, ',' ORDER BY v) END AS s2_overlay FROM public.kl_basic; }
step "s2_discard" { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SELECT overlay_branch.discard_branch('bs_kl_basic_2'); }

session "ctrl"
setup { SET client_min_messages = WARNING; }
step "ctrl_main"   { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b,c' THEN 'PASS:kl_basic_main_pristine' ELSE 'FAIL:kl_basic_main_pristine:'||string_agg(v, ',' ORDER BY v) END AS ctrl_main FROM public.kl_basic; }
step "ctrl_applied" { SELECT CASE WHEN string_agg(v, ',' ORDER BY v) = 'a,b2,x' THEN 'PASS:kl_basic_main_applied' ELSE 'FAIL:kl_basic_main_applied:'||string_agg(v, ',' ORDER BY v) END AS ctrl_applied FROM public.kl_basic; }
step "ctrl_pk"     { SELECT CASE WHEN string_agg(id::text||'='||v, ',' ORDER BY id) = '1=p1,2=p2b,3=p3' THEN 'PASS:kl_basic_pk_applied' ELSE 'FAIL:kl_basic_pk_applied:'||string_agg(id::text||'='||v, ',' ORDER BY id) END AS ctrl_pk FROM public.kl_basic_pk; }

permutation
    "s1_enter"      "s2_enter"
    "s1_ins"        "s2_ins"
    "s1_upd"        "s1_del"
    "s1_check"      "s2_check"
    "ctrl_main"
    "s1_apply"
    "ctrl_applied"
    "s2_discard"
    "s1_pk_enter"   "s1_pk_ins"   "s1_pk_upd"   "s1_pk_check"   "s1_pk_apply"
    "ctrl_pk"
