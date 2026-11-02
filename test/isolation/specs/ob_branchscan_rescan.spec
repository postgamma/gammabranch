# ob_branchscan_rescan.spec — L1 pg_isolation_regress (T8)
# FR6 cross-session fresh rescan (the inner-rescan path shares the same
# iterator re-open as ordinary exec: ReScan drops the OverlayScanState and
# the next ExecCustomScan re-opens it against the latest delta).
# 2 MAIN rows + WR delta 3 → read1=3.  s2 inserts WR delta id=4 (committed) →
# s1 read2=4 (fresh overlay under s2's new delta now visible).

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.t8_res(id int PRIMARY KEY, v text);
    INSERT INTO public.t8_res VALUES (1, 'm1'), (2, 'm2');
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.t8_res;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name = 'bs_rescan'
                   AND state IN ('active','applied','applying','discarded') LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "ctrl"
setup { SET client_min_messages = WARNING; }
step "ctrl_setup" {
    SELECT overlay_branch.create_branch('bs_rescan');
    SELECT overlay_branch.use_branch('bs_rescan', 'live');
    INSERT INTO public.t8_res VALUES (3, 'd1');
    COMMIT;
    RESET overlay_branch.current;
}

session "s1"
setup { SET client_min_messages = WARNING; }
step "s1_use"       { SELECT overlay_branch.use_branch('bs_rescan', 'live'); }
step "s1_read_iter"  { SELECT count(*) AS read1 FROM public.t8_res; }
step "s1_read_iter2" { SELECT count(*) AS read2 FROM public.t8_res; }

session "s2"
setup { SET client_min_messages = WARNING; }
step "s2_insert_delta" {
    SELECT overlay_branch.use_branch('bs_rescan', 'live');
    INSERT INTO public.t8_res VALUES (4, 'd2_written');
    COMMIT;
    RESET overlay_branch.current;
}

permutation "ctrl_setup" "s1_use" "s1_read_iter" "s2_insert_delta" "s1_read_iter2"
