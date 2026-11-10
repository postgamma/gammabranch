
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION overlay_branch" to load this file. \quit

-- ============================================================
-- Catalog tables for the extension
--   The extension lives in the fixed schema `overlay_branch'
--   (see overlay_branch.control).  Tables go there, never
--   into pg_catalog, because PG disallows unprivileged writes
--   into pg_catalog unless allow_system_table_modifications is
--   enabled superuser-only.
--
--   Every C-callable function body below carries
--     SET search_path = @extschema@, pg_catalog
--   so that the SPI queries inside C code (which use plain
--   "pg_branch" and "pg_branch_delta", unqualified) find them
--   without ambiguity.
-- ============================================================

CREATE SEQUENCE @extschema@.pg_branch_branch_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;

CREATE TABLE @extschema@.pg_branch (
    branch_id       integer NOT NULL DEFAULT nextval('@extschema@.pg_branch_branch_id_seq'),
    branch_name     name NOT NULL,
    owner           oid NOT NULL,
    created_at      timestamp with time zone NOT NULL DEFAULT now(),
    mode            text NOT NULL DEFAULT 'live',
    state           text NOT NULL DEFAULT 'active',
    CONSTRAINT pg_branch_pkey PRIMARY KEY (branch_id),
    CONSTRAINT pg_branch_name_ukey UNIQUE (branch_name)
);

ALTER SEQUENCE @extschema@.pg_branch_branch_id_seq OWNED BY @extschema@.pg_branch.branch_id;

CREATE TABLE @extschema@.pg_branch_delta (
    branch_id       integer NOT NULL,
    relid           oid NOT NULL,
    key             text NOT NULL,
    op              char(1) NOT NULL,
    old_version     text,
    tuple_data      bytea,
    created_at      timestamp with time zone NOT NULL DEFAULT now(),
    updated_at      timestamp with time zone NOT NULL DEFAULT now(),
    CONSTRAINT pg_branch_delta_pkey PRIMARY KEY (branch_id, relid, key),
    CONSTRAINT pg_branch_delta_op_check CHECK (op IN ('I', 'U', 'D'))
);

-- NOTE: no separate (branch_id, relid) index.  Every delta access is a
-- strict prefix of the PRIMARY KEY (branch_id, relid, key) -- point
-- lookup by full key, or prefix scan/aggregate/delete by (branch_id,
-- relid).  The pkey already serves all of them, and a narrower
-- (branch_id, relid) index would be redundant.  Worse, when planner
-- statistics on this internal table are stale (reltuples = -1, common
-- here because autovacuum rarely ANALYZEs it), the planner prefers the
-- narrower redundant index and demotes the key equality to a Filter,
-- turning the per-row delta existence probe on the write path from an
-- O(1) pkey point lookup into an O(delta) scan.  See doc/internals.md §5.4
-- for the before/after numbers.

-- ============================================================
-- §Gx / A7: Schema-epoch registry.
--
--   anti-drift hard guard: at create_branch() time, we scan every
--   user permanent table currently in the database (same filtering
--   rules as G01 create_branch scope: no system namespaces, no ext
--   schema, no objects owned by this extension) and record, for each
--   user relation, a compact 4-tuple fingerprint:
--     (relid, total_cols, pk_cols, md5(concat_ws('|', sorted attnames
--            with typbyval/typlen/typmod/collid and pk ordinality))).
--
--   registry rows are keyed by (branch_id, relid).  Before we allow a
--   session to enter a branch (use_branch/apply_branch), we re-run the
--   same scan and compare.  Any mismatch on total_cols / pk_cols /
--   hash → 55000 OBJECT_NOT_IN_PREREQUISITE_STATE — the DB schema has
--   drifted between create and enter, so MAIN/delta joins are no
--   longer valid.  This catches (a) out-of-band ALTER TABLE on a
--   parallel connection; (b) column type/pk reorder from restore
--   scripts; (c) any future DDL statement the ProcessUtility guard
--   list might temporarily miss.
--
--   On discard/remove branch we DELETE rows via bid (single statement
--   after the pg_branch_delta cascade delete; same SPI connection).
--
--   col_hash / pk_hash are stored as 16-byte MD5 bytea; total_cols
--   and pk_cols are included as plain integers so the error message
--   can tell the user "drift: total_cols 5→6, pk_cols 2→1" without
--   needing to decode a hash.
-- ============================================================
CREATE TABLE @extschema@.pg_branch_registry (
    branch_id       integer NOT NULL,
    relid           oid NOT NULL,
    schema_name     name NOT NULL,
    table_name      name NOT NULL,
    total_cols      integer NOT NULL,
    pk_cols         integer NOT NULL,
    col_hash        bytea NOT NULL,
    pk_hash         bytea NOT NULL,
    created_at      timestamp with time zone NOT NULL DEFAULT now(),
    CONSTRAINT pg_branch_registry_pkey PRIMARY KEY (branch_id, relid),
    CONSTRAINT pg_branch_registry_bid_fkey FOREIGN KEY (branch_id)
        REFERENCES @extschema@.pg_branch(branch_id) ON DELETE CASCADE
);

CREATE INDEX pg_branch_registry_bid_idx
    ON @extschema@.pg_branch_registry (branch_id);

COMMENT ON TABLE @extschema@.pg_branch_registry IS
  '§Gx/A7 schema epoch registry: per-create_branch fingerprint of every user table. Discard/recreate branch forces a fresh fingerprint snapshot.';
COMMENT ON COLUMN @extschema@.pg_branch_registry.total_cols IS 'Number of user columns (attnum>0, attisdropped=false) at create_branch time';
COMMENT ON COLUMN @extschema@.pg_branch_registry.pk_cols IS 'Cardinality of PRIMARY KEY (0 if table has no PK) at create_branch time';
COMMENT ON COLUMN @extschema@.pg_branch_registry.col_hash IS 'MD5 of sorted (attname|typid|typlen|typbyval|typmod|collid|attnotnull|attgenerated) for all user columns';
COMMENT ON COLUMN @extschema@.pg_branch_registry.pk_hash IS 'MD5 of sorted attname in PK (pk_cols=0 → digest of empty string)';

-- §G02/R22 ACL: lifecycle functions are SECURITY INVOKER, so plain callers
-- need SQL-level privileges on @extschema@.pg_branch_registry when C code
-- runs SPI SELECT/INSERT/DELETE.  The registry stores metadata only (no
-- tuple payload), so SELECT/INSERT/DELETE to public is safe (tampering is
-- self-sabotage, not a data leak).
REVOKE ALL ON @extschema@.pg_branch_registry FROM PUBLIC;
GRANT SELECT, INSERT, DELETE ON @extschema@.pg_branch_registry TO PUBLIC;
-- ============================================================
-- §G06 minimal leak prevention: explicitly mark internal tables publish=false (PG >=16: publication-level only)
--
-- Note: in PG 14-17 reloptions "publish=false" is **not a valid heap reloption** (publish
-- is a WITH option of the publication object, controlling which kind of DML events
-- 'insert/update/delete/truncate' that publication emits; it applies to pg_publication, not pg_class).
-- PG 14-17 FOR ALL TABLES selection set is filtered only by is_publishable_class():
--   - (RELKIND_RELATION / RELKIND_PARTITIONED_TABLE)
--   - !IsCatalogRelationOid(relid)  → must not be a pinned OID
--   - RELPERSISTENCE_PERMANENT
--   - relid >= FirstNormalObjectId
-- Tables created during extension install have a relid satisfying FirstNormalObjectId; a pinned
-- OID scheme is also infeasible (CREATE EXTENSION uses GetNewObjectId()).
--
-- Therefore G06 can only rely on RUNTIME detection + documentation:
--   - E-2 ob_g06_check_publication_leak_throttled() at create/enter/apply/
--     discard entry points finds puball=true or pubschema containing the overlay_branch schema
--     and emits WARNING (S26 productization may upgrade to ERROR);
--   - Here we only keep a placeholder reloption comment, no SET publish=false (otherwise CREATE EXTENSION
--     ERROR: unrecognized parameter "publish").
-- ============================================================
COMMENT ON TABLE @extschema@.pg_branch IS
  'overlay_branch internal branch catalog table (§G06: exclude from FOR ALL TABLES publication by site-local policy)';
COMMENT ON TABLE @extschema@.pg_branch_delta IS
  'overlay_branch internal per-branch per-table delta store (§G06: exclude from FOR ALL TABLES publication by site-local policy)';

-- ============================================================
-- §D4 / A9: identity / restore metadata on pg_branch.
--
--   4 restore-oriented metadata columns appended to pg_branch (default NULL
--   so existing branches before this upgrade are fully ABI-compatible, no
--   migration path required).  Filled at create_branch time by
--   overlay_branch_create_internal via a single SELECT over the same A7
--   registry fingerprint enumeration, then exposed by list_branches().
--
--   (1) schema_hash bytea:
--       MD5 of sorted (<schema_name>|<table_name>|<total_cols>|<pk_cols>
--       |hex(col_hash)|hex(pk_hash)) over every current user table at
--       create-branch entry-time.  This is a cheap global hash that a
--       backup-restore pipeline can compare to re-prove "we restored the
--       exact same schema shape the branch was created against".
--
--   (2) creation_snapshot_xmin xid:
--       GetTopTransactionId() / GetCurrentTransactionId() at
--       create_branch time.  Used together with schema_hash as the
--       authoritative "this is exactly the MAIN baseline the branch was
--       forked from" id.
--
--   (3) tablespace_list text:
--       Sorted distinct spcname list (comma-separated) of all tablespaces
--       used by user tables at create-branch entry-time.  Empty string if
--       all tables are in pg_default.  Backup/restore can re-create the
--       same tablespace layout before restore.
--
--   (4) col_signature bytea:
--       MD5 of sorted (<schema.table>|attnum|attname|typid|typmod|
--       attnotnull|attgenerated|pk_ordinality) over every user-column in
--       every user relation.  This is a finer-grained version of
--       per-table col_hash in the registry, aggregated to a single branch
--       value so package restore utilities can do one bytea compare.
-- ============================================================
ALTER TABLE @extschema@.pg_branch
  ADD COLUMN schema_hash bytea,
  ADD COLUMN creation_snapshot_xmin xid,
  ADD COLUMN tablespace_list text,
  ADD COLUMN col_signature bytea;

COMMENT ON COLUMN @extschema@.pg_branch.schema_hash
  IS '§D4/A9 global schema fingerprint at create-branch (MD5 of per-table registry 6-tuples)';
COMMENT ON COLUMN @extschema@.pg_branch.creation_snapshot_xmin
  IS '§D4/A9 txid of the transaction that ran create_branch(); used as baseline fork identity';
COMMENT ON COLUMN @extschema@.pg_branch.tablespace_list
  IS '§D4/A9 sorted comma-separated list of tablespace names used by user tables (empty = pg_default only)';
COMMENT ON COLUMN @extschema@.pg_branch.col_signature
  IS '§D4/A9 aggregate column signature (MD5 of every (schema.table,attnum,name,type) tuple)';


-- ============================================================
-- G01 support-matrix create_branch scope RESTRICTED list.
--
--   overlay_branch_create_internal runs ob_check_branch_scope() as
--   its FIRST executable statement after static name-length validation
--   but BEFORE touching pg_branch / pg_branch_branch_id_seq.  If ANY
--   user permanent relation in the database falls into one of the
--   categories below, create_branch() raises ERRCODE_FEATURE_NOT_SUPPORTED
--   (0A000) with a DETAIL pointing at the FIRST offending relation, and
--   with ZERO side effects (pg_branch_branch_id_seq is never advanced,
--   no catalog rows inserted).  Categories:
--     (A) FKs, any side (PK side or referencing side).  FK checks fire
--         MAIN-side only and would silently miss branch-side violations.
--     (B) Non-internal, non-disabled user TRIGGERs.  Trigger functions
--         run MAIN-only and miss rows queued in pg_branch_delta.
--     (C) PARTITIONED tables (parents relkind p/I OR any relispartition
--         leaf).  Relid-based MAIN/delta joins mix child relids wrongly
--         during APPLY.
--     (D) INHERITANCE relations (inhparent or inhrelid in pg_inherits).
--     (E) UNLOGGED permanent tables; no WAL, VACUUM semantics differ.
--     (F) GENERATED STORED columns; apply promotion skips the expression.
--     (G) FDW tables (f), views (v), matviews (m), composite
--         types-as-tables (c), toast (t), sequences/anything else
--         that is NOT relkind in {r=ordinary table, S=sequence, i=index}.
-- ============================================================

-- ============================================================
-- SQL-callable (C-language) management functions.
-- We also publish them via public synonyms so callers can use
-- bare names (SELECT create_branch('b1')) without prefix.
-- ============================================================

CREATE FUNCTION @extschema@.create_branch(branch_name name)
RETURNS integer
AS 'MODULE_PATHNAME', 'overlay_branch_create'
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

CREATE FUNCTION @extschema@.use_branch(branch_name name)
RETURNS void
AS 'MODULE_PATHNAME', 'overlay_branch_use'
LANGUAGE C VOLATILE
SET search_path = @extschema@, pg_catalog;
COMMENT ON FUNCTION @extschema@.use_branch(name) IS
'Enter a branch by name (live mode) or exit by passing NULL/empty string.';

-- ============================================================
-- 2-parameter use_branch(name, mode) with MVCC mode override.
--   mode = 'live'    : reads on MAIN use current snapshot (live behaviour)
--   mode = 'snapshot': reads on MAIN freeze at the snapshot taken when
--                      this function is called; see the documentation.
-- OVERLOAD RESOLUTION NOTE: mode has NO SQL DEFAULT so use_branch('foo')
--   UNIQUELY resolves to the 1-parameter form above; without this PG 17
--   raises "ERROR: function use_branch(unknown) is not unique".
-- ============================================================
CREATE FUNCTION @extschema@.use_branch(branch_name name, mode text)
RETURNS void
AS 'MODULE_PATHNAME', 'overlay_branch_use_with_mode'
LANGUAGE C VOLATILE
SET search_path = @extschema@, pg_catalog;

COMMENT ON FUNCTION @extschema@.use_branch(name, text) IS
'Enter a branch with an explicit MVCC mode.
mode = ''live''    : reads on MAIN use current snapshot (live behaviour).
mode = ''snapshot'': reads on MAIN freeze at the MVCC snapshot taken
                     when this function is called; see the documentation.';

CREATE FUNCTION @extschema@.current_branch()
RETURNS name
AS 'MODULE_PATHNAME', 'overlay_branch_current'
LANGUAGE C STRICT STABLE
SET search_path = @extschema@, pg_catalog;

CREATE FUNCTION @extschema@.apply_branch(branch_name name)
RETURNS void
AS 'MODULE_PATHNAME', 'overlay_branch_apply'
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

CREATE FUNCTION @extschema@.discard_branch(branch_name name)
RETURNS void
AS 'MODULE_PATHNAME', 'overlay_branch_discard'
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

-- NOTE: list_branches is intentionally implemented in pure SQL rather
-- than C for Step 1.  The naive C SRF had a number of stability
-- problems (deep-copying SPI datum values by hand is error-prone and
-- produced SIGSEGV / all-NULL rows in several builds).  The query
-- here is semantically identical to what the C stub used to do: a
-- LEFT JOIN between pg_branch and the per-branch delta count.  Since
-- the extension's catalog tables live in @extschema@ and we run with
-- the fixed search_path, this SQL version is also read-only and
-- needs no upgrade to remain composable with later Step code.
CREATE FUNCTION @extschema@.overlay_branch_cached_mode(bid integer)
RETURNS text
AS 'MODULE_PATHNAME', 'overlay_branch_cached_mode'
LANGUAGE C STABLE STRICT
SET search_path = @extschema@, pg_catalog;

DROP FUNCTION IF EXISTS @extschema@.list_branches();
CREATE FUNCTION @extschema@.list_branches()
RETURNS TABLE(
    branch_id integer,
    branch_name name,
    owner oid,
    created_at timestamp with time zone,
    mode text,
    state text,
    delta_count bigint,
    schema_hash bytea,
    creation_snapshot_xmin xid,
    tablespace_list text,
    col_signature bytea
)
LANGUAGE sql STABLE STRICT
SET search_path = @extschema@, pg_catalog
AS $$
    SELECT b.branch_id,
           b.branch_name,
           b.owner,
           b.created_at,
           COALESCE(@extschema@.overlay_branch_cached_mode(b.branch_id), b.mode) AS mode,
           b.state,
           COALESCE(d.cnt, 0)::bigint AS delta_count,
           b.schema_hash,
           b.creation_snapshot_xmin,
           b.tablespace_list,
           b.col_signature
    FROM pg_branch b
    LEFT JOIN (SELECT branch_id, count(*) AS cnt
               FROM pg_branch_delta
               GROUP BY branch_id) d
      ON d.branch_id = b.branch_id
    ORDER BY b.branch_id;
$$;

-- ============================================================
-- §5.6 B1: branch_delta_stats(branch_name) — read-only delta-size
-- observability.  Returns one row per (branch, relation) that has
-- delta rows, exposing counts and byte sizes only (never delta row
-- content).  Access mirrors list_branches(): it reads the delta
-- catalog, which is administrator-only per R22, so SECURITY INVOKER
-- is retained and a plain user gets permission denied, exactly as
-- with list_branches().  See doc/internals.md §5.6.
-- ============================================================
DROP FUNCTION IF EXISTS @extschema@.branch_delta_stats(name);
CREATE FUNCTION @extschema@.branch_delta_stats(b_name name)
RETURNS TABLE(
    branch_name name,
    relid oid,
    relation text,
    delta_rows bigint,
    delta_insert bigint,
    delta_update bigint,
    delta_delete bigint,
    delta_payload_bytes bigint,
    main_bytes bigint,
    amplification numeric
)
LANGUAGE sql STABLE STRICT
SET search_path = @extschema@, pg_catalog
AS $$
    WITH stats AS (
        SELECT d.branch_id,
               d.relid,
               count(*)::bigint                                   AS delta_rows,
               count(*) FILTER (WHERE d.op = 'I')::bigint         AS delta_insert,
               count(*) FILTER (WHERE d.op = 'U')::bigint         AS delta_update,
               count(*) FILTER (WHERE d.op = 'D')::bigint         AS delta_delete,
               (COALESCE(sum(octet_length(d.key)), 0)
                + COALESCE(sum(octet_length(d.old_version)), 0)
                + COALESCE(sum(octet_length(d.tuple_data)), 0))::bigint AS payload_bytes
        FROM @extschema@.pg_branch_delta d
        GROUP BY d.branch_id, d.relid
    )
    SELECT b.branch_name,
           s.relid,
           pg_catalog.format('%I.%I', n.nspname, c.relname) AS relation,
           s.delta_rows,
           s.delta_insert,
           s.delta_update,
           s.delta_delete,
           s.payload_bytes AS delta_payload_bytes,
           sz.main_bytes,
           CASE WHEN sz.main_bytes > 0
                THEN pg_catalog.round(s.payload_bytes::numeric / sz.main_bytes, 6)
                ELSE NULL
           END AS amplification
    FROM @extschema@.pg_branch b
    JOIN stats s ON s.branch_id = b.branch_id
    LEFT JOIN pg_catalog.pg_class c ON c.oid = s.relid
    LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
    CROSS JOIN LATERAL (
        SELECT pg_catalog.pg_total_relation_size(s.relid) AS main_bytes
    ) sz
    WHERE b.branch_name = b_name
    ORDER BY s.relid;
$$;

-- ============================================================
-- §D4 / A9: get_branch_identity(branch_name) — convenient helper
-- that returns a ROW of restore-metadata for a named branch, plus
-- the global A7 schema drift check to confirm the branch can still
-- be entered in this database.
--
-- Return columns (no C SRF overhead; pure SQL so ABI-stable):
--   branch_id, schema_hash, creation_snapshot_xmin, tablespace_list,
--   col_signature, registry_user_tables, registry_schema_current_match
-- ============================================================
CREATE FUNCTION @extschema@.get_branch_identity(b_name name)
RETURNS TABLE(
    branch_id integer,
    schema_hash bytea,
    creation_snapshot_xmin xid,
    tablespace_list text,
    col_signature bytea,
    registry_user_tables bigint,
    registry_schema_current_match boolean
)
LANGUAGE sql STABLE STRICT
SET search_path = @extschema@, pg_catalog
AS $$
    WITH br AS (
        SELECT b.branch_id AS bid,
               b.schema_hash,
               b.creation_snapshot_xmin,
               b.tablespace_list,
               b.col_signature
        FROM @extschema@.pg_branch b
        WHERE b.branch_name = b_name
    ),
    cur_fp AS (
        SELECT count(*)                                             AS _n,
               decode(md5(string_agg(concat_ws('|',
                   s.schema_name||'.'||s.table_name,
                   a.attnum::text, a.attname, a.atttypid::text,
                   a.atttypmod::text, a.attnotnull::text,
                   a.attgenerated::text,
                   COALESCE(pk.pk_ord::text, '0')), ''
                   ORDER BY s.relid, a.attnum)), 'hex')::bytea       AS _col_sig
        FROM (
            SELECT c.oid AS relid,
                   n.nspname::name AS schema_name,
                   c.relname::name AS table_name
            FROM pg_catalog.pg_class c
            JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
            WHERE c.relkind = 'r'
              AND c.relpersistence = 'p'
              AND n.nspname NOT IN ('pg_catalog', 'information_schema', 'pg_toast')
              AND n.nspname <> '@extschema@'
              AND NOT EXISTS (
                  SELECT 1 FROM pg_catalog.pg_depend d
                   WHERE d.classid = 'pg_catalog.pg_class'::regclass
                     AND d.objid = c.oid
                     AND d.deptype = 'e'
                     AND d.refobjid = (SELECT oid FROM pg_catalog.pg_extension
                                        WHERE extname = 'overlay_branch'))
        ) s
        LEFT JOIN pg_catalog.pg_attribute a
               ON a.attrelid = s.relid AND a.attnum > 0 AND NOT a.attisdropped
        LEFT JOIN (
            SELECT i.indrelid, k.n AS pk_ord, a.attnum
              FROM pg_catalog.pg_index i
              LEFT JOIN pg_catalog.generate_subscripts(i.indkey, 1) k(n) ON true
              LEFT JOIN pg_catalog.pg_attribute a
                     ON a.attrelid = i.indrelid
                    AND a.attnum = i.indkey[k.n]
                    AND NOT a.attisdropped
             WHERE i.indisprimary
        ) pk ON pk.indrelid = s.relid AND pk.attnum = a.attnum
    )
    SELECT br.bid,
           br.schema_hash,
           br.creation_snapshot_xmin,
           br.tablespace_list,
           br.col_signature,
           COALESCE(cur_fp._n, 0)::bigint,
           CASE
             WHEN br.col_signature IS NULL THEN NULL
             ELSE COALESCE(br.col_signature = cur_fp._col_sig, false)
           END
    FROM br
    LEFT JOIN cur_fp ON true;
$$;
REVOKE ALL ON FUNCTION @extschema@.get_branch_identity(name) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION @extschema@.get_branch_identity(name) TO PUBLIC;

-- public synonym for UX (match user_guide.md public prefix-less UX)
DROP FUNCTION IF EXISTS public.get_branch_identity(name);
CREATE FUNCTION public.get_branch_identity(b_name name)
RETURNS TABLE(
    branch_id integer,
    schema_hash bytea,
    creation_snapshot_xmin xid,
    tablespace_list text,
    col_signature bytea,
    registry_user_tables bigint,
    registry_schema_current_match boolean
)
LANGUAGE sql STABLE STRICT
SET search_path = @extschema@, pg_catalog, pg_temp
AS $$SELECT @extschema@.get_branch_identity(b_name)$$;
REVOKE ALL ON FUNCTION public.get_branch_identity(name) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION public.get_branch_identity(name) TO PUBLIC;


CREATE FUNCTION @extschema@.overlay_main_plus_delta(regclass)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'overlay_main_plus_delta'
LANGUAGE C STRICT STABLE
SET search_path = @extschema@, pg_catalog;

-- ============================================================
-- Debug SQL-callable wrappers for Delta Store (Step 2c smoke)
--
-- These expose the C overlay_delta_* internal SPI path so it
-- can be exercised from psql before the Write Redirect hooks
-- are wired in.  Signature names are deliberately ugly to
-- discourage end-user use; they can be dropped in future by
-- creating an overlay_branch--0.1--0.2.sql upgrade script.
-- ============================================================

CREATE FUNCTION @extschema@.overlay_debug_delta_insert(
    branch_id integer,
    relid oid,
    key text,
    op "char",
    old_version text,
    tuple_data bytea
) RETURNS text
AS 'MODULE_PATHNAME', 'overlay_debug_delta_insert'
LANGUAGE C VOLATILE
SET search_path = @extschema@, pg_catalog;
COMMENT ON FUNCTION @extschema@.overlay_debug_delta_insert IS
'NOT STRICT: old_version and tuple_data are legitimately NULL (DELETE, no base version yet).';

CREATE FUNCTION @extschema@.overlay_debug_delta_count(
    branch_id integer
) RETURNS bigint
AS 'MODULE_PATHNAME', 'overlay_debug_delta_count'
LANGUAGE C STRICT STABLE
SET search_path = @extschema@, pg_catalog;

CREATE FUNCTION @extschema@.overlay_debug_delta_delete_all(
    branch_id integer
) RETURNS void
AS 'MODULE_PATHNAME', 'overlay_debug_delta_delete_all'
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

-- ============================================================
-- Task 2 Layer 1 (FR1 + FR4): throttled catalog recheck
--   * overlay_branch_is_active(name) RETURNS bool
--       - returns ground truth from pg_branch catalog, performing
--         throttled heavy SPI recheck if enough calls have elapsed
--         (see GUCs overlay_branch.invalidation_check_threshold and
--         overlay_branch.invalidation_check_interval_ms).
--       - NOT STRICT / VOLATILE per project conventions: we need to
--         handle NULL inputs manually and recheck state each call.
--   * overlay_branch_force_invalidation_check() RETURNS bool
--       - user-visible "check NOW" bypass: forces a heavy SPI recheck
--         regardless of throttle counter / elapsed time.
--       - returns true if the branch context remained active AFTER the
--         recheck; false and NOTICE when rechecked and found inactive.
--   * NOTIFY channel "ob_branch_state" payload format "<bid>:<new_state>"
--     is broadcast from apply_branch / discard_branch C entry points
--     after catalog mutations commit; no SQL entry needed (registered by
--     _PG_init / called from branch_lifecycle.c).
-- ============================================================

CREATE FUNCTION @extschema@.overlay_branch_is_active(name name)
RETURNS bool
LANGUAGE C VOLATILE SET search_path = ''
AS 'MODULE_PATHNAME', 'overlay_branch_is_active_by_name';

CREATE FUNCTION @extschema@.overlay_branch_force_invalidation_check()
RETURNS bool
LANGUAGE C VOLATILE SET search_path = ''
AS 'MODULE_PATHNAME', 'overlay_branch_force_invalidation_check';

-- ============================================================
-- §G02 ACL (S13) — double-layer defense
--
--   Layer 1 (this file, SQL):
--     1. Builtin role overlay_branch_administrators — admin-level actors.
--     2. Internal catalog tables pg_branch / pg_branch_delta live in the
--        extension schema overlay_branch; ALL default PUBLIC privileges
--        on them are revoked (they get none because they're freshly
--        created, but an explicit REVOKE makes our intent permanent and
--        audit-friendly).
--     3. public.pg_branch view is still readable by PUBLIC (it only
--        carries metadata: name, state, mode, counts, owner oid).
--        public.pg_branch_delta view is revoked from PUBLIC — delta
--        rows contain full user-visible data; only the administrator
--        role may read/write it directly.
--     4. public.* lifecycle-function synonyms keep PUBLIC EXECUTE so the
--        enforcement point is single and unambiguous: the C-level
--        ob_acl_check_lifecycle() gate, which is unharmed by qualification
--        games (@extschema@ vs public).  Non-admin callers therefore get a
--        uniform 42501 rather than a raw table-level denial.
--
--   Layer 2 (C code):
--     See branch_lifecycle.c ob_acl_check_lifecycle(OB_OP_*, name).
--     The current user is re-checked inside C code, which means
--     (superuser OR mgmt role) is the final truth-table regardless of
--     how the function was invoked.
--
--   Privilege matrix (explicit because roles may be installed later at
--   upgrade time).  Branch administration is privileged end to end: the
--   pg_branch / pg_branch_delta catalogs carry DML only for the
--   administrator role, so a plainer caller could never complete the
--   underlying work regardless of the C gate:
--     create_branch                         -> superuser | mgmt
--     use_branch / use_branch(name, mode)   -> superuser | mgmt
--         EXCEPTION: use_branch(NULL/empty) -> always allowed (exit branch)
--     apply_branch / discard_branch         -> superuser | mgmt
--
-- NOTE: "overlay_branch_administrators" is the ONLY role recognised by
-- both layers.  DO NOT rename it without also updating the C macro
-- OB_MGMT_ROLE_NAME in include/overlay_branch.h and all grep hits.
-- ============================================================

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_roles WHERE rolname = 'overlay_branch_administrators') THEN
        CREATE ROLE overlay_branch_administrators NOLOGIN;
        COMMENT ON ROLE overlay_branch_administrators IS
            'Members may create overlay branches and administer any branch (use/apply/discard). Branch creation and administration are restricted to this role and superusers.';
    END IF;
END$$;

REVOKE ALL ON TABLE @extschema@.pg_branch       FROM PUBLIC;
REVOKE ALL ON TABLE @extschema@.pg_branch_delta FROM PUBLIC;
REVOKE ALL ON SEQUENCE @extschema@.pg_branch_branch_id_seq FROM PUBLIC;
-- SCHEMA @extschema@ USAGE is GRANTed to PUBLIC so that SECURITY INVOKER
-- public.* synonym wrappers (which SET search_path TO @extschema@, pg_catalog)
-- can resolve @extschema@.<c_fn> inside their body even when the caller is
-- a plain non-superuser without any explicit privileges on the extension
-- schema.
GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;
-- §G02 S13 catalog-level permissions (on top of public.* synonym views):
--   * @extschema@.pg_branch (TABLE) — branch metadata.  Administrator role
--     needs full DML + SELECT because create_branch_internal() INSERTs here
--     as the caller (SPI runs as GetUserId()).  SELECT is opened to PUBLIC
--     because the public.pg_branch VIEW already exposes the same columns;
--     the explicit base-table grant keeps audit symmetric.
--   * @extschema@.pg_branch_delta (TABLE) + sequence — administrator role
--     only; raw table-level access is revoked from PUBLIC (R22
--     business-data no-leak).  Branch writes flow through the C wrappers,
--     which run as the caller, so the administrator role is the single
--     authority for delta I/O.
GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE @extschema@.pg_branch       TO overlay_branch_administrators;
GRANT SELECT                                   ON TABLE @extschema@.pg_branch       TO PUBLIC;
GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE @extschema@.pg_branch_delta TO overlay_branch_administrators;
GRANT ALL   ON SEQUENCE @extschema@.pg_branch_branch_id_seq               TO overlay_branch_administrators;

-- ============================================================
-- Public aliases (views for tables, plain functions exposed)
-- ============================================================

-- Tables are private to extension schema.  Read-only views are
-- published for debugging / SELECT convenience.

CREATE OR REPLACE VIEW public.pg_branch
    WITH (security_barrier = true)
    AS SELECT * FROM @extschema@.pg_branch;
GRANT SELECT ON public.pg_branch TO PUBLIC;

CREATE OR REPLACE VIEW public.pg_branch_delta
    WITH (security_barrier = true)
    AS SELECT * FROM @extschema@.pg_branch_delta;
-- §G02 R22: delta rows carry business data — do not grant SELECT to
-- PUBLIC.  Keep the wrapper view in place so site-local upgrades can
-- selectively open it up for audit roles; default is NO ACCESS.
REVOKE ALL ON public.pg_branch_delta FROM PUBLIC;

-- ============================================================
-- Public synonyms for convenience.
--   Extensions with schema != public must still be usable from
--   plain `SELECT create_branch('b1')` to match the documented
--   UX in doc/user_guide.md.  Since PostgreSQL has no CREATE
--   SYNONYM we build SQL-language wrappers in the public schema
--   that just forward to the real @extschema@ functions.
--   search_path visibility: wrappers are in public, which is in
--   every user's default search_path.
--   IMPORTANT: we also add @extschema@ to the database's
--   search_path via ALTER DATABASE below is overkill.  Just
--   wrappers suffice.
--
--   §G02 S13 double-defense design (SQL-layer + C-layer):
--   - Lifecycle wrappers (create/use/apply/discard_branch*) are
--     declared SECURITY INVOKER (default) with the search_path
--     fixed to (@extschema@, pg_catalog).  SCHEMA @extschema@ has
--     USAGE granted to PUBLIC (see above), so the wrappers can
--     resolve their body references even for plain users.
--   - A second and non-bypassable ACL check
--     (ob_acl_check_lifecycle) is run at the TOP of every
--     C-exported wrapper using GetUserId() — this is the actual
--     superuser/mgmt-role gate, so even the extension
--     installation superuser cannot bypass it.
--   - Metadata-only helpers (current_branch/list_branches/
--     is_active/force_invalidation_check) stay SECURITY INVOKER
--     and PUBLIC EXECUTE; they never mutate state and only
--     return information the caller could otherwise see via
--     public views.
-- ============================================================

CREATE OR REPLACE FUNCTION public.create_branch(branch_name name)
RETURNS integer LANGUAGE sql VOLATILE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.create_branch(branch_name)$$;

CREATE OR REPLACE FUNCTION public.use_branch(branch_name name)
RETURNS void LANGUAGE sql VOLATILE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.use_branch(branch_name)$$;

CREATE OR REPLACE FUNCTION public.current_branch()
RETURNS name LANGUAGE sql STABLE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.current_branch()$$;





/* Thin wrappers.  apply_branch/discard_branch MUST always run the canonical
 * C implementation: it is the only path that takes the branch advisory lock,
 * CAS-guards the state transition, runs the schema-drift guard, broadcasts
 * the state change (NOTIFY) and flushes the session snapshot/mode caches.
 * An SQL fast path for the empty-branch case silently skipped all of that and
 * could race a concurrent writer into leaving orphan delta rows, so no such
 * shortcut exists here. */
DROP FUNCTION IF EXISTS public.apply_branch(name);
CREATE FUNCTION public.apply_branch(_name name)
RETURNS void LANGUAGE sql VOLATILE
SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.apply_branch(_name)$$;

DROP FUNCTION IF EXISTS public.discard_branch(name);
CREATE FUNCTION public.discard_branch(_name name)
RETURNS void LANGUAGE sql VOLATILE
SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.discard_branch(_name)$$;


CREATE OR REPLACE FUNCTION public.use_branch(branch_name name, mode text)
RETURNS void LANGUAGE sql VOLATILE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.use_branch(branch_name, mode)$$;

CREATE OR REPLACE FUNCTION public.is_active(name name)
RETURNS bool
SET search_path = @extschema@, pg_catalog, pg_temp
LANGUAGE sql
AS $$
    SELECT @extschema@.overlay_branch_is_active(name);
$$;

CREATE OR REPLACE FUNCTION public.force_invalidation_check()
RETURNS bool
SET search_path = @extschema@, pg_catalog, pg_temp
LANGUAGE sql
AS $$
    SELECT @extschema@.overlay_branch_force_invalidation_check();
$$;

DROP FUNCTION IF EXISTS public.list_branches();
CREATE FUNCTION public.list_branches()
RETURNS TABLE(branch_id integer, branch_name name, owner oid,
              created_at timestamp with time zone, mode text,
              state text, delta_count bigint,
              schema_hash bytea, creation_snapshot_xmin xid,
              tablespace_list text, col_signature bytea)
LANGUAGE sql STABLE SET search_path = @extschema@, pg_catalog
AS $$SELECT * FROM @extschema@.list_branches()$$;

DROP FUNCTION IF EXISTS public.branch_delta_stats(name);
CREATE FUNCTION public.branch_delta_stats(b_name name)
RETURNS TABLE(branch_name name, relid oid, relation text,
              delta_rows bigint, delta_insert bigint, delta_update bigint,
              delta_delete bigint, delta_payload_bytes bigint,
              main_bytes bigint, amplification numeric)
LANGUAGE sql STABLE SET search_path = @extschema@, pg_catalog
AS $$SELECT * FROM @extschema@.branch_delta_stats(b_name)$$;

-- NOTE: overlay_main_plus_delta() is intentionally NOT wrapped in public
-- for Step 1.  Its RETURNS SETOF record requires a column-definition
-- list at call time (e.g. FROM overlay_branch.overlay_main_plus_delta('t')
-- AS x(col type, ...)), which cannot be transparently forwarded through a
-- SQL-language wrapper.  Users can always qualify with the schema, and
-- Step 4 will introduce a more usable signature.

-- §G02 S13: lifecycle synonyms are declared SECURITY INVOKER with a fixed
-- search_path, so the C wrappers see the real caller.  PUBLIC EXECUTE is
-- kept on the synonyms; the non-bypassable ACL check lives at the very top
-- of each C-exported wrapper, which enforces the superuser/mgmt matrix and
-- raises a uniform 42501 for everyone else.
--
-- Metadata-only helpers stay PUBLIC EXECUTE by default (no-op).
GRANT EXECUTE ON FUNCTION public.create_branch(name)      TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.use_branch(name)         TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.apply_branch(name)       TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.discard_branch(name)     TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.use_branch(name, text)   TO PUBLIC;

-- Read-only helpers (metadata-only) remain PUBLIC-friendly.
GRANT EXECUTE ON FUNCTION public.current_branch()         TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.list_branches()          TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.branch_delta_stats(name) TO PUBLIC;

GRANT EXECUTE ON FUNCTION @extschema@.overlay_branch_is_active(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION @extschema@.overlay_branch_force_invalidation_check() TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.is_active(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.force_invalidation_check() TO PUBLIC;

COMMENT ON EXTENSION overlay_branch IS 'Overlay Branch - speculative database state using table overlay and delta store';
