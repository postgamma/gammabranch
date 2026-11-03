# Gamma Branch — A PostgreSQL extension for zero-copy, Git-branch style database branches

> ⚠️ **Do not use it in production.** This is an early-stage (v0.1)
> experimental extension. It is under active development: the public API, the
> catalog layout and the on-disk delta format may change without notice, and it
> has not been hardened for production workloads. Evaluate it in throw-away
> environments only.

## What is it

Git-branch style isolation for AI agents and draft-style workflows inside
PostgreSQL — no kernel changes required. It lets you create database-level draft branches **without copying the whole
data**.

Typical uses: AI agent workflows, interactive draft editing, multi-version data
comparison, A/B change rehearsal.

> The repository is named `gammabranch`; the extension it ships is named
> `overlay_branch`.

## 5-minute quick start

```sql
-- 0) Prerequisite: the extension is created and postgresql.conf sets shared_preload_libraries='overlay_branch'
CREATE EXTENSION IF NOT EXISTS overlay_branch;

-- 1) Create an ordinary table with a primary key (DML tables need a PK by default; PK-less tables require an experimental GUC, see "Limitations")
CREATE TABLE products (id INT PRIMARY KEY, name TEXT NOT NULL, price INT NOT NULL);
INSERT INTO products VALUES (1,'Apple',10), (2,'Banana',5), (3,'Cherry',20);

-- 2) Create a branch (zero copy — just one catalog row)
SELECT create_branch('my_draft');

-- 3) Enter the branch (session-scoped)
SELECT use_branch('my_draft');

-- 4) Write inside the branch — only pg_branch_delta changes, MAIN is physically untouched
INSERT INTO products VALUES (4,'Date',30);
UPDATE products SET price=100 WHERE id=1;
DELETE FROM products WHERE id=2;

-- 5) Read inside the branch: transparent MAIN ⊕ Delta merge
SELECT * FROM products ORDER BY id;
-- 1|Apple |100  (overridden)
-- 3|Cherry| 20  (as in MAIN)
-- 4|Date  | 30  (inserted)

-- 6) Back to MAIN — verify it is untouched
SELECT use_branch(NULL);
SELECT * FROM products ORDER BY id;
-- 1|Apple |10
-- 2|Banana| 5
-- 3|Cherry|20

-- 7) Confident? Apply the merge to MAIN
SELECT apply_branch('my_draft');
```

For full scenarios (Snapshot vs Live, cross-session kickout, pure delta,
RETURNING, UPSERT, DDL restrictions, ...) see [doc/user\_guide.md](doc/user_guide.md).

## Design overview

No kernel changes: a CustomScan + planner hook plus two catalog tables
(`pg_branch` / `pg_branch_delta`) provide an MVCC-layer incremental overlay.

```text
Client SQL
    │
    ▼
_PG_init() hooks
  - set_rel_pathlist  -> BranchScan
  - ExecutorRun       -> Write Redirect
  - ProcessUtility    -> DDL / COPY / MERGE gate
    │
    ├─ read path  -> BranchScan -> MAIN ⊕ Delta
    └─ write path -> redirect   -> Delta Store
                                  │
                                  ├─ MAIN heap tables
                                  └─ pg_branch / pg_branch_delta
```

For the internal design — the read/write overlay architecture, the BranchScan
algorithm, concurrency control, the experimental keyless ctid identity,
performance notes and the key invariants — see
[doc/internals.md](doc/internals.md).

## Documentation

| Document                                  | Description                                                                                           |
| ----------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| [doc/user\_guide.md](doc/user_guide.md)  | **User-facing**: the full SQL interface with expected output                                          |
| [doc/internals.md](doc/internals.md)      | Internal implementation: architecture, BranchScan, concurrency, keyless ctid, performance, invariants |

## Repository layout

```
gammabranch/
├── include/          # C headers
├── src/              # C sources (6 files)
├── test/
│   ├── regress/      # L3 pg_regress (21 suites)
│   ├── isolation/    # L1 pg_isolation_regress (15 specs)
│   └── bench/        # L2 pgbench stress harness
├── doc/              # Design and example documents
├── overlay_branch--0.1.sql
├── overlay_branch.control
├── Makefile
└── README.md
```

## Build & install

> Current supported PostgreSQL versions: **PostgreSQL 17** (the only major version tested
> so far). Other major versions are not yet validated — support will be added here
> as each one is tested.

This is a standalone PGXS extension:

```bash
git clone git@github.com:postgamma/gammabranch.git
cd gammabranch
# requires the pg_config shipped by postgresql-server-dev-17 to be on PATH
make USE_PGXS=1
sudo make USE_PGXS=1 install
```

> ⚠️ Before use you must set `shared_preload_libraries = 'overlay_branch'` in
> `postgresql.conf` and restart the postmaster. The extension installs its hooks
> in `_PG_init()`; without preloading, write redirection **silently fails**
> (writes go through to MAIN).

```sql
CREATE EXTENSION overlay_branch;
```

## Running the tests

```bash
# `make check` is unsupported by PGXS; run installcheck against a running instance instead
make USE_PGXS=1 install                 # install the extension
# make sure the instance sets shared_preload_libraries='overlay_branch' and has been restarted
make USE_PGXS=1 installcheck            # L3 + L1
```

> `make installcheck` needs a real, installed UTF-8 locale on the
> host (`LC_ALL=C.utf8`); a missing locale aborts with `invalid locale settings`,
> and forcing `LANG=C` produces bogus column-width differences.

## Limitations

This is a v0.1 release with a deliberately narrow scope. The main limits:

- Managed tables must be plain heap tables with a single-column `PRIMARY KEY`.
  - Partitioned / foreign tables, views, materialized views, foreign keys, multi-column PKs and non-PK `UNIQUE` constraints are rejected at `create_branch` time. 
  - PK-less tables need the experimental `overlay_branch.keyless_tables = on` GUC and a best-effort `ctid` identity that concurrent writes or `VACUUM FULL` / `CLUSTER` / `TRUNCATE` can invalidate.
- Blocked inside a branch (SQLSTATE 0A000): `COPY FROM`, `MERGE`,
  data-modifying CTEs, and `UPDATE`s that change the PK column.
- Apply is optimistic: if MAIN was written concurrently after the branch row
  was read, `apply_branch` raises a conflict error instead of merging.
- Snapshot mode is time-bounded: a snapshot session is forced back to MAIN
  after `overlay_branch.snapshot_max_hold_minutes` (default 10 min) so
  snapshotters cannot pin the MVCC horizon.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
