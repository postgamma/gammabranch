# Overlay Branch — User SQL Interface and Examples

> This document describes the SQL interface and behavior of the Overlay Branch extension from the **user's perspective**.
> All examples in this document can be reproduced directly in psql.
>
> **Scope**: only plain heap tables are supported; a primary key is required by default, and no-PK tables are covered by the experimental feature in §6.8. Function calls replace custom SQL syntax.

---

## Table of Contents

- [0. Prerequisites: Install the Extension and Create Tables](#0-prerequisites-install-the-extension-and-create-tables)
- [1. Branch Management](#1-branch-management)
  - [1.1 create_branch() — Create a Branch](#11-create_branch--create-a-branch)
  - [1.2 use_branch() — Switch Current Branch / Leave a Branch](#12-use_branch--switch-current-branch--leave-a-branch)
  - [1.3 current_branch() — Query the Current Branch](#13-current_branch--query-the-current-branch)
  - [1.4 list_branches() — List Branches](#14-list_branches--list-branches)
  - [1.5 discard_branch() — Discard a Branch](#15-discard_branch--discard-a-branch)
- [2. Reading and Writing Data Inside a Branch](#2-reading-and-writing-data-inside-a-branch)
  - [2.1 INSERT](#21-insert)
  - [2.2 UPDATE](#22-update)
  - [2.3 DELETE](#23-delete)
  - [2.4 Overlay Semantics Overview](#24-overlay-semantics-overview)
  - [2.5 Complete Overlay Example](#25-complete-overlay-example)
  - [2.6 A Branch Cannot See Other Branches' Changes](#26-a-branch-cannot-see-other-branches-changes)
- [3. Comparing Snapshot vs Live Branch Semantics](#3-comparing-snapshot-vs-live-branch-semantics)
  - [3.1 Typical Scenario Comparison](#31-typical-scenario-comparison)
  - [3.2 Live Semantics: Observing Main Changes](#32-live-semantics-observing-main-changes)
  - [3.3 Perception Differences Under the Same MAIN Change](#33-perception-differences-under-the-same-main-change)
  - [3.4 Self-Written Deltas Are Always Visible in a Branch (Both Semantics Agree)](#34-self-written-deltas-are-always-visible-in-a-branch-both-semantics-agree)
- [4. apply_branch() — Merge the Branch Delta into Main](#4-apply_branch--merge-the-branch-delta-into-main)
  - [4.1 APPLY Success Path](#41-apply-success-path)
  - [4.2 APPLY Conflict Path](#42-apply-conflict-path)
- [5. Not-Yet-Supported Scenarios (Explicit Errors) + Implemented UPSERT](#5-not-yet-supported-scenarios-explicit-errors--implemented-upsert)
  - [5.1 UPSERT Implementation Example](#51-upsert-implementation-example)
  - [5.1.1 Conflict Target Selection](#511-conflict-target-selection)
  - [5.2 Data-Modifying CTE: Explicit ERROR](#52-data-modifying-cte-explicit-error)
  - [5.3 Support Matrix](#53-support-matrix)
- [6. Special Cases](#6-special-cases)
  - [6.1 Pure Delta Scenario: INSERT Then UPDATE/DELETE in a Branch](#61-pure-delta-scenario-insert-then-updatedelete-in-a-branch)
  - [6.2 RETURNING Clause](#62-returning-clause)
  - [6.3 Applying Guard + Kickout Invalidation & New Helpers](#63-applying-guard--kickout-invalidation--new-helpers)
  - [6.4 list_branches() Identity and drift guard Fields](#64-list_branches-identity-and-drift-guard-fields)
  - [6.5 get_branch_identity() — Identity and Recovery Binding](#65-get_branch_identity--identity-and-recovery-binding)
  - [6.6 Isolation Level: READ COMMITTED Required](#66-isolation-level-read-committed-required)
  - [6.7 DDL RESTRICTED Inside an Active Branch](#67-ddl-restricted-inside-an-active-branch)
  - [6.8 No-PK Tables (Experimental Feature)](#68-no-pk-tables-experimental-feature)
  - [6.9 Delta Size Observability](#69-delta-size-observability)
  - [6.10 Canonical Keys and Values: Time, Float, bytea, money, jsonb](#610-canonical-keys-and-values-time-float-bytea-money-jsonb)

---

## 0. Prerequisites: Install the Extension and Create Tables

### 0.1 Installation (PG17)

```bash
# ① Confirm the target instance's PG_CONFIG (version 17)
pg_config --version; pg_config --pkglibdir; pg_config --sharedir

# ② Build
cd /path/to/overlay_branch
make USE_PGXS=1 PG_CONFIG=$(which pg_config) -j4

# ③ Install
make USE_PGXS=1 PG_CONFIG=$(which pg_config) install

# ④ Enable shared_preload_libraries, restart
grep -qE '^shared_preload_libraries\s*=\s*.*overlay_branch' $PGDATA/postgresql.conf \
  || echo "shared_preload_libraries = 'overlay_branch'" >> $PGDATA/postgresql.conf
pg_ctl -D $PGDATA restart -m fast

# ⑤ Create the extension
psql -d your_db <<'SQL'
CREATE EXTENSION overlay_branch;
SQL
```

### 0.2 Installation Verification

```sql
SHOW shared_preload_libraries;
-- Must include overlay_branch

SELECT proname FROM pg_proc WHERE proname IN ('create_branch','apply_branch','list_branches') ORDER BY 1;
SELECT count(*) FROM public.list_branches();

-- E2E smoke test
SELECT create_branch('smoke_test_001');
BEGIN;
  SELECT use_branch('smoke_test_001');
  EXPLAIN (COSTS OFF) SELECT 1;
ROLLBACK;
SELECT discard_branch('smoke_test_001');
```

### 0.3 Prepare Example Tables

```sql
CREATE TABLE products (
    id    integer PRIMARY KEY,
    name  text    NOT NULL,
    price integer NOT NULL
);

INSERT INTO products VALUES
  (1, 'Apple',  10),
  (2, 'Banana',  5),
  (3, 'Cherry', 20);
```

---

## 1. Branch Management

> All operations are performed via `public.*` function calls; the schema qualifier may be omitted.

> **Permissions.** Branch administration is privileged. `create_branch`,
> `use_branch`, `apply_branch` and `discard_branch` require a superuser or a
> member of the reserved role `overlay_branch_administrators`; any other
> caller gets `42501 insufficient_privilege`. Leaving a branch
> (`use_branch(NULL)`) and the read-only metadata helpers
> (`current_branch` / `list_branches`) are open to everyone. The
> `pg_branch.owner` column records who created the branch; it is not an
> authority grant.

### 1.1 create_branch() — Create a Branch

```sql
SELECT create_branch('agent_workspace');
 create_branch
---------------
             1
(1 row)
```

> Returns an internally incrementing `branch_id`. Creating a branch does not copy any data.

Failure example:

```sql
SELECT create_branch('agent_workspace');
ERROR:  branch "agent_workspace" already exists
```

### 1.2 use_branch() — Switch Current Branch / Leave a Branch

```sql
-- Enter a branch (session-level)
SELECT use_branch('agent_workspace');
 use_branch
------------

(1 row)
```

```sql
-- Specify the semantics mode when entering a branch:
--   mode => 'live'     (default: Latest Main + Branch Delta)
--   mode => 'snapshot' (MAIN table reads are frozen at the snapshot taken on entry)
SELECT use_branch('agent_workspace', mode => 'snapshot');

-- Switch back to live
SELECT use_branch('agent_workspace', mode => 'live');
```

```sql
-- Leave the current branch and return to Main
SELECT use_branch(NULL);
 use_branch
------------

(1 row)
```

Failure example:

```sql
SELECT use_branch('does_not_exist');
ERROR:  branch "does_not_exist" does not exist
```

Branch selection is **transaction-scoped**: a `use_branch()` issued inside a
transaction is undone when that transaction rolls back (or a savepoint rolls
back). If the session was already on another branch before `BEGIN`, the rollback
cannot silently re-enter it, so the session lands outside any branch and emits a
one-shot WARNING rather than keep the wrong branch.

```sql
BEGIN;
SELECT use_branch('agent_workspace');   -- inside the branch
ROLLBACK;                               -- branch selection undone
SELECT current_branch();                -- (empty) — back outside any branch
```

Setting the `overlay_branch.current` GUC is an equivalent way to enter or leave a
branch, including right after leaving one:

```sql
SELECT use_branch(NULL);                          -- leave, back in Main
SET overlay_branch.current = 'agent_workspace';   -- re-enter the branch
SELECT current_branch();                          -- agent_workspace
```

### 1.3 current_branch() — Query the Current Branch

```sql
-- In Main: returns empty
SELECT current_branch();
 current_branch
----------------

(1 row)

-- After entering a branch
SELECT use_branch('agent_workspace');
SELECT current_branch();
 current_branch
----------------
 agent_workspace
(1 row)
```

`current_branch()` reports the branch the session actually writes to. After a
cross-branch rollback (§1.2) it returns empty even though `overlay_branch.current`
may still name the pre-transaction branch.

### 1.4 list_branches() — List Branches

A table function; it can be JOINed and filtered, and returns 11 columns:

```sql
\x
SELECT * FROM list_branches() WHERE branch_name = 'agent_workspace';
-[ RECORD 1 ]-------------------------+----------------------------------
branch_id                             | 1
branch_name                           | agent_workspace
owner                                 | 10
created_at                            | 2026-09-10 10:00:00+08
mode                                  | live
state                                 | active
delta_count                           | 0
schema_hash                           | \x2d6f...
creation_snapshot_xmin                | 789
tablespace_list                       | 'pg_default:1663,my_tblspc:16789'
col_signature                         | \x8a3b...
registry_user_tables                  | 12
registry_schema_current_match         | t
\x
```

Overview of the 11 columns:

| Column | Type | Meaning |
|------|------|------|
| `branch_id` | integer | Internal incrementing id |
| `branch_name` | name | Branch name (unique) |
| `owner` | oid | Creator |
| `created_at` | timestamptz | Creation time |
| `mode` | text | `live` or `snapshot` (the mode currently used by this backend) |
| `state` | text | `active` / `applied` / `discarded` / `applying` (transient) |
| `delta_count` | bigint | Number of delta rows in this branch |
| `schema_hash` | bytea | MD5 fingerprint of the schema of all managed tables in MAIN at creation time |
| `creation_snapshot_xmin` | xid | MAIN transaction snapshot xmin at creation time |
| `tablespace_list` | text | CSV of managed-table tablespaces `name:oid` |
| `col_signature` | bytea | Column signature; the authoritative basis for drift detection |
| `registry_user_tables` | bigint | Number of managed tables in MAIN at creation time |
| `registry_schema_current_match` | bool | Whether MAIN's current schema matches that at creation time (no drift) |

### 1.5 discard_branch() — Discard a Branch

```sql
SELECT discard_branch('agent_workspace');
 discard_branch
----------------

(1 row)

-- After discarding: state = discarded, delta_count = 0
SELECT branch_name, state, delta_count FROM list_branches()
 WHERE branch_name = 'agent_workspace';
  branch_name   |   state   | delta_count
----------------+-----------+-------------
 agent_workspace | discarded |           0
(1 row)
```

If the branch being discarded is the one **currently in use**, the session automatically returns to Main:

```sql
SELECT use_branch('agent_workspace');
SELECT discard_branch('agent_workspace');
NOTICE:  discarding current branch "agent_workspace", reverting to Main
 discard_branch
----------------

(1 row)

SELECT current_branch();
 current_branch
----------------

(1 row)
```

Discarding a branch that is already `discarded` or `applied` again raises an error:

```sql
SELECT discard_branch('agent_workspace');
ERROR:  overlay_branch: discard_branch: branch "agent_workspace" is not active
             (already discarded or applied); discard cannot be called twice
             on the same branch
```

---

## 2. Reading and Writing Data Inside a Branch

**After entering a branch, all ordinary INSERT / UPDATE / DELETE operations do not write to the main table; they only write to the branch delta.**
Users do not need to change their DML syntax.

```sql
SELECT use_branch('agent_workspace');
```

### 2.1 INSERT

```sql
INSERT INTO products VALUES (4, 'Date', 30);
INSERT 0 1
```

### 2.2 UPDATE

```sql
UPDATE products SET price = 100 WHERE id = 1;
UPDATE 1
```

### 2.3 DELETE

```sql
DELETE FROM products WHERE id = 2;
DELETE 1
```

---

### 2.4 Overlay Semantics Overview

| Branch operation | Exists in Main | Not in Main | Output |
|-----------|---------|---------|------|
| INSERT | — | ✅ | Output the branch's new row |
| UPDATE | ✅ | — | Output the row after branch override |
| DELETE | ✅ | — | Suppress the Main row |
| No operation | ✅ | — | Output the Main row as-is |
| No operation | — | — | Not visible |

Two read paths:

```sql
-- Transparent read (recommended: zero syntax changes, requires shared_preload_libraries='overlay_branch')
SELECT * FROM products ORDER BY id;

-- Manual SRF (fallback entry point; the column type list must be given explicitly)
SELECT * FROM overlay_branch.overlay_main_plus_delta('products')
       AS x(id integer, name text, price integer)
 ORDER BY id;
```

A read is not limited to a bare `SELECT`. Inside a branch, a DML statement's
**read sources** — the FROM/USING side of `UPDATE ... FROM` / `DELETE ... USING`
and the source of `INSERT ... SELECT` — also see this branch's writes, so the
statement always operates on the same branch image a `SELECT` would return.

```sql
-- Branch sees products id=1 as price 100; a DML reading products must agree.
UPDATE orders SET note = products.name FROM products WHERE orders.pid = products.id;
```

The **DML target** itself may be a row that exists only in the branch (a row this
branch inserted, so Main has no such row). Such a target is updated or deleted
just like any other, including when it is reached through a join — the statement
sees the same branch image a `SELECT` would return.

```sql
INSERT INTO products VALUES (5, 'Fig', 40);      -- branch-only row
INSERT INTO price_ref(pid, new_price) VALUES (5, 42);

-- Both the target (products id=5) and the read source are branch-only.
UPDATE products SET price = price_ref.new_price
  FROM price_ref WHERE products.id = price_ref.pid;
-- UPDATE 1 ; products id=5 now has price 42 inside the branch

DELETE FROM products USING price_ref WHERE products.id = price_ref.pid;
-- DELETE 1 ; products id=5 is gone from the branch
```

**Constraint checks are eager.** A branch-local write validates the target
table's `NOT NULL` and `CHECK` constraints immediately, exactly like the same
statement run on Main: an invalid row raises `23502` / `23514` and never lands
in the branch delta (rather than being stored and only failing at apply time).
A duplicate against any plain-column, non-partial **`UNIQUE`** index is caught
the same way — whether the conflicting row lives in the branch delta, in Main,
or is proposed by an earlier row of the *same command* — and raises `23505`
before anything is written, so it is never deferred to `apply_branch`. An
`UPDATE` that leaves a `UNIQUE` column unchanged stays legal: the row being
rewritten is excluded from its own conflict probe.
For `INSERT ... ON CONFLICT` the candidate row is checked before the conflict is
resolved, and a `DO UPDATE` re-checks the merged row, so the behavior matches
Main even when `DO NOTHING` or `DO UPDATE` would otherwise discard the offending
value.

```sql
SELECT use_branch('agent_workspace');
UPDATE products SET price = NULL WHERE id = 1;
-- ERROR: 23502: null value in column "price" of relation "products" violates not-null constraint
```

```sql
-- (the UNIQUE index must exist in Main before entering the branch)
SELECT use_branch(NULL);
CREATE TABLE products_sku (id int PRIMARY KEY, sku text UNIQUE, price int);
INSERT INTO products_sku VALUES (1, 'A', 10);
SELECT use_branch('agent_workspace');
INSERT INTO products_sku VALUES (2, 'A', 20);   -- sku conflicts with the Main row id=1
-- ERROR: 23505: duplicate key value violates unique constraint "products_sku_sku_key"
```

### 2.5 Complete Overlay Example

Using the 3 DML statements written in §2: id=1 UPDATE, id=2 DELETE, id=4 INSERT.

**Main is unchanged**:

```sql
SELECT use_branch(NULL);
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |    10
  2 | Banana |     5
  3 | Cherry |    20
(3 rows)
```

**Viewing inside the branch**:

```sql
SELECT use_branch('agent_workspace');
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |   100
  3 | Cherry |    20
  4 | Date   |    30
(3 rows)
```

### 2.6 A Branch Cannot See Other Branches' Changes

```sql
SELECT create_branch('branch_B');
SELECT use_branch('branch_B');

-- branch_B has not changed anything, so it sees Main exactly as-is
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |    10
  2 | Banana |     5
  3 | Cherry |    20
(3 rows)

-- Changes made by agent_workspace are completely invisible to branch_B (branch isolation)
SELECT use_branch('agent_workspace');
SELECT * FROM products WHERE id = 1;
 id | name  | price
----+-------+-------
  1 | Apple |   100
(1 row)
```

---

## 3. Comparing Snapshot vs Live Branch Semantics

The same branch can be entered as snapshot or live in different sessions.

### 3.1 Typical Scenario Comparison

| Dimension | mode='live' (default) | mode='snapshot' |
|------|---|---|
| **Old rows of MAIN tables** (not written by yourself) | Always follow Main's latest committed state | **Frozen** at the snapshot taken at the moment of entry |
| **Rows written by the branch itself** | Always visible | Always visible |
| **Rows newly written by other sessions on the same branch** | Always visible | Always visible |
| **Typical scenario** | Agent collaboration workspace; keep your own changes while seeing others' new commits | Reporting / offline analysis; MAIN data does not drift during a query |
| **How to enter** | `use_branch('b')` or `use_branch('b','live')` | `use_branch('b','snapshot')` |

### 3.2 Live Semantics: Observing Main Changes

The default `mode='live'` semantics is an **overlay of Latest Main + Branch Delta**:
rows not changed by the branch always follow the latest committed version in Main; rows changed by the branch show the version overridden by the branch itself.

```sql
SELECT use_branch(NULL);
UPDATE products SET price = 9999 WHERE id = 3;
UPDATE 1

SELECT use_branch('branch_B');
SELECT * FROM products WHERE id = 3;
 id |  name  | price
----+--------+------
  3 | Cherry | 9999
(1 row)
```

### 3.3 Perception Differences Under the Same MAIN Change

```sql
-- Setup
SELECT use_branch(NULL);
TRUNCATE products;
INSERT INTO products VALUES (1,'A',10),(2,'B',5),(3,'C',20);
SELECT create_branch('b_snap');

-- s1: snapshot session
SELECT use_branch('b_snap', 'snapshot');
SELECT * FROM products ORDER BY id;
-- Sees MAIN baseline: A(10), B(5), C(20)

-- s2: live session writing to MAIN (another psql connection)
SELECT use_branch(NULL);
INSERT INTO products VALUES (4,'D',30);
UPDATE products SET price=9999 WHERE id=3;
COMMIT;

-- Back to s1: snapshot session
SELECT * FROM products ORDER BY id;
-- id | name | price      (MAIN frozen!)
--  1 |  A   | 10
--  2 |  B   |  5         Does not see MAIN id=4/D; id=3 is still 20
--  3 |  C   | 20
-- (3 rows)

-- s3: entering the same branch in live mode
SELECT use_branch(NULL);
SELECT use_branch('b_snap', 'live');
SELECT * FROM products ORDER BY id;
-- id | name | price
--  1 |  A   |   10
--  2 |  B   |    5         LIVE: id=4 is visible! id=3 price increased to 9999!
--  3 |  C   | 9999
--  4 |  D   |   30
-- (4 rows)
```

### 3.4 Self-Written Deltas Are Always Visible in a Branch (Both Semantics Agree)

```sql
SELECT use_branch('b_snap', 'snapshot');
UPDATE products SET price=7 WHERE id=2;
INSERT INTO products VALUES (99, 'Mine', 1);
SELECT * FROM products ORDER BY id;
-- 1|A|10       (MAIN frozen)
-- 2|B|7        (own delta)
-- 3|C|20       (MAIN frozen)
-- 99|Mine|1    (own delta)
```

---

## 4. apply_branch() — Merge the Branch Delta into Main

Merge the branch's current delta into Main, performing optimistic row-level conflict validation. A conflict raises a ERROR directly; no automatic merge is performed.

### 4.1 APPLY Success Path

```sql
SELECT use_branch(NULL);
SELECT apply_branch('agent_workspace');
NOTICE:  overlay_branch: APPLY BRANCH 'agent_workspace' completed
             (state=applied, delta rows deleted)
 apply_branch
--------------

(1 row)
```

After Apply, Main is actually rewritten, the branch's delta is cleared, and `state` becomes `applied`:

```sql
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |   100
  3 | Cherry |    20
  4 | Date   |    30
(3 rows)

SELECT branch_name, state, delta_count FROM list_branches()
 WHERE branch_name = 'agent_workspace';
  branch_name   |  state  | delta_count
----------------+---------+-------------
 agent_workspace | applied |           0
(1 row)
```

**Self apply**: if the current session is in `use_branch(X)`, then after it executes `apply_branch(X)` itself, the session automatically returns to Main; there is no need to call `use_branch(NULL)` again.

**Cross-session apply/discard**: if session A is in `use_branch(X)` and session B executes `apply_branch(X)` or `discard_branch(X)`, then session A's next DML raises `ERRCODE 55000` and it is kicked out of the branch (see §6.3).

### 4.2 APPLY Conflict Path

Scenario: after the branch is created, Main itself also modifies the same row, then the branch applies — a conflict is detected.

```sql
TRUNCATE products;
INSERT INTO products VALUES (1,'Apple',10),(2,'Banana',5),(3,'Cherry',20);

SELECT create_branch('alice');
SELECT use_branch('alice');
UPDATE products SET price = 999 WHERE id = 1;
UPDATE 1

SELECT use_branch(NULL);
UPDATE products SET price = 42 WHERE id = 1;
UPDATE 1

SELECT apply_branch('alice');
ERROR:  overlay_branch: apply conflict on relation products(primary key=["1"])
DETAIL:  branch's base old_version = '...'
         main's current old_version = '...'
         branch intends to write: (1,'Apple',999)
HINT:  Row was modified in Main after branch wrote it.
       Re-read current state and retry in a fresh branch.
```

---

## 5. Not-Yet-Supported Scenarios (Explicit Errors) + Implemented UPSERT

The following scenarios are **explicitly rejected** (they do not silently pass through to Main):

| Scenario | Behavior | Typical error |
|------|------|----------|
| DML on a **no-primary-key** table | Rejected by default; ✅ in the experimental keyless mode (see §6.8) | `table must have a PRIMARY KEY for branch-mode DML ...` |
| DDL inside a branch (CREATE/ALTER/DROP TABLE, etc.) | Rejected, with a hint to return to Main | `§A7 DDL RESTRICTED 0A000` (see §6.7) |
| `INSERT ... ON CONFLICT` (UPSERT) | **Supported** (see §5.1) | N/A |
| **Partitioned table** | Rejected | `overlay_branch does not support partitioned tables` |
| **FK cascade write** | Rejected | `cannot modify via FK-triggered write` |
| Writing directly to `pg_branch` / `pg_branch_delta` | Rejected | Native PG view error |
| DML on a VIEW / MATVIEW / FOREIGN TABLE | Rejected | Intercepted by the guard |
| **COPY FROM / MERGE / Data-Modifying CTE** | Rejected | `0A000 feature_not_supported` |
| **SELECT ... FOR UPDATE / FOR SHARE** | Rejected | `0A000 feature_not_supported` |
| **Reading `xmin` / `xmax` / `cmin` / `cmax`** | Rejected — a branch row has no storage tuple; only `ctid` (the row's MAIN anchor) and `tableoid` are meaningful | `0A000 does not support the "xmin" system column inside a branch` |
| **UPDATE SET pk_col = ...** (change the primary key) | Rejected | `Updating the PRIMARY KEY column ... is not supported` |
| **UPSERT ... DO UPDATE SET pk_col = ...** (change the primary key) | Rejected | `overlay_branch does not support UPSERT ON CONFLICT DO UPDATE that modifies primary key columns` |
| Insufficient privilege to call use/apply/discard/create | Rejected | `42501 insufficient_privilege` |

Identity columns (`GENERATED ... AS IDENTITY`) are supported — as PK or non-PK
columns. Apply replays the sequence-assigned value verbatim (a `GENERATED ALWAYS`
column can never change under a branch UPDATE) and never consumes the sequence:

```sql
CREATE TABLE t_ident (id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v text);
INSERT INTO t_ident (v) VALUES ('main');
SELECT create_branch('br_ident');
SELECT use_branch('br_ident');
INSERT INTO t_ident (v) VALUES ('branch');   -- id assigned by the sequence
UPDATE t_ident SET v = 'main!' WHERE id = 1;
SELECT use_branch(NULL);
SELECT apply_branch('br_ident');
SELECT * FROM t_ident ORDER BY id;           -- 1 | main!  /  2 | branch
```

### 5.1 UPSERT Implementation Example

`INSERT ... ON CONFLICT DO NOTHING / DO UPDATE` (for PK or UNIQUE-arbiter conflicts) works normally inside a branch:

```sql
SELECT create_branch('br_upsert_demo');
SELECT use_branch('br_upsert_demo');

-- ON CONFLICT DO NOTHING: PK conflict → NOP
INSERT INTO products VALUES (1, 'NewApple', 999) ON CONFLICT (id) DO NOTHING;
INSERT 0 0
SELECT * FROM products WHERE id = 1;
 id | name  | price
----+-------+-------
  1 | Apple |    10

-- ON CONFLICT DO UPDATE: PK conflict → update with the EXCLUDED new values
INSERT INTO products VALUES (3, 'Cherry_v2', 222)
  ON CONFLICT (id) DO UPDATE SET name  = EXCLUDED.name,
                                  price = EXCLUDED.price;
INSERT 0 1
SELECT * FROM products WHERE id = 3;
 id |   name    | price
----+-----------+-------
  3 | Cherry_v2 |   222

-- Conflict with the branch's own pure-origin INSERT
INSERT INTO products VALUES (100, 'First', 1) ON CONFLICT (id) DO NOTHING;
INSERT INTO products VALUES (100, 'Dupe',  2) ON CONFLICT (id) DO NOTHING;
INSERT 0 0
SELECT * FROM products WHERE id = 100;
 id  | name  | price
-----+-------+-------
 100 | First |     1

-- ON CONFLICT DO UPDATE ... WHERE: the predicate gates the update (both slots
-- it references -- the existing row and EXCLUDED -- are honored)
INSERT INTO products VALUES (3, 'Cherry_v3', 333)
  ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, price = EXCLUDED.price
  WHERE products.price < EXCLUDED.price;
SELECT * FROM products WHERE id = 3;
 id |   name    | price
----+-----------+-------
  3 | Cherry_v3 |   333

-- Multi-row UPSERT: every conflicting row in the batch is merged against its
-- own EXCLUDED tuple (works for several conflicting rows in one statement)
INSERT INTO products VALUES (1, 'Apple2', 20), (3, 'Cherry_v4', 444)
  ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name, price = EXCLUDED.price;
SELECT * FROM products WHERE id IN (1, 3) ORDER BY id;
 id |   name    | price
----+-----------+-------
  1 | Apple2    |    20
  3 | Cherry_v4 |   444

-- Repeated UPSERT on the same key: the branch delta shadows MAIN, so a second
-- UPSERT -- or any UPSERT after an UPDATE -- builds on the branch's own latest
-- value.  Columns absent from the SET list keep that value, not MAIN's.
UPDATE products SET price = 999 WHERE id = 3;
INSERT INTO products VALUES (3, 'Cherry_v5', 5)
  ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;
SELECT * FROM products WHERE id = 3;
 id |   name    | price
----+-----------+-------
  3 | Cherry_v5 |   999

-- After a branch-local DELETE, an UPSERT rebirths the row: the branch has
-- logically removed it, so ON CONFLICT must not treat the still-present MAIN
-- row as a conflict.  DO NOTHING and DO UPDATE ... WHERE false rebirth too.
DELETE FROM products WHERE id = 3;
INSERT INTO products VALUES (3, 'Cherry_reborn', 1)
  ON CONFLICT (id) DO UPDATE SET name  = EXCLUDED.name,
                                  price = EXCLUDED.price
  WHERE false;
SELECT * FROM products WHERE id = 3;
 id |     name      | price
----+---------------+-------
  3 | Cherry_reborn |     1
```

### 5.1.1 Conflict Target Selection

`ON CONFLICT (target)` honors the named target, not always the primary key:

```sql
-- (fresh MAIN table + branch)
CREATE TABLE sku_items (id int PRIMARY KEY, sku text UNIQUE, qty int);
INSERT INTO sku_items VALUES (1, 'A', 10);
SELECT create_branch('br_arb');
SELECT use_branch('br_arb');

-- arbiter on a NON-primary UNIQUE column: the conflict matches on `sku`, and
-- the delta row is re-keyed to the conflicted row's real PK
INSERT INTO sku_items VALUES (999, 'A', 20)
  ON CONFLICT (sku) DO UPDATE SET qty = EXCLUDED.qty;
SELECT * FROM sku_items;           -- id=1 updated in place; no id=999 row
 id | sku | qty
----+-----+-----
  1 | A   |  20

-- targetless DO NOTHING checks EVERY UNIQUE index: any conflict skips
INSERT INTO sku_items VALUES (1, 'B', 1) ON CONFLICT DO NOTHING;   -- PK id=1        → skip
INSERT 0 0
INSERT INTO sku_items VALUES (2, 'A', 1) ON CONFLICT DO NOTHING;   -- UNIQUE sku='A' → skip
INSERT 0 0

-- once the arbiter does NOT match, the row is still validated against the PK
-- and every other UNIQUE index: a hit is a hard duplicate-key error (23505),
-- exactly like Main -- never a delta write deferred to apply
INSERT INTO sku_items VALUES (1, 'C', 1) ON CONFLICT (sku) DO NOTHING;
ERROR:  duplicate key value violates unique constraint (23505)

-- a branch-local DELETE (or an UPDATE that moves the value away) FREES the
-- arbiter value: a later UPSERT on that value must INSERT, exactly like Main
-- -- the deleted row is gone, so ON CONFLICT must not resurrect the stale
-- MAIN row nor update the branch row that moved off the value
DELETE FROM sku_items WHERE id = 1;
INSERT INTO sku_items VALUES (2, 'A', 30)
  ON CONFLICT (sku) DO UPDATE SET qty = EXCLUDED.qty;
SELECT * FROM sku_items;
 id | sku | qty
----+-----+-----
  2 | A   |  30
```

The named target must resolve to exactly one **non-partial** UNIQUE index over
plain columns. An arbiter that is a **partial** index (`... WHERE <predicate>`)
or an **expression** index (`... ON (lower(col))`), or that names more than one
index, is rejected with `0A000` — for BOTH keyed and keyless tables:

```sql
-- partial-index arbiter -> 0A000 (the predicate could not be honoured, so the
-- probe would silently match a row outside the predicate's domain)
INSERT INTO sku_items VALUES (999, 'A', 1)
  ON CONFLICT (sku) WHERE id > 0 DO NOTHING;
ERROR:  overlay_branch: INSERT ... ON CONFLICT (UPSERT) requires exactly one
        non-partial UNIQUE arbiter index over plain columns (0A000)
```

### 5.2 Data-Modifying CTE: Explicit ERROR

Embedding DML in a CTE and then selecting from it in an outer SELECT is not allowed:

```sql
WITH u AS (UPDATE products SET price=price+1 RETURNING id, price)
SELECT * FROM u;
ERROR:  overlay_branch: Data-Modifying CTE (WITH ... UPDATE/INSERT/DELETE ... RETURNING)
        is not supported inside an active branch. Use top-level DML with RETURNING instead.
```

Alternative: use top-level DML with `RETURNING` directly (see §6.2).

### 5.3 Support Matrix

| Capability | Status |
| --- | --- |
| plain INSERT/UPDATE/DELETE | ✅ Supported |
| Duplicate-key INSERT (single- or multi-column PK, or any plain-column non-partial `UNIQUE` index) | ❌ Rejected eagerly with `23505`, same as Main — nothing is written to the delta |
| INSERT … ON CONFLICT (UPSERT) | ✅ Supported (arbitrates on the PK, or on the single non-partial plain-column `UNIQUE` index named in the statement — see §5.1.1); partial / expression arbiters are rejected with `0A000`; any *other* `UNIQUE` index (and the PK) is still enforced eagerly, and the `DO UPDATE` post-image is re-checked (`23505`) |
| UPSERT on a composite (multi-column) PK | ✅ Supported — the same conflict detection runs on every PK column, so `DO UPDATE` / `DO NOTHING` behave as in Main |
| UPDATE/DELETE … RETURNING | ✅ Supported |
| PK predicates (`pk = c` / `pk IN (…)` / `pk BETWEEN …` / `pk > c` / their `AND`/`OR` combinations) | ✅ Recommended path (goes through the PK index) |
| Non-PK predicates (arbitrary filter / Seq scan) | ✅ Supported |
| Volatile predicates (`nextval()`, …) | ✅ Supported (evaluated once per row, matching native — no doubled side effects) |
| TEMP table operations inside a branch | ✅ Supported (whitelisted) |
| CREATE TABLE / ALTER / DROP / CREATE INDEX / DROP TRIGGER | ❌ 0A000 (execute back in Main) |
| Partitioned table | ❌ Rejected |
| FK cascade write | ❌ Rejected |
| COPY FROM / MERGE / Data-Modifying CTE | ❌ Rejected |
| SELECT … FOR UPDATE / FOR SHARE / FOR NO KEY UPDATE | ❌ Rejected (row locks target Main rows, which a branch never modifies) |
| Reading `xmin` / `xmax` / `cmin` / `cmax` system columns | ❌ Rejected (a branch row owns no storage tuple; only `ctid` and `tableoid` are meaningful) |
| UPDATE SET pk_col | ❌ Rejected (use DELETE + INSERT instead) |
| DML on a no-PK table | ❌ Rejected by default; ✅ when `overlay_branch.keyless_tables=on` (experimental, see §6.8) |
| Non-deterministic (ICU) collation on a PK column | ❌ Rejected at `create_branch` — a text delta key cannot represent its equality classes (e.g. `'foo' = 'FOO'`); the same collation on a non-PK column is fine |
| `citext` on a PK column (through any chain of domain / array wrappers, e.g. `citext[]`, a domain over `citext`, a domain over `citext[]`, or an array of such a domain) | ❌ Rejected at `create_branch` — case-insensitive equality cannot be represented by a text delta key (e.g. `'foo' = 'FOO'`); a `citext` non-PK column (scalar or array) is fine |

Handling suggestions for RESTRICTED items:

| Restricted item | User workaround |
| --- | --- |
| DDL | Execute the DDL in Main first → create_branch again |
| COPY FROM | Rewrite as a multi-row `INSERT INTO t VALUES (...), (...)` |
| MERGE | Split into separate INSERT ON CONFLICT / UPDATE / DELETE |
| Data-Modifying CTE | Change to top-level DML + RETURNING |
| SELECT … FOR UPDATE/SHARE | Run it in Main (after `use_branch(NULL)`); a branch's writes never lock Main rows |
| Reading xmin/xmax/cmin/cmax | Read them in Main (after `use_branch(NULL)`); a branch row has no storage tuple to expose them |
| UPDATE pk | Change to `DELETE WHERE pk=old` + `INSERT VALUES (new_pk, ...)` |
| Non-deterministic collation on a PK column | Use a deterministic collation for the PK column (a non-deterministic one remains fine on a non-PK column) |
| `citext` on a PK column | Use a case-sensitive type (`text` / `varchar`) for the PK column (`citext` remains fine on a non-PK column) |
| FK cascade write | In the application layer, explicitly write the child table before the parent table |

---

## 6. Special Cases

### 6.1 Pure Delta Scenario: INSERT Then UPDATE/DELETE in a Branch

Rows INSERTed in the branch that do not exist in Main also work normally with subsequent UPDATE/DELETE.

#### 6.1.1 Pure Delta INSERT Then DELETE

```sql
SELECT use_branch('agent_workspace');

INSERT INTO products VALUES (99, 'PureInserted', 777);
INSERT 0 1
SELECT count(*) FROM products WHERE id = 99;
 count
-------
     1

DELETE FROM products WHERE id = 99;
DELETE 1
SELECT count(*) FROM products WHERE id = 99;
 count
-------
     0
```

Compound WHERE conditions:

```sql
CREATE TABLE bs_pd_multi (
    grp   text NOT NULL,
    score int4 NOT NULL,
    id    int4 PRIMARY KEY
);
INSERT INTO bs_pd_multi VALUES ('main', 0, 1);

SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_multi VALUES
  ('C', 333, 30), ('C', 333, 31), ('C', 999, 32);

DELETE FROM bs_pd_multi WHERE grp = 'C' AND score = 333;
DELETE 2
SELECT id, grp, score FROM bs_pd_multi WHERE grp = 'C' ORDER BY id;
 id | grp | score
----+-----+-------
 32 | C   |   999
(1 row)
```

#### 6.1.2 Pure Delta UPDATE

```sql
INSERT INTO products VALUES (999, 'WillBeUpdated', 1);
SELECT id, name, price FROM products WHERE id = 999;
 id  |      name       | price
-----+-----------------+-------
 999 | WillBeUpdated   |     1

UPDATE products SET price = 999999, name = 'UpdatedOK' WHERE id = 999;
UPDATE 1
SELECT id, name, price FROM products WHERE id = 999;
 id  |    name     |  price
-----+-------------+---------
 999 | UpdatedOK   | 999999
```

#### 6.1.3 Pure Delta × typmod PK: NUMERIC / BPCHAR

```sql
CREATE TABLE bs_pd_numeric(price NUMERIC(10,2) PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_numeric VALUES (30.33, 'pd-n1');
INSERT INTO bs_pd_numeric VALUES (40.44, 'pd-n2');
DELETE FROM bs_pd_numeric WHERE price = 30.33;
SELECT string_agg(tag, ',' ORDER BY tag) FROM bs_pd_numeric;
 string_agg
------------
 pd-n2

CREATE TABLE bs_pd_bpchar(code BPCHAR(6) PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_bpchar VALUES ('PD001', 'A'), ('PD002', 'B'), ('PD003', 'C');
DELETE FROM bs_pd_bpchar WHERE code = 'PD002';
SELECT code::text, tag FROM bs_pd_bpchar ORDER BY code;
 code  | tag
-------+-----
 PD001 | A
 PD003 | C
```

A NUMERIC PK **without** a typmod treats `=`-equal values that print with a
different scale as one row, exactly like MAIN:

```sql
CREATE TABLE bs_pd_numeric2(id NUMERIC PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_numeric2 VALUES (1.0, 'a');
INSERT INTO bs_pd_numeric2 VALUES (1.00, 'b');
ERROR:  duplicate key value violates unique constraint on relation "bs_pd_numeric2"
DETAIL:  Key (id)=(1.00) already exists in branch delta (R21 pre-check).
SELECT id, tag FROM bs_pd_numeric2 ORDER BY id;
 id  | tag
-----+-----
 1.0 | a
```

#### 6.1.4 Pure Delta × Empty MAIN Heap End-to-End

```sql
CREATE TABLE bs_empty_pk(id INT4 PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_empty_pk VALUES (1,'one'),(2,'two'),(3,'three'),(4,'four');

DELETE FROM bs_empty_pk WHERE id IN (2,4);
DELETE 2
SELECT count(*) FROM bs_empty_pk;
 count
-------
     2

DELETE FROM bs_empty_pk WHERE id = 3 RETURNING id, tag;
 id |  tag
----+-------
  3 | three
(1 row)

SELECT apply_branch('agent_workspace');
 apply_branch
--------------

SELECT * FROM bs_empty_pk ORDER BY id;
 id | tag
----+-----
  1 | one
```

### 6.2 RETURNING Clause

`INSERT/UPDATE/DELETE RETURNING` is fully supported:

```sql
SELECT use_branch('agent_workspace');

INSERT INTO products VALUES (10, 'ten', 10) RETURNING id, name, price;
 id | name | price
----+------+-------
 10 | ten  |    10
(1 row)

UPDATE products SET price = 100 WHERE id = 1 RETURNING id, price;
 id | price
----+-------
  1 |   100
(1 row)

DELETE FROM products WHERE id = 2 RETURNING *;
 id |  name  | price
----+--------+-------
  2 | Banana |     5
(1 row)
```

Pure-delta DELETE also supports RETURNING:

```sql
DELETE FROM bs_pd_multi WHERE id = 32 RETURNING id, grp, score;
 id | grp | score
----+-----+-------
 32 | C   |   999
(1 row)
```

### 6.3 Applying Guard + Kickout Invalidation & New Helpers

#### 6.3.1 Two Invalidation Check SQL Helpers

| Function | Return value | Purpose |
|---|---|---|
| `is_active(name)` | `bool` | Whether the branch is still `active` and the current session has not been kicked out |
| `force_invalidation_check()` | `bool` | Immediately check the branch state; if a state change is detected, kick out of the branch and return false |

```sql
SELECT use_branch('agent_workspace');

SELECT is_active('agent_workspace');
 is_active
-----------
 t

-- If another session has applied agent_workspace by now...
SELECT force_invalidation_check();
NOTICE:  overlay_branch: force_invalidation_check: branch "agent_workspace" state is applied,
             kicking out (was active)
 force_invalidation_check
--------------------------
 f

SELECT current_branch();
 current_branch
----------------
                (empty — returned to Main)
```

#### 6.3.2 DML Kickout After Cross-Session Apply/Discard

Once the branch has been applied/discarded by another session, the current session's next **DML** is intercepted with `ERRCODE 55000` and it is kicked out of the branch.
DQL (pure SELECT) only emits a NOTICE + fallback and does not raise an error.

```sql
-- sA: working in branch b_kick
SELECT create_branch('b_kick');
SELECT use_branch('b_kick');
INSERT INTO products VALUES (5, 'SA_work', 777);

-- sB: another connection applies b_kick
SELECT apply_branch('b_kick');

-- sA continues writing
INSERT INTO products VALUES (6, 'SA_late', 0);
ERROR:  overlay_branch: state of branch "b_kick" is not active (state=applied);
             drift writes to MAIN are blocked. DML cannot proceed inside a non-active
             branch context. Re-enter a fresh branch or return to MAIN.
HINT:  ERRCODE 55000 (object_not_in_prerequisite_state).

-- sA has been kicked out of the branch
SELECT current_branch() = '';
 ?column?
----------
 t
```

#### 6.3.3 DML During the Applying State

While apply_branch() runs, the target branch holds a branch-level lock until the
apply transaction ends. A concurrent writer to the same branch first blocks on
that lock; once apply commits and the branch is `applied`, the writer's state
re-check rejects the write, so no row is merged into MAIN and no orphan delta row
is left behind:

```sql
INSERT INTO products VALUES (7, 'rush', 1);
-- blocks until apply_branch() commits, then:
ERROR:  overlay_branch: cannot write delta rows to branch "b_large" (bid=3 state=applied).
             Writes are only allowed while branch is ACTIVE.
HINT:  ERRCODE 55000.
```

#### 6.3.4 DO Block Retry Wrapper Template

```sql
DO $$
DECLARE
    retries int := 0;
BEGIN
    <<retry_loop>>
    LOOP
        BEGIN
            INSERT INTO products VALUES (9, 'retryable', 99);
            EXIT retry_loop;
        EXCEPTION
            WHEN SQLSTATE '55000' THEN
                retries := retries + 1;
                IF retries > 3 THEN RAISE; END IF;
                PERFORM pg_sleep(0.1 * retries);
                PERFORM use_branch('fresh_' || retries::text, 'live');
        END;
    END LOOP retry_loop;
END $$;
```

### 6.4 list_branches() Identity and drift guard Fields

`list_branches()` returns the lifecycle + recovery identity + MAIN schema drift self-check in a single call:

```sql
SELECT branch_id, branch_name, state,
       schema_hash, creation_snapshot_xmin, tablespace_list, col_signature,
       registry_user_tables, registry_schema_current_match
  FROM list_branches() WHERE branch_name = 'agent_workspace';
```

If `registry_schema_current_match = false`, it means the MAIN schema has drifted (someone changed the structure of managed tables in Main), and apply may produce incorrect results. **Recommended process**: first discard the branch, create it again, then redo the modifications; do not force apply.

### 6.5 get_branch_identity() — Identity and Recovery Binding

A read-only helper used to self-check drift before apply:

| Column | Description |
|---|---|
| `branch_id` | Branch id |
| `schema_hash` | MAIN schema fingerprint at creation time (16-byte MD5) |
| `creation_snapshot_xmin` | MAIN snapshot at creation time |
| `tablespace_list` | Managed-table tablespaces CSV |
| `col_signature` | Column signature (authoritative basis for drift) |
| `registry_user_tables` | Number of managed tables in MAIN at creation time |
| `registry_schema_current_match` | Whether MAIN's current col_signature equals that at creation time |

```sql
SELECT * FROM public.get_branch_identity('agent_workspace');
```

If `registry_schema_current_match = false`, drift exists and apply is not possible; it must be handled manually.

### 6.6 Isolation Level: READ COMMITTED Required

Branch operations require **READ COMMITTED**. Inside a `REPEATABLE READ` or `SERIALIZABLE` transaction both entering a branch and writing to an active branch fail with `0A000`:

```sql
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT public.use_branch('b_live');
-- ERROR:  overlay_branch: use_branch() is not supported in a
--         REPEATABLE READ or SERIALIZABLE transaction

SELECT public.use_branch('b_live');
INSERT INTO public.accounts VALUES (999, 1);   -- active branch, READ COMMITTED
BEGIN ISOLATION LEVEL REPEATABLE READ;
INSERT INTO public.accounts VALUES (1000, 1);
-- ERROR:  overlay_branch: cannot write to a branch from a
--         REPEATABLE READ or SERIALIZABLE transaction
```

The freeze that blocks a writer while an apply/discard is in flight re-reads the branch state after an advisory-lock wait; under a transaction snapshot that read cannot observe a concurrent apply/discard commit. Use READ COMMITTED, or commit and retry in a new READ COMMITTED transaction.

`apply_branch()` / `discard_branch()` always run the same locked path whether the branch delta is empty or not: the branch advisory lock, the atomic state transition, the schema-drift guard, the session cache flush and the state NOTIFY are identical for empty and non-empty branches.

### 6.7 DDL RESTRICTED Inside an Active Branch

After entering an active branch, DDL on Main user objects is rejected with `0A000`:

```
ERROR:  overlay_branch: cannot CREATE TABLE inside active branch "b_live":
        CREATE TABLE on MAIN user objects during an active branch would modify
        underlying schema/tables in place and break MAIN/delta join semantics
        after apply.  (§A7 DDL RESTRICTED 0A000 — schema-drift guard)
HINT:  Switch back to the MAIN database before running this statement:
           SELECT overlay_branch.discard_branch('b_live');
         or
           RESET overlay_branch.current;
```

Protected DDL actions (all 0A000):

| Type | Behavior |
|------|------|
| CREATE TABLE / UNLOGGED TABLE | Intercepted (**except for TEMP tables**) |
| ALTER TABLE / ADD/DROP COLUMN / ALTER TYPE / SET SCHEMA | All intercepted |
| DROP TABLE / DROP INDEX / TRUNCATE | All intercepted |
| CREATE INDEX / DROP INDEX / REINDEX / CLUSTER | All intercepted |
| ALTER TYPE / CREATE TYPE / DROP TYPE | All intercepted |
| ALTER TABLE ADD/DROP/ENABLE/DISABLE TRIGGER | All intercepted |
| CREATE/ALTER/DROP SEQUENCE | All intercepted |

> **TEMP table whitelist**: `CREATE TEMP TABLE` / `CREATE TEMPORARY TABLE` are not restricted (session-private, not written to Main).

Recommended workflow:

```sql
-- step 1: create / modify tables in Main
CREATE TABLE orders (id int PRIMARY KEY, amount numeric(12,2));
ALTER TABLE products ADD COLUMN stock int DEFAULT 0;

-- step 2: open a branch and modify data
SELECT create_branch('b_order_work');
SELECT use_branch('b_order_work');
INSERT INTO orders VALUES (1, 99.0);
UPDATE products SET stock = 10 WHERE id = 1;
```

### 6.8 No-PK Tables (Experimental Feature)

DML on no-PK tables is **rejected by default** (see §5.3). After setting `overlay_branch.keyless_tables = on`, the extension emits a one-line WARNING and switches to using a row's physical location as its identity, so that no-PK tables can also enter a branch like PK tables.

```sql
-- Default (off): DML on a no-PK table is rejected
CREATE TABLE logs (msg text);
INSERT INTO logs VALUES ('boot');
SELECT create_branch('b_logs');
SELECT use_branch('b_logs');
INSERT INTO logs VALUES ('x');
ERROR:  overlay_branch: cannot INSERT relation "logs" inside active branch "b_logs": table must have a PRIMARY KEY for branch-mode DML (enable overlay_branch.keyless_tables for the experimental ctid fallback)
DETAIL:  Target relation: logs.
```

A no-PK table used only as a **read source** is not a DML target and is unaffected by
this rule: `INSERT INTO t SELECT ... FROM logs`, `UPDATE t ... FROM logs` and
`DELETE FROM t USING logs` read `logs` from Main (as any `SELECT` on a no-PK table
does), so they need no primary key on `logs`.

```sql
-- After enabling the experimental switch: one-line WARNING + normal read/write overlay
SET overlay_branch.keyless_tables = on;
INSERT INTO logs VALUES ('x');
WARNING:  overlay_branch: table "public.logs" has no PRIMARY KEY; using
          experimental ctid-based row identity (overlay_branch.keyless_tables=on)
INSERT 0 1

SELECT * FROM logs;   -- Main('boot') ⊕ branch('x')
  msg
-------
 boot
 x

-- apply / discard behavior is the same as for PK tables
SELECT apply_branch('b_logs');
```

`INSERT ... ON CONFLICT` (UPSERT) also works on a no-PK table. Because there is no primary key to arbitrate on, the conflict is resolved on the single `UNIQUE` constraint the statement names; `DO NOTHING` / `DO UPDATE` then behave exactly as for a PK table (the row is rewritten in the branch, Main stays untouched until `apply_branch`):

```sql
CREATE TABLE counters (id int UNIQUE, n int);
INSERT INTO counters VALUES (1, 10);
SELECT create_branch('b_ctr');
SELECT use_branch('b_ctr');
INSERT INTO counters VALUES (1, 11) ON CONFLICT (id) DO UPDATE SET n = EXCLUDED.n;
SELECT * FROM counters;              -- 1 = 11 (branch image)

-- a branch-local DELETE frees id=1: the later UPSERT inserts a fresh row,
-- exactly like Main (the deleted row is not resurrected)
DELETE FROM counters WHERE id = 1;
INSERT INTO counters VALUES (1, 12) ON CONFLICT (id) DO UPDATE SET n = EXCLUDED.n;
SELECT * FROM counters;              -- 1 = 12 (reborn)

SELECT use_branch(NULL);
SELECT * FROM counters;              -- 1 = 10 (Main untouched)
SELECT apply_branch('b_ctr');
```

Key behavior points:

| Item | Behavior |
|----|------|
| Switch | `overlay_branch.keyless_tables` (bool, default `off`, session-level SET, requires superuser) |
| WARNING | Emitted once per session the first time a no-PK table is hit |
| Read/write overlay | INSERT/UPDATE/DELETE/SELECT/apply/discard are the same as for PK tables |
| Row identity | Based on the row's **physical location** (best-effort): if another session modifies a Main row that the branch anchored to while the branch is alive, apply reports a conflict |
| UPSERT | `INSERT ... ON CONFLICT` arbitrates on the single `UNIQUE` index named in the statement (single- or multi-column); an arbiter that does not resolve to exactly one non-partial plain-column `UNIQUE` index (a partial or expression index, or several) is rejected with `0A000` — exactly as for a keyed table (see §5.1.1) |
| UPSERT with a NULL arbiter value | Follows the index's null handling: under the default `UNIQUE` (NULLS DISTINCT) a NULL never matches, so the statement degrades to a plain INSERT; under `UNIQUE NULLS NOT DISTINCT` a NULL conflicts with an existing NULL row, just like Main |

> This feature does not aim for full parity with PK tables and is recommended only for experimental / short-term draft scenarios; for long-term use, add a primary key to the table. For design and limitations, see [internals.md](internals.md) §4.

### 6.9 Delta Size Observability

A long-lived branch keeps accumulating delta rows, and each delta row also doubles
the weight of the MAIN row it shadows. Two read-only products let you watch that
growth without reading delta content.

`branch_delta_stats(branch_name)` returns one row per `(branch, relation)` that has
delta rows:

```sql
CREATE TABLE t (id int PRIMARY KEY, v text);
INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c');
SELECT create_branch('b_stat');
SELECT use_branch('b_stat');
INSERT INTO t VALUES (10, 'x');          -- I
INSERT INTO t VALUES (11, 'y');          -- I
UPDATE t SET v = 'a2' WHERE id = 1;      -- U
DELETE FROM t WHERE id = 2;              -- D

SELECT branch_name, relation, delta_rows, delta_insert, delta_update, delta_delete
  FROM branch_delta_stats('b_stat');
 branch_name | relation | delta_rows | delta_insert | delta_update | delta_delete
-------------+----------+------------+--------------+--------------+--------------
 b_stat      | public.t |          4 |            2 |            1 |            1
```

The remaining columns are environment-dependent byte sizes: `delta_payload_bytes`
sums the stored key / `old_version` / `tuple_data` octet lengths, `main_bytes` is the
MAIN relation's `pg_total_relation_size`, and `amplification` is `delta_payload_bytes
/ main_bytes` (it includes the `_base` doubling). The function exposes counts and
byte sizes only — never delta row content. A branch with no delta rows does not
appear; an unknown branch name (or `NULL`) returns no rows.

**Throttled WARNING.** Set a row threshold and the write path nudges you before the
delta grows unbounded:

```sql
SET overlay_branch.delta_size_warn_rows = 1000;       -- 0 (default) = disabled
SET overlay_branch.delta_size_check_interval = 1024;  -- check once per N writes

-- once the branch delta first exceeds the threshold:
WARNING:  overlay_branch: branch "b_stat" delta has reached 1024 rows (threshold 1000); consider apply_branch()/discard_branch() to bound growth
HINT:  Inspect per-relation delta size with SELECT * FROM branch_delta_stats('b_stat').
```

| Item | Behavior |
|----|------|
| `branch_delta_stats(name)` | Read-only; one row per `(branch, relation)` with delta rows. SECURITY INVOKER, like `list_branches()` |
| `overlay_branch.delta_size_warn_rows` | int, default `0` (disabled), `PGC_USERSET` |
| `overlay_branch.delta_size_check_interval` | int, default `1024`; delta writes between checks |
| WARNING | Amortized counter, emitted once per session on first overflow, and fail-open (never breaks user DML) |

> For the design, see [internals.md](internals.md) §5.6.

### 6.10 Canonical Keys and Values: Time, Float, bytea, money, jsonb

For these types a branch matches on the *value*, not on its text spelling, so it
behaves exactly like MAIN no matter how the writing session renders the key:
`timestamptz` compares as an instant, `float8` `-0.0` equals `+0.0`, and a `bytea`
value is the same bytes whether spelled in hex or escape form. A stored row value
is canonical too, so another session reads back exactly what was inserted even
under its own date/float display settings. That canonical form covers
`timestamp`, `timestamptz`, `date`, `interval`, `float4`, `float8`, and `money`:
each of these renders through a GUC (`DateStyle`, `IntervalStyle`,
`extra_float_digits`, `lc_monetary`) without leaving a self-describing marker, so
an unpinned payload would be re-parsed to a different value by a reader under
other settings. In particular a
`timestamptz` emits a zone offset, but that only fixes the instant's zone —
`DateStyle` still reorders the date fields, so `2024-06-01 …` written under
`SQL, DMY` would read back as 6 January under `ISO, MDY`.

```sql
CREATE TABLE bs_guc_ts(k timestamptz PRIMARY KEY, v text);
CREATE TABLE bs_guc_f(k float8 PRIMARY KEY, v text);
CREATE TABLE bs_guc_b(k bytea PRIMARY KEY, v text);
CREATE TABLE bs_guc_p(id int PRIMARY KEY, ts timestamp, tz timestamptz, f float8);
SELECT create_branch('bs_guc');
SELECT use_branch('bs_guc');

-- same instant written under two time zones
SET TimeZone = 'UTC';
INSERT INTO bs_guc_ts VALUES ('2024-06-01 00:00:00+00', 'a');
SET TimeZone = 'Asia/Shanghai';
INSERT INTO bs_guc_ts VALUES ('2024-06-01 08:00:00+08', 'b');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_ts"
DETAIL:  Key (k)=(2024-06-01 08:00:00+08) already exists in branch delta (R21 pre-check).
RESET TimeZone;

-- float8 -0.0 and +0.0
INSERT INTO bs_guc_f VALUES ('-0'::float8, 'neg');
INSERT INTO bs_guc_f VALUES ('0'::float8, 'pos');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_f"
DETAIL:  Key (k)=(0) already exists in branch delta (R21 pre-check).

-- bytea: hex vs escape spelling of the same bytes
SET bytea_output = 'hex';
INSERT INTO bs_guc_b VALUES ('\x00'::bytea, 'h');
SET bytea_output = 'escape';
INSERT INTO bs_guc_b VALUES ('\x00'::bytea, 'e');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_b"
DETAIL:  Key (k)=(\000) already exists in branch delta (R21 pre-check).
RESET bytea_output;

-- stored values keep their value across sessions
SET DateStyle = 'SQL, DMY';
SET extra_float_digits = 0;
INSERT INTO bs_guc_p VALUES (1, timestamp '2024-02-01 05:06:07',
                             timestamptz '2024-02-01 05:06:07+00',
                             0.30000000000000004);
RESET DateStyle;
RESET extra_float_digits;
SELECT ts, tz, f FROM bs_guc_p WHERE id = 1;
         ts          |           tz           |          f
---------------------+------------------------+---------------------
 2024-02-01 05:06:07 | 2024-02-01 05:06:07+00 | 0.30000000000000004
```

The same holds when a key column is a **domain** over one of these types: a
domain is matched through its base type, so a `numeric` domain folds `1.0` /
`1.00` and a `timestamptz` domain folds the same instant (arrays of a domain and
domains over an array included).

```sql
CREATE DOMAIN bs_dnum AS numeric;
CREATE TABLE bs_guc_dom(k bs_dnum PRIMARY KEY, v text);
SELECT create_branch('bs_guc_dom');
SELECT use_branch('bs_guc_dom');
INSERT INTO bs_guc_dom VALUES (1.00, 'a');
INSERT INTO bs_guc_dom VALUES (1.0, 'b');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_dom"
DETAIL:  Key (k)=(1.0) already exists in branch delta (R21 pre-check).
```

An **array** key column folds element-by-element the same way: `numeric[]` and
`float8[]` keys are compared element-wise, so `ARRAY[1.0]` / `ARRAY[1.00]` and
`ARRAY[-0.0]` / `ARRAY[0.0]` are one value and collide. A `bpchar[]` key folds
too — `array_eq` ignores each element's trailing blanks while `array_out` keeps
them, so `ARRAY['a']` / `ARRAY['a ']` are one value and collide. Arrays that
differ in shape or lower bound are different values and stay distinct.

```sql
CREATE TABLE bs_guc_arr(k numeric[] PRIMARY KEY, v text);
SELECT create_branch('bs_guc_arr');
SELECT use_branch('bs_guc_arr');
INSERT INTO bs_guc_arr VALUES (ARRAY[1.0::numeric], 'a');
INSERT INTO bs_guc_arr VALUES (ARRAY[1.00::numeric], 'b');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_arr"
DETAIL:  Key (k)=({1.00}) already exists in branch delta (R21 pre-check).
```

An **interval** key folds to a single span: `interval` equality counts a month as
30 days and a day as 24 hours, so `'1 mon'` / `'30 days'` and `'1 day'` /
`'24 hours'` are one value and collide. The fold is element-wise for an
`interval[]` key too.

```sql
CREATE TABLE bs_guc_iv(k interval PRIMARY KEY, v text);
SELECT create_branch('bs_guc_iv');
SELECT use_branch('bs_guc_iv');
INSERT INTO bs_guc_iv VALUES ('1 mon'::interval, 'a');
INSERT INTO bs_guc_iv VALUES ('30 days'::interval, 'b');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_iv"
DETAIL:  Key (k)=(30 days) already exists in branch delta (R21 pre-check).
```

A **range** key folds through its bounds: a range renders each bound with its
subtype's output function and compares them with the subtype's `=`, so
`'[1.0,2.0)'` and `'[1,2)'` are one `numrange` value and collide. The fold is
element-wise for an array of ranges, and a range over a `DateStyle` /
`TimeZone`-sensitive subtype (`daterange`, `tsrange`, `tstzrange`) renders
canonically regardless of the session.

```sql
CREATE TABLE bs_guc_rng(k numrange PRIMARY KEY, v text);
SELECT create_branch('bs_guc_rng');
SELECT use_branch('bs_guc_rng');
INSERT INTO bs_guc_rng VALUES ('[1.0,2.0)'::numrange, 'a');
INSERT INTO bs_guc_rng VALUES ('[1,2)'::numrange, 'b');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_rng"
DETAIL:  Key (k)=([1,2)) already exists in branch delta (R21 pre-check).
```

A **multirange** key folds through its element ranges the same way: a multirange
renders each element range through `range_out` and compares them bound-wise, so
`'{[1.0,2.0)}'` and `'{[1,2)}'` are one `nummultirange` value and collide. The
fold is element-wise for an array of multiranges, and a multirange over a
`DateStyle` / `TimeZone`-sensitive subtype (`datemultirange`, `tsmultirange`,
`tstzmultirange`) renders canonically regardless of the session.

```sql
CREATE TABLE bs_guc_mrng(k nummultirange PRIMARY KEY, v text);
SELECT create_branch('bs_guc_mrng');
SELECT use_branch('bs_guc_mrng');
INSERT INTO bs_guc_mrng VALUES ('{[1.0,2.0)}'::nummultirange, 'a');
INSERT INTO bs_guc_mrng VALUES ('{[1,2)}'::nummultirange, 'b');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_mrng"
DETAIL:  Key (k)=({[1,2)}) already exists in branch delta (R21 pre-check).
```

A **`money`** column is canonical too, and it is unique among these types in that
the branch pins it on *both* the write and the read side. `money` renders through
`lc_monetary` (`cash_out`), but its input function (`cash_in`) accepts **only the
currency symbol and separators of the reading session's own locale** — a value
written under one locale could otherwise fail to re-parse under another. So a
branch stores the canonical `C` form (`$1,234.56`) and every replay re-parses it
under that same form, whatever the session's `lc_monetary` is. Two consequences
are visible to the user: a `money` key matches on its value across locales (a
`money` written under a foreign locale collides with the canonical one), and a
`money` payload reads back exactly as written. Set `lc_monetary` back to its own
locale after leaving branch mode if you rely on it elsewhere.

```sql
CREATE TABLE bs_guc_mon(k money PRIMARY KEY, v text);
CREATE TABLE bs_guc_mone(id int PRIMARY KEY, m money);
SELECT create_branch('bs_guc_mon');
SELECT use_branch('bs_guc_mon');

-- written under a foreign locale, read back under C
SET lc_monetary = 'en_GB.utf8';
INSERT INTO bs_guc_mon VALUES (1234.56::money, 'gb');
INSERT INTO bs_guc_mone VALUES (1, 9999.99::money);
SET lc_monetary = 'C';
-- the same amount collides: identity compares by value, not by rendered symbol
INSERT INTO bs_guc_mon VALUES (1234.56::money, 'us');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_mon"
DETAIL:  Key (k)=($1,234.56) already exists in branch delta (R21 pre-check).
-- the stored payload round-trips to the canonical C rendering
SELECT m FROM bs_guc_mone WHERE id = 1;
     m
------------
 $9,999.99
RESET lc_monetary;
```

A **`jsonb`** key folds its numerics the same way: `jsonb` compares numbers by
value, so `'{"a":1.0}'` and `'{"a":1.00}'` are one value even though a jsonb
datum keeps the numeric's written scale and would otherwise print them
differently. A branch folds every number at any depth — object values, array
elements, and a scalar root — so a duplicate branch INSERT of an `=`-equal jsonb
key raises `23505` like MAIN.

```sql
CREATE TABLE bs_guc_jb(k jsonb PRIMARY KEY, v text);
SELECT create_branch('bs_guc_jb');
SELECT use_branch('bs_guc_jb');
INSERT INTO bs_guc_jb VALUES ('{"a":1.0}'::jsonb, 'a');
INSERT INTO bs_guc_jb VALUES ('{"a":1.00}'::jsonb, 'b');
ERROR:  duplicate key value violates unique constraint on relation "bs_guc_jb"
DETAIL:  Key (k)=({"a": 1.00}) already exists in branch delta (R21 pre-check).
```

A **`json`** / **`jsonb`** column is stored structurally, not as text: a branch
read returns the object or array itself rather than a string that merely looks
like one, and `apply_branch` writes the same structure back to MAIN. A `jsonb[]`
column round-trips element-wise; a JSON-`null` jsonb value reads back as SQL
`NULL` (the same collapse `row_to_json()` makes).

```sql
CREATE TABLE bs_guc_jp(id int PRIMARY KEY, j jsonb, jc json, ja jsonb[]);
SELECT create_branch('bs_guc_jp');
SELECT use_branch('bs_guc_jp');
INSERT INTO bs_guc_jp
    VALUES (1, '{"a":1,"nested":{"b":2}}'::jsonb,
            '{"c":3}'::json, ARRAY['{"x":1}'::jsonb]);
SELECT jsonb_typeof(j) AS jt, jc::text AS jc, ja FROM bs_guc_jp WHERE id = 1;
   jt   |    jc    |       ja
--------+----------+----------------
 object | {"c": 3} | {"{\"x\": 1}"}
```

