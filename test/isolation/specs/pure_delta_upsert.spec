# pure_delta_upsert.spec — L1 pg_isolation_regress
# B2 Pure Delta UPSERT (ON CONFLICT) — 2 permutations
#
# P1 MAIN-conflict + apply: session s1 creates br_p1, runs DO UPDATE UPSERT
#    on MAIN-seed id=2 (promo=2), applies branch; parallel session s3 writes
#    an unrelated pure branch (no data overlap).  Final MAIN check: id=2 v=205
#    materialized on baseline.
#
# P2 pure-conflict + discard: session s2 creates br_p2, pure-inserts id=11 v=1,
#    then runs DO UPDATE UPSERT id=11→v=101 (promo=3), discards branch.
#    Raw MAIN check: id=11 absent (discard correctly rolled back pure delta).

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t8_up(id int PRIMARY KEY, v int, t text default 'main');
    INSERT INTO public.t8_up SELECT g, g*10, 'main' FROM generate_series(1,5) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t8_up;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name LIKE 'bs_up_p%' OR branch_name = 'bs_up_parallel' LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

# s1 MAIN-conflict UPSERT + apply: MAIN id=2 v=20 → DO UPDATE v=205 → apply.
session "s1"
setup { SET client_min_messages = ERROR; }
step "s1_use_p1"    { SELECT overlay_branch.create_branch('bs_up_p1'); SELECT overlay_branch.use_branch('bs_up_p1'); }
step "s1_upsert_a"  { INSERT INTO public.t8_up VALUES (2, 205, 'excluded') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v RETURNING id,v,t; }
step "s1_apply"     { SELECT overlay_branch.apply_branch('bs_up_p1'); }
step "s1_check"     { SELECT v AS v2 FROM public.t8_up WHERE id = 2; }

# s2 pure-conflict UPSERT + discard: pure seed id=11 v=1 → DO UPDATE v=101 → discard.
session "s2"
setup { SET client_min_messages = ERROR; }
step "s2_use_p2"    { SELECT overlay_branch.create_branch('bs_up_p2'); SELECT overlay_branch.use_branch('bs_up_p2'); }
step "s2_pure_seed" { INSERT INTO public.t8_up VALUES (11, 1, 'pure'); }
step "s2_upsert_b"  { INSERT INTO public.t8_up VALUES (11, 101, 'excluded') ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v RETURNING id,v,t; }
step "s2_discard"   { SELECT overlay_branch.use_branch(NULL); RESET overlay_branch.current; SET overlay_branch.current = ''; SELECT overlay_branch.discard_branch('bs_up_p2'); }
step "s2_check"     { SELECT count(*) AS c FROM public.t8_up WHERE id = 11; }

# s3 parallel: no-overlap pure branch (independent write, no PK conflict with s1).
session "s3"
setup { SET client_min_messages = ERROR; }
step "s3_parallel"  {
    SELECT overlay_branch.create_branch('bs_up_parallel');
    BEGIN;
    SELECT overlay_branch.use_branch('bs_up_parallel');
    INSERT INTO public.t8_up VALUES (55, 555, 'pure');
    COMMIT;
    RESET overlay_branch.current;
}

# P1 MAIN-conflict apply: upsert id=2 DO UPDATE hits baseline → apply → id=2 v=205 persisted on MAIN.
permutation "s1_use_p1" "s3_parallel" "s1_upsert_a" "s1_apply" "s1_check"

# P2 pure-conflict discard: upsert id=11 DO UPDATE hits pure row → discard → MAIN id=11 absent.
permutation "s2_use_p2" "s2_pure_seed" "s2_upsert_b" "s2_discard" "s2_check"
