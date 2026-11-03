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
  - [5.2 Data-Modifying CTE: Explicit ERROR](#52-data-modifying-cte-explicit-error)
  - [5.3 Support Matrix](#53-support-matrix)
- [6. Special Cases](#6-special-cases)
  - [6.1 Pure Delta Scenario: INSERT Then UPDATE/DELETE in a Branch](#61-pure-delta-scenario-insert-then-updatedelete-in-a-branch)
  - [6.2 RETURNING Clause](#62-returning-clause)
  - [6.3 Applying Guard + Kickout Invalidation & New Helpers](#63-applying-guard--kickout-invalidation--new-helpers)
  - [6.4 list_branches() Identity and drift guard Fields](#64-list_branches-identity-and-drift-guard-fields)
  - [6.5 get_branch_identity() — Identity and Recovery Binding](#65-get_branch_identity--identity-and-recovery-binding)
  - [6.6 Fast Return for Empty Branch Apply/Discard](#66-fast-return-for-empty-branch-applydiscard)
  - [6.7 DDL RESTRICTED Inside an Active Branch](#67-ddl-restricted-inside-an-active-branch)
  - [6.8 No-PK Tables (Experimental Feature)](#68-no-pk-tables-experimental-feature)
  - [6.9 Delta Size Observability](#69-delta-size-observability)

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
| **UPDATE SET pk_col = ...** (change the primary key) | Rejected | `Updating the PRIMARY KEY column ... is not supported` |
| Insufficient privilege to call use/apply/discard/create | Rejected | `42501 insufficient_privilege` |

### 5.1 UPSERT Implementation Example

`INSERT ... ON CONFLICT DO NOTHING / DO UPDATE` (for PK conflicts) works normally inside a branch:

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
| INSERT … ON CONFLICT (UPSERT) | ✅ Supported (arbitrates on the PK, or on the single UNIQUE constraint in keyless mode — see §6.8) |
| UPDATE/DELETE … RETURNING | ✅ Supported |
| PK predicates (`pk = c` / `pk IN (…)` / `pk BETWEEN …` / `pk > c` / their `AND`/`OR` combinations) | ✅ Recommended path (goes through the PK index) |
| Non-PK predicates (arbitrary filter / Seq scan) | ✅ Supported |
| TEMP table operations inside a branch | ✅ Supported (whitelisted) |
| CREATE TABLE / ALTER / DROP / CREATE INDEX / DROP TRIGGER | ❌ 0A000 (execute back in Main) |
| Partitioned table | ❌ Rejected |
| FK cascade write | ❌ Rejected |
| COPY FROM / MERGE / Data-Modifying CTE | ❌ Rejected |
| UPDATE SET pk_col | ❌ Rejected (use DELETE + INSERT instead) |
| DML on a no-PK table | ❌ Rejected by default; ✅ when `overlay_branch.keyless_tables=on` (experimental, see §6.8) |

Handling suggestions for RESTRICTED items:

| Restricted item | User workaround |
| --- | --- |
| DDL | Execute the DDL in Main first → create_branch again |
| COPY FROM | Rewrite as a multi-row `INSERT INTO t VALUES (...), (...)` |
| MERGE | Split into separate INSERT ON CONFLICT / UPDATE / DELETE |
| Data-Modifying CTE | Change to top-level DML + RETURNING |
| UPDATE pk | Change to `DELETE WHERE pk=old` + `INSERT VALUES (new_pk, ...)` |
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

#### 6.3.3 DML Blocking During the Applying State

While apply_branch() is running, the state first becomes `applying` and changes to `applied` only after the merge succeeds.
DML in that branch from other sessions is blocked during this window:

```sql
INSERT INTO products VALUES (7, 'rush', 1);
ERROR:  overlay_branch: state of branch "b_large" is 'applying'; write to delta
             is forbidden while an apply is in progress. Please retry.
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

### 6.6 Fast Return for Empty Branch Apply/Discard

For an empty branch (delta is 0) that is **not currently active**, apply/discard returns quickly and does not emit the `NOTICE: overlay_branch: APPLY BRANCH ...` message:

```sql
DO $$
DECLARE
    _s timestamp; _ms int;
BEGIN
    FOR i IN 1..5 LOOP
        PERFORM public.create_branch('b_empty_'||i);
        _s := clock_timestamp();
        PERFORM public.apply_branch('b_empty_'||i);
        _ms := round(1000 * EXTRACT(EPOCH FROM (clock_timestamp()-_s)))::int;
        RAISE NOTICE 'empty apply #% elapsed=% ms', i, _ms;
    END LOOP;
END $$;
```

For non-empty branches or the currently active branch, apply/discard still takes the full path, and all NOTICEs are unchanged.

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
| UPSERT | `INSERT ... ON CONFLICT` arbitrates on the single `UNIQUE` index named in the statement (single- or multi-column); an arbiter that does not resolve to exactly one plain-column `UNIQUE` index (e.g. an expression index) is still rejected with `0A000` |

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
