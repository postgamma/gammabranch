/*-------------------------------------------------------------------------
 *
 * overlay_branch.h
 *	  Type and function declarations for the Overlay Branch extension.
 *
 *-------------------------------------------------------------------------
 */
#ifndef OVERLAY_BRANCH_H
#define OVERLAY_BRANCH_H

#include "postgres.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "datatype/timestamp.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"
#include "storage/lmgr.h"
#include "storage/lockdefs.h"
#include "storage/lock.h"

/* ----------
 * Branch modes
 * ----------
 */
#define BRANCH_MODE_LIVE		"live"
#define BRANCH_MODE_SNAPSHOT	"snapshot"

/* ----------
 * FR2: Branch-level Advisory Lock magic key.
 *
 * int64 key layout (exactly matches PG's native SET_LOCKTAG_ADVISORY form
 * used by pg_advisory_xact_lock(int8)):
 *   bits 63..48  = 0x4F42 = magic prefix 'O' 'B'  ("overlay branch")
 *   bits 47..32  = reserved, set 0
 *   bits 31..0   = uint32 view of int32 branch_id (from pg_branch.branch_id)
 *
 * Collision probability with user-generated advisory keys is ~1/2^16 since
 * ordinary keys almost never carry high bits 0x4F42.
 *
 * We locally re-define SET_LOCKTAG_INT64 here (mirroring lockfuncs.c L613)
 * because that macro is a file-local helper in the backend, not exported
 * through headers.
 * ----------
 */
#define OB_ADVISORY_MAGIC_HI16  ((uint64) 0x4F42ULL << 48)
#define OB_MAKE_ADVISORY_KEY(bid)  \
	(OB_ADVISORY_MAGIC_HI16 | ((uint64) ((uint32) (bid))))

#ifndef SET_LOCKTAG_INT64
#define SET_LOCKTAG_INT64(tag, key64) \
	SET_LOCKTAG_ADVISORY(tag, \
						 MyDatabaseId, \
						 (uint32) ((key64) >> 32), \
						 (uint32) (key64), \
						 1)
#endif

/* ----------
 * FR2 helpers: take/release xact-scoped advisory lock on a branch_id.
 *
 * Use sessionLock = false so the lock is tied to the current transaction
 * (auto-released on COMMIT / ABORT, never leaks).
 *
 * Two modes are used on the same key (doc/internals.md §3.3):
 *   - delta writers take ShareLock (blocking) and hold it to end of xact;
 *   - apply/discard take ExclusiveLock non-blocking, so a conflicting writer
 *     (or another apply/discard) makes them fail fast instead of waiting and
 *     forming a ShareLock -> ExclusiveLock upgrade deadlock.
 * ----------
 */
static inline void
ob_take_branch_advisory_lock(int32 bid, LOCKMODE lockmode)
{
	int64		key = (int64) OB_MAKE_ADVISORY_KEY(bid);
	LOCKTAG		tag;

	if (bid <= 0)
		elog(ERROR,
			 "overlay_branch: invalid branch_id=%d for advisory lock",
			 bid);
	SET_LOCKTAG_INT64(tag, key);
	(void) LockAcquire(&tag, lockmode, false, false);
}

/* Non-blocking acquire.  Returns the raw LockAcquireResult so callers can tell
 * a fresh acquire (LOCKACQUIRE_OK) from one already held by this transaction
 * (LOCKACQUIRE_ALREADY_HELD); LOCKACQUIRE_NOT_AVAIL means a conflicting holder
 * exists and dontWait refused it.  ob_try_branch_advisory_lock() below is the
 * boolean view. */
static inline LockAcquireResult
ob_try_branch_advisory_lock_ex(int32 bid, LOCKMODE lockmode)
{
	int64		key = (int64) OB_MAKE_ADVISORY_KEY(bid);
	LOCKTAG		tag;

	if (bid <= 0)
		elog(ERROR,
			 "overlay_branch: invalid branch_id=%d for advisory lock",
			 bid);
	SET_LOCKTAG_INT64(tag, key);
	return LockAcquire(&tag, lockmode, false, true);
}

