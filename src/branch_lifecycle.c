/*-------------------------------------------------------------------------
 *
 * branch_lifecycle.c
 *	  Overlay Branch lifecycle: create / use / current / apply / discard
 *	  internals plus 3-pass optimistic apply (DELETE → UPDATE → INSERT)
 *	  with per-row version-based conflict detection.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "commands/async.h"
#include "utils/timestamp.h"
#include "utils/syscache.h"
#include "utils/hsearch.h"
#include "access/xact.h"

#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

/* ---- External globals owned by overlay_branch.c ---- */
extern bool ob_in_apply_operation;
extern bool ob_in_guc_setconfig;

/* ================================================================
 * §G02 ACL helpers (S13) — double-layer defense
 *
 *   Layer 1 (SQL):  overlay_branch--0.1.sql revokes EXECUTE TO PUBLIC
 *                   on lifecycle functions; grants only to the builtin
 *                   role OB_MGMT_ROLE_NAME.
 *   Layer 2 (C  ):  this `ob_acl_check_lifecycle` helper, re-checked
 *                   inside every SQL-callable wrapper so direct
 *                   @extschema@.qualification bypasses of layer-1 still
 *                   get 42501.
 *
 *   Branch administration is privileged (superuser or OB_MGMT_ROLE):
 *   the delta/branch catalogs are DML-able only by that role, so a
 *   non-admin caller could never complete the underlying work anyway.
 *   Denying at the C layer keeps the SQLSTATE uniform (42501) instead
 *   of leaking a raw table-level "permission denied for table".
 * ================================================================ */

static Oid
ob_lookup_mgmt_role_oid(void)
{
	return GetSysCacheOid1(AUTHNAME,
						   Anum_pg_authid_oid,
						   CStringGetDatum(OB_MGMT_ROLE_NAME));
}

static bool
ob_user_is_superuser_or_mgmt(void)
{
	Oid			mgmt_oid;

	if (superuser())
		return true;

	mgmt_oid = ob_lookup_mgmt_role_oid();
	if (OidIsValid(mgmt_oid) && is_member_of_role(GetUserId(), mgmt_oid))
		return true;

	return false;
}

void
ob_acl_check_lifecycle(ObLifecycleOp op, const char *branch_name)
{
	const char *opname;
	bool		allowed;

	switch (op)
	{
		case OB_OP_CREATE_BRANCH:
			opname = "create_branch";
			allowed = ob_user_is_superuser_or_mgmt();
			break;

		case OB_OP_USE_BRANCH:
			/* EXIT path (NULL / empty name) is always permitted —
			 * it only touches the session's local context and the GUC. */
			if (branch_name == NULL || *branch_name == '\0')
				return;
			opname = "use_branch";
			allowed = ob_user_is_superuser_or_mgmt();
			break;

		case OB_OP_APPLY_BRANCH:
			opname = "apply_branch";
			allowed = ob_user_is_superuser_or_mgmt();
			break;

		case OB_OP_DISCARD_BRANCH:
			opname = "discard_branch";
			allowed = ob_user_is_superuser_or_mgmt();
			break;

		default:
			opname = "lifecycle_op";
			allowed = false;
			break;
	}

	if (!allowed)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for %s", opname),
				 errhint("Must be superuser or member of role \"%s\".",
						 OB_MGMT_ROLE_NAME)));
}

/* ================================================================
 * §G01 Branch-scope pre-flight (support matrix fail-fast gate).
 *
 * Implementation notes:
 *   We could walk pg_class via systable_beginscan(SysCacheRel) but a
 *   single SPI SELECT with EXISTS(...) subqueries against the
 *   standard catalog views is shorter, uses the same snapshot used
 *   by every other catalog helper in this file, and is exactly as
 *   performant (all subqueries are backed by primary btree indexes on
 *   pg_class / pg_constraint / pg_trigger / pg_inherits /
 *   pg_attribute / pg_namespace).  Performance is not an issue
 *   because scope checks run ONCE per create_branch() call, not per
 *   DML.
 *
 *   Output row: 1-column text "CATEGORY|relname.nspname" describing
 *   the FIRST offending relation.  We fail-fast on the very first
 *   row because showing a 100-row list of violations overwhelms the
 *   user.  If the user fixes that one and re-runs, the next hit is
 *   shown (like a standard compiler).
 * ================================================================ */
/* ================================================================
 * G01 Branch-scope pre-flight (support matrix fail-fast gate).
 *
 * Implementation notes:
 *   - Prefer flat scalar subqueries against pg_extension/pg_namespace/
 *     pg_depend.  Category 9 uses a WITH RECURSIVE, but it is deliberately
 *     local (inside an IN() subquery) and uncorrelated -- a correlated or
 *     top-level WITH CTE combined with SPI_execute(readonly=false) was seen
 *     failing silently, so keep CTEs self-contained.  Category 9's
 *     recursive set walks BOTH edges out of citext -- domain over
 *     (typbasetype) and array-of (typelem, typlen = -1) -- so that
 *     citext[], a domain over citext, a domain over citext[], and an
 *     array of such a domain are all caught.  A single recursive term
 *     with an OR-join (not stacked UNION ALL branches, which PostgreSQL
 *     rejects as "recursive reference ... in its non-recursive term").
 *   - Extension-owned objects are filtered out using pg_depend
 *     (deptype = 'e', refobjid = overlay_branch.oid), regardless of which
 *     namespace they live in.  This is critical because public.pg_branch
 *     VIEW and 12 public.* synonyms live in schema public.
 *   - 11 categories are ordered by "most user-harmful first" for fail-fast
 *     first-hit reporting.  Category 10 rejects a PRIMARY KEY declared
 *     DEFERRABLE INITIALLY DEFERRED: native checks PK uniqueness only at
 *     COMMIT (a transient duplicate PK the transaction resolves is legal),
 *     but the branch delta's identity key IS the serialized PK, so it cannot
 *     represent two rows bearing the same PK.  An INITIALLY IMMEDIATE PK is
 *     checked at end of statement and is accepted.  Category 11 rejects an
 *     EXCLUDE constraint: native enforces it immediately, but the redirect's
 *     eager probe only rebinds PLAIN-COLUMN UNIQUE indexes to a text check
 *     and cannot model an arbitrary exclusion operator, so a violating write
 *     would be silently accepted into the delta and surface only at
 *     apply_branch.
 * ================================================================ */
void
ob_check_branch_scope(void)
{
	int			ret;
	StringInfoData sql;
	char	   *detail;
	MemoryContext caller_cxt = CurrentMemoryContext;

	initStringInfo(&sql);
	appendStringInfoString(&sql,
"SELECT cat.reason FROM ( "
"  SELECT 1 AS ord, "
"    'FK: ' || n.nspname || '.' || c.relname || ' -> ' || c2.relname AS reason "
"  FROM pg_constraint x "
"  JOIN pg_class c ON c.oid = x.conrelid "
"  JOIN pg_class c2 ON c2.oid = x.confrelid "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  WHERE x.contype = 'f' "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_constraint'::regclass "
"                      AND d.objid = x.oid) "
"  UNION ALL "
"  SELECT 2, "
"    'TRIGGER: ' || n.nspname || '.' || c.relname || '.' || t.tgname "
"  FROM pg_trigger t "
"  JOIN pg_class c ON c.oid = t.tgrelid "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  WHERE NOT t.tgisinternal "
"    AND t.tgenabled <> 'D' "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_trigger'::regclass "
"                      AND d.objid = t.oid) "
"  UNION ALL "
"  SELECT 3, "
"    'PARTITION: ' || n.nspname || '.' || c.relname "
"  FROM pg_class c "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  WHERE (c.relkind IN ('p','I') OR c.relispartition) "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_class'::regclass "
"                      AND d.objid = c.oid) "
"  UNION ALL "
"  SELECT 4, "
"    'INHERITS: ' || n.nspname || '.' || c.relname "
"  FROM pg_inherits i "
"  JOIN pg_class c ON c.oid IN (i.inhrelid, i.inhparent) "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  WHERE n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_class'::regclass "
"                      AND d.objid = c.oid) "
"  UNION ALL "
"  SELECT 5, "
"    'UNLOGGED: ' || n.nspname || '.' || c.relname "
"  FROM pg_class c "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  WHERE c.relpersistence = 'u' "
"    AND c.relkind = 'r' "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_class'::regclass "
"                      AND d.objid = c.oid) "
"  UNION ALL "
"  SELECT 6, "
"    'GENERATED STORED in ' || n.nspname || '.' || c.relname || '.' || a.attname "
"  FROM pg_attribute a "
"  JOIN pg_class c ON c.oid = a.attrelid "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  WHERE a.attgenerated = 's' "
"    AND NOT a.attisdropped "
"    AND a.attnum > 0 "
"    AND c.relkind = 'r' "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_class'::regclass "
"                      AND d.objid = c.oid) "
"  UNION ALL "
"  SELECT 7, "
"    'UNSUPPORTED RELKIND ' || c.relkind::text || ': ' || n.nspname || '.' || c.relname "
"  FROM pg_class c "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  WHERE c.relkind NOT IN ('r','S','i') "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_class'::regclass "
"                      AND d.objid = c.oid) "
"  UNION ALL "
"  SELECT 8, "
"    'NONDET PK COLLATION: ' || n.nspname || '.' || c.relname || '.' || a.attname "
"  FROM pg_index i "
"  JOIN pg_class c ON c.oid = i.indrelid "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY(i.indkey[0:i.indnkeyatts-1]) "
"  JOIN pg_collation co ON co.oid = a.attcollation "
"  WHERE i.indisprimary "
"    AND NOT co.collisdeterministic "
"    AND c.relkind = 'r' "
"    AND a.attnum > 0 "
"    AND NOT a.attisdropped "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_class'::regclass "
"                      AND d.objid = c.oid) "
"  UNION ALL "
"  SELECT 9, "
"    'CITEXT PK: ' || n.nspname || '.' || c.relname || '.' || a.attname "
"  FROM pg_index i "
"  JOIN pg_class c ON c.oid = i.indrelid "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY(i.indkey[0:i.indnkeyatts-1]) "
"  WHERE i.indisprimary "
"    AND c.relkind = 'r' "
"    AND a.attnum > 0 "
"    AND NOT a.attisdropped "
"    AND a.atttypid IN ( "
"      WITH RECURSIVE cit(oid) AS ( "
"        SELECT t.oid FROM pg_type t JOIN pg_extension e ON e.extnamespace = t.typnamespace AND e.extname = 'citext' WHERE t.typname = 'citext' "
"        UNION ALL "
"        SELECT ch.oid FROM cit cs JOIN pg_type ch "
"          ON ch.typbasetype = cs.oid "
"          OR (ch.typelem = cs.oid AND ch.typlen = -1)) "
"      SELECT oid FROM cit) "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_class'::regclass "
"                      AND d.objid = c.oid) "
"  UNION ALL "
"  SELECT 10, "
"    'DEFERRABLE PK: ' || n.nspname || '.' || c.relname "
"  FROM pg_index i "
"  JOIN pg_class c ON c.oid = i.indrelid "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  JOIN pg_constraint con ON con.conindid = i.indexrelid "
"  WHERE i.indisprimary "
"    AND con.contype = 'p' "
"    AND con.condeferred "
"    AND c.relkind = 'r' "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_class'::regclass "
"                      AND d.objid = c.oid) "
"  UNION ALL "
"  SELECT 11, "
"    'EXCLUDE: ' || n.nspname || '.' || c.relname "
"  FROM pg_constraint x "
"  JOIN pg_class c ON c.oid = x.conrelid "
"  JOIN pg_namespace n ON n.oid = c.relnamespace "
"  WHERE x.contype = 'x' "
"    AND c.relkind = 'r' "
"    AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast') "
"    AND n.oid <> (SELECT n.oid FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'overlay_branch') "
"    AND NOT EXISTS (SELECT 1 FROM pg_depend d "
"                    WHERE d.refclassid = 'pg_extension'::regclass "
"                      AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname='overlay_branch') "
"                      AND d.deptype = 'e' "
"                      AND d.classid = 'pg_constraint'::regclass "
"                      AND d.objid = x.oid) "
"  ORDER BY ord LIMIT 1 "
") cat;");

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect failed in ob_check_branch_scope");

	ret = SPI_execute(sql.data, false, 1);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: could not run scope precheck: SPI ret=%d", ret)));
	}

	if (SPI_processed == 0 || SPI_tuptable == NULL || SPI_tuptable->vals == NULL || SPI_tuptable->vals[0] == NULL)
	{
		SPI_finish();
		pfree(sql.data);
		return;
	}

	/* SPI_getvalue() returns a pointer into the SPI proc context, which
	 * SPI_finish() destroys.  Copy it into the caller's context first: the
	 * ereport below expands its %s immediately, so the copy only has to
	 * outlive SPI_finish() and must not be parked in TopMemoryContext (this
	 * error path never returns, so a TopMC copy would leak on every rejected
	 * create_branch()). */
	detail = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
	if (detail != NULL)
		detail = MemoryContextStrdup(caller_cxt, detail);
	SPI_finish();
	if (detail != NULL && *detail != '\0')
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay branch cannot be created because this database contains at least one table in an unsupported category (G01 support matrix)"),
				 errdetail("First unsupported relation: %s", detail),
				 errhint("Remove or rename the relation, or exclude its schema from the overlay scope.  RESTRICTED categories: FKs, user triggers, partitions, table inheritance, UNLOGGED tables, GENERATED STORED columns, VIEWs/MATVIEWs/FDW tables, any relkind other than ordinary table ('r'), sequence ('S'), or btree index ('i'), EXCLUDE constraints, a PRIMARY KEY declared DEFERRABLE INITIALLY DEFERRED (the delta identity is the PK, so it cannot hold a transient duplicate PK), and a PRIMARY KEY column whose '=' operator is coarser than its output text -- a non-deterministic (e.g. ICU case-insensitive) collation or a citext (case-insensitive text) column; such equality classes cannot be represented by a text key.")));
	}
	pfree(sql.data);
}

/* ============ FR5: per-branch frozen snapshot cache (TopMC hash) ============
 * snapshot for snapshot-mode branches that have been entered at least once.
 *
 * Why a global cache instead of just ctx->branch_main_snapshot:
 *   A session can use_branch('A', snapshot) → use_branch('') → then later
 *   use_branch('A', snapshot) AGAIN. If we re-capture GetTransactionSnapshot
 *   on the 2nd entry, MAIN rows that were committed between entry 1 and
 *   entry 2 (e.g. via apply_branch of another LIVE branch) become visible —
 *   which violates "frozen at t0" semantics.  So we must remember the
 *   ORIGINAL snapshot per branch_id and re-install the SAME pointer.
 *
 * Lifetime rules:
 *   • INSERT: ob_install_snapshot_for_mode when a snapshot-mode branch is
 *             entered for the FIRST time (hash miss → capture + insert).
 *   • REUSE:  ob_install_snapshot_for_mode when the SAME branch_id is
 *             re-entered (hash hit → just copy pointer, no re-capture).
 *   • DELETE: (a) apply_branch / discard_branch on that bid, OR
 *             (b) _PG_fini (extension unload).  pfree the snapshot AND remove
 *             the hash entry.  Idempotent.
 *   • The ctx->snapshot_registered flag is a local-per-ctx "soft ownership"
 *     bit: it means CurrentBranchContext holds a REFERENCE count pin on
 *     the global cached snapshot, so ensure_unregister MUST NOT pfree the
 *     snapshot pointer (that's owned by the hash).  It only clears ctx's
 *     reference.  The hash owns the underlying allocation.
 * ==================================================================== */

typedef struct OBSnapshotHashKey
{
	int32	branch_id;
} OBSnapshotHashKey;

typedef struct OBSnapshotHashEnt
{
	OBSnapshotHashKey	key;		/* hash key: MUST be first field */
	Snapshot			frozen;		/* non-NULL = captured CopySnapshot ptr */
	int					refcnt;		/* number of BranchContexts referencing */
} OBSnapshotHashEnt;

static HTAB *ob_snapshot_cache = NULL;

static void
ob_snapshot_cache_init(void)
{
	HASHCTL		ctl;

	if (ob_snapshot_cache != NULL)
		return;
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(int32);
	ctl.entrysize = sizeof(OBSnapshotHashEnt);
	ctl.hcxt = TopMemoryContext;
	ob_snapshot_cache = hash_create("OverlayBranchSnapshotCache",
									16, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static Snapshot
ob_snapshot_cache_lookup(int32 bid)
{
	OBSnapshotHashEnt *ent;

	if (ob_snapshot_cache == NULL)
		return NULL;
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_FIND, NULL);
	return ent ? ent->frozen : NULL;
}

static void
ob_snapshot_cache_insert(int32 bid, Snapshot snap)
{
	OBSnapshotHashEnt *ent;
	bool	found;

	Assert(bid > 0);
	Assert(snap != NULL);
	ob_snapshot_cache_init();
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "overlay_branch: snapshot cache duplicate insert bid=%d", bid);
	ent->frozen = snap;
	ent->refcnt = 0;
}

static void
ob_snapshot_cache_pin(int32 bid)
{
	OBSnapshotHashEnt *ent;

	if (ob_snapshot_cache == NULL)
		return;
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_FIND, NULL);
	if (ent)
		ent->refcnt++;
}

static void
ob_snapshot_cache_unpin(int32 bid)
{
	OBSnapshotHashEnt *ent;

	if (ob_snapshot_cache == NULL)
		return;
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_FIND, NULL);
	if (ent && ent->refcnt > 0)
		ent->refcnt--;
}

/* Delete-and-pfree for this bid (apply / discard). Safe if missing. */
static void
ob_snapshot_cache_remove(int32 bid)
{
	OBSnapshotHashEnt *ent;

	if (ob_snapshot_cache == NULL)
		return;
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_REMOVE, NULL);
	if (ent && ent->frozen)
		pfree(ent->frozen);
}

/* ============ I8.2: session-local per-bid mode cache ============
 *
 * Replaces catalog UPDATE pg_branch.mode in use_branch() with a pure
 * session-local bid → last-mode hash.
 *
 * Rationale (locked in § B.9.1 I8.2):
 *   Prior code persisted every use_branch(mode) call to pg_branch catalog via
 *   SPI UPDATE.  Each UPDATE acquires RowExclusiveLock on the matching
 *   pg_branch row.  Two concurrent sessions calling use_branch('same_bid')
 *   therefore serialized at the heavyweight row lock.  Under pg_isolation_regress
 *   this manifested as s2_p1_use `<waiting ...>` for 360s (test timeout) then
 *   cancel → apply_mutex P1 false deadlock.
 *
 * Semantics:
 *   • Set on every use_branch(name, mode) call (including default 1-arg use,
 *     which passes mode='live').
 *   • Overlaid inside list_branches() SRF (overlay_branch.c) for the matching
 *     bid; catalog mode is read as fallback.
 *   • Removed on apply / discard (bid no longer ACTIVE so re-use impossible
 *     until a future recreate, which inserts a fresh catalog row with the
 *     default mode='live').
 *
 * No heavyweight locks, no catalog serialization.  PART D/E/F mode tests all
 * run inside a single session so hash matches prior catalog semantics exactly.
 * ==================================================================== */

typedef struct OBModeHashEnt
{
	int32	branch_id;			/* hash key: MUST be first field */
	char	mode[16];			/* "live" or "snapshot", NUL-padded */
} OBModeHashEnt;

static HTAB *ob_mode_cache = NULL;

