-- mixed_impact_fixture.sql -- one-time setup for the G05/S28 mixed
-- foreground-impact bench (test/bench/run_mixed_impact.sh).  Idempotent:
-- safe to re-run before every measurement.
--
-- Two disjoint tables on purpose:
--   public.mi_accounts  -- MAIN OLTP target (the "foreground" business table)
--   public.mi_agent     -- Agent branch read/write target
--
-- They never share rows, so any MAIN throughput/latency regression observed
-- with the Agent attached is attributable to instance-level pressure
-- (CPU / IO / buffer / locks), not to row contention.  The Agent writes land
-- in the branch delta only; MAIN rows are left untouched until APPLY, which
-- keeps repeated runs reproducible.
--
-- Comments in English per project rule (only .md files may contain Chinese).

CREATE EXTENSION IF NOT EXISTS overlay_branch;

-- 1. MAIN foreground table ------------------------------------------------
CREATE TABLE IF NOT EXISTS public.mi_accounts (
    aid      int NOT NULL PRIMARY KEY,
    abalance int NOT NULL DEFAULT 0
);

INSERT INTO public.mi_accounts (aid, abalance)
    SELECT g, 0 FROM generate_series(1, 100000) AS g
    WHERE NOT EXISTS (SELECT 1 FROM public.mi_accounts WHERE aid = 1);

-- 2. Agent table ----------------------------------------------------------
CREATE TABLE IF NOT EXISTS public.mi_agent (
    id  int  NOT NULL PRIMARY KEY,
    v   text NOT NULL DEFAULT ''
);

INSERT INTO public.mi_agent (id, v)
    SELECT g, 'm_' || g FROM generate_series(1, 50000) AS g
    WHERE NOT EXISTS (SELECT 1 FROM public.mi_agent WHERE id = 1);

-- 3. Baseline sanity (printed so the transcript is self-describing) -------
SELECT 'mi_accounts' AS tbl, count(*) AS rows FROM public.mi_accounts
UNION ALL
SELECT 'mi_agent', count(*) FROM public.mi_agent
ORDER BY 1;
