-- =====================================================================
-- overlay_branch_user.sql — User-facing "happy path" regression test.
--
-- Mirror of doc/example_sql.md §0~§4, using the actual V1 SQL functions
-- (SELECT overlay_branch.create_branch/use_branch/discard_branch/apply_branch
--  instead of the future CREATE BRANCH / USE BRANCH / APPLY BRANCH / DISCARD
--  BRANCH grammar sugar that does not exist yet).
--
-- Scope: ONE clean happy-path end-to-end story.  Engineering-level
-- assertions (guard coverage, delta-physics edge-cases, idempotency,
-- conflict ERRORs) live in overlay_branch_basic.sql.  This file is the
-- test that a new-reader user will glance at and say "oh, that is how I
-- actually use this extension".
-- =====================================================================

-- ===== 0. Prerequisite guard (same as overlay_branch_basic.sql) =====
DO $$
DECLARE
  _s text := current_setting('shared_preload_libraries', true);
  _msg text;
BEGIN
  IF _s NOT LIKE '%overlay_branch%' THEN
    _msg := 'Postmaster started WITHOUT shared_preload_libraries=''overlay_branch''.  '
         || 'The 3 correct ways to invoke these tests (DON''T `cd test/` first):'
         || chr(10)||chr(10)||'  (A) make check                    <- temp-instance, hooks auto-loaded'
         || chr(10)||'  (B) make installcheck EXTRA_REGRESS_OPTS="--use-existing --host=/tmp --port=15433"'
         || chr(10)||'  (C) pg_regress overlay_branch_user overlay_branch_basic \'
         || chr(10)||'         --inputdir=./test --outputdir=./test_output \'
         || chr(10)||'         --temp-config=$(pwd)/test/temp_instance_shared_libs.conf \'
         || chr(10)||'         --bindir=$(pg_config --bindir)'
         || chr(10)||chr(10)
         || 'If you just ran `cd test && pg_regress overlay_branch_user` then this is exactly'
         || chr(10)
         || 'the bug: the temp-instance postmaster boots with an EMPTY shared_preload_libraries,'
         || chr(10)
         || 'so overlay_branch hooks never install.';
    RAISE EXCEPTION '%', _msg;
  END IF;
END $$;

-- ===== Idempotent cleanup =====
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.products CASCADE;
DROP EXTENSION IF EXISTS overlay_branch CASCADE;
RESET client_min_messages;

-- ===== §0. Install extension + create a PK table =====
CREATE EXTENSION overlay_branch;

CREATE TABLE public.products (
    id    integer PRIMARY KEY,
    name  text    NOT NULL,
    price integer NOT NULL
);

INSERT INTO public.products VALUES
  (1, 'Apple',  10),
  (2, 'Banana',  5),
  (3, 'Cherry', 20);

-- A quick sanity peek at Main before we branch.
SELECT * FROM public.products ORDER BY id;

-- ===== §1. Branch management =====

-- 1.1 Create a workspace branch.
SELECT overlay_branch.create_branch('agent_workspace');

-- 1.2 List branches immediately after creation (delta_count should be 0).
SELECT branch_name, state FROM public.pg_branch WHERE branch_name='agent_workspace';

-- 1.3 In Main, current_branch() returns NULL (empty string in C impl means no branch).
SELECT overlay_branch.current_branch();

-- 1.4 Switch INTO the branch (session-level).
SELECT overlay_branch.use_branch('agent_workspace');

-- 1.5 Now current_branch() == 'agent_workspace'.
SELECT overlay_branch.current_branch();

-- ===== §2. Write in the branch (write-redirect auto-applies) =====

-- 2.1 INSERT (id=4).
INSERT INTO public.products VALUES (4, 'Date', 30);

-- 2.2 UPDATE id=1 price 10 → 100.
UPDATE public.products SET price = 100 WHERE id = 1;

-- 2.3 DELETE id=2.
DELETE FROM public.products WHERE id = 2;

-- ===== §3. Read in the branch (Main ⊕ Delta via SRF overlay_main_plus_delta) =====
-- Note: V1 does not yet inject a CustomScan / BranchScan node for SeqScan
-- transparently; to see the combined view users call this SRF.

SELECT * FROM overlay_branch.overlay_main_plus_delta('public.products')
  AS x(id integer, name text, price integer)
ORDER BY id;

-- ===== §3. MAIN ZERO-POLLUTION CHECK (THE INVARIANT) =====
-- Step back to Main and verify products is still the original 3 rows
-- (Apple@10 / Banana@5 / Cherry@20) — NO changes leaked through.

RESET overlay_branch.current;   -- equivalent to future USE BRANCH NONE

SELECT * FROM public.products ORDER BY id;

SELECT CASE WHEN string_agg(id::text||'='||name||'@'||price, ',' ORDER BY id)
                 = '1=Apple@10,2=Banana@5,3=Cherry@20'
            THEN 'PASS:USER_HAPPY_PATH_MAIN_UNCHANGED_AFTER_BRANCH_DML'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||'='||name||'@'||price, ',' ORDER BY id),'NULL')
       END AS s0_main_immutable
FROM public.products;

-- ===== §4. APPLY BRANCH — commit deltas back into Main atomically =====

-- Re-enter the workspace (we just stepped out to verify Main clean).
SELECT overlay_branch.use_branch('agent_workspace');