static void
ob_mode_cache_init_lazy(void)
{
	HASHCTL		ctl;

	if (ob_mode_cache != NULL)
		return;
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(int32);
	ctl.entrysize = sizeof(OBModeHashEnt);
	ctl.hcxt = TopMemoryContext;
	ob_mode_cache = hash_create("OverlayBranchModeCache",
								16, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

const char *
ob_mode_cache_lookup(int32 bid)
{
	OBModeHashEnt *ent;

	if (ob_mode_cache == NULL)
		return NULL;
	ent = (OBModeHashEnt *) hash_search(ob_mode_cache,
										&bid, HASH_FIND, NULL);
	return ent ? ent->mode : NULL;
}

void
ob_mode_cache_set(int32 bid, const char *mode)
{
	OBModeHashEnt *ent;
	bool	found;

	Assert(bid > 0);
	Assert(mode != NULL);
	ob_mode_cache_init_lazy();
	ent = (OBModeHashEnt *) hash_search(ob_mode_cache,
										&bid, HASH_ENTER, &found);
	strlcpy(ent->mode, mode, sizeof(ent->mode));
}

void
ob_mode_cache_remove(int32 bid)
{
	if (ob_mode_cache == NULL)
		return;
	(void) hash_search(ob_mode_cache, &bid, HASH_REMOVE, NULL);
}

/* ============ Internal apply helpers (static to this TU) ============ */

/*
 * ObApplyRelInfo - per-relation apply metadata, built once per relation in the
 * apply loop so the per-delta-row path stops recomputing the qualified table
 * name, the column lists and the PK column name/type literals.
 */
typedef struct ObApplyRelInfo
{
	Relation	rel;
	Oid			relid;
	char	   *q_relname;			/* schema-qualified, quoted table name */
	int			n_pk;				/* PK column count; 0 => no PK (keyless) */
	char	  **pk_colname;			/* per PK col: raw attribute name */
	char	  **pk_coltype;			/* per PK col: format_type_with_typemod() */
	char	   *colnames;			/* "a","b" (quoted identifiers) */
	char	   *colnames_parens;	/* j."a",j."b" */
	/* UPDATE replay must NOT assign a GENERATED ALWAYS AS IDENTITY column:
	 * PostgreSQL rejects "SET id = <expr>" for such a column (only DEFAULT is
	 * accepted), and a branch UPDATE can never change it anyway.  These lists
	 * are colnames/colnames_parens minus ATTRIBUTE_IDENTITY_ALWAYS columns;
	 * when the table has no such column they alias the full lists. */
	char	   *upd_colnames;
	char	   *upd_colnames_parens;
	Oid			bytea_out_func;		/* bytea output function for tuple_data */
	bool		bytea_out_varlena;
} ObApplyRelInfo;

static ObApplyRelInfo *ob_apply_rel_info_build(Relation rel);
static void ob_apply_rel_info_free(ObApplyRelInfo *ri);
static char *ob_build_pk_where_clause(ObApplyRelInfo *ri, const char *pk_json_array);
static char *ob_build_identity_where_clause(ObApplyRelInfo *ri, const char *key);
static bool ob_fetch_main_version(ObApplyRelInfo *ri, const char *where_clause,
								  char **version);
/* branch_name is threaded down so apply-time conflict errors name the branch
 * being applied (apply runs outside branch mode — see overlay_guard_ereport_fail). */
static void apply_check_base_image_match(ObApplyRelInfo *ri, DeltaTuple *dt,
										 const char *where_clause,
										 const char *branch_name);
static void apply_relation_delete_pass(ObApplyRelInfo *ri, DeltaTuple *dt,
									   const char *branch_name);
static void apply_relation_update_pass(ObApplyRelInfo *ri, DeltaTuple *dt,
									   const char *branch_name);
static void apply_relation_insert_pass(ObApplyRelInfo *ri, DeltaTuple *dt,
									   const char *branch_name);
/* Phase 3B: set-based replay (one statement per relation and operation). */
static void ob_apply_relation_per_row(ObApplyRelInfo *ri, List *deltas,
									  const char *branch_name);
static bool ob_apply_batch_can_use(ObApplyRelInfo *ri, List *deltas);
static void ob_apply_relation_batch(ObApplyRelInfo *ri, int32 bid, List *deltas,
									const char *branch_name);

/* ================================================================
 * §G06 minimal leak prevention: detect whether the current database has any publication
 *   - puball=true (FOR ALL TABLES), or
 *   - pubschema=true and the publication has FOR TABLES IN SCHEMA over the overlay_branch schema
 * Match on either case emits WARNING (does not block normal use; S26 productization may upgrade to ERROR).
 * Access control: SPI SELECT pg_publication; if the table is missing (--without-publication build) silently skip.
 * Throttling: the 5 sites on the same connection (create/enter/exit/apply/discard) warn only once per process,
 * avoiding a flood on every use_branch().
 * ================================================================ */
static void
ob_g06_check_publication_leak_throttled(void)
{
	/* per-session throttled flag; reset only on process exit */
	static bool s_warned = false;
	static bool s_checked = false;
	StringInfoData sql;
	int			ret;
	bool		found = false;
	bool		isnull;
	/* volatile: set inside the PG_TRY and read in the PG_CATCH to decide
	 * whether to SPI_finish; a non-volatile local's value is indeterminate
	 * after a longjmp (C99 7.13.2.1) and the frame would leak. */
	volatile bool spi_connected = false;

	if (s_warned || s_checked)
		return;

	/*
	 * Use SPI SELECT EXISTS for exact detection; wrap in PG_TRY/PG_CATCH,
	 * preventing a PANIC when the catalog is not ready / --without-publication build.
	 */
	initStringInfo(&sql);
	appendStringInfoString(&sql,
						   "SELECT EXISTS ("
						   " SELECT 1 FROM pg_catalog.pg_publication p WHERE p.puballtables = true"
						   "  OR EXISTS ("
						   "    SELECT 1 FROM pg_catalog.pg_publication_namespace pn"
						   "    JOIN pg_catalog.pg_namespace n ON pn.pnnspid = n.oid"
						   "    WHERE pn.pnpubid = p.oid AND n.nspname = 'overlay_branch'))");

	PG_TRY();
	{
		if (SPI_connect() == SPI_OK_CONNECT)
			spi_connected = true;

		if (spi_connected)
		{
			ret = SPI_execute(sql.data, true, 1);
			if (ret == SPI_OK_SELECT && SPI_processed == 1)
			{
				Datum		d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);

				if (!isnull)
					found = DatumGetBool(d);
			}
		}
	}
	PG_CATCH();
	{
		/*
		 * Any exception (relation not exists / permission / SPI error / catalog
		 * error) fails open: silently skip. Does not block user flow, does not pollute logs.
		 */
		if (spi_connected)
		{
			SPI_finish();
			spi_connected = false;
		}
		FlushErrorState();
	}
	PG_END_TRY();

	pfree(sql.data);
	if (spi_connected)
		SPI_finish();
	s_checked = true;

	if (found)
	{
		s_warned = true;
		ereport(WARNING,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("overlay_branch: detected publication FOR ALL TABLES or FOR TABLES IN SCHEMA overlay_branch"),
				 errhint("Internal tables pg_branch/pg_branch_delta (stored in schema overlay_branch) will be "
						 "included in any logical replication slot reading FOR ALL TABLES publications. "
						 "Production deployments should restrict publications to an explicit table list "
						 "and keep schema overlay_branch excluded.")));
	}
}

/* ================================================================
 * §Gx / A7: schema-epoch registry helpers.
 *
 *   All 3 helpers are SPI-only, so they can be reused 1:1 from the
 *   same SQL query the surrounding lifecycle function already uses.
 *   We explicitly SPI_connect/SPI_finish inside each helper because
 *   (a) callers at different stack depths (create / use / apply /
 *   discard) have different pre-existing SPI lifetimes; (b) a clean
 *   open/close avoids cross-contaminating SPI_tuptable of the
 *   caller.
 * ================================================================ */

/* ---------- ob_registry_current_db_tables_query ----------
 * Builds the SELECT that enumerates *current* user tables in this
 * database, with exactly the same double-filtering rules as G01 scope
 * (sys ns / ext ns / ext-owned via pg_depend).  Returns one row per
 * user table: (relid, schema_name, table_name, total_cols, pk_cols,
 * col_hash bytea, pk_hash bytea).
 *
 * total_cols / pk_cols come first so the 55000 error detail can
 * quote human-readable numbers without decoding a hash.
 * ---------- */
static char *
ob_registry_fingerprint_sql(void)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf,
"SELECT\n"
"  c.oid AS relid,\n"
"  n.nspname::name AS schema_name,\n"
"  c.relname::name AS table_name,\n"
"  (SELECT count(*) FROM pg_catalog.pg_attribute a\n"
"     WHERE a.attrelid = c.oid\n"
"       AND a.attnum > 0 AND NOT a.attisdropped) AS total_cols,\n"
"  COALESCE((SELECT count(*) FROM pg_catalog.pg_index i\n"
"              JOIN pg_catalog.pg_attribute a ON a.attrelid = i.indrelid\n"
"                 AND a.attnum = ANY(i.indkey)\n"
"                 AND NOT a.attisdropped\n"
"              WHERE i.indrelid = c.oid AND i.indisprimary), 0) AS pk_cols,\n"
"  (SELECT decode(md5(string_agg(concat_ws('|',\n"
"                   a.attname, a.atttypid::text, a.attlen::text,\n"
"                   a.attbyval::text, a.atttypmod::text,\n"
"                   a.attcollation::text, a.attnotnull::text,\n"
"                   a.attgenerated::text), '' ORDER BY a.attnum)), 'hex')\n"
"     FROM pg_catalog.pg_attribute a\n"
"     WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped) AS col_hash,\n"
"  (SELECT decode(md5(COALESCE(string_agg(a.attname, '' ORDER BY k.n), '')), 'hex')\n"
"     FROM pg_catalog.pg_index i\n"
"     LEFT JOIN pg_catalog.generate_subscripts(i.indkey, 1) k(n) ON true\n"
"     LEFT JOIN pg_catalog.pg_attribute a\n"
"       ON a.attrelid = i.indrelid AND a.attnum = i.indkey[k.n] AND NOT a.attisdropped\n"
"     WHERE i.indrelid = c.oid AND i.indisprimary) AS pk_hash\n"
"FROM pg_catalog.pg_class c\n"
"JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
"WHERE c.relkind = 'r'\n"
"  AND c.relpersistence = 'p'\n"
"  AND n.nspname NOT IN ('pg_catalog', 'information_schema', 'pg_toast')\n"
"  AND n.nspname <> '%s'\n"
"  AND NOT EXISTS (\n"
"     SELECT 1 FROM pg_catalog.pg_depend d\n"
"     WHERE d.classid = 'pg_catalog.pg_class'::regclass\n"
"       AND d.objid = c.oid\n"
"       AND d.deptype = 'e'\n"
"       AND d.refobjid = (SELECT oid FROM pg_catalog.pg_extension\n"
"                         WHERE extname = 'overlay_branch'))\n"
"ORDER BY c.oid",
		OBSCHEMA);
	return buf.data;
}

static void
ob_registry_populate_spi_connected(int32 bid)
{
	const char *fingerprint = ob_registry_fingerprint_sql();
	StringInfoData sql;
	int ret;

	if (bid <= 0)
		return;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "INSERT INTO %s "
					 "(branch_id, relid, schema_name, table_name, "
					 " total_cols, pk_cols, col_hash, pk_hash)\n"
					 "SELECT %d, s.relid, s.schema_name, s.table_name, "
					 "       s.total_cols, s.pk_cols, s.col_hash, s.pk_hash\n"
					 "FROM (%s) s",
					 OBTABLE_REGISTRY, (int) bid, fingerprint);

	ret = SPI_execute(sql.data, false, 0);
	pfree(sql.data);

	if (ret != SPI_OK_INSERT)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: registry_populate for bid=%d SPI ret=%d (expected INSERT ok)",
						(int) bid, ret)));
	elog(DEBUG1, "ob_registry_populate(bid=%d, first-use): inserted %lu rows",
		 (int) bid, (unsigned long) SPI_processed);
}

void
ob_registry_populate_for_create(int32 bid)
{
	if (bid <= 0)
		return;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect failed in registry_populate");

	ob_registry_populate_spi_connected(bid);

	SPI_finish();
}

void
ob_registry_check_current_schema(int32 bid, const char *op_name)
{
	const char *fingerprint = ob_registry_fingerprint_sql();
	StringInfoData sql;
	int ret;
	bool	any_rows;
	MemoryContext caller_cxt = CurrentMemoryContext;

	if (bid <= 0)
		return;
	if (!op_name) op_name = "schema check";

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect failed in registry_check");

	/* Stage 1: does this bid have any registry rows?
	 * If 0 rows → old branch created before 1.3 upgrade; SKIP check
	 * (preserve backward compat).  Only when registry has data do we
	 * run the drift LEFT JOIN below. */
	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT 1 FROM %s WHERE branch_id = %d LIMIT 1",
					 OBTABLE_REGISTRY, (int) bid);
	ret = SPI_execute(sql.data, true, 1);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		pfree(sql.data);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: registry_existence check for bid=%d SPI ret=%d",
						(int) bid, ret)));
	}
	any_rows = (SPI_processed == 1);
	pfree(sql.data);

	if (!any_rows)
	{
		/* First-use snapshot: populate registry NOW at use_branch /
		 * apply_branch entry time so subsequent entries/applies detect
		 * drift against a stable fingerprint baseline.
		 *
		 * IMPORTANT: ob_registry_populate_spi_connected may ereport if
		 * the INSERT fails — wrap the call in PG_TRY so we still call
		 * SPI_finish on the outer (check_current_schema) SPI connection
		 * before the error propagates.  Without this the connection
		 * leaks and the end-of-transaction machinery raises
		 * "WARNING:  transaction left non-empty SPI stack". */
		bool		pop_ok = false;

		PG_TRY();
		{
			ob_registry_populate_spi_connected(bid);
			pop_ok = true;
		}
		PG_CATCH();
		{
			SPI_finish();
			PG_RE_THROW();
		}
		PG_END_TRY();

		if (pop_ok)
			SPI_finish();
		elog(DEBUG1, "ob_registry_check(bid=%d): no registry rows — first-use populate done at %s.",
			 (int) bid, op_name);
		return;
	}

	/* Stage 2a: FIRST-hit drift report for *already-registered* tables only.
	 * 5 UNION ALL subqueries (mirror G01 scope fail-fast style):
	 *   1 = table GONE (registry exists, current DB missing relid — DROP on
	 *       MAIN after this table was already registered to the branch)
	 *   2 = total_cols mismatch
	 *   3 = pk_cols mismatch
	 *   4 = col_hash mismatch
	 *   5 = pk_hash mismatch
	 * Tables currently present in MAIN but never registered are handled in
	 * Stage 2b (incremental INSERT into registry) — they do NOT produce a
	 * drift error because users may legitimately CREATE TABLE on MAIN
	 * between create_branch and first/next use_branch.
	 *
	 * ORDER BY ord LIMIT 1 → exactly FIRST mismatch reported. */
	initStringInfo(&sql);
	appendStringInfo(&sql,
"SELECT ord, detail FROM (\n"
"  SELECT 1::smallint AS ord,\n"
"         format('table %%I.%%I (registry relid=%%s) has been DROPPED from the database after it was registered to this branch',\n"
"                r.schema_name, r.table_name, r.relid::text) AS detail\n"
"    FROM %s r\n"
"    WHERE r.branch_id = %d\n"
"      AND NOT EXISTS (SELECT 1 FROM (%s) cur WHERE cur.relid = r.relid)\n"
"  UNION ALL\n"
"  SELECT 2::smallint AS ord,\n"
"         format('total_cols drift for %%I.%%I: registry=%%s, current=%%s',\n"
"                cur.schema_name, cur.table_name, r.total_cols::text, cur.total_cols::text)\n"
"    FROM %s r JOIN (%s) cur ON cur.relid = r.relid\n"
"    WHERE r.branch_id = %d AND r.total_cols <> cur.total_cols\n"
"  UNION ALL\n"
"  SELECT 3::smallint AS ord,\n"
"         format('pk_cols drift for %%I.%%I: registry=%%s, current=%%s (PK added/dropped/rekeyed)',\n"
"                cur.schema_name, cur.table_name, r.pk_cols::text, cur.pk_cols::text)\n"
"    FROM %s r JOIN (%s) cur ON cur.relid = r.relid\n"
"    WHERE r.branch_id = %d AND r.pk_cols <> cur.pk_cols\n"
"  UNION ALL\n"
"  SELECT 4::smallint AS ord,\n"
"         format('column-hash drift for %%I.%%I: column list order/type/collation/notnull/generated changed since table was registered',\n"
"                cur.schema_name, cur.table_name)\n"
"    FROM %s r JOIN (%s) cur ON cur.relid = r.relid\n"
"    WHERE r.branch_id = %d AND r.col_hash <> cur.col_hash\n"
"  UNION ALL\n"
"  SELECT 5::smallint AS ord,\n"
"         format('pk-hash drift for %%I.%%I: PK column order/set changed since table was registered',\n"
"                cur.schema_name, cur.table_name)\n"
"    FROM %s r JOIN (%s) cur ON cur.relid = r.relid\n"
"    WHERE r.branch_id = %d AND r.pk_hash <> cur.pk_hash\n"
") all_mismatches ORDER BY ord LIMIT 1",
		/* 1 uses */ OBTABLE_REGISTRY, (int) bid, fingerprint,
		/* 2 uses */ OBTABLE_REGISTRY, fingerprint, (int) bid,
		/* 3 uses */ OBTABLE_REGISTRY, fingerprint, (int) bid,
		/* 4 uses */ OBTABLE_REGISTRY, fingerprint, (int) bid,
		/* 5 uses */ OBTABLE_REGISTRY, fingerprint, (int) bid);

	ret = SPI_execute(sql.data, true, 1);
	if (ret != SPI_OK_SELECT)
	{
		/* sql.data lives in the SPI proc context; copy it into the caller's
		 * context before SPI_finish() destroys it (doc/internals.md §2.8). */
		char *saved = MemoryContextStrdup(caller_cxt, sql.data);

		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: schema drift LEFT JOIN for bid=%d SPI ret=%d (sql[0:80]=%.80s)",
						(int) bid, ret, saved)));
	}
	if (SPI_processed == 1 && SPI_tuptable && SPI_tuptable->vals && SPI_tuptable->vals[0])
	{
		bool ordnull;
		bool detailnull;
		Datum d_detail;
		char *detail_text;

		(void) SPI_getbinval(SPI_tuptable->vals[0],
							 SPI_tuptable->tupdesc,
							 1, &ordnull);
		d_detail = SPI_getbinval(SPI_tuptable->vals[0],
								 SPI_tuptable->tupdesc,
								 2, &detailnull);
		if (detailnull || d_detail == (Datum) 0)
			detail_text = "unknown schema drift";
		else
		{
			/* text_to_cstring() allocates in the current (SPI proc) context;
			 * keep only the copy in the caller's context.  TopMemoryContext
			 * is wrong here: this error path never returns, so anything left
			 * there leaks on every drift detection. */
			text *t = DatumGetTextPP(d_detail);

			detail_text = MemoryContextStrdup(caller_cxt,
											  text_to_cstring(t));
		}
		pfree(sql.data);
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("overlay_branch: cannot %s on branch_id=%d — MAIN schema drifted since the table was registered to this branch (A7 registry mismatch, 55000)",
						op_name, (int) bid),
				 errdetail("%s", detail_text),
				 errhint("Discard the branch, apply the schema change on MAIN, "
						 "then create_branch() a fresh branch.")));
	}
	pfree(sql.data);

	/* Stage 2b: incremental registration.  MAIN tables currently present
	 * but not yet registered to this branch → INSERT 1 row each into the
	 * registry so future entries/applies can detect DROP/ALTER drift on
	 * them as well. */
	{
		StringInfoData ins;
		initStringInfo(&ins);
		appendStringInfo(&ins,
"INSERT INTO %s "
"(branch_id, relid, schema_name, table_name, "
" total_cols, pk_cols, col_hash, pk_hash)\n"
"SELECT %d, s.relid, s.schema_name, s.table_name, "
"       s.total_cols, s.pk_cols, s.col_hash, s.pk_hash\n"
"FROM (%s) s\n"
"WHERE NOT EXISTS (SELECT 1 FROM %s r\n"
"                   WHERE r.branch_id = %d AND r.relid = s.relid)",
			OBTABLE_REGISTRY, (int) bid, fingerprint,
			OBTABLE_REGISTRY, (int) bid);
		ret = SPI_execute(ins.data, false, 0);
		if (ret != SPI_OK_INSERT)
		{
			/* ins.data lives in the SPI proc context; copy it out before
			 * SPI_finish() (doc/internals.md §2.8). */
			char *saved = MemoryContextStrdup(caller_cxt, ins.data);

			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: registry incremental INSERT bid=%d SPI ret=%d (sql[0:80]=%.80s)",
							(int) bid, ret, saved)));
		}
		elog(DEBUG1, "ob_registry_check(bid=%d, %s): incremental registered %lu new rows",
			 (int) bid, op_name, (unsigned long) SPI_processed);
		pfree(ins.data);
	}

	SPI_finish();
}

