-- mixed_agent.sql -- Agent branch workload for the G05/S28 mixed-impact
-- bench.  Each transaction enters the branch, performs one overlay read
-- (BranchScan over MAIN + delta) and one delta write.
--
-- The branch name is supplied by the driver:  -D brname=<name>
--   NOTE: the driver runs this script with -M prepared, so :brname is sent as
--   a bind parameter and must NOT be wrapped in SQL quotes.
-- Run with:  pgbench -n -c <M> -j 1 -T <T> -M prepared -D brname=... \
--                     -f mixed_agent.sql

\set id random(1, 50000)

BEGIN;
SELECT overlay_branch.use_branch(:brname, 'live');
SELECT count(*) FROM public.mi_agent WHERE id <= :id;
UPDATE public.mi_agent SET v = 'u' || :id WHERE id = :id;
COMMIT;