/* Boolean view of ob_try_branch_advisory_lock_ex(): true = held.  Callers that
 * must tell "newly acquired" from "already held" (the delta-write state-check
 * fast path, doc/internals.md §3.3) use the _ex form directly. */
static inline bool
ob_try_branch_advisory_lock(int32 bid, LOCKMODE lockmode)
{
	return (ob_try_branch_advisory_lock_ex(bid, lockmode)
			!= LOCKACQUIRE_NOT_AVAIL);
}

static inline void
ob_release_branch_advisory_lock(int32 bid, LOCKMODE lockmode)
{
	int64		key = (int64) OB_MAKE_ADVISORY_KEY(bid);
	LOCKTAG		tag;

	if (bid <= 0)
		return;
	SET_LOCKTAG_INT64(tag, key);
	(void) LockRelease(&tag, lockmode, false);
}

/* ----------
 * Fully qualified schema (control file pins schema='overlay_branch',
 * non-relocatable).  Exposed so branch_scan.c Planner hook can skip
 * overlay_branch-owned catalog tables (pg_branch, pg_branch_delta)
 * which would otherwise cause infinite recursion via SPI lookups.
 * ----------
 */
#define OBSCHEMA				"overlay_branch"

/* ----------
 * Branch states
 * ----------
 */
#define BRANCH_STATE_ACTIVE	"active"
#define BRANCH_STATE_APPLYING	"applying"	/* FR3 transient CAS state while apply runs */
#define BRANCH_STATE_APPLIED	"applied"
#define BRANCH_STATE_DISCARDED	"discarded"

/* ----------
 * Layer1 NOTIFY channel name (FR1: broadcast branch state changes).
 * Payload format: "<branch_id>:<new_state>"
 * ----------
 */
#define OB_NOTIFY_CHANNEL		"ob_branch_state"

/* ----------
 * Delta operation types
 * ----------
 */
#define DELTA_OP_INSERT		'I'
#define DELTA_OP_UPDATE		'U'
#define DELTA_OP_DELETE		'D'

/* ----------
 * BranchContext: per-session current branch state
 * ----------
 */
typedef struct BranchContext
{
	int32		branch_id;		/* current branch id, 0 means none */
	char		branch_name[NAMEDATALEN];	/* current branch name */
	bool		is_active;		/* whether we are inside a branch */
	char		mode[16];		/* "live" or "snapshot" */
	TimestampTz	created_at;		/* branch creation time */
	Oid			owner;			/* branch owner (from pg_branch.owner) */
	/* ===== multi-session fields (append-only, keep ABI compat) ===== */
	struct SnapshotData *branch_main_snapshot;	/* NULL=live; non-NULL=snapshot frozen MAIN reads */
	bool		snapshot_registered;			/* true iff UnregisterSnapshot is pending */
	uint32		invalidation_counter;			/* FR4: throttle SPI re-checks */
	TimestampTz	invalidation_last_check;		/* FR4: throttle SPI re-checks */
	TimestampTz	entered_at;						/* REVIEW-260926/R19: when use_internal last activated this ctx (snapshot TTL clock).  0 = not in snapshot mode. */
} BranchContext;

/* ----------
 * BranchExecContext (A1/A6): per-statement branch identity.
 *
 * Bound exactly once per statement in overlay_ExecutorStart(), derived from
 * CurrentBranchContext at bind time.  Read sites for the rest of the
 * statement consult this read-only copy instead of re-deriving identity from
 * mutable globals, so a statement cannot observe a torn branch identity via
 * nested SPI, subtransaction rollback or cross-session kickout.
 *
 * Storage: a fixed, backend-local stack (no palloc; see ob_exec_ctx_push()).
 * CurrentBranchExecContext always points at the innermost open statement,
 * or NULL when no statement is executing.
 *
 * Deliberately NOT in this struct: the ob_in_* recursion bypass flags (they
 * guard same-backend re-entrancy, which is inherently scoped to one
 * statement) and is_active/kickout (FR4 must stay live).
 * ----------
 */
typedef struct BranchExecContext
{
	int32		bid;			/* branch in effect; 0 = MAIN */
	char		mode[16];		/* BRANCH_MODE_LIVE / BRANCH_MODE_SNAPSHOT */
	struct SnapshotData *stmt_snap;	/* MAIN read snapshot; NULL = live */
} BranchExecContext;