void
ob_registry_cascade_discard(int32 bid)
{
	StringInfoData sql;
	int ret;

	if (bid <= 0)
		return;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect failed in registry_cascade_discard");

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM %s WHERE branch_id = %d",
					 OBTABLE_REGISTRY, (int) bid);
	ret = SPI_execute(sql.data, false, 0);
	if (ret != SPI_OK_DELETE)
	{
		pfree(sql.data);
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: registry_cascade_delete for bid=%d SPI ret=%d",
						(int) bid, ret)));
	}
	elog(DEBUG1, "ob_registry_cascade_discard(bid=%d): deleted %lu registry rows",
		 (int) bid, (unsigned long) SPI_processed);
	pfree(sql.data);
	SPI_finish();
}

/* ================================================================
 * ---------- Branch Context (internal implementations) ----------
 * ================================================================ */

int32
overlay_branch_create_internal(const char *branch_name)
{
	int			ret;
	Oid			owner;
	int32		new_branch_id;
	bool		isnull;
	char	   *esc_name;
	StringInfoData sql;

	ob_g06_check_publication_leak_throttled();

	if (branch_name == NULL || *branch_name == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("branch name cannot be empty")));

	if (strlen(branch_name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("branch name too long (max %d characters)",
						NAMEDATALEN - 1)));


	/* ================================================================
	 * G01 Branch-scope preflight.  Runs BEFORE we touch the branch
	 * catalog (including pg_branch_branch_id_seq) so that a REJECTED
	 * create_branch has ZERO side effects on the database.
	 * ================================================================ */
	ob_check_branch_scope();

	owner = GetUserId();
	esc_name = quote_literal_cstr(branch_name);
	initStringInfo(&sql);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect() failed in create_branch");

	/* 1) Duplicate check */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT 1 FROM " OBTABLE_BRANCH " WHERE branch_name = %s LIMIT 1",
					 esc_name);
	ret = SPI_execute(sql.data, true, 1);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: could not check existing branch '%s': SPI ret=%d",
						branch_name, ret)));
	}
	if (SPI_processed > 0)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", branch_name)));
	}

	/* 2) INSERT new row */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "INSERT INTO " OBTABLE_BRANCH " "
					 "(branch_name, owner, created_at, mode, state) "
					 "VALUES (%s, %u, now(), '%s', '%s')",
					 esc_name,
					 owner,
					 BRANCH_MODE_LIVE,
					 BRANCH_STATE_ACTIVE);

	ret = SPI_execute(sql.data, false, 0);
	if (ret != SPI_OK_INSERT || SPI_processed != 1)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: could not insert new branch '%s': SPI ret=%d processed=%lu",
						branch_name, ret, (unsigned long) SPI_processed)));
	}

	/* ===== REVIEW-260926 / R09 =====
	 *   Previously we returned a static process-local counter
	 *   `s_next_return_id` which was NOT synchronized with the
	 *   catalog sequence `pg_branch_branch_id_seq`.  Under concurrent
	 *   branch creation the returned int could collide / misalign with
	 *   the real branch_id stored in the catalog, causing later
	 *   apply/discard/lookup to operate on the WRONG branch (silent
	 *   data loss / wrong-row mutation).
	 *
	 *   Fix: after INSERT (which triggers DEFAULT nextval(...)), call
	 *   currval() in the same session to return the value the catalog
	 *   actually assigned.  currval() is session-local and only
	 *   reflects our own session's last nextval() call on that
	 *   sequence; it never collides with other sessions. */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT currval('%s.pg_branch_branch_id_seq'::regclass)",
					 OBSCHEMA);
	ret = SPI_execute(sql.data, true, 1);
	if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
		SPI_tuptable == NULL || SPI_tuptable->vals == NULL ||
		SPI_tuptable->vals[0] == NULL)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: could not retrieve currval() for new branch '%s' (SPI ret=%d processed=%lu)",
						branch_name, ret, (unsigned long) SPI_processed)));
	}
	new_branch_id = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
												SPI_tuptable->tupdesc,
												1, &isnull));
	if (isnull)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: currval() for new branch '%s' returned NULL (sequence broken)",
						branch_name)));
	}

	/* ================================================================
	 * §D4 / A9: identity / restore metadata.
	 *
	 *   We have the new branch_id and our SPI connection is still
	 *   open.  Run a single aggregated SELECT over the exact same A7
	 *   registry table-enumeration query (ob_registry_fingerprint_sql
	 *   plus a user-column + tablespace enumerator) then UPDATE the
	 *   pg_branch row with the four restore-metadata columns.
	 *
	 *   This UPDATE is deliberately placed HERE — after the bid is
	 *   stable but before SPI_finish or the A7 registry deferred
	 *   populate — so fingerprint values match what the registry rows
	 *   would have captured at create-time.  Old branches that
	 *   existed before this A9 upgrade keep NULL in these columns,
	 *   which is fully ABI-compatible.
	 * ================================================================ */
	{
		TransactionId xmin = GetTopTransactionId();
		char	   *esc_xmin;
		const char *fp = ob_registry_fingerprint_sql();
		StringInfoData upsql;
		StringInfoData csql;
		int			up_ret;

		/* Creation snapshot xmin: the txid running create_branch().
		 * We store the decimal text form; the column type is xid so
		 * simply pass text::xid cast for free validation. */
		esc_xmin = DatumGetCString(DirectFunctionCall1(xidout,
							TransactionIdGetDatum(xmin)));

		/* Build the 4 aggregates as scalar columns over the same
		 * enumeration SQL (LEFT JOIN to pg_attribute for col_signature
		 * and pg_class. reltablespace → spcname). */
		initStringInfo(&csql);
		appendStringInfo(&csql,
"SELECT\n"
"  decode(md5(string_agg(concat_ws('|', s.schema_name, s.table_name,\n"
"                                 s.total_cols::text, s.pk_cols::text,\n"
"                                 encode(s.col_hash, 'hex'),\n"
"                                 encode(s.pk_hash, 'hex')), ''\n"
"                      ORDER BY s.relid)), 'hex')::bytea    AS _schema_hash,\n"
"  COALESCE(\n"
"    string_agg(DISTINCT t.spcname, ',' ORDER BY t.spcname) FILTER (WHERE t.spcname <> 'pg_default'),\n"
"    '')::text                                             AS _tbs_list,\n"
"  decode(md5(string_agg(concat_ws('|',\n"
"                     s.schema_name||'.'||s.table_name,\n"
"                     a.attnum::text, a.attname, a.atttypid::text,\n"
"                     a.atttypmod::text, a.attnotnull::text,\n"
"                     a.attgenerated::text,\n"
"                     COALESCE(pk.pk_ord::text, '0')), ''\n"
"                ORDER BY s.relid, a.attnum)), 'hex')::bytea AS _col_sig\n"
"FROM (%s) s\n"
"LEFT JOIN pg_catalog.pg_attribute a\n"
"       ON a.attrelid = s.relid AND a.attnum > 0 AND NOT a.attisdropped\n"
"LEFT JOIN (SELECT i.indrelid, k.n AS pk_ord, a.attnum\n"
"             FROM pg_catalog.pg_index i\n"
"             LEFT JOIN pg_catalog.generate_subscripts(i.indkey,1) k(n) ON true\n"
"             LEFT JOIN pg_catalog.pg_attribute a\n"
"                    ON a.attrelid = i.indrelid AND a.attnum = i.indkey[k.n]\n"
"                   AND NOT a.attisdropped\n"
"             WHERE i.indisprimary) pk\n"
"       ON pk.indrelid = s.relid AND pk.attnum = a.attnum\n"
"LEFT JOIN pg_catalog.pg_class cs ON cs.oid = s.relid\n"
"LEFT JOIN pg_catalog.pg_tablespace t ON t.oid = cs.reltablespace\n",
		fp);

		initStringInfo(&upsql);
		appendStringInfo(&upsql,
"UPDATE %s SET\n"
"  schema_hash             = agg._schema_hash,\n"
"  creation_snapshot_xmin  = '%s'::xid,\n"
"  tablespace_list         = agg._tbs_list,\n"
"  col_signature           = agg._col_sig\n"
"FROM (%s) agg\n"
"WHERE branch_id = %d",
			OBTABLE_BRANCH,
			esc_xmin,
			csql.data,
			(int) new_branch_id);

		up_ret = SPI_execute(upsql.data, false, 0);
		pfree(upsql.data);
		pfree(csql.data);
		pfree(esc_xmin);

		if (up_ret != SPI_OK_UPDATE || SPI_processed != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: A9 identity UPDATE for bid=%d SPI ret=%d processed=%lu (expected 1)",
							(int) new_branch_id, up_ret, (unsigned long) SPI_processed)));
		elog(DEBUG1, "A9 identity populated for bid=%d", (int) new_branch_id);
	}

	SPI_finish();

	/* Normal StringInfo teardown: free the data buffer (we won't reuse
	 * `sql` any further).  `initStringInfo` earlier allocated `sql.data`
	 * in CurrentMemoryContext; pfree is balanced here. */
	pfree(sql.data);

	/* §Gx / A7: schema epoch registry — first-use snapshot at use_branch.
	 * Populate is deliberately deferred to the first use_branch/apply_branch
	 * call so that create_branch → CREATE TABLE on MAIN → use_branch order
	 * (used by guard/mvcc_bounds L1 tests) correctly takes the snapshot at
	 * entry-time, not create-time. */

	elog(DEBUG1, "overlay_branch_create_internal: name='%s' return_id=%d owner=%u",
		 branch_name, (int) new_branch_id, (unsigned) owner);

	return new_branch_id;
}
/* ------------------------------------------------------------------
 * snapshot lifecycle helpers
 * ------------------------------------------------------------------ */
void
ob_ensure_unregister_snapshot(BranchContext *ctx)
{
	int32		saved_bid;

	if (ctx == NULL)
		return;

	/* CRITICAL idempotency guard: reset flags BEFORE releasing ref,
	 * so any re-entrant / concurrent (e.g. GUC savepoint rollback
	 * re-check) call skips the unpin branch even if ERROR occurs
	 * below.  Note: snapshot allocation is now OWNED by
	 * ob_snapshot_cache (hash table), so we MUST NOT pfree here —
	 * only clear ctx pointer and decrement hash refcnt.  The real
	 * pfree happens only when apply/discard the branch
	 * (ob_snapshot_cache_remove) or during _PG_fini. */
	saved_bid = ctx->branch_id;
	ctx->branch_main_snapshot = NULL;
	if (!ctx->snapshot_registered)
		return;
	ctx->snapshot_registered = false;

	if (saved_bid > 0)
	{
		ob_snapshot_cache_unpin(saved_bid);
		elog(DEBUG2, "overlay_branch: unregistered MAIN snapshot ref (unpin bid=%d)",
			 (int) saved_bid);
	}
}


void
ob_exit_branch_cleanup(BranchContext *ctx)
{
	if (ctx == NULL)
		return;

	/* Idempotency: mark inactive BEFORE any cleanup work, so nested or
	 * re-entrant calls from GUC hooks / abort paths become no-ops
	 * (ensures single-shot cleanup even if ERROR interrupts the flow
	 * mid-way). */
	if (!ctx->is_active)
	{
		/* !active path: only clean any lingering snapshot ref that
		 * might have leaked through a prior ERROR mid-cleanup. */
		ob_ensure_unregister_snapshot(ctx);
		ctx->branch_id = 0;
		ctx->branch_name[0] = '\0';
		ctx->invalidation_counter = 0;
		ctx->invalidation_last_check = 0;
		ctx->entered_at = 0;
		return;
	}
	ctx->is_active = false;
	ob_ensure_unregister_snapshot(ctx);
	ctx->branch_id = 0;
	ctx->branch_name[0] = '\0';
	ctx->invalidation_counter = 0;
	ctx->invalidation_last_check = 0;
	ctx->entered_at = 0;
	elog(DEBUG1, "overlay_branch: left current branch (cleanup helper)");
}

static void
ob_install_snapshot_for_mode(BranchContext *ctx, const char *mode)
{
	Assert(ctx != NULL);
	Assert(mode != NULL);
	Assert(ctx->branch_id > 0 ||
		   (strcmp(mode, BRANCH_MODE_SNAPSHOT) != 0 &&
			strcmp(mode, BRANCH_MODE_LIVE) == 0));

	if (strcmp(mode, BRANCH_MODE_SNAPSHOT) == 0)
	{
		Snapshot	existing;

		/* --- FR5 freeze semantics: once a branch is first entered
		 * in snapshot mode, its frozen MAIN snapshot is stored in the
		 * global bid-keyed hash. Subsequent re-entries (use_branch('',
		 * snapshot) on the SAME bid) MUST reuse the original capture
		 * — re-running GetTransactionSnapshot would pick up MAIN rows
		 * committed between session entries and violate "frozen at t0".
		 *
		 * N4 re-entry (use twice in a row without exit): caller
		 * (use_with_mode_internal L417) calls
		 * ob_ensure_unregister_snapshot BEFORE reaching here, which
		 * decrements refcnt but leaves the hash entry intact (only
		 * apply/discard removes it). So first use inserts with
		 * refcnt=0; second use → unpin → refcnt back to 0; then we
		 * arrive here and find HASH HIT → reuse SAME snapshot.
		 * Correct! */
		existing = ob_snapshot_cache_lookup(ctx->branch_id);
		if (existing != NULL)
		{
			/* HASH HIT: re-install SAME frozen snapshot (not a copy). */
			ctx->branch_main_snapshot = existing;
			ctx->snapshot_registered = true;
			ob_snapshot_cache_pin(ctx->branch_id);
			elog(DEBUG1, "overlay_branch: snapshot REUSE (cache hit bid=%d) xmin=%u",
				 (int) ctx->branch_id, (uint32) existing->xmin);
			return;
		}

		/* HASH MISS: first-ever snapshot-mode entry for this bid →
		 * capture once & store in hash as canonical frozen view. */
		{
			Snapshot cur_snap;
			Snapshot copied;
			MemoryContext oldcxt;
			Size subxipoff;
			Size size_alloc;

			/* PERFORMANCE TODO: long-lived snapshot holds back
			 * xmin horizon / VACUUM. Correctness first; add max-hold
			 * GUC + HINT later.
			 *
			 * Do NOT use RegisterSnapshot() / UnregisterSnapshot()
			 * here: RegisterSnapshot binds to CurrentResourceOwner
			 * (= the SQL callable's Portal owner) and auto-releases
			 * at statement end, or CurTransactionResourceOwner
			 * auto-drops on COMMIT/ROLLBACK — both wrong for a
			 * snapshot branch that must outlive many tx until apply/
			 * discard. We hash-cache with manual lifetime. */
			cur_snap = GetTransactionSnapshot();
			size_alloc = subxipoff = sizeof(SnapshotData) +
				cur_snap->xcnt * sizeof(TransactionId);
			if (cur_snap->subxcnt > 0)
				size_alloc += cur_snap->subxcnt * sizeof(TransactionId);
			oldcxt = MemoryContextSwitchTo(TopMemoryContext);
			copied = (Snapshot) palloc(size_alloc);
			memcpy(copied, cur_snap, sizeof(SnapshotData));
			/* ---- CRITICAL REGISTRATION PSEUDO-PIN (T5 pitfall fix)
			 *
			 * PG PopActiveSnapshot() [snapmgr.c L753-755] frees the
			 * Snapshot (via FreeSnapshot) when BOTH
			 * active_count==0 AND regd_count==0.  CopySnapshot above
			 * left both 0 → first push→pop around Pass1 MAIN SELECT
			 * would free our shared hash-owned snapshot WHILE IT IS
			 * STILL BEING REFERENCED by other concurrent sessions.
			 * Setting regd_count=1 masks as "persistently registered"
			 * so PopActiveSnapshot never auto-deallocates; real pfree
			 * happens only at apply/discard in
			 * ob_snapshot_cache_remove(). */
			copied->regd_count = 1;
			copied->active_count = 0;
			copied->copied = true;
			copied->snapXactCompletionCount = 0;
			if (cur_snap->xcnt > 0)
			{
				copied->xip = (TransactionId *) (copied + 1);
				memcpy(copied->xip, cur_snap->xip,
					cur_snap->xcnt * sizeof(TransactionId));
			}
			else
				copied->xip = NULL;
			if (cur_snap->subxcnt > 0 &&
				(!cur_snap->suboverflowed || cur_snap->takenDuringRecovery))
			{
				copied->subxip = (TransactionId *) ((char *) copied + subxipoff);
				memcpy(copied->subxip, cur_snap->subxip,
					cur_snap->subxcnt * sizeof(TransactionId));
			}
			else
				copied->subxip = NULL;
			MemoryContextSwitchTo(oldcxt);

			ob_snapshot_cache_insert(ctx->branch_id, copied);
			ctx->branch_main_snapshot = copied;
			ctx->snapshot_registered = true;
			ob_snapshot_cache_pin(ctx->branch_id);
			elog(DEBUG1, "overlay_branch: snapshot FIRST CAPTURE (bid=%d) xmin=%u",
				 (int) ctx->branch_id, (uint32) copied->xmin);
		}
	}
	else
	{
		ctx->branch_main_snapshot = NULL;
		ctx->snapshot_registered = false;
	}
}


void
overlay_branch_use_internal(const char *branch_name)
{
	/* T8 contract: 1-arg use_branch(name) (explicit SQL call) ALWAYS
	 * resolves mode = 'live' regardless of any prior session-local mode
	 * cache entry.  Mode-preserve applies ONLY to the GUC RESTORE path
	 * (assign hook overlay_branch.c), which calls
	 * use_with_mode_internal(name, cached_mode) directly.  This matches
	 * PART F2/F1 semantics: 1-param = live. */
	overlay_branch_use_with_mode_internal(branch_name, BRANCH_MODE_LIVE);
}


