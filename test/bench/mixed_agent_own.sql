-- mixed_agent_own.sql -- per-client INDEPENDENT-branch Agent workload for the
-- multi-agent concurrency-scaling probe (test/bench/run_agent_scale.sh).
--
-- Unlike mixed_agent.sql -- where every client shares ONE branch -- each
-- pgbench client here enters its OWN branch, so M concurrent Agents exercise
-- CROSS-branch interference (pg_branch registry rows, per-branch advisory
-- locks, the GUC mode cache, state-change NOTIFY) rather than intra-branch
-- delta write contention.
--
-- The driver pre-creates one branch per client named  <brpfx><client_id>
-- and passes the shared prefix with:
--     pgbench ... -D brpfx=<prefix>  -f mixed_agent_own.sql
--   NOTE: run with -M prepared, so :brpfx is sent as a bind parameter and must
--   NOT be wrapped in SQL quotes.  pgbench's builtin :client_id is 0-based.
--
-- The query shape is IDENTICAL to mixed_agent.sql (one overlay read + one
-- delta write per transaction) so the two probes remain comparable.

\set id random(1, 50000)

BEGIN;
SELECT overlay_branch.use_branch(:brpfx || :client_id, 'live');
SELECT count(*) FROM public.mi_agent WHERE id <= :id;
UPDATE public.mi_agent SET v = 'u' || :id WHERE id = :id;
COMMIT;