extern BranchExecContext *ob_get_exec_context(void);

/* ----------
 * TypedKey (REVIEW-260926/R14):
 *   Per-column typed primary-key value used for BranchScan comparisons
 *   that MUST match PostgreSQL's native type equality semantics (BPCHAR
 *   r-trim, NUMERIC precision-insensitive equals, TIMESTAMPTZ absolute
 *   UTC comparison).  The legacy JSON-serialised `key` TEXT field is
 *   kept for persistence into pg_branch_delta and for legacy helper
 *   callers; the TypedKey array is the authoritative comparison source
 *   inside the C extension.
 *
 *   Memory: typbyval=false Datum values (text/numeric/timestamptz etc.)
 *   are palloc'd inside the same context as the owner DeltaTuple, and
 *   are freed by overlay_typed_pk_free().
 * ----------
 */
typedef struct TypedKey
{
	Oid			typid;			/* pg_type.oid */
	int32		typmod;			/* resolved attribute typmod, -1 = none */
	Oid			collid;			/* collation Oid, InvalidOid = default */
	Datum		value;			/* datum value */
	bool		isnull;			/* true = NULL PK part (error for strict) */
	bool		typbyval;		/* cached: true = Datum is pass-by-value */
	int16		typlen;			/* cached: typlen from pg_type */
	/* ===== RD-5: memoised comparison functions for typid =====
	 * Points at the per-type FmgrInfo owned by delta_store.c's PK cache
	 * (allocated in TopMemoryContext, never freed), so the pointers stay
	 * valid for the whole backend lifetime regardless of when the owning
	 * DeltaTuple / TypedKey array is released.  NULL = unavailable (the
	 * type has no btree sort operators); overlay_typed_pk_cmp() then
	 * falls back to its legacy per-comparison catalog lookup. */
	struct FmgrInfo *eq_finfo;	/* "<->" (btree equality) */
	struct FmgrInfo *lt_finfo;	/* "<"   (btree less-than) */
} TypedKey;

/* ----------
 * DeltaTuple: in-memory representation of one delta entry
 * ----------
 */
typedef struct DeltaTuple
{
	int32		branch_id;
	Oid			relid;
	char	   *key;			/* serialized PK value (TEXT, for SPI storage + legacy) */
	char		op;				/* 'I', 'U', or 'D' */
	char	   *old_version;	/* base version for conflict check */
	bytea	   *tuple_data;		/* serialized new tuple (NULL for delete) */
	bool		emitted;		/* Step4b: has this entry been output in the main pass? */
	/* ===== append-only fields (ABI-safe) — typed pk, REVIEW-260926/R14 ===== */
	TypedKey  *typed_pk;		/* NULL=fallback strcmp(dt->key); non-NULL=typed comparison */
	int			typed_pk_n;		/* -1=uninitialised, else length of typed_pk[] */
} DeltaTuple;

/* ----------
 * GUC: current_branch session variable
 * ----------
 */
extern char *overlay_branch_current_name;
extern bool overlay_branch_enabled;
extern bool overlay_keyless_tables;	/* experimental: ctid identity for no-PK tables */

/* ----------
 * multi-session tuning GUCs
 * ----------
 */
extern int	ob_invalidation_check_threshold;	/* every N is_active() calls → SPI check */
extern int	ob_invalidation_check_interval_ms;	/* every M ms → SPI check */
extern bool	ob_apply_strict_pins;				/* apply fails if other sessions hold pins */
extern bool	ob_apply_batch;						/* apply replays deltas per (relid, op) */
extern bool	ob_use_shared_mem_pin_table;		/* optional: shmem pin tracking */
extern bool	ob_in_snapshot_mode_helper;		/* internal: PushActiveSnapshot guard */
extern int	ob_snapshot_max_hold_minutes;		/* R19 RESTRICTED: snapshot mode max minutes */
extern int	ob_delta_size_warn_rows;			/* delta rows above which the write path warns (0 = off) */
extern int	ob_delta_size_check_interval;		/* check delta size once every N delta writes */

/* ----------
 * Recursion-guard globals (owned by overlay_branch.c; written
 * directly by use_internal/apply_internal in branch_lifecycle.c
 * to avoid layering extra trivial setters/getters).
 * ----------
 */
