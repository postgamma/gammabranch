# ob_nl_rescan_inner.spec — L1 pg_isolation_regress (D-2 A2.3)
# SAME-CSS ExecReScanCustomScan deterministic proof:
#   Nested Loop with 3-row outer driver + LATERAL (SELECT count(*)
#   FROM branch_table NO WHERE) forces ExecReScan(inner CustomScan)
#   per outer tuple.  If ob_branchscan_rescan does not properly
#   reset cursor/list_slots/materialized flags, 2nd or 3rd inner
#   scan returns 0 → inner_cnt <> 14 → deterministic FAIL.
#
# Overlay row count (3 MAIN inserts per grp, then branch: DEL 1 row,
# INS 5 new, then total = 14) → every rescan must yield inner_cnt=14.

setup
{
    CREATE SCHEMA IF NOT EXISTS overlay_branch;
    CREATE EXTENSION IF NOT EXISTS overlay_branch WITH SCHEMA overlay_branch;
    CREATE TABLE public.nl_drv(g int PRIMARY KEY);
    INSERT INTO public.nl_drv VALUES (1),(2),(3);
    CREATE TABLE public.nl_tbl(id int PRIMARY KEY, grp int NOT NULL, v text);
    INSERT INTO public.nl_tbl
      SELECT g, ((g-1)%5)+1, 'm_'||g FROM generate_series(1,10) AS g;
}

teardown
{
    RESET overlay_branch.current;
    DROP TABLE IF EXISTS public.nl_drv;
    DROP TABLE IF EXISTS public.nl_tbl;
    DO $$ DECLARE b text;
    BEGIN
        FOR b IN SELECT branch_name FROM overlay_branch.pg_branch
                 WHERE branch_name = 'bs_nl' LOOP
            BEGIN PERFORM overlay_branch.discard_branch(b); EXCEPTION WHEN OTHERS THEN NULL; END;
        END LOOP;
    END $$;
}

session "ctrl"
setup { SET client_min_messages = WARNING; }
step "ctrl_setup" {
    SELECT overlay_branch.create_branch('bs_nl');
    SELECT overlay_branch.use_branch('bs_nl', 'live');
    -- Overlay composition: 10 MAIN - 1 DEL (id=3) + 5 INS (ids 11..15) = 14
    INSERT INTO public.nl_tbl VALUES
      (11, 1, 'd11'), (12, 2, 'd12'), (13, 3, 'd13'),
      (14, 4, 'd14'), (15, 5, 'd15');
    DELETE FROM public.nl_tbl WHERE id = 3;
    UPDATE public.nl_tbl SET v = 'u_'||id WHERE id IN (1,2);
    COMMIT;
    RESET overlay_branch.current;
}

session "s1"
setup {
    SET client_min_messages = WARNING;
    SET enable_hashjoin  = off;
    SET enable_mergejoin = off;
    SET enable_nestloop  = on;
}
step "s1_use"     { SELECT overlay_branch.use_branch('bs_nl', 'live'); }
step "s1_nl_cnts" {
    SELECT o.g, x.inner_cnt
      FROM public.nl_drv o,
           LATERAL (SELECT count(*) AS inner_cnt FROM public.nl_tbl) x
     ORDER BY o.g;
}
step "s1_exit"    { SELECT overlay_branch.use_branch(NULL); }

# Expected result s1_nl_cnts:  (1, 14), (2, 14), (3, 14)
# Inner count stays 14 for ALL 3 outer tuples → rescan 4-step contract
# (invalidated-check + drop slots + list_free + cursor reset) honoured.
permutation "ctrl_setup" "s1_use" "s1_nl_cnts" "s1_exit"
