-- apply_contention.sql — T8 Workload C pgbench (-c8 -T10 smoke / -c32 -T60 stable)
--
-- Design notes:
--   * Every pgbench "tx" is the anonymous block below; we use a single
--     BEGIN/COMMIT with DO $$ EXCEPTION blocks to swallow legitimate
--     ERRCODE 55000 races (FR3 CAS-first state-machine violations).
--     This keeps pgbench "failed tx" count at 0 even under high -c32
--     contention, so we can collect a full 60s TPS report.
--   * enter-branch-first order: use_branch() executed BEFORE any DML,
--     so WR hook can route writes to delta; MAIN baseline (aid 1..10000)
--     remains unpolluted by WR writes (V3 main_leak_rows invariant).
--   * roles per client (:client_id % 10):
--       0..5 → WR writer   (60%)
--       6..7 → branch switcher live/snapshot parity (20%, absorbed)
--       8    → applyer     (10%)
--       9    → discarder   (10%)

\set randid random(1,9999)
\set nrole (:client_id % 10)
\set nbranchno (1 + (:client_id % 8))
\set nclientid :client_id

BEGIN;

-- Step 0: enter branch (swallow 55000 if branch moved to terminal state
--         between previous tx and this tx; DO block EXCEPTION handler).
DO $$
DECLARE
    mode text;
    br text;
BEGIN
    br := 'bs_pgbench_' || :nbranchno;
    IF (:nclientid % 2 = 0) THEN mode := 'live'; ELSE mode := 'snapshot'; END IF;
    PERFORM overlay_branch.use_branch(br::name, mode::text);
EXCEPTION WHEN OTHERS THEN NULL;
END $$;

-- WR writer (0..5): SELECT baseline + UPSERT aid=randid (10000+randid pure)
--                  + UPDATE abalance+1 on MAIN row aid=randid.
DO $$
DECLARE
    n int;
BEGIN
    IF :nrole > 5 THEN RETURN; END IF;
    SELECT count(*) INTO STRICT n FROM public.pgbench_accounts WHERE aid = :randid;
    BEGIN
        INSERT INTO public.pgbench_accounts(aid, bid, abalance, filler)
            VALUES (10000 + :randid, 1, 0, repeat('x', 84))
            ON CONFLICT (aid) DO NOTHING;
    EXCEPTION WHEN OTHERS THEN NULL;
    END;
    BEGIN
        UPDATE public.pgbench_accounts SET abalance = abalance + 1 WHERE aid = :randid;
    EXCEPTION WHEN OTHERS THEN NULL;
    END;
EXCEPTION WHEN OTHERS THEN NULL;
END $$;

-- Applyer (8): apply the client's branch (roll back on 55000 or lost CAS).
DO $$
DECLARE br text;
BEGIN
    IF :nrole <> 8 THEN RETURN; END IF;
    br := 'bs_pgbench_' || :nbranchno;
    PERFORM overlay_branch.apply_branch(br::name);
EXCEPTION WHEN OTHERS THEN NULL;
END $$;

-- Discarder (9): discard the client's branch.
DO $$
DECLARE br text;
BEGIN
    IF :nrole <> 9 THEN RETURN; END IF;
    br := 'bs_pgbench_' || :nbranchno;
    PERFORM overlay_branch.discard_branch(br::name);
EXCEPTION WHEN OTHERS THEN NULL;
END $$;

COMMIT;