extern bool ob_in_apply_operation;
extern bool ob_in_guc_setconfig;
extern bool ob_in_invalidation_check;
extern bool ob_in_planner_hook;  /* set = throttled SPI running, depth-1 guard. */
extern bool ob_throttled_allow_kickout;  /* only ExecutorRun WR dml_split entry sets true = do cleanup. */

/* ----------
 * Global branch context (for the current session)
 * ----------
 */
extern BranchContext *CurrentBranchContext;

/* ----------
 * §G02 ACL unified helpers
 *
 * All SQL-callable lifecycle wrappers MUST call these before dispatching
 * to the internal implementation.  The helpers implement double-layer
 * defense: (1) SQL layer limits EXECUTE to owner / builtin role
 * `overlay_branch_administrators`; (2) C layer re-evaluates privileges
 * using the branch row's stored owner Oid, so even if someone bypasses
 * the SQL grant via direct @extschema@ qualification they still get
 * 42501.
 *
 * Builtin role name (kept in one place):
 *   #define OB_MGMT_ROLE  "overlay_branch_administrators"
 *
 * Privilege matrix:
 *   create_branch()                    → superuser OR member of OB_MGMT_ROLE
 *   use_branch(name) / use_with_mode  → owner OR superuser OR mgmt (NULL=EXIT
 *                                       is unrestricted — session-level action)
 *   apply_branch(name)                → owner OR superuser OR mgmt
 *   discard_branch(name)              → owner OR superuser OR mgmt
 * ----------
 */
#define OB_MGMT_ROLE_NAME		"overlay_branch_administrators"
typedef enum ObLifecycleOp
{
	OB_OP_CREATE_BRANCH = 0,
	OB_OP_USE_BRANCH,       /* branch_name may be NULL → skip owner check */
	OB_OP_APPLY_BRANCH,
	OB_OP_DISCARD_BRANCH
} ObLifecycleOp;

/* Raises 42501 with a uniform message when the caller is not authorised.
 * When op == OB_OP_USE_BRANCH and branch_name is NULL/empty the caller
 * is trying to EXIT the branch — that path is always allowed and we
 * return without error. */
extern void			ob_acl_check_lifecycle(ObLifecycleOp op,
										   const char *branch_name);

/* ----------
 * G01 Branch-scope pre-flight (support matrix fail-fast gate).
 *
 * Scan current database for all user permanent relations (excludes
 * pg_catalog, information_schema and the extension schema) and
 * raises 0A000 at the FIRST offending relation.  Supported relkinds
 * are only permanent tables ('r') plus their plain indexes
 * ('i') and sequences ('S'); all else plus 7 categories below are
 * RESTRICTED.  This avoids silent data corruption when apply-time
 * semantics do not match MAIN-side behaviour (e.g. FKs fire MAIN
 * only, triggers are MAIN-only, partition promotion mixes relids
 * wrong).
 *
 * Rejected categories:
 *   (A) ANY foreign key reference (PK side OR FK side).
 *   (B) User triggers (non-internal, non-disabled).
 *   (C) Partitioning (relkind 'p'/'I' OR relispartition).
 *   (D) Table inheritance (inhparent OR inhrelid in pg_inherits).
 *   (E) Unlogged tables (relpersistence = 'u').
 *   (F) GENERATED STORED columns.
 *   (G) FDW tables / views / matviews / anything not in {'r','S','i'}.
 * ----------
 */
extern void			ob_check_branch_scope(void);