void
overlay_branch_use_with_mode_internal(const char *branch_name, const char *mode)
{
	if (mode == NULL || *mode == '\0')
		mode = BRANCH_MODE_LIVE;

	ob_g06_check_publication_leak_throttled();

	if (strcmp(mode, BRANCH_MODE_LIVE) != 0 &&
		strcmp(mode, BRANCH_MODE_SNAPSHOT) != 0)
	{
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid branch mode \"%s\"", mode),
			 errhint("Supported modes: 'live' (default), 'snapshot' (frozen MAIN MVCC view).")));
	}

	if (branch_name == NULL || *branch_name == '\0')
	{
		/* I8.2: save cached mode BEFORE clearing current, so that if we are
		 * on an ACTIVE branch its last-used mode survives GUC RESTORE +
		 * re-entry (which calls ob_mode_cache_lookup → cache wins over
		 * 'live' default). */
		if (CurrentBranchContext && CurrentBranchContext->is_active)
		{
			ob_mode_cache_set(CurrentBranchContext->branch_id,
							  CurrentBranchContext->mode);
			ob_exit_branch_cleanup(CurrentBranchContext);
		}
		{
			bool saved_flag = ob_in_guc_setconfig;
			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", "",
						PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}
		/* ===== REVIEW-260926 / R07 S09 PlanCache invalidation =====
		 *   When we EXIT a branch, cached CustomScan plans (prepared plans,
		 *   plancache entries) must be invalidated so that the NEXT query
		 *   in the same session, which will run WITHOUT the overlay, does
		 *   not reuse a plan that still routes through BranchScan.  Without
		 *   this invalidation, the user could run "PREPARE / EXECUTE"
		 *   BEFORE entering the branch, which caches a normal SeqScan
		 *   plan on MAIN (correct).  Then inside the branch the planner
		 *   would (correctly) re-plan and produce BranchScan.  But on
		 *   EXIT+next EXECUTE: PG plancache might serve the stale plan
		 *   captured INSIDE the branch (which does BranchScan overlay),
		 *   causing "outside branch" DML to still route through overlay
		 *   = silent wrong-row reads.
		 *
		 *   CacheInvalidateRelcacheAll() broadcasts SI invalidation for
		 *   ALL relations (we don't know which ones were touched during
		 *   the branch).  The plancache listens for those and marks
		 *   prepared plans as invalid → next EXECUTE does a full re-plan
		 *   against the new CURRENT state.  Cost is negligible compared
		 *   to the wrong-read/write risk.
		 *
		 *   CRITICAL extra step: CacheInvalidateRelcacheAll() only
		 *   *registers* the invalidation message into
		 *   `PrepareInvalidationState()->CurrentCmdInvalidMsgs` (see
		 *   inval.c L1549 RegisterRelcacheInvalidation).  Those are
		 *   *normally* flushed to the plancache callback at the END of
		 *   the top-level user command (CommandEndInvalidationMessages
		 *   in exec_simple_query → finish_xact_command path).  But
		 *   use_branch(b) itself IS the currently-executing top-level
		 *   command (`SELECT use_branch('b');`) so NEXT command's
		 *   prepared-plan EXECUTE might either (a) see the message and
		 *   re-plan OR (b) the function-call portal's invalidation
		 *   state be in a sub-transaction without propagation depending
		 *   on how the SPI stack works.  To guarantee that the plancache
		 *   marks cached plans invalid *before* we return to the user
		 *   we therefore *also* run CommandEndInvalidationMessages()
		 *   right here — LocalExecuteInvalidationMessage digests the
		 *   queue synchronously → plancache invalidation callback fires
		 *   → CachedPlanSource->invalidated = true → next EXECUTE
		 *   re-plans with 100% certainty (fixes the R07 test case where
		 *   prepared-in-main EXECUTE inside branch continued to serve
		 *   the stale SeqScan MAIN plan). */
		CacheInvalidateRelcacheAll();
		CommandEndInvalidationMessages();
		return;
	}

	/* REPEATABLE READ / SERIALIZABLE unsupported (doc/internals.md §3.1).
	 * A session branch view is only correct under READ COMMITTED
	 * per-statement snapshots: a transaction snapshot would be pinned for
	 * the whole view and could not observe a concurrent apply/discard.  The
	 * branch-exit path (empty name) returns above and stays allowed. */
	if (IsolationUsesXactSnapshot())
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("overlay_branch: use_branch() is not supported in a "
					"REPEATABLE READ or SERIALIZABLE transaction"),
			 errhint("Use READ COMMITTED, or commit and call use_branch() "
					 "in a new READ COMMITTED transaction.")));

	{
		int ret;
		char *esc_name;
		StringInfoData sql;
		int32 real_branch_id = 0;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "overlay_branch: SPI_connect() failed in use_branch");

		esc_name = quote_literal_cstr(branch_name);
		initStringInfo(&sql);

		/* Resolve id + state in ONE statement: branch_name is unique, so
		 * LIMIT 1 is exact.  Splitting existence / state / id into three
		 * lookups would open a TOCTOU window in which a concurrent
		 * apply/discard could flip the state between the checks. */
		appendStringInfo(&sql,
				 "SELECT branch_id, state FROM " OBTABLE_BRANCH " "
				 "WHERE branch_name = %s LIMIT 1",
				 esc_name);
		ret = SPI_execute(sql.data, true, 1);
		if (ret != SPI_OK_SELECT)
		{
			SPI_finish();
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: SPI branch lookup failed ret=%d", ret)));
		}
		if (SPI_processed == 0)
		{
			SPI_finish();
			ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", branch_name)));
		}

		{
			bool isnull;
			Datum d;
			char *st;
			bool state_active;

			d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
							  1, &isnull);
			if (isnull)
			{
				SPI_finish();
				elog(ERROR, "overlay_branch: branch_id for '%s' is NULL",
					 branch_name);
			}
			real_branch_id = DatumGetInt32(d);

			d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
							  2, &isnull);
			if (isnull)
			{
				SPI_finish();
				elog(ERROR, "overlay_branch: state for '%s' is NULL",
					 branch_name);
			}
			/* st lives in the SPI proc context: consumed here, freed by
			 * SPI_finish() below. */
			st = TextDatumGetCString(d);
			state_active = (strcmp(st, BRANCH_STATE_ACTIVE) == 0);
			SPI_finish();

			if (!state_active)
				ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("branch \"%s\" is not active", branch_name),
					 errhint("Create a fresh branch, or use DISCARD BRANCH to clean up.")));
		}

		/* §Gx / A7: schema drift guard.  Before we install ANY branch
		 * state (ctx / snapshot / cache / GUC), re-scan current DB
		 * user tables and compare to registry for this bid — any
		 * total/pk/hash drift → 55000 immediately (no side effects). */
		ob_registry_check_current_schema(real_branch_id, "use_branch");

		/* FR2 (I8.1 L1 2026-09-17 dry-run fix): use_branch NO LONGER
		 * takes SHARED advisory lock.
		 *
		 * Rationale (locked in doc/internals.md §3.3):
		 * Old V0: use=SHARED, apply=EXCLUSIVE → two sessions
		 *   S1=SHARED + S2=SHARED then S1=EXCLUSIVE-upgrade + S2=EXCLUSIVE-upgrade
		 *   = PostgreSQL classic shared→exclusive upgrade DEADLOCK 40P01.
		 * Fix: (a) use_branch = NO LOCK (just read catalog + install GUC/snapshot).
		 *          (b) apply_branch/discard_branch = FIRST run FR3 CAS
		 *              (UPDATE pg_branch SET state={applying,discarding} WHERE state='active'
		 *               — row-level UPDATE itself is 1-winner atomic CAS;
		 *               losers SPI_processed = 0 → kick out with ERROR immediately, no waiting).
		 *          (c) ONLY after CAS winner, take EXCLUSIVE advisory for the
		 *              duration of MAIN merge + catalog ops (single lock level
		 *              no upgrade → no deadlock possible).
		 * Deadlock freedom proof: lock graph has shared→exclusive upgrade edges removed;
		 * every lock transition is nolock→exclusive → wait-for graph is a forest of
		 * DAGs → cycles impossible (§ B.9.1 I8.1).
		 */

		if (CurrentBranchContext == NULL)
			CurrentBranchContext = MemoryContextAllocZero(TopMemoryContext,
									sizeof(BranchContext));
		else
			ob_ensure_unregister_snapshot(CurrentBranchContext);

		CurrentBranchContext->branch_id = real_branch_id;
		strlcpy(CurrentBranchContext->branch_name, branch_name, NAMEDATALEN);
		CurrentBranchContext->owner = GetUserId();
		CurrentBranchContext->is_active = true;
		strncpy(CurrentBranchContext->mode, mode, sizeof(CurrentBranchContext->mode) - 1);
		CurrentBranchContext->mode[sizeof(CurrentBranchContext->mode) - 1] = '\0';
		CurrentBranchContext->created_at = 0;
		CurrentBranchContext->invalidation_counter = 0;
		CurrentBranchContext->invalidation_last_check = 0;
		/* REVIEW-260926 / R19: Start snapshot-TTL clock only for
		 * snapshot mode.  For live mode we leave entered_at=0 so the
		 * throttled() TTL gate is never taken.  */
		CurrentBranchContext->entered_at =
			(strcmp(mode, BRANCH_MODE_SNAPSHOT) == 0)
			? GetCurrentTimestamp() : 0;

		ob_install_snapshot_for_mode(CurrentBranchContext, mode);

		/* I8.2: persist bid→mode to session-local cache AFTER all state
		 * installs but BEFORE SetConfigOption so that if SetConfigOption
		 * triggers nested assign-hook re-entry (common on GUC RESTORE
		 * paths) the nested use_internal → mode_cache_lookup returns the
		 * mode we're ABOUT to commit, not some stale prior value.  Cache
		 * write here replaces catalog UPDATE (RowExclusiveLock removed). */
		ob_mode_cache_set(real_branch_id, mode);

		{
			bool saved_flag = ob_in_guc_setconfig;
			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", branch_name,
						PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}
		/* ===== REVIEW-260926 / R07 S09 PlanCache invalidation (ENTER) =====
		 *   ENTER branch also needs relcache invalidation: PREPARE a plan in
		 *   plain MAIN mode (caches SeqScan path without BranchScan), then
		 *   the user calls use_branch(b1) inside the same session.  Without
		 *   invalidation, the NEXT EXECUTE of that prepared statement
		 *   would serve the OLD plan from plancache (direct SeqScan) and
		 *   completely bypass the overlay = silent drift-read from MAIN.
		 *   By invalidating here, PREPAREd statements are forced to
		 *   re-plan on the next EXECUTE → re-plan with branch active →
		 *   Planner hook fires → BranchScan injected → correct overlay
		 *   semantics. */
		CacheInvalidateRelcacheAll();
		CommandEndInvalidationMessages();
		/* NOTE: sql.data / esc_name are palloc'd in caller's portal context;
		 * we do NOT manual pfree here — portal cleanup handles them.  Prior
		 * attempts at explicit pfree caused "pfree called with invalid pointer"
		 * because the appendStringInfo pipeline had realloc'd the buffer and
		 * the old pointer was stale after the 3-stage reset/rewrite cycle. */
		elog(DEBUG1, "overlay_branch_use_with_mode_internal: name='%s' mode='%s'",
			 branch_name, mode);
	}
}

const char *
overlay_branch_get_current_name(void)
{
	if (CurrentBranchContext == NULL || !CurrentBranchContext->is_active)
		return NULL;
	return CurrentBranchContext->branch_name;
}

int32
overlay_branch_get_current_id(void)
{
	if (CurrentBranchContext == NULL || !CurrentBranchContext->is_active)
		return 0;
	return CurrentBranchContext->branch_id;
}

bool
overlay_branch_is_active(void)
{
	extern bool overlay_branch_enabled;

	/* FR4: run throttled invalidation check on every public "are we on
	 * a branch?" guard.  DQL path only (for_dml=false) so the caller —
	 * BranchScan, rel_ok, ddl_ok, planner shortcuts — all get a soft
	 * NOTICE + MAIN-fallback kickout.  The DML hard-ERROR path is
	 * triggered *separately* at write_redirect ExecutorRun entry and
	 * overlay_delta_insert safety net with for_dml=true.
	 *
	 * The throttled() call below itself carries ob_in_invalidation_check
	 * depth-1 recursion guard (include/overlay_branch.h L115 extern,
	 * overlay_branch.c L95 definition), so even if the SPI ground truth
	 * inside throttled re-enters PG's planner which re-calls THIS
	 * function, the inner invocation skips the SPI heavy block via the
	 * flag and just returns the pure-memory truth check.  See
	 * test/sql/overlay_branch_invalidation.sql Sections C/D/J. */
	(void) ob_invalidate_check_throttled(false);

	/* Fall-back to pure-memory truth check after kickout; if the
	 * throttled check decided the branch is dead, ctx->is_active is
	 * already false so this returns false. */
	return (overlay_branch_enabled &&
			CurrentBranchContext != NULL &&
			CurrentBranchContext->is_active &&
			overlay_branch_current_name != NULL &&
			*overlay_branch_current_name != '\0');
}

/* ----------
 * ob_broadcast_state_change — FR1 fast-path NOTIFY
 *
 * Payload format: "<int32 bid>:<new_state>"
 * Channel:       OB_NOTIFY_CHANNEL ("ob_branch_state")
 *
 * Called *immediately after* the catalog UPDATE + cascade (delta rows
 * DELETE) succeed, i.e. AFTER ground truth in pg_branch has flipped.
 * Receivers drain via AcceptInvalidationMessages() (sinval shared bus)
 * on their next ob_invalidate_check_throttled() call.
 * ----------
 */
void
ob_broadcast_state_change(int32 bid, const char *new_state)
{
	char   *payload;

	if (bid <= 0 || new_state == NULL || *new_state == '\0')
		return;

	payload = psprintf("%d:%s", (int) bid, new_state);
	Async_Notify(OB_NOTIFY_CHANNEL, payload);
	pfree(payload);

	elog(DEBUG1, "overlay_branch: broadcast bid=%d state=%s channel=%s",
		 (int) bid, new_state, OB_NOTIFY_CHANNEL);
}

/* ----------
 * ob_invalidate_check_throttled — FR4: counter + wall-clock + sinval
 * combined invalidation trigger.
 *
 * Three independent "run heavy SPI ground truth" conditions (OR):
 *   (B) ctx->invalidation_counter >= ob_invalidation_check_threshold
 *       (default 32 — L3 pg_regress can force this via
 *       force_invalidation_check() by bumping the counter)
 *   (C) wall-clock: (now - invalidation_last_check) >
 *       ob_invalidation_check_interval_ms ms (default 100 ms)
 *   (D) sinval drain: every call drains AcceptInvalidationMessages().
 *       NOTIFY via shared bus also re-checks so stale state never
 *       survives a sinval delivery.
 *
 * SPI ground truth: SELECT state FROM pg_branch WHERE branch_id = X AND
 * branch_name = Y LIMIT 1 (bid + name to prevent name reuse collision).
 *
 * @for_dml = split decision (FR1 + FR4):
 *   false → DQL guard: state != active → ereport(NOTICE) +
 *           ob_exit_branch_cleanup(ctx) + set overlay_branch.current='' +
 *           return false.  (Callers fall through to MAIN reads.)
 *   true  → DML guard: state != active → ereport(ERROR,
 *           ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE) + cleanup.
 *           (Drift-writes into MAIN are 100% disallowed.)
 *
 * Returns true iff branch is still active (either stale-true or just
 * verified true); false iff kickout happened.
 * ----------
 */
