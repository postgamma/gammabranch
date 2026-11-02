-- apply_contention_fixture.sql — one-time setup for T8 L2 pgbench race detection
-- Run once BEFORE pgbench (idempotent: IF NOT EXISTS everywhere).
-- Comments in English per project rule (doc .md may contain Chinese).

-- 1. Load extension; do not drop if already present
CREATE EXTENSION IF NOT EXISTS overlay_branch;

-- 2. MAIN target table (mirrors pgbench_accounts shape with explicit PK)
--    Row count = 10000 baseline.  Post-run verification checks exact count.
CREATE TABLE IF NOT EXISTS public.pgbench_accounts (
    aid     int         NOT NULL,
    bid     int         NOT NULL,
    abalance int        NOT NULL DEFAULT 0,
    filler  char(84)    NOT NULL DEFAULT '',
    PRIMARY KEY (aid)
);

-- 3. Seed baseline rows if empty (idempotent seed — only on cold start)
INSERT INTO public.pgbench_accounts (aid, bid, abalance, filler)
    SELECT  g.aid, 1, 0, repeat('x', 84)
    FROM    generate_series(1, 10000) AS g(aid)
    WHERE   NOT EXISTS (SELECT 1 FROM public.pgbench_accounts WHERE aid = 1);

-- 4. Permutation branches: 8 branches named bs_pgbench_{1..8}.
--    Each pgbench client with (client_id % 8)+1 targets its own branch,
--    so apply/discard contention is concentrated across the 8 slots.
DO $$
DECLARE
    i   int;
    bn  text;
BEGIN
    FOR i IN 1..8 LOOP
        bn := 'bs_pgbench_' || i;
        IF NOT EXISTS (
            SELECT 1 FROM overlay_branch.pg_branch WHERE branch_name = bn
        ) THEN
            PERFORM overlay_branch.create_branch(bn);
        END IF;
    END LOOP;
END $$;