/* ----------
 * §Gx / A7 Schema epoch registry helpers.
 *
 * Registry catalog table (overlay_branch.pg_branch_registry) stores
 * per-create_branch (bid, relid) fingerprints of every user table
 * at the time create_branch succeeded.  4-tuple signature =
 * (total_cols, pk_cols, md5 col_hash, md5 pk_hash) so we can tell
 * the user exactly *what* drifted in the 55000 detail/hint.
 *
 *   ob_registry_populate_for_create(bid): called IMMEDIATELY after
 *     create_branch gets new_branch_id (after currval() commit,
 *     after scope preflight passed).  SPI scan current DB user
 *     tables + INSERT rows, ZERO row = no user tables = NOP.
 *
 *   ob_registry_check_current_schema(bid, op_name): called from
 *     use_with_mode_internal after bid fetched but BEFORE any
 *     snapshot install, AND from apply_internal after bid fetched
 *     BEFORE advisory lock / CAS.  SPI scans current DB user tables
 *     and LEFT JOINs registry WHERE bid = given.  Raises
 *     ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE (55000) with detail
 *     of FIRST mismatch (missing table / new table / cols / pk /
 *     hash drift).  bid missing from registry at all → DEFER silently
 *     (backwards compat: old branches created before 1.3 upgrade
 *     might have no rows; check skipped).
 *
 *   ob_registry_cascade_discard(bid, spi_connected_already): called
 *     from discard_internal right BEFORE SPI_finish() so we share
 *     the open SPI connection; DELETE FROM registry WHERE bid.
 * ----------
 */
#define OBTABLE_REGISTRY  OBSCHEMA ".pg_branch_registry"

extern void			ob_registry_populate_for_create(int32 bid);
extern void			ob_registry_check_current_schema(int32 bid, const char *op_name);
extern void			ob_registry_cascade_discard(int32 bid);

/* ----------
 * Function declarations for branch management
 * ----------
 */
extern void		overlay_branch_init(void);
extern int32	overlay_branch_create_internal(const char *branch_name);
extern void		overlay_branch_use_internal(const char *branch_name);
extern void		overlay_branch_use_with_mode_internal(const char *branch_name,
													   const char *mode);
extern const char *overlay_branch_get_current_name(void);
extern int32	overlay_branch_get_current_id(void);
extern bool		overlay_branch_is_active(void);
extern void		overlay_branch_apply_internal(const char *branch_name);
extern void		overlay_branch_discard_internal(const char *branch_name);
extern void		ob_ensure_unregister_snapshot(BranchContext *ctx);
extern void		ob_exit_branch_cleanup(BranchContext *ctx);
	/* Layer1: NOTIFY broadcast + throttled invalidation SPI recheck */
	extern void		ob_broadcast_state_change(int32 bid, const char *new_state);
	extern bool		ob_invalidate_check_throttled(bool for_dml);

/* I8.2 (2026-09-17 L1 ob_apply_mutex deadlock-timeout fix):
 * session-local per-bid mode cache, replaces use_branch catalog UPDATE.
 *
 * Old pre-I8.2 code persisted every use_branch(name, mode) via SPI
 * UPDATE pg_branch.mode → each UPDATE took RowExclusiveLock on the
 * matching catalog row. Two sessions calling use_branch('same_bid')
 * therefore serialized at the heavyweight lock → under
 * pg_isolation_regress this manifest as s2_p1_use `<waiting ...>`
 * 360s timeout → false deadlock cancel on permutation P2.
 *
 * Cache semantics (identical to catalog behavior within one session):
 *   • bid→last-used-mode HTAB, session lifetime.
 *   • Set on every with_mode_internal success (incl. 1-arg default live).
 *   • Overlaid in list_branches SRF over catalog mode.
 *   • Removed by apply_internal / discard_internal when bid no longer
 *     ACTIVE (no re-enter possible until future recreate; catalog row
 *     then inserts mode='live' default).
 * No heavyweight locks, no cross-session serialization.
 */
extern const char *ob_mode_cache_lookup(int32 bid);
extern void     ob_mode_cache_set(int32 bid, const char *mode);
extern void     ob_mode_cache_remove(int32 bid);

/* T8 § B.11 debug GUCs (NOT ABI-stable, _debug_ prefix) */
extern int		ob_debug_apply_sleep_sec;
extern int		ob_debug_invalidation_counter_throttle_override;

/* ----------
 * Function declarations for Delta Store operations
 * ----------
 */
extern void		overlay_delta_insert(int32 branch_id, Oid relid,
									  const char *key, char op,
									  const char *old_version, const char *tuple_json_cstr);
extern void		overlay_delta_update(int32 branch_id, Oid relid,
									  const char *key, char op,
									  const char *old_version, const char *tuple_json_cstr);
extern bool		overlay_delta_lookup(int32 branch_id, Oid relid,
									  const char *key, DeltaTuple *out_tuple);