bool
ob_invalidate_check_throttled(bool for_dml)
{
	BranchContext *ctx;
	bool		need_heavy_check;
	TimestampTz now;
	bool		saved_flag;
	bool		result;
	bool		kicked_out = false;
	bool		local_is_applied = false;
	bool		local_is_discarded = false;
	char		local_state_buf[32];
	char		local_branch_name[NAMEDATALEN];
	const char *local_what_happened;
	/* REVIEW-260926 / R19: still_active and got_row live in function-scope
	 * (not block-scope) because snapshot TTL kickout path jumps directly
	 * to snapshot_ttl_kickout which lies PAST the Heavy SPI ground-truth
	 * block.  Moving declarations up here keeps the variables in scope at
	 * both the original kickout entry point and the goto landing point. */
	bool		still_active = false;
	bool		got_row = false;
	/* R19: true when the kickout was triggered by the local snapshot TTL,
	 * not by another session flipping the branch state.  Keeps the TTL
	 * diagnostic from being overwritten by the generic invalidated text. */
	bool		ttl_expired = false;

	local_state_buf[0] = '\0';
	local_branch_name[0] = '\0';
	local_what_happened = "invalidated by another session";

	/* ---- Depth-1 recursion guard.  ANY caller path may re-enter here via
	 *   throttled → SPI_execute(pg_branch) → planner → is_active → throttled.
	 * Return the in-memory ctx->is_active snapshot; the outer call will do
	 * the real SPI work once it unwinds.  See long comment in the previous
	 * committed version for the full rationale (kept in git history).
	 * ---- */
	/* Fast path: nothing to throttle if not on an active branch. */
	if (ob_in_invalidation_check)
	{
		if (CurrentBranchContext == NULL)
			return false;
		return CurrentBranchContext->is_active;
	}

	/* Fast path: nothing to throttle if not on an active branch. */
	if (CurrentBranchContext == NULL || !CurrentBranchContext->is_active)
		return false;

	ctx = CurrentBranchContext;
	need_heavy_check = false;
	now = 0;
	result = true;

	/* — Outer exception safety: raise the guard flag and make sure it is
	 * always lowered on every exit path (normal-return / ereport /
	 * longjmp from PG_CATCH sub-blocks). */
	saved_flag = ob_in_invalidation_check;
	ob_in_invalidation_check = true;
	PG_TRY();
	{
		/* (D) Always drain the shared invalidation + NOTIFY bus first.
		 * Delivers cross-session ob_branch_state notifications too. */
		AcceptInvalidationMessages();

		/* (A) Count-based throttle.  Increment BEFORE compare so
		 * threshold=1 fires on the very first call (matches §J test).
		 *
		 * I8.3: Allow L1 _debug_invalidation_counter_throttle_override to
		 * force a smaller-than-default count (e.g. 2) so ob_state_inval
		 * spec permutations don't need 32 calls to force the kickout. */
		{
			uint32 effective_threshold;
			if (ob_debug_invalidation_counter_throttle_override >= 0)
				effective_threshold =
					(uint32) ob_debug_invalidation_counter_throttle_override;
			else
				effective_threshold =
					(uint32) ob_invalidation_check_threshold;
			if (effective_threshold > 0)
			{
				ctx->invalidation_counter++;
				if (ctx->invalidation_counter >= effective_threshold)
					need_heavy_check = true;
			}
		}

		/* (B) Wall-clock throttle.  First-call (last_check==0) always
		 * fires once to establish the baseline timestamp. */
		now = GetCurrentTimestamp();
		if (ob_invalidation_check_interval_ms > 0)
		{
			long	secs;
			int		usecs;
			long	interval_ms;

			if (ctx->invalidation_last_check == 0)
			{
				need_heavy_check = true;
			}
			else
			{
				TimestampDifference(ctx->invalidation_last_check, now,
									&secs, &usecs);
				interval_ms = secs * 1000L + (long) (usecs / 1000);
				if (interval_ms >= ob_invalidation_check_interval_ms)
					need_heavy_check = true;
			}
		}

		/* (B-2) REVIEW-260926 / R19: snapshot mode TTL hard cap.
		 *
		 *   snapshot mode pins RecentGlobalXmin for the whole cluster,
		 *   which delays global VACUUM and grows storage indefinitely
		 *   for all tables if a user forgets to exit snapshot mode.
		 *   Enforce max-hold minutes once per throttled() invocation
		 *   (every is_active / DML / DQL gate goes through here, so
		 *   worst-case overshoot is one throttle cycle after the
		 *   budget expires).  entered_at is set by use_with_mode_internal
		 *   on mode==snapshot activation; live mode clears it to 0 so
		 *   the gate is never taken on the default live path. */
		if (ctx->entered_at != 0 &&
			(ob_snapshot_max_hold_minutes > 0 ||
			 ob_debug_snapshot_ttl_seconds >= 0))
		{
			long		elapsed_secs;
			int			elapsed_usecs;
			int64		elapsed_units;
			int64		limit_units;

			TimestampDifference(ctx->entered_at, now,
								&elapsed_secs, &elapsed_usecs);
			/* The debug override (_debug_snapshot_ttl_seconds >= 0) uses a
			 * per-second cap so an L3/L1 test can reach the kickout without
			 * waiting a full minute; the production path counts whole
			 * minutes. */
			if (ob_debug_snapshot_ttl_seconds >= 0)
			{
				elapsed_units = (int64) elapsed_secs;
				limit_units = (int64) ob_debug_snapshot_ttl_seconds;
			}
			else
			{
				elapsed_units = (int64) elapsed_secs / 60;
				limit_units = (int64) ob_snapshot_max_hold_minutes;
			}
			if (elapsed_units >= limit_units)
			{
				/* TTL expired — a purely local kickout: the branch is
				 * still 'active' in the catalog (no other session
				 * touched it); we force the user off because the
				 * snapshot pinned the cluster horizon too long.  We do
				 * NOT touch SPI (this is a local-side guard).  Record
				 * the real reason so the kickout diagnostic below is
				 * not overwritten by the generic "invalidated by
				 * another session" text. */
				local_is_applied = false;
				local_is_discarded = false;
				strcpy(local_state_buf, "expired");
				local_what_happened =
					"exited because the snapshot hold time exceeded "
					"overlay_branch.snapshot_max_hold_minutes (R19 TTL)";
				ttl_expired = true;
				still_active = false;
				got_row = true;
				goto snapshot_ttl_kickout;
			}
		}

		/* Fast path: stale-true return.  Both throttle gates say "not
		 * yet" — the optimistic contract of FR4. */
		if (!need_heavy_check)
		{
			result = true;
			goto throttle_clean_exit;
		}

		/* ==========  Heavy SPI ground-truth  ========== */
		{
			StringInfoData sql;
			int			ret;
			char	   *esc_name;
			int			spi_connect_ret;
			bool		did_spi_connect = false;

			still_active = false;
			got_row = false;

			spi_connect_ret = SPI_connect();
			if (spi_connect_ret == SPI_OK_CONNECT)
				did_spi_connect = true;
			else if (spi_connect_ret != SPI_ERROR_CONNECT)
				elog(ERROR, "overlay_branch: throttled SPI_connect "
					 "failed ret=%d", spi_connect_ret);

			ctx->invalidation_counter = 0;
			ctx->invalidation_last_check = now;

			esc_name = quote_literal_cstr(ctx->branch_name);
			initStringInfo(&sql);
			appendStringInfo(&sql,
							 "SELECT state FROM " OBTABLE_BRANCH " "
							 "WHERE branch_id = %d AND branch_name = %s LIMIT 1",
							 (int) ctx->branch_id, esc_name);
			pfree(esc_name);

			overlay_overlay_helper_enter();
			ret = SPI_execute(sql.data, true, 1);
			pfree(sql.data);

			if (ret == SPI_OK_SELECT && SPI_processed == 1)
			{
				bool		isnull;
				Datum		d;

				d = SPI_getbinval(SPI_tuptable->vals[0],
								  SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
				{
					char   *st = TextDatumGetCString(d);

					got_row = true;
					if (strcmp(st, BRANCH_STATE_ACTIVE) == 0)
						still_active = true;
					pfree(st);
				}
			}
			overlay_overlay_helper_exit();

			if (did_spi_connect)
			{
				SPI_finish();
				did_spi_connect = false;
			}

			if (still_active)
			{
				elog(DEBUG2, "overlay_branch: throttled verified "
					 "bid=%d name='%s' active=true",
					 (int) ctx->branch_id, ctx->branch_name);
				result = true;
				goto throttle_clean_exit;
			}
snapshot_ttl_kickout:
			/* ===== Kickout path: ground truth says state != active ===== */
			/* (R19 snapshot TTL jump lands here with local_state_buf
			 * and what_happened already populated; other paths fall
			 * through and fill both below.)  The TTL case must not be
			 * re-read: the catalog still says 'active', so a re-read
			 * would clobber the 'expired' reason we recorded. */
			if (!ttl_expired)
				strcpy(local_state_buf, got_row ? "<known>" : "missing");

			if (got_row && !ttl_expired)
			{
				StringInfoData sq2;
				int			r2;
				char	   *esc2;
				int			reread_spi_ret;
				bool		reread_did_connect = false;

				reread_spi_ret = SPI_connect();
				if (reread_spi_ret == SPI_OK_CONNECT)
					reread_did_connect = true;
				else if (reread_spi_ret != SPI_ERROR_CONNECT)
					elog(ERROR, "overlay_branch: kickout reread "
						 "SPI_connect ret=%d", reread_spi_ret);

				esc2 = quote_literal_cstr(ctx->branch_name);
				initStringInfo(&sq2);
				appendStringInfo(&sq2,
								 "SELECT state FROM " OBTABLE_BRANCH " "
								 "WHERE branch_id = %d AND branch_name = %s LIMIT 1",
								 (int) ctx->branch_id, esc2);
				pfree(esc2);

				overlay_overlay_helper_enter();
				r2 = SPI_execute(sq2.data, true, 1);
				pfree(sq2.data);
				if (r2 == SPI_OK_SELECT && SPI_processed == 1)
				{
					bool	isnull2;
					Datum	d2;

					d2 = SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, 1,
									   &isnull2);
					if (!isnull2)
					{
						char   *st = TextDatumGetCString(d2);

						strncpy(local_state_buf, st,
								sizeof(local_state_buf) - 1);
						local_state_buf[sizeof(local_state_buf) - 1] = '\0';
						if (strcmp(st, BRANCH_STATE_APPLIED) == 0)
							local_is_applied = true;
						else if (strcmp(st, BRANCH_STATE_DISCARDED) == 0)
							local_is_discarded = true;
						pfree(st);
					}
				}
				overlay_overlay_helper_exit();
				if (reread_did_connect)
					SPI_finish();
			}

			/* --- Planner-hook / observer suppression gate (extended) ---
			 * The heavy SPI check has determined the branch is
			 * invalid, but WE MUST NOT DO THE DESTRUCTIVE CLEANUP
			 * SIDE-EFFECTS here unless the caller is the ONE entry
			 * point that explicitly opted in (see write_redirect.c
			 * ExecutorRun hook, which sets ob_throttled_allow_kickout
			 * when it calls throttled with dml_split already decided).
			 *
			 * Rationale — ordering race resolved by this gate:
			 *
			 * Consider an UPDATE statement against a user table whose
			 * branch context has stale `is_active=true` (another
			 * session flipped state to applied/discarded while we
			 * weren't looking).  With FR4 throttling threshold=1
			 * (user-settable), EVERY call to throttled() passes the
			 * counter gate, including "observer" calls that do SPI
			 * SELECTs on pg_branch (e.g. our own heavy-check internal
			 * SPI re-read, or the planner hook re-planning a
			 * trigger-side query, or rel_ok/ddl_ok guards examining
			 * the target RTE before the actual ModifyTable
			 * ExecutorRun hook does its dml_split-aware check).
			 *
			 * If any observer call with for_dml=false actually ran
			 * ob_exit_branch_cleanup() + cleared the GUC + emitted a
			 * NOTICE BEFORE the real ModifyTable ExecutorRun called
			 * throttled(for_dml=true), the DML ERROR split would
			 * NEVER fire (the L565 fast-path sees ctx.is_active=false
			 * and returns false immediately, fall-through writes
			 * silent MAIN pollution).
			 *
			 * D7 Section of test/sql/overlay_branch_invalidation.sql
			 * is the exact deterministic reproduction.
			 *
			 * The observer suppression gate therefore extends the
			 * planner-hook-only suppression (ob_in_planner_hook) to a
			 * broader, caller-opted-in model: ONLY the caller that
			 * has ALREADY committed to a specific dml_split outcome
			 * — write_redirect ExecutorRun — sets `ob_throttled_allow_
			 * kickout = true` around the call.  Every other caller
			 * (planner, rel_ok, ddl_ok, the is_active_by_name(name)
			 * synonym, force_invalidation_check SQL wrapper, Process
			 * Utility command guards) just gets the boolean truth
			 * WITHOUT destructive state mutation.  The actual cleanup
			 * + NOTICE / ERROR + GUC reset is deferred to the next
			 * opted-in ExecutorRun call, where CmdType is known so
			 * the DQL vs DML split can choose the correct NOTICE vs
			 * ERROR + HINT.
			 *
			 * Note: force_invalidation_check() is special: it MUST
			 * still kick the user off the branch for UX (user
			 * explicitly said "check NOW").  That call site is opted
			 * in by wrapping its throttled(false) call in allow_
			 * kickout=true.  See overlay_branch.c L1000. */
			if (!ob_throttled_allow_kickout)
			{
				result = false;
				kicked_out = false;
				goto throttle_clean_exit;
			}

			/* Save kickout inputs from ctx BEFORE cleanup zeroes it. */
			strncpy(local_branch_name, ctx->branch_name, NAMEDATALEN - 1);
			local_branch_name[NAMEDATALEN - 1] = '\0';
			if (!ttl_expired)
				local_what_happened = local_is_applied ? "applied by another session" :
									  local_is_discarded ? "discarded by another session"
														 : "invalidated by another session";

			/* Planner-hook suppression (kept as dead-code but safe:
			 * ob_in_planner_hook implies allow_kickout is NOT set,
			 * so we bailed above.  Retained so future reordering of
			 * the code cannot accidentally expose the cleanup to
			 * planner paths even if someone removes the flag. */
			if (ob_in_planner_hook)
			{
				result = false;
				kicked_out = false;
				goto throttle_clean_exit;
			}

			ob_exit_branch_cleanup(ctx);

			{
				bool s2 = ob_in_guc_setconfig;
				ob_in_guc_setconfig = true;
				SetConfigOption("overlay_branch.current", "",
								PGC_USERSET, PGC_S_SESSION);
				ob_in_guc_setconfig = s2;
			}
			/* R07 S09 PlanCache invalidation (throttled kickout): see
			 * enter/exit paths above for rationale (CacheInvalidateRelcacheAll
			 * prevents stale BranchScan plan reuse from non-branch EXECUTE. */
			CacheInvalidateRelcacheAll();
			CommandEndInvalidationMessages();

			kicked_out = true;
			result = false;
		}
throttle_clean_exit:	;
	}
	PG_CATCH();
	{
		ob_in_invalidation_check = saved_flag;
		PG_RE_THROW();
	}
	PG_END_TRY();
	ob_in_invalidation_check = saved_flag;

	if (kicked_out)
	{
		/* EREPORT after lowering the guard flag and after PG_TRY so
		 * ereport's longjmp (ERROR) cannot see a stale guard flag. */
		if (for_dml)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_branch: branch \"%s\" is no longer "
							"active (state=%s); cannot write through it",
							local_branch_name, local_state_buf),
					 errhint("The branch was %s.  "
							 "Re-connect to MAIN and retry the write.",
							 local_what_happened)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("overlay_branch: branch \"%s\" is no longer "
							"active (state=%s); reverting to Main",
							local_branch_name, local_state_buf)));
			return false;
		}
	}
	return result;
}

/* ---------- apply_internal: 3-pass optimistic replay ---------- */
void
overlay_branch_apply_internal(const char *branch_name)
{
	int			ret;
	char	   *esc_name;
	int32		bid = 0;
	char	   *state = NULL;
	bool		leaving_current = false;
	List	   *relids = NIL;
	ListCell   *rlc;
	Relation	rel = NULL;

	ob_g06_check_publication_leak_throttled();

	if (branch_name == NULL || *branch_name == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("branch name cannot be empty")));

	elog(NOTICE, "overlay_branch: APPLY BRANCH '%s' (passes D→U→I with optimistic conflict detection)",
		 branch_name);

	/* Step 1: verify branch is ACTIVE */
	{
		StringInfoData sql1;

		initStringInfo(&sql1);
		esc_name = quote_literal_cstr(branch_name);
		appendStringInfo(&sql1,
						 "SELECT branch_id, state FROM " OBTABLE_BRANCH " "
						 "WHERE branch_name = %s LIMIT 1",
						 esc_name);
		ret = ob_spi_one_shot(sql1.data, true, 1);
		pfree(sql1.data);
		if (ret != SPI_OK_SELECT)
		{
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: apply existence check failed for '%s': SPI ret=%d",
							branch_name, ret)));
		}
		if (SPI_processed == 0)
		{
			SPI_finish();
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" does not exist", branch_name)));
		}
		{
			bool		isnull;
			Datum		v;

			v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
			if (isnull)
			{
				SPI_finish();
				pfree(esc_name);
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("overlay_branch: apply found NULL branch_id for '%s'",
								branch_name)));
			}
			bid = DatumGetInt32(v);

			v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
			if (isnull)
			{
				state = NULL;
			}
			else
			{
				MemoryContext oldmc2;

				oldmc2 = MemoryContextSwitchTo(TopMemoryContext);
				state = TextDatumGetCString(v);
				MemoryContextSwitchTo(oldmc2);
			}
		}
		SPI_finish();
	}

	/* §Gx / A7: schema drift guard.  Run BEFORE we take the advisory
	 * lock (fast fail before any serialization) so drift errors don't
	 * block other apply/discard operations on unrelated branches. */
	ob_registry_check_current_schema(bid, "apply_branch");

	/* FR2/FR3: take branch-level EXCLUSIVE advisory lock BEFORE any catalog
	 * UPDATE / RowExclusiveLock on user tables, so we strictly follow
	 * Advisory → (catalog locks + user RowExclusiveLock) deadlock order.
	 * The acquire is NON-BLOCKING (doc/internals.md §3.3): if a delta writer
	 * holds ShareLock, or another apply/discard holds ExclusiveLock, we fail
	 * fast instead of waiting — a blocking acquire would recreate the classic
	 * ShareLock→ExclusiveLock upgrade deadlock.  Self-deadlock is impossible
	 * because apply/discard never hold ShareLock themselves. */
	if (bid <= 0)
		elog(ERROR, "overlay_branch: apply found invalid branch_id=%d", bid);
	if (!ob_try_branch_advisory_lock(bid, ExclusiveLock))
	{
		if (state) pfree(state);
		pfree(esc_name);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("overlay_branch: apply_branch(\"%s\") cannot acquire the branch lock",
						branch_name),
				 errdetail("Another apply/discard is in progress, or in-flight "
						   "delta writers have not yet committed."),
				 errhint("Retry once the conflicting transaction ends.")));
	}

	if (state == NULL || strcmp(state, BRANCH_STATE_ACTIVE) != 0)
	{
		char	   *reason;

		reason = psprintf("branch \"%s\" is not active (state=%s); "
						  "apply can only be called once on an active branch",
						  branch_name, state ? state : "NULL");
		if (state)
			pfree(state);
		pfree(esc_name);
		overlay_guard_ereport_fail("apply_branch", NULL, branch_name, reason);
	}

	/* FR3: CAS transition to transient state='applying' BEFORE any 3-pass writes, to
	 * guarantee no other session can sneak in a delta-write while we are halfway
	 * between D-pass and I-pass (which would cause silent data loss — see
	 * doc/internals.md §3.4).
	 *
	 * Note: we already hold EXCLUSIVE advisory lock on bid (FR2) so no real
	 * concurrent apply/discard can race with us — still double guard (atomic CAS) ensures correctness.
	 */
	{
		StringInfoData cas_sql;
		int			cas_ret_local;

		cas_ret_local = 0;
		initStringInfo(&cas_sql);
		appendStringInfo(&cas_sql,
						 "UPDATE " OBTABLE_BRANCH " "
						 "SET state = '%s' WHERE branch_id = %d "
						 "AND state = '%s'",
						 BRANCH_STATE_APPLYING,
						 (int) bid,
						 BRANCH_STATE_ACTIVE);
		cas_ret_local = ob_spi_one_shot(cas_sql.data, false, 0);
		if (cas_ret_local != SPI_OK_UPDATE &&
			cas_ret_local != SPI_OK_UPDATE_RETURNING)
		{
			/* ob_spi_one_shot leaves SPI connected on non-fatal returns;
			 * finish it before ereport to balance connect count */
			SPI_finish();
			pfree(cas_sql.data);
			if (state) pfree(state);
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_branch: apply_branch(\"%s\") failed to transition to \"%s\" state",
							branch_name, BRANCH_STATE_APPLYING),
					 errhint("Concurrent state mutation; branch may no longer be ACTIVE.")));
		}
		if (SPI_processed != 1)
		{
			SPI_finish();
			pfree(cas_sql.data);
			if (state) pfree(state);
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_branch: apply_branch(\"%s\") CAS state transition refused"
							" (SPI_processed=%lu != 1); branch no longer ACTIVE)",
							branch_name, (unsigned long) SPI_processed),
					 errhint("Branch state was changed externally between SELECT and UPDATE.")));
		}
		SPI_finish();
		pfree(cas_sql.data);
	}

	/* I8.4: Debug apply-state window — widen the transient applying
	 * state for L1 ob_applying_freeze.spec.  After CAS-winner moved us
	 * into 'applying' state (above), pg_sleep(N) so s2_insert_delta can
	 * enter and hit the state guard while CAS winner is still holding
	 * EXCLUSIVE advisory.  0 = production default, no sleep. */
	if (ob_debug_apply_sleep_sec > 0)
	{
		StringInfoData slp;
		int slpret;
		initStringInfo(&slp);
		appendStringInfo(&slp, "SELECT pg_sleep(%d)",
						 ob_debug_apply_sleep_sec);
		if (SPI_connect() == SPI_OK_CONNECT)
		{
			slpret = SPI_execute(slp.data, false, 0);
			SPI_finish();
			(void) slpret;
		}
		pfree(slp.data);
	}

	/* Step 2: kick user out of this session branch (if on it) */
	if (CurrentBranchContext &&
		CurrentBranchContext->is_active &&
		strncmp(CurrentBranchContext->branch_name, branch_name,
				NAMEDATALEN) == 0)
	{
		leaving_current = true;
		ob_exit_branch_cleanup(CurrentBranchContext);
	}

	/* Step 3: gather relid set */
	{
		StringInfoData sql2;

		initStringInfo(&sql2);
		appendStringInfo(&sql2,
						 "SELECT DISTINCT relid::oid FROM " OBTABLE_DELTA " "
						 "WHERE branch_id = %d ORDER BY 1",
						 (int) bid);
		ret = ob_spi_one_shot(sql2.data, true, 0);
		pfree(sql2.data);
		if (ret != SPI_OK_SELECT)
		{
			if (state) pfree(state);
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: apply relid-set lookup failed bid=%d SPI ret=%d",
							bid, ret)));
		}
		if (SPI_processed > 0 && SPI_tuptable && SPI_tuptable->vals)
		{
			MemoryContext oldmc = NULL;
			uint64		i;

			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			for (i = 0; i < SPI_processed; i++)
			{
				bool		isnull;
				Datum		d;

				d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
					relids = lappend_oid(relids, DatumGetObjectId(d));
			}
			MemoryContextSwitchTo(oldmc);
		}
		SPI_finish();
	}

	elog(DEBUG1, "apply[dbg] relids count=%d", list_length(relids));

	/* For each relation: three-pass (D→U→I) replay with apply-bypass guard */
	{
		bool		saved_in_apply = ob_in_apply_operation;

		ob_in_apply_operation = true;
		PG_TRY();
		{
			foreach(rlc, relids)
			{
				Oid			relid = lfirst_oid(rlc);
				List	   *deltas;
				ListCell   *dlc;
				ObApplyRelInfo *ri;

				elog(DEBUG1, "apply[dbg] rel=%u table_open", (unsigned) relid);
				if (rel != NULL)
					table_close(rel, NoLock);
				rel = table_open(relid, RowExclusiveLock);
				ri = ob_apply_rel_info_build(rel);

				elog(DEBUG1, "apply[dbg] overlay_delta_list_for_rel");
				deltas = overlay_delta_list_for_rel(bid, relid);
				elog(DEBUG1, "apply[dbg] delta count=%d", list_length(deltas));

				/* Phase 3B: a single set-based statement per operation when
				 * the relation qualifies, else the legacy per-row replay. */
				if (ob_apply_batch_can_use(ri, deltas))
				{
					elog(DEBUG1, "apply[dbg] batch replay");
					ob_apply_relation_batch(ri, bid, deltas, branch_name);
				}
				else
				{
					elog(DEBUG1, "apply[dbg] per-row replay");
					ob_apply_relation_per_row(ri, deltas, branch_name);
				}

				elog(DEBUG1, "apply[dbg] freeing deltas");
				foreach(dlc, deltas)
				{
					DeltaTuple *dt = (DeltaTuple *) lfirst(dlc);
					if (dt->key) pfree(dt->key);
					if (dt->old_version) pfree(dt->old_version);
					if (dt->tuple_data) pfree(dt->tuple_data);
					pfree(dt);
				}
				list_free(deltas);
				ob_apply_rel_info_free(ri);
				elog(DEBUG1, "apply[dbg] rel loop end");
			}
			if (rel != NULL)
				table_close(rel, NoLock);
		}
		PG_CATCH();
		{
			ob_in_apply_operation = saved_in_apply;
			PG_RE_THROW();
		}
		PG_END_TRY();
		ob_in_apply_operation = saved_in_apply;
	}

	elog(DEBUG1, "apply[dbg] Step4 finalize");
	/* Step 4: finalize. state→applied + delete delta rows. */
	{
		StringInfoData sql_final;

		initStringInfo(&sql_final);
		appendStringInfo(&sql_final,
						 "UPDATE " OBTABLE_BRANCH " SET state = '%s' "
						 "WHERE branch_name = %s AND state = '%s'",
						 BRANCH_STATE_APPLIED,
						 esc_name,
						 BRANCH_STATE_APPLYING);
		ret = ob_spi_one_shot(sql_final.data, false, 1);
		if (ret != SPI_OK_UPDATE)
		{
			pfree(sql_final.data);
			if (state) pfree(state);
			pfree(esc_name);
			list_free(relids);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: apply final UPDATE pg_branch failed for '%s' SPI ret=%d",
							branch_name, ret)));
		}
		/* CAS parity with the Step0 active→applying transition: the final
		 * applying→applied UPDATE must touch exactly one row.  A zero-row
		 * result means the state was already moved externally; treat it as
		 * a refusal rather than silently reporting a successful apply. */
		if (SPI_processed != 1)
		{
			SPI_finish();
			pfree(sql_final.data);
			if (state) pfree(state);
			pfree(esc_name);
			list_free(relids);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_branch: apply final transition to \"%s\" refused"
							" for '%s' (SPI_processed=%lu != 1)",
							BRANCH_STATE_APPLIED, branch_name,
							(unsigned long) SPI_processed),
					 errhint("Branch state changed externally during apply.")));
		}
		SPI_finish();

		resetStringInfo(&sql_final);
		appendStringInfo(&sql_final,
						 "DELETE FROM " OBTABLE_DELTA " WHERE branch_id = %d",
						 (int) bid);
		ret = ob_spi_one_shot(sql_final.data, false, 0);
		if (ret != SPI_OK_DELETE)
		{
			pfree(sql_final.data);
			if (state) pfree(state);
			pfree(esc_name);
			list_free(relids);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: apply final DELETE delta rows failed for '%s' SPI ret=%d",
							branch_name, ret)));
		}
		SPI_finish();
		pfree(sql_final.data);

		/* ACC-1: an applied branch is terminal — the idempotency guard
		 * refuses a later discard_branch() — so its schema-epoch
		 * fingerprint rows would otherwise leak forever (only discard
		 * cascades them).  Delete them here too, symmetric with
		 * discard_branch(). */
		ob_registry_cascade_discard(bid);
	}

	ob_broadcast_state_change(bid, BRANCH_STATE_APPLIED);
	/* FR5: branch is APPLIED now — evict its frozen MAIN snapshot
	 * from the per-bid hash cache (if any) and free the TopMC
	 * CopySnapshot chunk.  After apply, bid no longer ACTIVE so no
	 * future snapshot-mode entry can reference it.  Safe if cache
	 * entry didn't exist (LIVE branches never capture one). */
	ob_snapshot_cache_remove(bid);
	/* I8.2: symmetric mode-cache cleanup.  After apply, bid → 'applying'
	 * then 'applied' state, so any future re-create would insert a new
	 * catalog row with mode='live' default.  Removing the stale session-
	 * local bid mode avoids cache collisions. */
	ob_mode_cache_remove(bid);


	if (leaving_current)
	{
		bool saved_flag = ob_in_guc_setconfig;
		ob_in_guc_setconfig = true;
		SetConfigOption("overlay_branch.current", "",
						PGC_USERSET, PGC_S_SESSION);
		ob_in_guc_setconfig = saved_flag;
		/* R07 S09 PlanCache invalidation (apply kickout): same rationale
		 * as exit_path above — stale plancache entries for non-branch
		 * mode must be invalidated otherwise next PREPAREd EXECUTE
		 * routes through old BranchScan plan. */
		CacheInvalidateRelcacheAll();
		CommandEndInvalidationMessages();
	}

	pfree(esc_name);
	if (state) pfree(state);
	list_free(relids);

	elog(NOTICE, "overlay_branch: APPLY BRANCH '%s' completed (state=applied, delta rows deleted)",
		 branch_name);
}