-- (Optional sanity: check that overlay still shows id=1→100, id=2 gone, id=4 new.)
SELECT string_agg(id::text||'='||name||'@'||price, ',' ORDER BY id) AS before_apply_overlay
FROM overlay_branch.overlay_main_plus_delta('public.products')
  AS x(id integer, name text, price integer);

-- The actual apply call — Main should now transform into the overlay view.
SELECT overlay_branch.apply_branch('agent_workspace');

-- 4.1 Main now contains the post-apply 3 rows: id=1 Apple@100, id=3 Cherry@20, id=4 Date@30
RESET overlay_branch.current;
SELECT * FROM public.products ORDER BY id;

SELECT CASE WHEN string_agg(id::text||'='||name||'@'||price, ',' ORDER BY id)
                 = '1=Apple@100,3=Cherry@20,4=Date@30'
            THEN 'PASS:USER_HAPPY_PATH_APPLY_PRECISE_MAIN_MUTATION'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||'='||name||'@'||price, ',' ORDER BY id),'NULL')
       END AS s4_apply_ok
FROM public.products;

-- 4.2 Branch state is 'applied' (can no longer be re-applied or discarded).
SELECT state FROM public.pg_branch WHERE branch_name='agent_workspace';

-- =====================================================================
-- ===== Part 5: BranchScan MVP (SELECT * FROM table automatically
-- ===== returns MAIN⊕branch-delta — no more SRF boilerplate).
-- =====================================================================

-- 5.0 Teardown so we can repurpose the sandbox: drop existing extension
--     (drops products table too), then reinstall from scratch with a
--     tiny BranchScan-only fixture table.
DROP EXTENSION overlay_branch CASCADE;
CREATE EXTENSION overlay_branch;

CREATE TABLE public.bs_user (id INT4 PRIMARY KEY, name TEXT, color TEXT);
INSERT INTO public.bs_user VALUES
  (10, 'apple',  'red'),
  (20, 'banana', 'yellow'),
  (30, 'cherry', 'red');

-- 5.1 MAIN mode: BranchScan MUST NOT inject (IndexScan/SeqScan win normally).
RESET overlay_branch.current;
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT * FROM public.bs_user WHERE id = 10;

SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id) AS s51_main
FROM public.bs_user;

-- 5.2 Create + enter branch 'bs_user_b'.
SELECT overlay_branch.create_branch('bs_user_b') AS id_bs_user_b;
SELECT overlay_branch.use_branch('bs_user_b');

-- 5.3 Assertion A: Planner MUST inject Custom Scan for a plain SELECT.
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT * FROM public.bs_user;

-- 5.4 Asserion B: Aggregate walks CustomScan (count(*)).
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT count(*) FROM public.bs_user;

-- 5.5 Write 3 mutations into the branch (redirected to delta).
DELETE FROM public.bs_user WHERE id = 20;
UPDATE public.bs_user SET color = 'green' WHERE id = 10;
INSERT INTO public.bs_user VALUES (40, 'date', 'brown');

-- 5.6 MAIN heap must not have changed (RESET to MAIN then verify —
--     this exercises the guard that Planner skips CustomScan when
--     there is no active branch).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
                = '10=apple:red,20=banana:yellow,30=cherry:red'
         THEN 'PASS:BS_MAIN_UNTOUCHED'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||'='||name||':'||color, ',' ORDER BY id),'NULL')
       END AS s56_main_unchanged
FROM public.bs_user;

-- 5.7 Back into the branch.  Now 2 independent assertions that the
--     transparent BranchScan returns EXACTLY the same overlay as the
--     SRF workhorse that Step4 shipped with.
SELECT overlay_branch.use_branch('bs_user_b');

-- 5.7a SRF baseline (the known-correct 2-pass implementation).
SELECT string_agg((r).id::text||'='||(r).name||':'||(r).color, ',' ORDER BY (r).id) AS srf_row_picture
FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
  AS r(id int4, name text, color text);

-- 5.7b Transparent BranchScan (NEW for Slice2/3).
SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id) AS cs_row_picture
FROM public.bs_user;

-- 5.7c Equality assertion — this is the BranchScan MVP contract.
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||'='||(r).name||':'||(r).color, ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text))
              =
              (SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
               FROM public.bs_user)
         THEN 'PASS:BS_EQUIVALENT_TO_SRF'
         ELSE 'FAIL:BS_MISMATCH'
       END AS s57c_equivalence;

-- 5.7d Counts match.
SELECT (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
          AS r(id int4, name text, color text)) AS srf_n,
       (SELECT count(*) FROM public.bs_user) AS cs_n;

-- 5.7e WHERE id=10 returns the UPDATED tuple (green not red).
SELECT CASE WHEN color = 'green' AND id = 10 THEN 'PASS:BS_OVERRIDE_VISIBLE'
            ELSE 'FAIL:'||id::text||':'||COALESCE(color,'NULL') END AS s57e_override
FROM public.bs_user WHERE id = 10;

-- 5.7f ORDER BY walks through Sort → CustomScan (ensures the MVP path
--      override has not leaked an IndexScan into the transparent SELECT).
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT * FROM public.bs_user ORDER BY id DESC;
SELECT id, name FROM public.bs_user ORDER BY id DESC;

-- 5.8 Final cleanup (keep this idempotent with the Part 0 guard).
RESET overlay_branch.current;
DROP TABLE public.bs_user;

-- ===== Final cleanup =====
DROP TABLE IF EXISTS public.products;
DROP EXTENSION overlay_branch CASCADE;