extern List    *overlay_delta_list_for_rel(int32 branch_id, Oid relid);
extern int64	overlay_delta_count(int32 branch_id);
extern void		overlay_delta_delete_all(int32 branch_id);

/* ----------
 * Function declarations for Write Redirect
 * ----------
 */
extern bool		overlay_should_redirect(Relation rel);
extern void		overlay_modify_insert(Relation rel, TupleTableSlot *slot);
extern void		overlay_modify_update(Relation rel, TupleTableSlot *oldslot,
									   TupleTableSlot *newslot);
extern void		overlay_modify_delete(Relation rel, TupleTableSlot *slot);

/* ----------
 * Helper utilities
 * ----------
 */
extern char    *overlay_serialize_pk(Relation rel, TupleTableSlot *slot);
extern char    *overlay_serialize_tuple(Relation rel, TupleTableSlot *slot);
extern char    *overlay_tuple_version(Relation rel, TupleTableSlot *slot);

/* ----------
 * Base-version token codec (A1/A6 stage 2, serves R15/R18).
 *
 * The optimistic CAS token that ties a delta row to the MAIN image it was
 * first written against is the string "<blk>:<off>-x<xmin>".  It is produced
 * by overlay_tuple_version() on the write path and by ob_fetch_main_version()
 * at apply time, and parsed back by ob_anchor_is_stale() on the read path.
 * Those three sites must agree byte-for-byte, so the shape lives here only:
 * every producer calls overlay_base_ver_format(), every consumer calls one of
 * the parse helpers.  Buffer size for the formatted token; the longest form
 * ("4294967295:65535-x4294967295") is well under 32 bytes.
 * ----------
 */
#define OB_BASE_VER_MAX		64

extern bool		overlay_base_ver_parse_ctid(const char *ctid_txt,
										   uint32 *blk, uint32 *off);
extern void		overlay_base_ver_format(char *buf, size_t buflen,
									   uint32 blk, uint32 off,
									   const char *xmin_txt);
extern bool		overlay_base_ver_parse_xmin(const char *token,
										TransactionId *xmin_out);

extern bool		overlay_relation_has_pk(Relation rel);
extern char    *overlay_serialize_identity(Relation rel, TupleTableSlot *slot, char op);
extern char    *overlay_anchor_key_from_ctid(const char *ctid_cstr);
extern char    *overlay_synthetic_key(int32 branch_id, Oid relid);
extern void		overlay_keyless_warn_once(Relation rel);

extern bool		overlay_get_pk_single_attno(Relation rel,
											 AttrNumber *out_pk_attno,
											 const char **out_pk_colname);
extern List    *overlay_get_pk_attnos_list(Relation rel);
extern char    *overlay_serialize_pk_from_single_datum(Relation rel,
													   AttrNumber pk_attno,
													   Datum pk_val,
													   bool pk_isnull,
													   Oid consttype);

/* ---------- Typed PK helpers (REVIEW-260926/R14) ----------
 *
 * All return non-NULL TypedKey* arrays on success (caller must
 * overlay_typed_pk_free to avoid typbyval=false datum leaks).
 * out_n_pk is set to the number of PK columns on success (>= 1).
 * On any failure the functions raise ERROR with a descriptive message
 * and do not return; they never return NULL with a valid *out_n_pk.
 */
extern TypedKey *overlay_build_typed_pk_from_slot(Relation rel,
												  TupleTableSlot *slot,
												  int *out_n_pk);
extern TypedKey *overlay_build_typed_pk_from_key_text(Relation rel,
													  const char *key_json,
													  int *out_n_pk);
extern int  overlay_typed_pk_cmp(const TypedKey *a, int a_n,
								 const TypedKey *b, int b_n);
extern void overlay_typed_pk_free(TypedKey *pk, int n_pk);