/* ---------- discard_internal ---------- */
void
overlay_branch_discard_internal(const char *branch_name)
{
	int			ret;
	char	   *esc_name;
	StringInfoData sql;
	bool		leaving_current = false;
	int32		bid = 0;
	bool		already_discarded_applied = false;

	ob_g06_check_publication_leak_throttled();

	if (branch_name == NULL || *branch_name == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("branch name cannot be empty")));

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect() failed in discard_branch");

	esc_name = quote_literal_cstr(branch_name);
	initStringInfo(&sql);

	/* 1) Existence + current-state check */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT branch_id, state FROM " OBTABLE_BRANCH " "
					 "WHERE branch_name = %s LIMIT 1",
					 esc_name);
	ret = SPI_execute(sql.data, true, 1);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: discard existence check failed for '%s': SPI ret=%d",
						branch_name, ret)));
	}
	if (SPI_processed == 0)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", branch_name)));
	}
	{
		bool		isnull;
		Datum		v;
		char	   *st;

		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		if (isnull)
		{
			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: discard found NULL branch_id for '%s'",
							branch_name)));
		}
		bid = DatumGetInt32(v);

		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
		st = isnull ? NULL : TextDatumGetCString(v);
		if (st != NULL &&
			(strcmp(st, BRANCH_STATE_DISCARDED) == 0 ||
			 strcmp(st, BRANCH_STATE_APPLIED) == 0))
			already_discarded_applied = true;
	}

	if (already_discarded_applied)
	{
		SPI_finish();
		overlay_guard_ereport_fail("discard_branch",
								   NULL,
								   branch_name,
								   psprintf("branch \"%s\" is not active (already discarded or applied); "
											"discard cannot be called twice on the same branch",
											branch_name));
	}

	/* FR2/FR3: take branch-level EXCLUSIVE advisory lock BEFORE the first
	 * catalog UPDATE, so discard serialises vs concurrent apply/discard
	 * on same bid AND obeys Advisory → catalog RowExclusiveLock order
	 * (prevents deadlock).  NON-BLOCKING acquire (doc/internals.md §3.3):
	 * a delta writer holding ShareLock, or another apply/discard, makes us
	 * fail fast rather than wait (no ShareLock→ExclusiveLock upgrade). */
	if (bid <= 0)
		elog(ERROR, "overlay_branch: discard found invalid branch_id=%d", bid);
	if (!ob_try_branch_advisory_lock(bid, ExclusiveLock))
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("overlay_branch: discard_branch(\"%s\") cannot acquire the branch lock",
						branch_name),
				 errdetail("Another apply/discard is in progress, or in-flight "
						   "delta writers have not yet committed."),
				 errhint("Retry once the conflicting transaction ends.")));
	}

	/* 2) UPDATE state -> discarded */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "UPDATE " OBTABLE_BRANCH " SET state = '%s' "
					 "WHERE branch_name = %s AND state = '%s'",
					 BRANCH_STATE_DISCARDED,
					 esc_name,
					 BRANCH_STATE_ACTIVE);
	ret = SPI_execute(sql.data, false, 1);
	if (ret != SPI_OK_UPDATE)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: discard update pg_branch failed for '%s' SPI ret=%d",
						branch_name, ret)));
	}
	/* CAS parity with apply_branch Step0: the active→discarded UPDATE must
	 * touch exactly one row.  A zero-row result means the branch was already
	 * moved out of ACTIVE externally; refuse instead of reporting success. */
	if (SPI_processed != 1)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("overlay_branch: discard_branch(\"%s\") CAS state transition refused"
						" (SPI_processed=%lu != 1); branch no longer ACTIVE",
						branch_name, (unsigned long) SPI_processed),
				 errhint("Branch state was changed externally between SELECT and UPDATE.")));
	}

	/* 3) CASCADE: delete all delta rows for this branch_id */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM " OBTABLE_DELTA " WHERE branch_id = %d",
					 (int) bid);
	ret = SPI_execute(sql.data, false, 0);
	if (ret != SPI_OK_DELETE)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: discard delete delta rows failed for '%s' SPI ret=%d",
						branch_name, ret)));
	}

	/* §Gx / A7: registry cascade DELETE.  Shares the still-open SPI
	 * connection (discard started one at entry L2198) so we avoid a
	 * second SPI_connect().  ON DELETE CASCADE on pg_branch_registry
	 * FK would clean this automatically, but we want to log the row
	 * count via DEBUG1 AND guarantee a deterministic SPI ret code in
	 * case the catalog schema is ever broken. */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM %s WHERE branch_id = %d",
					 OBTABLE_REGISTRY, (int) bid);
	ret = SPI_execute(sql.data, false, 0);
	if (ret != SPI_OK_DELETE)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: discard delete registry rows failed for '%s' SPI ret=%d",
						branch_name, ret)));
	}
	elog(DEBUG1, "discard_internal: removed %lu registry rows for bid=%d",
		 (unsigned long) SPI_processed, (int) bid);

	SPI_finish();

	/* FR1: ALWAYS broadcast DISCARDED state after catalog UPDATE +
	 * delta CASCADE succeed — other sessions may be LISTENing on this
	 * branch via NOTIFY even if *this* session isn't currently on it. */
	ob_broadcast_state_change(bid, BRANCH_STATE_DISCARDED);
	/* FR5: branch DISCARDED — evict frozen snapshot cache for bid
	 * (if any) and free TopMC CopySnapshot.  After discard, bid no
	 * longer ACTIVE so no future snapshot-mode reference possible. */
	ob_snapshot_cache_remove(bid);
	/* I8.2: symmetric per-bid mode cache cleanup.  Discard moves bid to
	 * state='discarded'.  Any future recreate would INSERT a new catalog
	 * row with mode='live' default; dropping the session-local cached
	 * mode avoids overlaying a stale prior-mode on that fresh row. */
	ob_mode_cache_remove(bid);


	/* 4) If user is on this branch, revert to Main */
	if (CurrentBranchContext &&
		CurrentBranchContext->is_active &&
		strncmp(CurrentBranchContext->branch_name, branch_name,
				NAMEDATALEN) == 0)
	{
		leaving_current = true;
		ob_exit_branch_cleanup(CurrentBranchContext);
		{
			bool saved_flag = ob_in_guc_setconfig;
			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", "",
							PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}
		/* R07 S09 PlanCache invalidation (discard exit): prevents stale
		 * BranchScan plans from being reused in non-branch mode (same
		 * rationale as ENTER/EXIT paths above). */
		CacheInvalidateRelcacheAll();
		CommandEndInvalidationMessages();
	}

	if (leaving_current)
		ereport(NOTICE,
				(errmsg("discarding current branch \"%s\", reverting to Main",
						branch_name)));

	elog(DEBUG1, "overlay_branch_discard_internal: name='%s'", branch_name);
}


/* ================================================================
 * ============ Apply: PK WHERE builder + MAIN fetch + 3 passes ===
 * ================================================================ */

/*
 * ob_build_pk_where_clause - produce "pkcol1 = v1::type1 AND ..."
 * from a serialized JSON-array PK (DeltaTuple->key).
 *
 * Single SPI round-trip (3A.1): the JSON key is passed once; a bare
 * targetlist returns jsonb_array_length() plus each array element as text.
 * The final conjunctive WHERE clause is folded together in C from the
 * per-relation ObApplyRelInfo cache (raw column names and format_type
 * strings), so the per-row path performs no catalog access and the planned
 * statement stays a trivial no-FROM select.  This matters because apply
 * plans this statement once per delta row and planning time, not SPI call
 * count, dominates the per-row cost.
 */
static char *
ob_build_pk_where_clause(ObApplyRelInfo *ri, const char *pk_json_array)
{
	StringInfoData sql;
	StringInfoData where;
	int			ret;
	int			json_len = 0;
	char	   *q_pk;
	char	  **vals;
	char	   *result;
	int			i;
	MemoryContext entry_cxt = CurrentMemoryContext;

	Assert(ri != NULL && pk_json_array != NULL);

	if (ri->n_pk <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("apply requires a primary key on %s",
						RelationGetRelationName(ri->rel))));

	q_pk = quote_literal_cstr(pk_json_array);

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT jsonb_array_length(%s::jsonb)", q_pk);
	for (i = 0; i < ri->n_pk; i++)
		appendStringInfo(&sql, ", (%s::jsonb)->>%d", q_pk, i);

	ret = ob_spi_one_shot(sql.data, true, 1);
	pfree(sql.data);
	if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
		!SPI_tuptable || !SPI_tuptable->vals || !SPI_tuptable->vals[0])
	{
		pfree(q_pk);
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("apply pk where build SPI call failed ret=%d", ret)));
	}

	{
		bool		isnull;
		Datum		v;

		v = SPI_getbinval(SPI_tuptable->vals[0],
						  SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull)
			json_len = DatumGetInt32(v);
	}
	if (json_len != ri->n_pk)
	{
		pfree(q_pk);
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply pk JSON length %d differs from PK column count %d on %s",
						json_len, ri->n_pk, RelationGetRelationName(ri->rel))));
	}

	/* Copy the element texts out before SPI_finish() tears the result down.
	 * SPI_getvalue() allocates in the *current* context — which here is the
	 * SPI proc context — so switch to the caller's context first.  Going
	 * through TopMemoryContext would leak one string per PK column per call
	 * (these are function-local and freed below). */
	{
		MemoryContext oldmc;

		oldmc = MemoryContextSwitchTo(entry_cxt);
		vals = (char **) palloc0(sizeof(char *) * ri->n_pk);
		for (i = 0; i < ri->n_pk; i++)
			vals[i] = SPI_getvalue(SPI_tuptable->vals[0],
								   SPI_tuptable->tupdesc, i + 2);
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	pfree(q_pk);

	initStringInfo(&where);
	for (i = 0; i < ri->n_pk; i++)
	{
		if (i > 0)
			appendStringInfoString(&where, " AND ");
		appendStringInfo(&where, "%s = ",
						 quote_identifier(ri->pk_colname[i]));
		if (vals[i] == NULL)
			appendStringInfoString(&where, "NULL");
		else
		{
			char	   *lit = quote_literal_cstr(vals[i]);

			appendStringInfo(&where, "%s::%s", lit, ri->pk_coltype[i]);
			pfree(lit);
		}
		if (vals[i])
			pfree(vals[i]);
	}
	pfree(vals);

	{
		MemoryContext oldmc;

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		result = pstrdup(where.data);
		MemoryContextSwitchTo(oldmc);
	}
	pfree(where.data);
	return result;
}

/*
 * ob_build_identity_where_clause - keyless/ctid-aware apply row locator.
 *
 * Legacy PK tables store a JSON-array key and locate the MAIN row via
 * ob_build_pk_where_clause().  Experimental keyless tables
 * (overlay_branch.keyless_tables) store a tagged physical key instead:
 *   "A:<blk>:<off>"  anchored to a MAIN row  -> ctid = '(blk,off)'::tid
 *   "S:<seq>"        branch-only INSERT       -> no MAIN row; return NULL
 * Returns a palloc'd WHERE clause, or NULL when there is nothing to locate
 * on MAIN (callers treat NULL as "row absent").
 */
static char *
ob_build_identity_where_clause(ObApplyRelInfo *ri, const char *key)
{
	Assert(key != NULL);

	if (key[0] == 'A' && key[1] == ':')
	{
		unsigned int	blk = 0;
		unsigned int	off = 0;

		if (sscanf(key + 2, "%u:%u", &blk, &off) != 2)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("apply on %s: malformed keyless anchor key \"%s\"",
							RelationGetRelationName(ri->rel), key)));
		return psprintf("ctid = '(%u,%u)'::tid", blk, off);
	}

	if (key[0] == 'S' && key[1] == ':')
		return NULL;			/* synthetic branch-only row: no MAIN anchor */

	return ob_build_pk_where_clause(ri, key);
}

/*
 * ob_apply_rel_info_build - compute the per-relation apply metadata once per
 * relation, so the per-delta-row path stops re-deriving the qualified table
 * name, the column lists and the PK column name/type literals (3A.4).
 */
static ObApplyRelInfo *
ob_apply_rel_info_build(Relation rel)
{
	ObApplyRelInfo *ri;
	TupleDesc	reldesc = RelationGetDescr(rel);
	List	   *indexoids;
	ListCell   *lc;
	Oid			pk_index_oid = InvalidOid;
	Relation	pk_rel = NULL;
	StringInfoData colnames;
	StringInfoData colnames_parens;
	StringInfoData upd_colnames;
	StringInfoData upd_colnames_parens;
	bool		first = true;
	bool		upd_first = true;
	int			i;
	MemoryContext oldmc;

	ri = (ObApplyRelInfo *) palloc0(sizeof(ObApplyRelInfo));
	ri->rel = rel;
	ri->relid = RelationGetRelid(rel);
	ri->n_pk = 0;

	oldmc = MemoryContextSwitchTo(TopMemoryContext);

	ri->q_relname = quote_qualified_identifier(
		get_namespace_name(RelationGetNamespace(rel)),
		RelationGetRelationName(rel));

	initStringInfo(&colnames);
	initStringInfo(&colnames_parens);
	initStringInfo(&upd_colnames);
	initStringInfo(&upd_colnames_parens);
	for (i = 0; i < reldesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(reldesc, i);

		if (att->attisdropped)
			continue;
		if (!first)
		{
			appendStringInfoChar(&colnames, ',');
			appendStringInfoChar(&colnames_parens, ',');
		}
		appendStringInfo(&colnames, "%s",
						 quote_identifier(NameStr(att->attname)));
		appendStringInfo(&colnames_parens, "j.%s",
						 quote_identifier(NameStr(att->attname)));
		first = false;

		/* GENERATED ALWAYS AS IDENTITY columns are excluded from the UPDATE
		 * SET list (PostgreSQL rejects any assignment to them but DEFAULT). */
		if (att->attidentity == ATTRIBUTE_IDENTITY_ALWAYS)
			continue;
		if (!upd_first)
		{
			appendStringInfoChar(&upd_colnames, ',');
			appendStringInfoChar(&upd_colnames_parens, ',');
		}
		appendStringInfo(&upd_colnames, "%s",
						 quote_identifier(NameStr(att->attname)));
		appendStringInfo(&upd_colnames_parens, "j.%s",
						 quote_identifier(NameStr(att->attname)));
		upd_first = false;
	}
	ri->colnames = colnames.data;
	ri->colnames_parens = colnames_parens.data;
	/* Alias the full lists unless an ALWAYS identity column forced the
	 * updatable set to differ.  A table whose every column is ALWAYS identity
	 * has no updatable column at all (an UPDATE on it can never yield a net
	 * change, so no U delta exists); fall back to the full list there. */
	if (upd_colnames.len > 0)
	{
		ri->upd_colnames = upd_colnames.data;
		ri->upd_colnames_parens = upd_colnames_parens.data;
	}
	else
	{
		pfree(upd_colnames.data);
		pfree(upd_colnames_parens.data);
		ri->upd_colnames = ri->colnames;
		ri->upd_colnames_parens = ri->colnames_parens;
	}

	getTypeOutputInfo(BYTEAOID, &ri->bytea_out_func, &ri->bytea_out_varlena);

	MemoryContextSwitchTo(oldmc);

	/* Locate the primary key, if any (keyless tables have none). */
	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		Relation	idxrel = index_open(idxoid, AccessShareLock);

		if (idxrel->rd_index && idxrel->rd_index->indisprimary)
		{
			pk_index_oid = idxoid;
			pk_rel = idxrel;
			break;
		}
		index_close(idxrel, AccessShareLock);
	}

	if (OidIsValid(pk_index_oid) && pk_rel != NULL)
	{
		/* KEY columns only: the apply MAIN<->delta join must use the same
		 * identity the delta key was serialized with (see ob_pk_rel_meta),
		 * which excludes the PK index's INCLUDE (non-key) columns. */
		int			n_pk = pk_rel->rd_index->indnkeyatts;

		if (n_pk > 0)
		{
			ri->n_pk = n_pk;
			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			ri->pk_colname = (char **) palloc0(sizeof(char *) * n_pk);
			ri->pk_coltype = (char **) palloc0(sizeof(char *) * n_pk);
			for (i = 0; i < n_pk; i++)
			{
				AttrNumber	attno = pk_rel->rd_index->indkey.values[i];
				Form_pg_attribute att;

				if (attno == 0)
				{
					ri->pk_colname[i] = pstrdup("");
					ri->pk_coltype[i] = pstrdup("");
					continue;
				}
				att = TupleDescAttr(reldesc, attno - 1);
				ri->pk_colname[i] = pstrdup(NameStr(att->attname));
				ri->pk_coltype[i] =
					pstrdup(format_type_with_typemod(att->atttypid,
													 att->atttypmod));
			}
			MemoryContextSwitchTo(oldmc);
		}
		index_close(pk_rel, AccessShareLock);
	}

	return ri;
}

