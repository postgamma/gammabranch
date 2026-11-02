# ob_snapshot_mode.spec — L1 pg_isolation_regress (T8)
# I8.5 FR5 SNAPSHOT FREEZE contract:
#   - MAIN snapshot frozen at use_branch('snapshot') time.
#   - New WR delta rows ALWAYS visible to snapshot reader (append-only monotonic).
#   - MAIN fresh writes by a non-branch session NOT visible until mode switch live.
#
# 100 MAIN rows; 2 WR delta (101,102) set up.
# Exact counts per I8.5 contract (validated 2026-09-17 dry run):
#   C1 (snap first read)           = 100 MAIN + 2 WR delta           = 102
#   C2 (after MAIN 999 insert)     = MAIN frozen (still 100) + 2 delta = 102  ← freeze
#   C3 (after WR delta id=103)     = 100 MAIN + 3 delta (new delta seen) = 103  ← new delta visible
#   C4 (switch live)               = 101 MAIN (999+100) + 3 delta    = 104

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t8_snap(id int PRIMARY KEY, v text);
    INSERT INTO public.t8_snap SELECT g, 'm_'||g FROM generate_series(1, 100) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t8_snap;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name = 'bs_snap'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "ctrl"
setup { SET client_min_messages = WARNING; }
step "ctrl_setup" {
    SELECT overlay_branch.create_branch('bs_snap');
    SELECT overlay_branch.use_branch('bs_snap', 'live');
    INSERT INTO public.t8_snap VALUES (101, 'd1'), (102, 'd2');
    COMMIT;
    RESET overlay_branch.current;
}

session "s1"
setup { SET client_min_messages = WARNING; }
step "s1_snap_use" { SELECT overlay_branch.use_branch('bs_snap', 'snapshot'); }
step "s1_r1"       { SELECT count(*) AS c1 FROM public.t8_snap; }
step "s1_r2"       { SELECT count(*) AS c2 FROM public.t8_snap; }
step "s1_r3"       { SELECT count(*) AS c3 FROM public.t8_snap; }
step "s1_live_use" { SELECT overlay_branch.use_branch('bs_snap', 'live'); }
step "s1_r_live"   { SELECT count(*) AS c4 FROM public.t8_snap; }

session "s2"
setup { SET client_min_messages = WARNING; }
step "s2_insert_main" {
    RESET overlay_branch.current;
    INSERT INTO public.t8_snap VALUES (999, 'main_new_after_snap');
}

session "s3"
setup { SET client_min_messages = WARNING; }
step "s3_insert_delta" {
    SELECT overlay_branch.use_branch('bs_snap', 'live');
    INSERT INTO public.t8_snap VALUES (103, 'delta_new_visible');
    COMMIT;
    RESET overlay_branch.current;
}

# Expected exact counts: C1=102, C2=102, C3=103, C4=104
permutation "ctrl_setup" "s1_snap_use" "s1_r1" "s2_insert_main" "s1_r2" "s3_insert_delta" "s1_r3" "s1_live_use" "s1_r_live"