/* ----------
 * Step7 HARD GUARD: shared helper for checking rel eligibility inside an
 * active branch.  Returns NULL if ok, otherwise returns a malloc'd cstring
 * explaining why the guard fired (caller ereports it with a uniform
 * HINT to leave branch mode).
 *
 * We also expose individual predicate helpers so both ExecutorRun (per-DML)
 * and ProcessUtility (per-DDL) hooks can compose the same error messages.
 *
 *  Guard level order (cheapest → most expensive; any one true → ERROR):
 *    G1: relkind != RELKIND_RELATION       (partitioned table/view/matview/foreign/sequence/composite/TOAST)
 *    G2: rel is partition child            (rd_rel->relispartition)
 *    G3: rel has triggers (excluding internal) → ri_Triggers != NIL or
 *        trigdesc != NULL && any NOT tgisinternal
 *    G4: rel is FK-referenced / has FKs    (rd_att->constr has FK / ref FK count>0)
 *    G5: NO PRIMARY KEY                    (overlay_relation_has_pk == false)
 * ----------
 */
extern bool overlay_guard_rel_ok(Relation rel, char **reason);
extern void overlay_guard_ensure_branch_or_main_active(void); /* prerequisite */
/* branch_name: the branch this failure belongs to.  Callers that run
 * OUTSIDE branch mode (apply/discard) must pass it explicitly; callers
 * running INSIDE branch mode pass NULL and the active branch is used. */
extern void overlay_guard_ereport_fail(const char *operation, const char *relname,
									   const char *branch_name, const char *reason);

/* Step7 ProcessUtility guard: T_TruncateStmt / T_DropStmt / T_AlterTable /
 * T_CreateStmt / T_IndexStmt / T_VacuumStmt / T_ClusterStmt / T_RenameStmt etc.
 * — any DDL on user rels inside branch mode. */
extern bool overlay_guard_ddl_ok_for_branch(Node *parsetree, char **operation,
											 char **objname, char **reason);

/* ----------
 * Shared internals exposed to branch_scan.c / write_redirect.c /
 * branch_lifecycle.c
 * ---------- */
extern bool             overlay_in_apply_operation(void);
extern bool             overlay_in_overlay_helper(void);
extern void             overlay_overlay_helper_enter(void);
extern void             overlay_overlay_helper_exit(void);
extern bool             overlay_in_write_redirect(void);
extern void             overlay_write_redirect_enter(void);
extern void             overlay_write_redirect_exit(void);
extern int              ob_spi_one_shot(const char *sql, bool read_only, uint64 tcount);
/* WR-1a: parameterized prepared-plan reuse (spi_helpers.c).  Same
 * connect/finish contract as ob_spi_one_shot(); values are bound as $n
 * parameters instead of being interpolated into the SQL text. */
extern int              ob_spi_exec_params(const char *sql, bool read_only,
										   uint64 tcount, int nargs,
										   const Oid *argtypes,
										   Datum *values, const char *nulls);
extern TupleTableSlot  *reconstruct_slot_from_delta(Relation rel, bytea *tuple_data,
													MemoryContext result_cxt);

/* ----------
 * Slot/fetch helpers (delta_store.c) — used by write_redirect.c
 * to translate junk-slot attnos → clean relation slots via ctid.
 * ----------
 */
extern AttrNumber       rel_attno_to_slot_idx(TupleDesc slotdesc, AttrNumber rel_attno);
extern char            *slot_get_ctid_cstr(TupleTableSlot *slot);
extern TupleTableSlot  *fetch_tuple_by_ctid(Relation rel, const char *ctid_cstr,
											MemoryContext result_cxt);

/* ----------
 * write_redirect.c entry point — thin dispatch from
 * overlay_ExecutorRun in main overlay_branch.c.  mt_state_result_rel
 * is also exposed because the original forward-decl was static.
 * ----------
 */
#include "executor/execdesc.h"      /* QueryDesc + ScanDirection */
struct ModifyTableState;            /* forward decl */
struct ResultRelInfo;
typedef void (*ExecutorRun_hook_type_fn)(QueryDesc *queryDesc,
										 ScanDirection direction,
										 uint64 count, bool execute_once);
extern bool             overlay_executor_run_intercept(QueryDesc *queryDesc,
													   ScanDirection direction,
													   uint64 count,
													   bool execute_once,
													   ExecutorRun_hook_type_fn prev_ExecutorRun);
extern struct ResultRelInfo *mt_state_result_rel(struct ModifyTableState *mt, int i);

#endif							/* OVERLAY_BRANCH_H */