/* ob_apply_rel_info_free - release everything ob_apply_rel_info_build() made. */
static void
ob_apply_rel_info_free(ObApplyRelInfo *ri)
{
	int			i;

	if (ri == NULL)
		return;
	if (ri->q_relname)
		pfree(ri->q_relname);
	if (ri->colnames)
		pfree(ri->colnames);
	if (ri->colnames_parens)
		pfree(ri->colnames_parens);
	if (ri->upd_colnames && ri->upd_colnames != ri->colnames)
		pfree(ri->upd_colnames);
	if (ri->upd_colnames_parens && ri->upd_colnames_parens != ri->colnames_parens)
		pfree(ri->upd_colnames_parens);
	for (i = 0; i < ri->n_pk; i++)
	{
		if (ri->pk_colname && ri->pk_colname[i])
			pfree(ri->pk_colname[i]);
		if (ri->pk_coltype && ri->pk_coltype[i])
			pfree(ri->pk_coltype[i]);
	}
	if (ri->pk_colname)
		pfree(ri->pk_colname);
	if (ri->pk_coltype)
		pfree(ri->pk_coltype);
	pfree(ri);
}

/*
 * ob_fetch_main_version - locate and FOR UPDATE-lock the MAIN row matching
 * where_clause, returning its optimistic CAS token (3A.2).
 *
 * Returns true when a MAIN row exists; *version then receives a palloc'd
 * "<blk>:<off>-x<xmin>" token (TopMemoryContext) with exactly the shape
 * overlay_tuple_version() produces, so callers may strcmp() the two directly.
 * Returns false when nothing matches (caller treats the row as absent).
 *
 * Taking the lock here replaces the previous fetch-row plus separate heap
 * version lookup pair; the lock is what the follow-up base_image compare and
 * DML rely on.  The statement must not be flagged read-only: the locking
 * clause makes it non read-only and SPI would reject that combination.
 */
static bool
ob_fetch_main_version(ObApplyRelInfo *ri, const char *where_clause,
					  char **version)
{
	StringInfoData sql;
	int			ret;
	bool		found = false;

	Assert(ri != NULL && where_clause != NULL && version != NULL);
	*version = NULL;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT ctid::text, xmin::text FROM %s WHERE %s "
					 "FOR UPDATE LIMIT 1",
					 ri->q_relname, where_clause);
	ret = ob_spi_one_shot(sql.data, false, 1);
	pfree(sql.data);

	if (ret == SPI_OK_SELECT && SPI_processed == 1 &&
		SPI_tuptable && SPI_tuptable->vals && SPI_tuptable->vals[0])
	{
		char	   *ctid_txt = SPI_getvalue(SPI_tuptable->vals[0],
										SPI_tuptable->tupdesc, 1);
		char	   *xmin_txt = SPI_getvalue(SPI_tuptable->vals[0],
										SPI_tuptable->tupdesc, 2);
		char		token[OB_BASE_VER_MAX];
		uint32		blk = 0;
		uint32		off = 0;

		found = true;
		if (overlay_base_ver_parse_ctid(ctid_txt, &blk, &off))
		{
			MemoryContext oldmc;

			overlay_base_ver_format(token, sizeof(token), blk, off, xmin_txt);
			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			*version = pstrdup(token);
			MemoryContextSwitchTo(oldmc);
		}
	}
	SPI_finish();
	return found;
}

/* ================================================================
 * apply_check_base_image_match — REVIEW-260926/R15 & R18 HOT-Update
 * Token Collision Guard
 *
 * overlay_tuple_version() uses the (ctid, xmin) pair as the optimistic
 * CAS token.  On PostgreSQL HOT updates the row stays at the same
 * ctid/xmin (heap-only tuple chain reuse) so the token does NOT change
 * even though MAIN data columns have been rewritten by a concurrent
 * session.  Without a secondary check an apply-UPDATE/DELETE would
 * silently overwrite the concurrent MAIN modification → data loss.
 *
 * We encode the MAIN row at branch-write time as the "_base" top-level
 * key of the stored tuple_data JSON blob.  Here we compare that saved
 * base image (stripped of system columns that row_to_json does not
 * include) against the CURRENTLY LOCKED MAIN row at apply time, using
 * jsonb_object_agg(key, value ORDER BY key) to canonicalise both sides
 * to a field-order-independent representation.  The MAIN row was already
 * locked FOR UPDATE by ob_fetch_main_version(), so the compared MAIN row
 * is the one we will subsequently DELETE/UPDATE.
 *
 * The "_base" presence probe happens in C: the stored JSON payload is
 * scanned for the "_base" key before any SPI round-trip, so the common
 * case (no base image recorded) costs nothing.  Only when a "_base" key is
 * actually present do we run the comparison statement, which returns three
 * booleans (has_base, main_absent, differs).  When the canonicalised base
 * and MAIN representations differ we raise a conflict and abort the whole
 * branch apply.
 * ================================================================ */

/*
 * ob_delta_has_base_key - C-level probe for a top-level "_base" key in the
 * stored delta tuple_data JSON payload.
 *
 * overlay_branch only records a base image for UPDATE/DELETE deltas written
 * by the R15/R18 guard writer; every other delta (and every delta produced
 * by the current write-redirect path) carries none.  Probing the raw bytes
 * here avoids planning and executing the expensive canonicalising compare
 * statement once per delta row, which otherwise dominates apply time.
 */
static bool
ob_delta_has_base_key(const bytea *tuple_data)
{
	const char *p;
	int			len;
	int			i;

	if (tuple_data == NULL)
		return false;

	p = (const char *) VARDATA_ANY(tuple_data);
	len = VARSIZE_ANY_EXHDR(tuple_data);
	if (len < 7)
		return false;

	for (i = 0; i + 7 <= len; i++)
	{
		if (memcmp(p + i, "\"_base\"", 7) == 0)
			return true;
	}
	return false;
}

static void
apply_check_base_image_match(ObApplyRelInfo *ri, DeltaTuple *dt,
							 const char *where_clause, const char *branch_name)
{
	char	   *bytea_lit;
	char	   *ql;
	StringInfoData sql;
	int			ret;
	bool		has_base = false;
	bool		main_found = false;
	bool		differs = false;

	if (dt->tuple_data == NULL)
		return;

	/* Fast path: nothing to compare when the payload has no base image. */
	if (!ob_delta_has_base_key(dt->tuple_data))
		return;

	bytea_lit = OidOutputFunctionCall(ri->bytea_out_func,
									  PointerGetDatum(dt->tuple_data));
	ql = quote_literal_cstr(bytea_lit ? bytea_lit : "");

	/* One statement yields: has_base, main_absent, differs. */
	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT d.j ? '_base', "
					 "       m.main_norm IS NULL, "
					 "       CASE WHEN d.j ? '_base' "
					 "            THEN COALESCE(b.base_norm, '{}'::jsonb) "
					 "                 <> COALESCE(m.main_norm, '{}'::jsonb) "
					 "            ELSE false END "
					 "  FROM (SELECT convert_from(decode(substring(%1$s from 3), 'hex'), 'UTF8')::jsonb AS j) d "
					 "  LEFT JOIN LATERAL (SELECT jsonb_object_agg(key, value ORDER BY key) AS base_norm "
					 "                       FROM jsonb_each(COALESCE(d.j -> '_base', '{}'::jsonb))) b ON true "
					 "  LEFT JOIN LATERAL (SELECT jsonb_object_agg(key, value ORDER BY key) AS main_norm "
					 "                       FROM jsonb_each(COALESCE((SELECT to_jsonb(r) "
					 "                                                  - 'ctid' - 'xmin' - 'cmin' "
					 "                                                  - 'xmax' - 'cmax' - 'tableoid' "
					 "                                                 FROM (SELECT * FROM %2$s WHERE %3$s) r), '{}'::jsonb))) m ON true",
					 ql, ri->q_relname, where_clause);
	ret = ob_spi_one_shot(sql.data, true, 1);
	if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
		!SPI_tuptable || !SPI_tuptable->vals || !SPI_tuptable->vals[0])
	{
		pfree(sql.data);
		if (bytea_lit) pfree(bytea_lit);
		pfree(ql);
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("apply_check_base_image_match: SPI ret=%d", ret)));
	}
	{
		bool		null1;
		bool		null2;
		bool		null3;
		Datum		d1;
		Datum		d2;
		Datum		d3;

		d1 = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &null1);
		d2 = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &null2);
		d3 = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3, &null3);
		has_base = null1 ? false : DatumGetBool(d1);
		main_found = null2 ? false : !DatumGetBool(d2);
		differs = null3 ? false : DatumGetBool(d3);
	}
	pfree(sql.data);
	if (bytea_lit) pfree(bytea_lit);
	pfree(ql);
	SPI_finish();

	if (!has_base)
		return;

	if (!main_found)
	{
		char	   *reason = psprintf(
			"conflict pk=%s: MAIN row vanished during base_image compare (concurrent DELETE outside branch)",
			dt->key ? dt->key : "(null)");

		overlay_guard_ereport_fail("apply_branch (conflict R15/R18)",
								   RelationGetRelationName(ri->rel),
								   branch_name,
								   reason);
	}

	if (differs)
	{
		char	   *reason = psprintf(
			"conflict pk=%s: MAIN row has diverged from branch base_image "
			"(ctid/xmin token unchanged, so R15/R18 HOT-update collision "
			"was caught by the secondary per-column compare).  Resolve by "
			"discarding this branch, re-branching from current MAIN, and "
			"re-applying the branch edits.",
			dt->key ? dt->key : "(null)");

		overlay_guard_ereport_fail("apply_branch (conflict R15/R18)",
								   RelationGetRelationName(ri->rel),
								   branch_name,
								   reason);
	}
}

/* ================================================================
 * --- 3 apply passes: DELETE → UPDATE → INSERT -----------------
 * ================================================================ */

static void
apply_relation_delete_pass(ObApplyRelInfo *ri, DeltaTuple *dt,
						   const char *branch_name)
{
	char	   *where_clause;
	char	   *main_ver = NULL;

	if (dt->key == NULL || *dt->key == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply DELETE on %s: delta has empty key",
						RelationGetRelationName(ri->rel))));

	where_clause = ob_build_identity_where_clause(ri, dt->key);
	/* Pure-delta origin: old_version == NULL means this row never existed
	 * on MAIN (it was INSERTed then DELETEd entirely inside the branch).
	 * There is nothing to DELETE on MAIN; skipping prevents false-positive
	 * "row no longer present" conflict errors. */
	if (dt->old_version == NULL)
	{
		if (where_clause) pfree(where_clause);
		return;
	}

	if (where_clause == NULL || *where_clause == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply DELETE on %s: cannot build WHERE for pk=%s",
						RelationGetRelationName(ri->rel), dt->key)));

	/* Locate + FOR UPDATE-lock the MAIN row; the same SPI call returns the
	 * optimistic CAS token so no separate heap lookup is needed. */
	if (!ob_fetch_main_version(ri, where_clause, &main_ver))
	{
		char	   *reason = psprintf("DELETE delta pk=%s: MAIN row no longer present at apply time (concurrent DELETE or UPDATE rewrite).  expected old_version=%s.  Resolve manually and re-branch.",
									 dt->key, dt->old_version ? dt->old_version : "(null)");

		pfree(where_clause);
		overlay_guard_ereport_fail("apply_branch (conflict)",
								   RelationGetRelationName(ri->rel),
								   branch_name,
								   reason);
	}

	if (main_ver == NULL || strcmp(dt->old_version, main_ver) != 0)
	{
		char	   *reason = psprintf("conflict pk=%s: old_version expected=%s but MAIN latest=%s",
									 dt->key,
									 dt->old_version,
									 main_ver ? main_ver : "(null)");

		if (main_ver) pfree(main_ver);
		pfree(where_clause);
		overlay_guard_ereport_fail("apply_branch (conflict)",
								   RelationGetRelationName(ri->rel),
								   branch_name,
								   reason);
	}
	if (main_ver) pfree(main_ver);

	apply_check_base_image_match(ri, dt, where_clause, branch_name);

	{
		StringInfoData del_sql;
		int			ret;

		initStringInfo(&del_sql);
		appendStringInfo(&del_sql, "DELETE FROM %s WHERE %s",
						 ri->q_relname, where_clause);
		pfree(where_clause);

		ret = ob_spi_one_shot(del_sql.data, false, 1);
		SPI_finish();
		pfree(del_sql.data);
		if (ret != SPI_OK_DELETE)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("apply DELETE failed ret=%d", ret)));
	}
}

static void
apply_relation_update_pass(ObApplyRelInfo *ri, DeltaTuple *dt,
						   const char *branch_name)
{
	char	   *where_clause;
	char	   *main_ver = NULL;
	bool		main_found;

	if (dt->key == NULL || *dt->key == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply UPDATE on %s: delta has empty key",
						RelationGetRelationName(ri->rel))));
	if (dt->tuple_data == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply UPDATE on %s: delta has NULL tuple_data for pk=%s",
						RelationGetRelationName(ri->rel), dt->key)));

	where_clause = ob_build_identity_where_clause(ri, dt->key);
	/* A NULL clause means a keyless "S:" synthetic key: there is no MAIN
	 * anchor, so behave as a missing MAIN row (the pure-delta branch below
	 * then falls back to insert_pass). */
	main_found = (where_clause != NULL)
		? ob_fetch_main_version(ri, where_clause, &main_ver)
		: false;
	if (!main_found)
	{
		char	   *reason;

		/* Pure-delta origin UPDATE: row never existed in MAIN,
		 * old_version is NULL because there was never any MAIN
		 * baseline.  Fall back to INSERT pass to materialize the
		 * final post-image.  Engineering spec: "Apply logic:
		 * handle pure UPDATE when old_version empty by insert_pass". */
		if (dt->old_version == NULL)
		{
			if (where_clause) pfree(where_clause);
			apply_relation_insert_pass(ri, dt, branch_name);
			return;
		}
		reason = psprintf("UPDATE pk=%s MAIN row missing at apply time "
						  "(concurrent UPDATE/DELETE or table rewrite)",
						  dt->key);
		if (where_clause) pfree(where_clause);
		overlay_guard_ereport_fail("apply_branch (conflict)",
								   RelationGetRelationName(ri->rel),
								   branch_name,
								   reason);
	}

	if (dt->old_version == NULL || main_ver == NULL ||
		strcmp(dt->old_version, main_ver) != 0)
	{
		/* A NULL old_version means the delta carries no MAIN baseline, so
		 * the conflict is simply "MAIN row unexpectedly present".  The live
		 * (ctid,xmin) token is an internal, run-dependent value and is
		 * deliberately not surfaced in that case. */
		char	   *reason = psprintf("conflict pk=%s: old_version expected=%s MAIN latest=%s",
									 dt->key,
									 dt->old_version ? dt->old_version : "(null)",
									 (dt->old_version && main_ver) ? main_ver : "(null)");

		if (main_ver) pfree(main_ver);
		pfree(where_clause);
		overlay_guard_ereport_fail("apply_branch (conflict)",
								   RelationGetRelationName(ri->rel),
								   branch_name,
								   reason);
	}
	if (main_ver) pfree(main_ver);

	apply_check_base_image_match(ri, dt, where_clause, branch_name);

	{
		char	   *bytea_lit;
		StringInfoData upd_sql;
		int			ret;

		bytea_lit = OidOutputFunctionCall(ri->bytea_out_func,
										  PointerGetDatum(dt->tuple_data));
		{
			char	   *ql = quote_literal_cstr(bytea_lit ? bytea_lit : "");

			initStringInfo(&upd_sql);
			appendStringInfo(&upd_sql,
							 "UPDATE %s SET (%s) = ("
							 "SELECT %s FROM jsonb_populate_record(NULL::%s, "
							 "convert_from(decode(substring(%s from 3), 'hex'), 'UTF8')::jsonb) "
							 "AS j) "
							 "WHERE %s",
							 ri->q_relname, ri->upd_colnames,
							 ri->upd_colnames_parens, ri->q_relname,
							 ql, where_clause);
			pfree(ql);
		}
		pfree(where_clause);
		if (bytea_lit) pfree(bytea_lit);

		ret = ob_spi_one_shot(upd_sql.data, false, 1);
		pfree(upd_sql.data);
		if (ret != SPI_OK_UPDATE && ret != SPI_OK_UPDATE_RETURNING)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("apply UPDATE failed ret=%d", ret)));
		SPI_finish();
	}
}

static void
apply_relation_insert_pass(ObApplyRelInfo *ri, DeltaTuple *dt,
						   const char *branch_name)
{
	int			ret;
	char	   *bytea_lit;
	StringInfoData ins_sql;

	if (dt->tuple_data == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply INSERT on %s: delta has NULL tuple_data pk=%s",
						RelationGetRelationName(ri->rel),
						dt->key ? dt->key : "(null)")));

	if (dt->key != NULL && *dt->key != '\0')
	{
		char	   *where_clause;
		bool		exists = false;

		where_clause = ob_build_identity_where_clause(ri, dt->key);
		/* NULL clause = keyless "S:" synthetic key (branch-only row); there
		 * is no MAIN anchor to locate, so treat as row absent. */
		if (where_clause != NULL)
		{
			StringInfoData probe_sql;
			uint64		nrows;

			initStringInfo(&probe_sql);
			appendStringInfo(&probe_sql, "SELECT 1 FROM %s WHERE %s LIMIT 1",
							 ri->q_relname, where_clause);
			ret = ob_spi_one_shot(probe_sql.data, true, 1);
			/* SPI_finish() restores SPI_processed from the outer SPI level,
			 * so the row count must be captured before finishing. */
			nrows = SPI_processed;
			SPI_finish();
			pfree(probe_sql.data);
			exists = (ret == SPI_OK_SELECT && nrows == 1);
			pfree(where_clause);
		}
		if (exists)
		{
			char	   *reason = psprintf("INSERT pk=%s MAIN row already present (MAIN pk already contains this PK)",
									   dt->key);

			overlay_guard_ereport_fail("apply_branch (conflict)",
									   RelationGetRelationName(ri->rel),
									   branch_name,
									   reason);
		}
	}

	bytea_lit = OidOutputFunctionCall(ri->bytea_out_func,
								  PointerGetDatum(dt->tuple_data));
	{
		char	   *ql = quote_literal_cstr(bytea_lit ? bytea_lit : "");

		initStringInfo(&ins_sql);
		appendStringInfo(&ins_sql,
						 "INSERT INTO %s (%s) OVERRIDING SYSTEM VALUE "
						 "SELECT %s FROM jsonb_populate_record(NULL::%s, "
						 "convert_from(decode(substring(%s from 3), 'hex'), 'UTF8')::jsonb) "
						 "AS j",
						 ri->q_relname, ri->colnames, ri->colnames_parens,
						 ri->q_relname,
						 ql);
		pfree(ql);
	}
	if (bytea_lit)
		pfree(bytea_lit);

	ret = ob_spi_one_shot(ins_sql.data, false, 1);
	pfree(ins_sql.data);
	if (ret != SPI_OK_INSERT && ret != SPI_OK_INSERT_RETURNING)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("apply INSERT failed ret=%d", ret)));
	SPI_finish();
}

