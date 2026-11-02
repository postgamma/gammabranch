-- mixed_main_oltp.sql -- MAIN foreground OLTP workload for the G05/S28
-- mixed-impact bench.  Pure heap-table OLTP: one indexed UPDATE + one
-- indexed SELECT per transaction, no overlay_branch involvement at all.
--
-- Run with:  pgbench -n -c <N> -j <J> -T <T> -M prepared -f mixed_main_oltp.sql

\set aid random(1, 100000)

BEGIN;
UPDATE public.mi_accounts SET abalance = abalance + 1 WHERE aid = :aid;
SELECT abalance FROM public.mi_accounts WHERE aid = :aid;
COMMIT;
