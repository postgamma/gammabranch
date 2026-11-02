-- CONTROL workload for run_mixed_impact.sh:  identical query shape and load to
-- mixed_agent.sql, but WITHOUT entering any branch (no use_branch call), so the
-- reads/writes go straight to MAIN.  Run it via:
--   PGAGENTSQL=test/bench/mixed_agent_nobranch.sql PGTAG=nobranch \
--       ./test/bench/run_mixed_impact.sh 100 30 1
-- If the MAIN delta of this control matches the branch run, the observed
-- "impact" is caused by the extra concurrent client / second pgbench process,
-- NOT by the Agent overlay path.  The -D brname=... argument passed by the
-- driver is simply unused here.
\set id random(1, 50000)
BEGIN;
SELECT count(*) FROM public.mi_agent WHERE id <= :id;
UPDATE public.mi_agent SET v = 'u' || :id WHERE id = :id;
COMMIT;