/* ================================================================
 * Phase 3B: set-based apply replay (one statement per relation+operation)
 *
 * The per-row replay above issues a few SPI statements per delta row, and
 * planning those statements — not the SPI call count — dominates its cost
 * (doc/internals.md §5.5).  Phase 3B folds each operation into a single
 * statement per relation:
 *
 *   1. a validation SELECT that returns the first conflicting delta row, and
 *   2. the batch DML itself.
 *
 * Both use the same predicate ("same-source predicate"), so a validation
 * pass means the DML touches exactly the rows the C-side delta list counts.
 * The batch DML carries the optimistic token in its own WHERE, so its row
 * lock and the token comparison happen in one statement — the equivalent of
 * the per-row "SELECT ... FOR UPDATE, compare, DML" sequence.
 *
 * A row-count mismatch between the batch DML and the C-side expectation can
 * only come from a concurrent MAIN change slipping in between the two
 * statements; it aborts the apply and rolls the whole transaction back, so
 * nothing is partially applied.
 *
 * Keyless relations join on their ctid anchor ("A:<blk>:<off>") instead of a
 * PK; relations whose deltas carry a base image ("_base", the R15/R18
 * HOT-update guard) stay on the per-row replay.
 * ================================================================ */

/* TOKEN(alias) - canonical MAIN-row CAS token, byte-identical to the
 * "<blk>:<off>-x<xmin>" string ob_fetch_main_version() produces. */
static char *
ob_batch_token_expr(const char *main_alias)
{
	return psprintf("(replace(translate(%s.ctid::text, '()', ''), ',', ':') "
					"|| '-x' || %s.xmin::text)",
					main_alias, main_alias);
}

/* PKJOIN(delta_alias, main_alias) - one equality per PK column against the
 * matching element of the delta's JSON-array key. */
static char *
ob_batch_pkjoin_expr(ObApplyRelInfo *ri, const char *delta_alias,
					 const char *main_alias)
{
	StringInfoData buf;
	int			i;

	Assert(ri != NULL && ri->n_pk > 0);

	initStringInfo(&buf);
	for (i = 0; i < ri->n_pk; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, " AND ");
		appendStringInfo(&buf, "%s.%s = ((%s.key)::jsonb->>%d)::%s",
						 main_alias, quote_identifier(ri->pk_colname[i]),
						 delta_alias, i, ri->pk_coltype[i]);
	}
	return buf.data;
}

/*
 * IDENTITYJOIN(delta_alias, main_alias) - the batch join on the row identity.
 *
 * PK relations use the JSON-array key (ob_batch_pkjoin_expr).  Keyless
 * relations have no PK column, so the join decodes the tagged physical key:
 *   "A:<blk>:<off>" -> main.ctid = '(blk,off)'::tid
 *   "S:<seq>"       -> no MAIN anchor; the CASE yields NULL, which never
 *                      matches a ctid, so the row is treated as branch-only
 *                      (pure INSERT).
 */
static char *
ob_batch_identity_join_expr(ObApplyRelInfo *ri, const char *delta_alias,
							const char *main_alias)
{
	if (ri->n_pk > 0)
		return ob_batch_pkjoin_expr(ri, delta_alias, main_alias);

	return psprintf("%s.ctid = CASE WHEN substr(%s.key, 1, 2) = 'A:' "
					"THEN (('(' || replace(substr(%s.key, 3), ':', ',') || ')')::tid) "
					"ELSE NULL END",
					main_alias, delta_alias, delta_alias);
}

/* First conflicting delta row for one operation, as reported by the
 * validation SELECT.  All string fields are palloc'd in TopMemoryContext. */
typedef struct ObBatchConflict
{
	bool		found;
	char	   *key;			/* JSON-array PK text */
	char	   *old_version;	/* delta base version, NULL when unset */
	bool		main_missing;	/* no MAIN row matched the key */
	char	   *main_ver;		/* canonical MAIN token, NULL when absent */
} ObBatchConflict;

/*
 * ob_batch_validate - return the first (lowest-key) delta row of `op` that
 * violates the batch DML predicate, or found=false when the operation can be
 * replayed as one batch.
 */
static ObBatchConflict
ob_batch_validate(ObApplyRelInfo *ri, int32 bid, char op)
{
	ObBatchConflict c;
	StringInfoData sql;
	char	   *pkjoin;
	char	   *token;
	int			ret;

	c.found = false;
	c.key = NULL;
	c.old_version = NULL;
	c.main_missing = false;
	c.main_ver = NULL;

	pkjoin = ob_batch_identity_join_expr(ri, "d", "m");
	token = ob_batch_token_expr("m");

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT d.key, d.old_version, (m.ctid IS NULL), %s "
					 "FROM %s d LEFT JOIN %s m ON %s "
					 "WHERE d.branch_id = %d AND d.relid = %u AND d.op = '%c' AND ",
					 token, OBTABLE_DELTA, ri->q_relname, pkjoin,
					 (int) bid, (unsigned) ri->relid, op);

	if (op == DELTA_OP_DELETE)
	{
		/* A pure-delta DELETE (no MAIN baseline) is a no-op, so only rows
		 * that claim a MAIN baseline can conflict. */
		appendStringInfo(&sql,
						 "d.old_version IS NOT NULL AND "
						 "d.old_version IS DISTINCT FROM %s", token);
	}
	else if (op == DELTA_OP_UPDATE)
	{
		/* Either a MAIN row appeared for a pure-delta UPDATE, or a row that
		 * should be there is gone/rewritten.  One query covers both so the
		 * first offender by key order wins, matching the per-row order. */
		appendStringInfo(&sql,
						 "((d.old_version IS NULL AND m.ctid IS NOT NULL) OR "
						 "(d.old_version IS NOT NULL AND "
						 "d.old_version IS DISTINCT FROM %s))", token);
	}
	else
	{
		appendStringInfoString(&sql, "m.ctid IS NOT NULL");
	}
	appendStringInfoString(&sql, " ORDER BY d.key LIMIT 1");

	pfree(pkjoin);
	pfree(token);

	ret = ob_spi_one_shot(sql.data, true, 1);
	pfree(sql.data);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("apply batch validation on %s failed ret=%d",
						RelationGetRelationName(ri->rel), ret)));
	}

	if (SPI_processed == 1 && SPI_tuptable && SPI_tuptable->vals &&
		SPI_tuptable->vals[0])
	{
		HeapTuple	tup = SPI_tuptable->vals[0];
		TupleDesc	td = SPI_tuptable->tupdesc;
		bool		isnull;
		Datum		dv;
		MemoryContext oldmc;

		/* SPI_finish() tears the tuple down, so copy what we need. */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		c.found = true;
		c.key = SPI_getvalue(tup, td, 1);
		c.old_version = SPI_getvalue(tup, td, 2);
		dv = SPI_getbinval(tup, td, 3, &isnull);
		c.main_missing = isnull ? false : DatumGetBool(dv);
		c.main_ver = SPI_getvalue(tup, td, 4);
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();

	return c;
}

/*
 * ob_batch_report_conflict - raise the conflict error for a validated batch
 * offender.  The wording matches the per-row passes verbatim so existing
 * baselines are unaffected; never returns.
 */
static void
ob_batch_report_conflict(ObApplyRelInfo *ri, char op, ObBatchConflict *c,
						 const char *branch_name)
{
	char	   *reason;

	Assert(c->found);

	if (op == DELTA_OP_DELETE)
	{
		if (c->main_missing)
			reason = psprintf("DELETE delta pk=%s: MAIN row no longer present at apply time (concurrent DELETE or UPDATE rewrite).  expected old_version=%s.  Resolve manually and re-branch.",
							  c->key,
							  c->old_version ? c->old_version : "(null)");
		else
			reason = psprintf("conflict pk=%s: old_version expected=%s but MAIN latest=%s",
							  c->key,
							  c->old_version ? c->old_version : "(null)",
							  c->main_ver ? c->main_ver : "(null)");
	}
	else if (op == DELTA_OP_UPDATE)
	{
		/* A NULL old_version means the delta carries no MAIN baseline, so the
		 * conflict is simply "MAIN row unexpectedly present": the live
		 * (ctid,xmin) token is internal and deliberately not surfaced. */
		if (c->old_version == NULL)
			reason = psprintf("conflict pk=%s: old_version expected=(null) MAIN latest=(null)",
							  c->key);
		else if (c->main_missing)
			reason = psprintf("UPDATE pk=%s MAIN row missing at apply time (concurrent UPDATE/DELETE or table rewrite)",
							  c->key);
		else
			reason = psprintf("conflict pk=%s: old_version expected=%s MAIN latest=%s",
							  c->key, c->old_version,
							  c->main_ver ? c->main_ver : "(null)");
	}
	else
	{
		reason = psprintf("INSERT pk=%s MAIN row already present (MAIN pk already contains this PK)",
						  c->key ? c->key : "(null)");
	}

	/* ob_batch_validate() parked these in TopMemoryContext; the guard below
	 * never returns, so release them (reason already holds its own copies). */
	if (c->key)
		pfree(c->key);
	if (c->old_version)
		pfree(c->old_version);
	if (c->main_ver)
		pfree(c->main_ver);

	overlay_guard_ereport_fail("apply_branch (conflict)",
							   RelationGetRelationName(ri->rel),
							   branch_name,
							   reason);
}

/* ob_batch_check_count - the validation SELECT ran right before the batch
 * DML, so a differing row count means a concurrent MAIN change slipped into
 * the gap.  Abort: the enclosing transaction rolls the whole apply back. */
static void
ob_batch_check_count(ObApplyRelInfo *ri, const char *op_name,
					 uint64 expected, uint64 actual)
{
	if (expected == actual)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("apply batch %s on %s processed " UINT64_FORMAT
					" rows, expected " UINT64_FORMAT
					" (concurrent MAIN modification during batch replay)",
					op_name, RelationGetRelationName(ri->rel),
					actual, expected),
			 errhint("Re-run apply_branch(); the branch state was rolled back to ACTIVE.")));
}

static void
ob_batch_delete_pass(ObApplyRelInfo *ri, int32 bid, uint64 expected)
{
	StringInfoData sql;
	char	   *pkjoin;
	char	   *token;
	int			ret;
	uint64		n;

	pkjoin = ob_batch_identity_join_expr(ri, "d", "m");
	token = ob_batch_token_expr("m");

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM %s m USING %s d "
					 "WHERE d.branch_id = %d AND d.relid = %u "
					 "AND d.op = 'D' AND d.old_version IS NOT NULL "
					 "AND %s AND d.old_version = %s",
					 ri->q_relname, OBTABLE_DELTA, (int) bid,
					 (unsigned) ri->relid, pkjoin, token);
	pfree(pkjoin);
	pfree(token);

	ret = ob_spi_one_shot(sql.data, false, 1);
	n = SPI_processed;
	SPI_finish();
	pfree(sql.data);

	if (ret != SPI_OK_DELETE)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("apply batch DELETE on %s failed ret=%d",
						RelationGetRelationName(ri->rel), ret)));

	ob_batch_check_count(ri, "DELETE", expected, n);
}

static void
ob_batch_update_pass(ObApplyRelInfo *ri, int32 bid, uint64 expected)
{
	StringInfoData sql;
	char	   *pkjoin;
	char	   *token;
	int			ret;
	uint64		n;

	pkjoin = ob_batch_identity_join_expr(ri, "s", "m");
	token = ob_batch_token_expr("m");

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "UPDATE %s m SET (%s) = ("
					 "SELECT %s FROM jsonb_populate_record(NULL::%s, s.tj) AS j) "
					 "FROM (SELECT convert_from(d.tuple_data, 'UTF8')::jsonb AS tj, "
					 "d.key AS key, d.old_version AS old_version "
					 "FROM %s d WHERE d.branch_id = %d AND d.relid = %u "
					 "AND d.op = 'U' AND d.old_version IS NOT NULL) s "
					 "WHERE %s AND s.old_version = %s",
					 ri->q_relname, ri->upd_colnames, ri->upd_colnames_parens,
					 ri->q_relname, OBTABLE_DELTA, (int) bid,
					 (unsigned) ri->relid, pkjoin, token);
	pfree(pkjoin);
	pfree(token);

	ret = ob_spi_one_shot(sql.data, false, 1);
	n = SPI_processed;
	SPI_finish();
	pfree(sql.data);

	if (ret != SPI_OK_UPDATE && ret != SPI_OK_UPDATE_RETURNING)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("apply batch UPDATE on %s failed ret=%d",
						RelationGetRelationName(ri->rel), ret)));

	ob_batch_check_count(ri, "UPDATE", expected, n);
}

static void
ob_batch_insert_pass(ObApplyRelInfo *ri, int32 bid, uint64 expected)
{
	StringInfoData sql;
	char	   *pkjoin;
	int			ret;
	uint64		n;

	pkjoin = ob_batch_identity_join_expr(ri, "d", "m");

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "INSERT INTO %s (%s) OVERRIDING SYSTEM VALUE "
					 "SELECT %s FROM %s d "
					 "CROSS JOIN LATERAL jsonb_populate_record(NULL::%s, "
					 "convert_from(d.tuple_data, 'UTF8')::jsonb) AS j "
					 "WHERE d.branch_id = %d AND d.relid = %u "
					 "AND (d.op = 'I' OR (d.op = 'U' AND d.old_version IS NULL)) "
					 "AND NOT EXISTS (SELECT 1 FROM %s m WHERE %s)",
					 ri->q_relname, ri->colnames, ri->colnames_parens,
					 OBTABLE_DELTA, ri->q_relname, (int) bid,
					 (unsigned) ri->relid, ri->q_relname, pkjoin);
	pfree(pkjoin);

	ret = ob_spi_one_shot(sql.data, false, 1);
	n = SPI_processed;
	SPI_finish();
	pfree(sql.data);

	if (ret != SPI_OK_INSERT && ret != SPI_OK_INSERT_RETURNING)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("apply batch INSERT on %s failed ret=%d",
						RelationGetRelationName(ri->rel), ret)));

	ob_batch_check_count(ri, "INSERT", expected, n);
}

/* ob_apply_relation_per_row - the legacy replay: full D→U→I sweeps of the
 * delta list, one SPI statement group per row. */
static void
ob_apply_relation_per_row(ObApplyRelInfo *ri, List *deltas,
						  const char *branch_name)
{
	ListCell   *dlc;
	bool		needs_lc_pin = ob_relation_needs_lc_monetary_pin(ri->rel);
	int			guc_nl = 0;

	/* Replaying a delta re-parses its stored payload; a money column also
	 * needs the parse pinned so cash_in accepts the C-rendered currency
	 * symbol the payload was written with (ob_type_needs_lc_monetary_pin).
	 * PG_FINALLY restores the GUCs on any error path, so an error swallowed
	 * by an inner PG_CATCH can never leave the session pinned to locale C. */
	if (needs_lc_pin)
		guc_nl = ob_canonical_guc_pin();

	PG_TRY();
	{
		foreach(dlc, deltas)
		{
			DeltaTuple *dt = (DeltaTuple *) lfirst(dlc);

			if (dt->op == DELTA_OP_DELETE)
			{
				elog(DEBUG1, "apply[dbg] DELETE pass key=%s", dt->key ? dt->key : "NULL");
				apply_relation_delete_pass(ri, dt, branch_name);
				elog(DEBUG1, "apply[dbg] DELETE pass DONE");
			}
		}
		foreach(dlc, deltas)
		{
			DeltaTuple *dt = (DeltaTuple *) lfirst(dlc);

			if (dt->op == DELTA_OP_UPDATE)
			{
				elog(DEBUG1, "apply[dbg] UPDATE pass key=%s", dt->key ? dt->key : "NULL");
				apply_relation_update_pass(ri, dt, branch_name);
				elog(DEBUG1, "apply[dbg] UPDATE pass DONE");
			}
		}
		foreach(dlc, deltas)
		{
			DeltaTuple *dt = (DeltaTuple *) lfirst(dlc);

			if (dt->op == DELTA_OP_INSERT)
			{
				elog(DEBUG1, "apply[dbg] INSERT pass key=%s", dt->key ? dt->key : "NULL");
				apply_relation_insert_pass(ri, dt, branch_name);
				elog(DEBUG1, "apply[dbg] INSERT pass DONE");
			}
		}
	}
	PG_FINALLY();
	{
		if (needs_lc_pin)
			ob_canonical_guc_unpin(guc_nl);
	}
	PG_END_TRY();
}

/* ob_apply_batch_can_use - can this relation be replayed with batch SQL? */
static bool
ob_apply_batch_can_use(ObApplyRelInfo *ri, List *deltas)
{
	ListCell   *lc;

	if (!ob_apply_batch)
		return false;

	/* Both PK and keyless relations batch: the join key differs (JSON PK vs
	 * ctid anchor, see ob_batch_identity_join_expr).  The R15/R18 base-image
	 * compare stays row-scoped: one delta carrying a "_base" key sends the
	 * whole relation down the per-row replay. */
	foreach(lc, deltas)
	{
		DeltaTuple *dt = (DeltaTuple *) lfirst(lc);

		if (ob_delta_has_base_key(dt->tuple_data))
			return false;
	}
	return true;
}

/*
 * ob_apply_relation_batch - replay one relation's deltas with batch SQL,
 * keeping the D→U→I order (and thus the conflict error priority).
 */
static void
ob_apply_relation_batch(ObApplyRelInfo *ri, int32 bid, List *deltas,
						const char *branch_name)
{
	uint64		n_del = 0;		/* DELETE rows claiming a MAIN baseline */
	uint64		n_upd = 0;		/* UPDATE rows claiming a MAIN baseline */
	uint64		n_orphan = 0;	/* pure-delta UPDATE rows (no MAIN row) */
	uint64		n_ins = 0;		/* INSERT rows + materialised orphan-U rows */
	ListCell   *lc;
	ObBatchConflict c;
	bool		needs_lc_pin = ob_relation_needs_lc_monetary_pin(ri->rel);
	int			guc_nl = 0;

	/* See ob_apply_relation_per_row: pin the canonical locale around the
	 * batch replay's stored-payload parse when the relation carries money.
	 * PG_FINALLY restores the GUCs on any error path. */
	if (needs_lc_pin)
		guc_nl = ob_canonical_guc_pin();

	PG_TRY();
	{
		foreach(lc, deltas)
		{
			DeltaTuple *dt = (DeltaTuple *) lfirst(lc);

			if (dt->op == DELTA_OP_DELETE)
			{
				if (dt->old_version != NULL)
					n_del++;
			}
			else if (dt->op == DELTA_OP_UPDATE)
			{
				if (dt->old_version != NULL)
					n_upd++;
				else
				{
					n_orphan++;
					n_ins++;
				}
			}
			else if (dt->op == DELTA_OP_INSERT)
				n_ins++;
		}

		elog(DEBUG1, "apply[dbg] batch counts D=" UINT64_FORMAT " U=" UINT64_FORMAT
			 " orphan=" UINT64_FORMAT " I=" UINT64_FORMAT,
			 n_del, n_upd, n_orphan, n_ins);

		/* ---- DELETE pass (pure-delta DELETEs are no-ops and need no SQL) ---- */
		if (n_del > 0)
		{
			c = ob_batch_validate(ri, bid, DELTA_OP_DELETE);
			if (c.found)
				ob_batch_report_conflict(ri, DELTA_OP_DELETE, &c, branch_name);
			ob_batch_delete_pass(ri, bid, n_del);
		}

		/* ---- UPDATE pass ----
		 * Validate whenever the relation carries any UPDATE delta: an orphan-U
		 * whose MAIN row exists must report the UPDATE-specific conflict before
		 * the INSERT pass runs. */
		if (n_upd + n_orphan > 0)
		{
			c = ob_batch_validate(ri, bid, DELTA_OP_UPDATE);
			if (c.found)
				ob_batch_report_conflict(ri, DELTA_OP_UPDATE, &c, branch_name);
			if (n_upd > 0)
				ob_batch_update_pass(ri, bid, n_upd);
		}

		/* ---- INSERT pass: new rows plus materialised orphan-U rows ---- */
		if (n_ins > 0)
		{
			c = ob_batch_validate(ri, bid, DELTA_OP_INSERT);
			if (c.found)
				ob_batch_report_conflict(ri, DELTA_OP_INSERT, &c, branch_name);
			ob_batch_insert_pass(ri, bid, n_ins);
		}
	}
	PG_FINALLY();
	{
		if (needs_lc_pin)
			ob_canonical_guc_unpin(guc_nl);
	}
	PG_END_TRY();
}
