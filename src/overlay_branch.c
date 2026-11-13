/*-------------------------------------------------------------------------
 *
 * overlay_branch.c
 *	  Overlay Branch extension: speculative database state using
 *	  table overlay and delta store.
 *
 *	  MAIN MODULE (kept lean after 4-way split):
 *		- Module load callback (_PG_init) with GUC registration
 *		- GUC variables + check-assign hook
 *		- 4-layer recursion-guard flags (apply/guc_setconfig/
 *		  overlay_helper/write_redirect) + their getters/setters
 *		- 4 Executor hooks (Start/Run/Finish/End) + ProcessUtility hook
 *		  (ExecutorRun dispatches to write_redirect.c intercept)
 *		- Step7 HARD GUARD functions (rel_ok / ddl_ok / ereport_fail)
 *		- SQL-callable function thin-dispatch wrappers (all real logic
 *		  lives in branch_lifecycle.c / delta_store.c / write_redirect.c)
 *		- overlay_main_plus_delta SRF (manual Main+Delta view) and
 *		  overlay_branch_list SRF — both kept here because they use
 *		  public helpers only and are simple SQL-callable entry points.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"
#include "branch_scan.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#ifndef BOOTSTRAP_SUPERUSERID
#define BOOTSTRAP_SUPERUSERID	((Oid) 10)
#endif

/* Fully qualified table / schema names */
#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

/* ================================================================
 * Module magic — EXACTLY ONE definition per extension, lives here.
 * ================================================================ */
PG_MODULE_MAGIC;

/* ================================================================
 * GUC variables (session-scoped; owned by GUC framework)
 * ================================================================ */
char	   *overlay_branch_current_name = NULL;
bool		overlay_branch_enabled = true;

/* Experimental keyless-table support (see doc/internals.md §4).
 * When false (default) a table without a PRIMARY KEY is rejected by the
 * G5 guard exactly as before.  When true the extension falls back to a
 * best-effort physical (ctid, xmin) row identity, emitting a one-shot
 * WARNING the first time such a table is touched in a session. */
bool		overlay_keyless_tables = false;

/* ================================================================
 * multi-session tuning GUCs
 * ================================================================ */
int			ob_invalidation_check_threshold = 32;
int			ob_invalidation_check_interval_ms = 100;
bool		ob_apply_strict_pins = true;
/* V6 / phase 3B: replay each relation's deltas as one set-based batch per
 * operation instead of one row at a time.  Semantics are unchanged; this only
 * removes the per-row planning cost (see doc/internals.md §5). */
bool		ob_apply_batch = true;
bool		ob_use_shared_mem_pin_table = false;
bool		ob_in_snapshot_mode_helper = false;

/* ================================================================
 * REVIEW-260926 / R19 GUC: snapshot mode maximum hold time (minutes)
 *
 *   Snapshot mode captures and registers a snapshot that pins the
 *   global RecentGlobalXmin, which delays vacuuming of dead rows
 *   for the ENTIRE cluster.  Indefinite pinning is a resource
 *   exhaustion DoS hazard.  To keep the RESTRICTED posture, we
 *   refuse to enter snapshot mode past this per-session lifetime
 *   budget (0 = allow infinite; PGC_POSTMASTER so superuser chooses).
 *   Enforcement lives in branch_lifecycle.c
 *   use_with_mode_internal() and throttled() helper.
 * ================================================================ */
int			ob_snapshot_max_hold_minutes = 10; /* 10 minutes = default */

/* ================================================================
 * B1 delta-size observability GUCs
 *
 *   The write path already funnels every delta row through one
 *   choke point (overlay_delta_insert).  When delta_size_warn_rows
 *   is > 0 the choke point counts the current branch's delta rows
 *   once every delta_size_check_interval writes and emits a single
 *   WARNING per session when the count first exceeds the threshold
 *   (see doc/internals.md §5.6).  Default 0 disables the whole
 *   check, so the write path stays byte-for-byte unchanged.
 * ================================================================ */
int			ob_delta_size_warn_rows = 0;
int			ob_delta_size_check_interval = 1024;

/* ================================================================
 * Global session state
 * ================================================================ */
BranchContext *CurrentBranchContext = NULL;

/* ================================================================
 * A1/A6: per-statement branch identity (BranchExecContext)
 *
 * ob_exec_ctx_push()/pop() bracket every ExecutorStart/ExecutorEnd, so the
 * innermost executing statement is always reachable via
 * CurrentBranchExecContext.  The context is derived once, at push time, from
 * the live CurrentBranchContext; read sites then consume the copy.
 *
 * The stack is a fixed backend-local array (no palloc, no free): executor
 * nesting depth from SPI is a handful at most.  It only needs an explicit
 * reset because PostgreSQL skips ExecutorEnd on error abort (see
 * portalcmds.c PortalCleanup), so aborted statements would otherwise leak
 * stack slots; ob_exec_ctx_reset() is driven from the transaction callback.
 * ================================================================ */
BranchExecContext *CurrentBranchExecContext = NULL;

#define OB_EXEC_CTX_MAX_DEPTH	64
static BranchExecContext ob_exec_ctx_stack[OB_EXEC_CTX_MAX_DEPTH];
static void *ob_exec_ctx_owner[OB_EXEC_CTX_MAX_DEPTH];	/* QueryDesc*, parallel */
static int	ob_exec_ctx_depth = 0;			/* number of slots in use */
static int	ob_exec_ctx_overflow = 0;		/* pushes past OB_EXEC_CTX_MAX_DEPTH */

/* Subtransaction bookkeeping: depth at each open subxact level, so an
 * aborted subxact can drop every statement it started. */
#define OB_SUBXACT_MAX_LEVEL	64
static int	ob_subxact_saved_depth[OB_SUBXACT_MAX_LEVEL];
static int	ob_subxact_level = 0;

/* ---------- Recursion guard flags (5-layer Bypass stack) ---------- */
bool		ob_in_apply_operation = false;		/* extern: branch_lifecycle.c writes */
bool		ob_in_guc_setconfig = false;		/* extern: branch_lifecycle.c writes */
bool		ob_in_invalidation_check = false;	/* extern: branch_lifecycle.c throttled() */
bool		ob_in_planner_hook = false;		/* extern: branch_scan planner_hook wrap */
bool		ob_throttled_allow_kickout = false;	/* extern: only WR ExecutorRun allows cleanup */
static bool ob_in_overlay_helper = false;
static bool ob_in_write_redirect = false;

/*
 * The ob_in_* recursion guards must never outlive the statement / subxact
 * that set them.  An error thrown inside a guard region longjmps out of the
 * C helper, skipping its matching overlay_*_exit(), so the flag stays stuck
 * true and silently disables the overlay for the rest of the session (reads
 * fall back to MAIN).  The executor-context stack above is unwound exactly
 * this way by the xact/subxact callbacks; the guard flags ride along on the
 * same save/restore so a caught error cannot leak them.
 *
 * Order is fixed and shared by ob_guards_save() / ob_guards_restore().
 */
#define OB_GUARD_COUNT	7
static bool	ob_subxact_saved_guards[OB_SUBXACT_MAX_LEVEL][OB_GUARD_COUNT];

static void
ob_guards_save(bool *dst)
{
	dst[0] = ob_in_apply_operation;
	dst[1] = ob_in_guc_setconfig;
	dst[2] = ob_in_invalidation_check;
	dst[3] = ob_in_planner_hook;
	dst[4] = ob_throttled_allow_kickout;
	dst[5] = ob_in_overlay_helper;
	dst[6] = ob_in_write_redirect;
}

static void
ob_guards_restore(const bool *src)
{
	ob_in_apply_operation = src[0];
	ob_in_guc_setconfig = src[1];
	ob_in_invalidation_check = src[2];
	ob_in_planner_hook = src[3];
	ob_throttled_allow_kickout = src[4];
	ob_in_overlay_helper = src[5];
	ob_in_write_redirect = src[6];
}

static void
ob_guards_clear(void)
{
	ob_in_apply_operation = false;
	ob_in_guc_setconfig = false;
	ob_in_invalidation_check = false;
	ob_in_planner_hook = false;
	ob_throttled_allow_kickout = false;
	ob_in_overlay_helper = false;
	ob_in_write_redirect = false;
}

static void
ob_exec_ctx_push(void *owner)
{
	BranchExecContext *c;

	if (ob_exec_ctx_depth < OB_EXEC_CTX_MAX_DEPTH)
	{
		c = &ob_exec_ctx_stack[ob_exec_ctx_depth];
		ob_exec_ctx_owner[ob_exec_ctx_depth] = owner;
		ob_exec_ctx_depth++;
	}
	else
	{
		/* Pathological nesting only: saturate on the top slot so the
		 * matching pop stays balanced.  Never hit in practice. */
		c = &ob_exec_ctx_stack[OB_EXEC_CTX_MAX_DEPTH - 1];
		ob_exec_ctx_overflow++;
	}

	if (CurrentBranchContext != NULL && CurrentBranchContext->is_active)
	{
		c->bid = CurrentBranchContext->branch_id;
		strlcpy(c->mode, CurrentBranchContext->mode, sizeof(c->mode));
		c->stmt_snap = CurrentBranchContext->branch_main_snapshot;
	}
	else
	{
		c->bid = 0;
		strlcpy(c->mode, BRANCH_MODE_LIVE, sizeof(c->mode));
		c->stmt_snap = NULL;
	}

	CurrentBranchExecContext = c;
}

/* Unwind down to (and including) the entry owned by `owner`.  Statements
 * that aborted before ExecutorEnd never popped themselves, so entries above
 * the match are leaked ones and are dropped here as well. */
static void
ob_exec_ctx_unwind(void *owner)
{
	int			i;

	if (ob_exec_ctx_overflow > 0)
		ob_exec_ctx_overflow--;
	else
	{
		for (i = ob_exec_ctx_depth - 1; i >= 0; i--)
		{
			if (ob_exec_ctx_owner[i] == owner)
			{
				ob_exec_ctx_depth = i;
				break;
			}
		}
	}

	CurrentBranchExecContext = (ob_exec_ctx_depth > 0)
		? &ob_exec_ctx_stack[ob_exec_ctx_depth - 1]
		: NULL;
}

static void
ob_exec_ctx_reset(void)
{
	ob_exec_ctx_depth = 0;
	ob_exec_ctx_overflow = 0;
	ob_subxact_level = 0;
	CurrentBranchExecContext = NULL;
	ob_guards_clear();
}

/* PostgreSQL skips ExecutorEnd on error abort (portalcmds.c PortalCleanup),
 * so aborted statements cannot pop themselves.  A subtransaction abort drops
 * every statement started since that savepoint; a transaction end drops all
 * of them. */
static void
ob_exec_ctx_xact_cb(XactEvent event, void *arg)
{
	(void) arg;

	switch (event)
	{
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
			ob_exec_ctx_reset();
			break;
		default:
			break;
	}
}

static void
ob_exec_ctx_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
					   SubTransactionId parentSubid, void *arg)
{
	(void) mySubid;
	(void) parentSubid;
	(void) arg;

	switch (event)
	{
		case SUBXACT_EVENT_START_SUB:
			if (ob_subxact_level < OB_SUBXACT_MAX_LEVEL)
			{
				ob_subxact_saved_depth[ob_subxact_level] = ob_exec_ctx_depth;
				ob_guards_save(ob_subxact_saved_guards[ob_subxact_level]);
			}
			ob_subxact_level++;
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			if (ob_subxact_level > 0)
				ob_subxact_level--;
			break;
		case SUBXACT_EVENT_ABORT_SUB:
			if (ob_subxact_level > 0)
			{
				ob_subxact_level--;
				if (ob_subxact_level < OB_SUBXACT_MAX_LEVEL)
				{
					ob_exec_ctx_depth = ob_subxact_saved_depth[ob_subxact_level];
					if (ob_exec_ctx_depth > OB_EXEC_CTX_MAX_DEPTH)
						ob_exec_ctx_depth = OB_EXEC_CTX_MAX_DEPTH;
					ob_exec_ctx_overflow = 0;
					CurrentBranchExecContext = (ob_exec_ctx_depth > 0)
						? &ob_exec_ctx_stack[ob_exec_ctx_depth - 1] : NULL;
					/* The error longjmp'd past the matching overlay_*_exit(),
					 * so fold the guard flags back to their pre-subxact state. */
					ob_guards_restore(ob_subxact_saved_guards[ob_subxact_level]);
				}
			}
			break;
		default:
			break;
	}
}

BranchExecContext *
ob_get_exec_context(void)
{
	return CurrentBranchExecContext;
}

/* T8 § B.9.1 Debug GUCs — § B.11 documented.  Default values match L3
 * expected baseline.  Underlying storage: these two are extern-accessible
 * from branch_lifecycle.c for actual enforcement. */
int			ob_debug_apply_sleep_sec = 0;			/* PGC_SUSET: pg_sleep(N)
													 * between CAS-winner
													 * state='applying' and
													 * MAIN merge; 0 = OFF */
int			ob_debug_invalidation_counter_throttle_override = -1;
										/* PGC_USERSET: >= 0 overrides
										 * DEFAULT_SPI_INVALIDATION_THROTTLE
										 * constant for L1 state_inval
										 * permutation; -1 = use default */
int			ob_debug_snapshot_ttl_seconds = -1;
										/* PGC_SUSET: >= 0 overrides the R19
										 * snapshot hold cap (in seconds) so a
										 * regression test can reach the TTL
										 * kickout without waiting a whole
										 * minute; -1 = use
										 * snapshot_max_hold_minutes */
bool		ob_debug_delta_count_error = false;
										/* PGC_SUSET: when on, the internal
										 * delta-count SPI statement raises
										 * right after SPI_connect() so a
										 * regression test can pin the
										 * SPI_finish-on-error cleanup */

/* --- Layer 1: apply_operation bypass --- */
bool
overlay_in_apply_operation(void)
{
	return ob_in_apply_operation;
}

/* --- Layer 2: shared 2-pass helper bypass --- */
bool
overlay_in_overlay_helper(void)
{
	return ob_in_overlay_helper;
}

void
overlay_overlay_helper_enter(void)
{
	ob_in_overlay_helper = true;
}

void
overlay_overlay_helper_exit(void)
{
	ob_in_overlay_helper = false;
}

/* --- Layer 3: write-redirection bypass --- */
bool
overlay_in_write_redirect(void)
{
	return ob_in_write_redirect;
}

void
overlay_write_redirect_enter(void)
{
	ob_in_write_redirect = true;
}

void
overlay_write_redirect_exit(void)
{
	ob_in_write_redirect = false;
}

/* ================================================================
 * Saved hook values (chaining)
 * ================================================================ */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ================================================================
 * Forward declarations
 * ================================================================ */
void		_PG_init(void);

static bool overlay_guc_check_assign_current_branch(char **newval,
												   void **extra,
												   GucSource source);
static void overlay_guc_assign_current_branch(const char *newval,
											  void *extra);
static void overlay_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void overlay_ExecutorRun(QueryDesc *queryDesc,
					ScanDirection direction,
					uint64 count, bool execute_once);
static void overlay_ExecutorFinish(QueryDesc *queryDesc);
static void overlay_ExecutorEnd(QueryDesc *queryDesc);
static void overlay_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								   bool readOnlyTree, ProcessUtilityContext context,
								   ParamListInfo params, QueryEnvironment *queryEnv,
								   DestReceiver *dest, QueryCompletion *qc);

/* Step7 HARD GUARD forward declarations */
static bool ob_relid_is_user_table(Oid relid, Relation *outrel, char **relname_out);
static const char *ob_utility_opname(NodeTag tag);

/* ================================================================
 * SQL-callable function declarations
 * ================================================================ */
PG_FUNCTION_INFO_V1(overlay_branch_create);
PG_FUNCTION_INFO_V1(overlay_branch_use);
PG_FUNCTION_INFO_V1(overlay_branch_use_with_mode);
PG_FUNCTION_INFO_V1(overlay_branch_is_active_by_name);
PG_FUNCTION_INFO_V1(overlay_branch_force_invalidation_check);
PG_FUNCTION_INFO_V1(overlay_branch_cached_mode);

PG_FUNCTION_INFO_V1(overlay_branch_current);
PG_FUNCTION_INFO_V1(overlay_branch_apply);
PG_FUNCTION_INFO_V1(overlay_branch_discard);
PG_FUNCTION_INFO_V1(overlay_branch_list);
PG_FUNCTION_INFO_V1(overlay_main_plus_delta);
/* debug_delta_* live in delta_store.c; NOT redeclared here to avoid
 * duplicate-PG_FUNCTION_INFO_V1 symbol collisions. */

/* ================================================================
 * Module load callback
 * ================================================================ */
void
_PG_init(void)
{
	MemoryContext oldctx;

	if (CurrentBranchContext == NULL)
	{
		oldctx = MemoryContextSwitchTo(TopMemoryContext);

		CurrentBranchContext = (BranchContext *) palloc0(sizeof(BranchContext));
		CurrentBranchContext->branch_id = 0;
		CurrentBranchContext->branch_name[0] = '\0';
		CurrentBranchContext->is_active = false;
		strcpy(CurrentBranchContext->mode, BRANCH_MODE_LIVE);
		CurrentBranchContext->created_at = 0;
		CurrentBranchContext->branch_main_snapshot = NULL;
		CurrentBranchContext->snapshot_registered = false;
		CurrentBranchContext->invalidation_counter = 0;
		CurrentBranchContext->invalidation_last_check = 0;

		MemoryContextSwitchTo(oldctx);
	}

	/* GUC: overlay_branch.current (triggers USE via assign_hook —
	 *
	 * IMPORTANT PG PATTERN: check hooks MUST be side-effect free.  PG can
	 * and will re-invoke check hooks for pre-flight validation,
	 * multi-pass SET LOCAL savepoint restoration, subxact abort GUC
	 * rollback, etc.  All side-effects (use/exit branch) MUST live in
	 * the assign hook, which is called exactly once per real GUC
	 * assignment (once savepoint rollbacks have been negotiated).  If
	 * we put side-effects in the check hook, savepoint rollback after
	 * an ERROR inside a PL/pgSQL EXCEPTION block double-invokes
	 * ob_exit_branch_cleanup → TopMemoryContext double-pfree → PANIC. */
	DefineCustomStringVariable("overlay_branch.current",
							   "Set the current active branch for this session.",
							   "Set to the branch name, or empty/NULL to leave the branch.",
							   &overlay_branch_current_name,
							   "",
							   PGC_USERSET,
							   0,
							   overlay_guc_check_assign_current_branch,
							   overlay_guc_assign_current_branch,
							   NULL);

	DefineCustomBoolVariable("overlay_branch.enabled",
							 "Enable or disable overlay branch processing.",
							 NULL,
							 &overlay_branch_enabled,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/* Experimental: allow branch-mode DML on tables WITHOUT a PRIMARY
	 * KEY by falling back to a best-effort physical (ctid, xmin) row
	 * identity.  Off by default: a no-PK table is rejected by the G5
	 * guard exactly as before.  See doc/internals.md §4. */
	DefineCustomBoolVariable("overlay_branch.keyless_tables",
							 "Allow branch-mode DML on tables without a PRIMARY KEY using an experimental ctid-based row identity.",
							 "Off (default): a table without a PRIMARY KEY is rejected (0A000).  On: a one-shot WARNING is emitted and rows are tracked by physical ctid (unsafe under concurrent MAIN writes; conflict detected only at apply time).",
							 &overlay_keyless_tables,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/* -------- multi-session GUCs (FR2/FR4/FR7 tunables) -------- */
	DefineCustomIntVariable("overlay_branch.invalidation_check_threshold",
							"Run a SPI state-recheck after N calls to overlay_branch_is_active (FR4 fast-path override of NOTIFY push).",
							"Set to 0 to disable call-count throttling and require time-based or NOTIFY-based invalidation only.",
							&ob_invalidation_check_threshold,
							32,
							0,
							INT_MAX / 2,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("overlay_branch.invalidation_check_interval_ms",
							"Minimum wall-clock interval between SPI state re-checks, in milliseconds (FR4 fallback).",
							"Set to 0 to disable time-based throttling and rely on NOTIFY + call-count triggers.",
							&ob_invalidation_check_interval_ms,
							100,
							0,
							INT_MAX / 2,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("overlay_branch.apply_strict_pins",
							 "apply_branch() fails with ERROR if other sessions still hold use_branch() pins on the branch.",
							 "Strict mode (on) guarantees zero post-apply data drift at the cost of requiring all users to exit the branch first; relaxed mode warns and proceeds.",
							 &ob_apply_strict_pins,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/* V6 / phase 3B: set-based apply.  On (default) replays each relation's
	 * deltas with one SQL statement per operation instead of one per row;
	 * Off restores the per-row 3-pass replay.  Turn off to A/B compare or
	 * to fall back if a batch bug is suspected. */
	DefineCustomBoolVariable("overlay_branch.apply_batch",
							 "Replay apply_branch() deltas with set-based batch SQL (one statement per relation and operation).",
							 "On (default): one validation SELECT plus one batch DML per (relation, operation).  Off: legacy per-row replay.  Relations without a PRIMARY KEY, or whose delta rows carry a base image, always use the per-row replay.",
							 &ob_apply_batch,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("overlay_branch.use_shared_mem_pin_table",
							 "Enable optional shared-memory pin-count table to speed up apply_branch pinning decisions (FR7).",
							 "Requires loading overlay_branch via shared_preload_libraries. When off, falls back to pure SPI-based state checks.",
							 &ob_use_shared_mem_pin_table,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/* T8 § B.11: Debug GUCs — NOT ABI stable, prefixed with _debug_.
	 * Default values match L3 expected baseline. */
	DefineCustomIntVariable("overlay_branch._debug_apply_sleep_sec",
							"(DEBUG) Number of seconds to pg_sleep between apply_branch CAS-winner state='applying' and MAIN row merge.",
							"Set >0 to widen the applying-state window for L1 ob_applying_freeze.spec.  Production default 0 (off).",
							&ob_debug_apply_sleep_sec,
							0,
							0,
							3600,
							PGC_SUSET,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("overlay_branch._debug_invalidation_counter_throttle_override",
							"(DEBUG) Override FR4 DEFAULT_SPI_INVALIDATION_THROTTLE call-count constant.",
							"Set >= 0 to force a custom throttle value; -1 (default) means use the compiled-in constant (32).  Used by L1 ob_state_inval.spec to shorten permutations.",
							&ob_debug_invalidation_counter_throttle_override,
							-1,
							-1,
							INT_MAX / 2,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	/* -------- REVIEW-260926 / R19 snapshot lifetime hard cap -------- */
	DefineCustomIntVariable("overlay_branch.snapshot_max_hold_minutes",
							"Maximum wall-clock minutes a session may remain in snapshot mode (R19 RESTRICTED guard).",
							"Snapshots pin RecentGlobalXmin and delay VACUUM across the whole cluster.  0 = unlimited (use only in single-user test environments).  Default 10.",
							&ob_snapshot_max_hold_minutes,
							10,
							0,
							365 * 24 * 60,        /* ~1 year = upper bound */
							PGC_POSTMASTER,
							GUC_UNIT_MIN,
							NULL,
							NULL,
							NULL);

	/* -------- B1 delta-size observability (§5.6) -------- */
	DefineCustomIntVariable("overlay_branch.delta_size_warn_rows",
							"Emit a WARNING when the current branch's delta exceeds this many rows.",
							"0 (default) disables the check entirely.  When > 0 the write path counts the delta rows of the session's branch once every overlay_branch.delta_size_check_interval writes and warns once per session on first overflow.",
							&ob_delta_size_warn_rows,
							0,
							0,
							INT_MAX / 2,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("overlay_branch.delta_size_check_interval",
							"Number of delta writes between delta-size checks (B1).",
							"Amortizes the delta-size count over this many writes.  Only consulted when overlay_branch.delta_size_warn_rows > 0.",
							&ob_delta_size_check_interval,
							1024,
							1,
							INT_MAX / 2,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("overlay_branch._debug_snapshot_ttl_seconds",
							"(DEBUG) Override the R19 snapshot hold cap with a seconds granularity so tests reach the TTL kickout without a whole minute of waiting.",
							"Set >= 0 to enforce a per-session snapshot hold cap of N seconds; -1 (default) honours overlay_branch.snapshot_max_hold_minutes.  NOT ABI stable.",
							&ob_debug_snapshot_ttl_seconds,
							-1,
							-1,
							365 * 24 * 60 * 60,
							PGC_SUSET,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("overlay_branch._debug_delta_count_error",
							 "(DEBUG) Force the internal branch-delta row count to raise right after SPI_connect().",
							 "Set on to drive overlay_delta_count()'s SPI_finish-on-error cleanup through a deterministic failure.  The error is swallowed by the delta-size fail-open path.  NOT ABI stable.",
							 &ob_debug_delta_count_error,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("overlay_branch");

	/* Install executor + utility hooks */
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = overlay_ExecutorStart;

	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = overlay_ExecutorRun;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = overlay_ExecutorFinish;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = overlay_ExecutorEnd;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = overlay_ProcessUtility;

	/* A1/A6: keep the per-statement branch-identity stack consistent across
	 * error aborts (ExecutorEnd is skipped on abort). */
	RegisterXactCallback(ob_exec_ctx_xact_cb, NULL);
	RegisterSubXactCallback(ob_exec_ctx_subxact_cb, NULL);

	elog(DEBUG1, "overlay_branch: module loaded");

	branch_scan_init();
}

/* ================================================================
 * GUC hooks for overlay_branch.current
 *
 *   check hook  = SIDE-EFFECT FREE validation only.  PG can (and will)
 *                 re-invoke this for savepoint-rollback GUC restore,
 *                 multi-pass SET LOCAL pre-flight, etc.  Any side-effect
 *                 here → double cleanup → TopMemoryContext double-pfree
 *                 → PANIC (T5 pitfall #3).
 *
 *                 MUST NOT touch ob_in_guc_setconfig here: the flag is
 *                 OWNED by the assign hook.  SetConfigOption calls the
 *                 hooks in order: (1) check — possibly repeated — then
 *                 (2) assign — exactly once per real assignment.  If we
 *                 reset the recursion flag in check, the assign hook
 *                 sees it as FALSE and re-runs the side-effect — a
 *                 second cleanup or a second use_branch — leading to
 *                 the exact double-pfree we saw in T5 Section N entry
 *                 after a PL/pgSQL EXCEPTION block.
 *
 *   assign hook = REAL side-effects: enter/leave branch.  Called exactly
 *                 once per true GUC assignment (post-negotiation).
 *                 THIS is where we inspect/flip ob_in_guc_setconfig to
 *                 break the SetConfigOption recursion that use_branch()
 *                 creates when it wants to persist the GUC.
 * ================================================================ */
static bool
overlay_guc_check_assign_current_branch(char **newval, void **extra,
										GucSource source)
{
	const char *name;

	/* ---- PG GUC CONTRACT -------------------------------------------------
	 * Check hook = SIDE-EFFECT-FREE VALIDATION ONLY (PG may call this
	 * MULTIPLE times for a single SET: preflight, savepoint rollback,
	 * post-check pre-assign revalidation).  We may use READ-ONLY SPI to
	 * query ground-truth catalog state, but we MUST NOT flip any flags,
	 * write any catalogs, take any locks, or touch CurrentBranchContext.
	 * Return false → PG aborts the SET (no GUC write, PG emits the
	 * standard "invalid value for parameter" error).  Return true →
	 * proceed to assign hook (which performs real side-effects and is
	 * GUARANTEED to succeed because validation is already complete).
	 * ------------------------------------------------------------------ */

	/* NULL / empty = "leave branch" → always valid (no SPI needed) */
	if (newval == NULL || *newval == NULL || **newval == '\0')
		return true;

	name = *newval;

	/* Hard length bound (must fit BranchContext.name[NAMEDATALEN]) */
	if (strlen(name) >= NAMEDATALEN)
		return false;

	/* ------------------------------------------------------------------
	 * PARALLEL WORKER GUARD (MF-3): ParallelWorkerMain restores the
	 * leader's GUC state via RestoreGUCState() → set_config_option(),
	 * which fires this check hook.  At that point the worker has neither
	 * an active snapshot nor a portal, so the read-only SPI validation
	 * below would fail with "cannot execute SQL without an outer snapshot
	 * or portal" and abort the worker.  The leader already validated the
	 * value (this very hook ran there), so we simply accept it: the
	 * worker never executes overlay scans anyway. */
	if (IsParallelWorker())
		return true;

	/* ------------------------------------------------------------------
	 * EARLY-INIT GUARD: During _PG_init / post-startup GUC sweep, no
	 * database has been connected yet (MyDatabaseId is InvalidOid) and
	 * SPI_connect will fail (no active snapshot / transaction).  In
	 * that phase we simply accept any syntactically-valid name; the
	 * assign hook will still validate via use_internal() if the name
	 * is actually written, and first user SET during normal backend
	 * operation will re-run this full SPI check. */
	if (MyDatabaseId == InvalidOid)
		return true;

	/* ------------------------------------------------------------------
	 * READ-ONLY SPI VALIDATION — branch EXISTS + state = ACTIVE.
	 * Uses the current LatestSnapshot (same visibility as user SELECTs
	 * on the catalog).  Errors from SPI (e.g. table missing because
	 * CREATE EXTENSION not yet run in this database → fail-closed
	 * reject the value.  Extension not loaded? user can still call
	 * use_branch() SQL-callable directly; SET-based path is a luxury.
	 *
	 * Before returning false (PG will raise the generic "invalid value
	 * for parameter" error), we first emit a CUSTOM ereport(ERROR) with
	 * human-readable text matching the public use_branch() API.  PG's
	 * check-hook contract allows ereport + return false as a pair: the
	 * ERROR is delivered to the user exactly as-is, and the GUC value
	 * remains unchanged. */
	{
		bool		is_valid = false;
		bool		found = false;
		bool		state_active = false;
		int			spi_rc;
		StringInfoData sql;
		char	   *esc_name;

		if (SPI_connect() != SPI_OK_CONNECT)
			return false;

		initStringInfo(&sql);
		esc_name = quote_literal_cstr(name);
		appendStringInfo(&sql,
						 "SELECT state FROM " OBTABLE_BRANCH
						 " WHERE branch_name = %s LIMIT 1",
						 esc_name);

		spi_rc = SPI_execute(sql.data, true, 1);
		pfree(sql.data);

		if (spi_rc == SPI_OK_SELECT && SPI_processed == 1)
		{
			Datum		state_datum;
			bool		isnull;
			char	   *state_text;

			found = true;
			state_datum = SPI_getbinval(SPI_tuptable->vals[0],
										SPI_tuptable->tupdesc,
										1, &isnull);
			if (!isnull)
			{
				state_text = TextDatumGetCString(state_datum);
				if (state_text != NULL &&
					strcmp(state_text, BRANCH_STATE_ACTIVE) == 0)
				{
					is_valid = true;
					state_active = true;
				}
				else
				{
					state_active = false;
				}
				pfree(state_text);
			}
		}

		SPI_finish();

		if (!is_valid)
		{
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("branch \"%s\" does not exist", name)));
			else if (!state_active)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("branch \"%s\" is not active", name)));
			/* (fall-through) return false below in case ereport path
			 * was compiled out; normally ereport(ERROR) above does not
			 * return. */
		}
		return is_valid;
	}
}

static void
overlay_guc_assign_current_branch(const char *newval, void *extra)
{
	/* ---- Recursion guard (caller-owned flag) ---------------------------
	 *
	 * External callers of SetConfigOption (use_with_mode_internal /
	 * throttle cleanup helper / etc.) lift ob_in_guc_setconfig to TRUE
	 * *before* calling SetConfigOption and restore the saved pre-call
	 * value (typically FALSE) immediately after SetConfigOption returns.
	 *
	 * WE NEVER TOUCH THIS FLAG HERE.  If we cleared it inside the hook,
	 * a single SetConfigOption that internally invokes the assign hook
	 * TWO or more times (PG GUC machinery does internal two-pass updates
	 * for some value classes; savepoint rollback also re-checks) would
	 * see flag=FALSE on the SECOND pass and enter the real side-effect
	 * code — double-cleanup / double-pfree / state corruption. */
	if (ob_in_guc_setconfig)
		return;

	/* ------------------------------------------------------------------
	 * PARALLEL WORKER GUARD (MF-3): the assign hook is fired by
	 * RestoreGUCState() in a fresh parallel worker, where SPI (and hence
	 * use_internal()/ob_exit_branch_cleanup()) cannot run — there is no
	 * snapshot or portal yet.  PG has already written the variable; we
	 * only skip the side effects.  Workers never run overlay scans, so
	 * this is safe and keeps the worker from aborting. */
	if (IsParallelWorker())
		return;

	/* ---- PG GUC CONTRACT ------------------------------------------------
	 * Assign hook = REAL SIDE-EFFECTS ONLY, MUST NOT FAIL.
	 *
	 * The check hook overlay_guc_check_assign_current_branch() has
	 * already performed FULL validation:
	 *   • Name length < NAMEDATALEN
	 *   • (if non-empty) branch EXISTS in pg_branch AND state='active'
	 *
	 * If check returned true, this assign hook runs and the below
	 * operations are GUARANTEED error-free.  The only non-standard
	 * case is the PL/pgSQL abort-restore path (T5 PANIC fix), handled
	 * by LAYER-1 below.  No PG_TRY needed — error paths simply cannot
	 * happen given check-hook prevalidation. */

	/* Empty/NULL → leave any active branch.  Check hook has already
	 * validated empty strings (always valid). */
	if (newval == NULL || *newval == '\0')
	{
		if (CurrentBranchContext != NULL && CurrentBranchContext->is_active)
		{
			/* I8.2: persist cached mode BEFORE cleanup so GUC RESTORE +
			 * re-entry on the same bid re-enters the same mode the user
			 * had selected (snapshot survives leave→re-enter cycles). */
			ob_mode_cache_set(CurrentBranchContext->branch_id,
							  CurrentBranchContext->mode);
			ob_exit_branch_cleanup(CurrentBranchContext);
		}
		return;
	}

	/* Non-empty → use branch. */

	/* FIX-B: Zombie empty-ctx pass-through.
	 *
	 * When GUC "" reset runs on a session where CurrentBranchContext was
	 * never fully populated (bid=0 / name='') but is_active is still TRUE
	 * (leftover baseline init state), the next non-empty SET would enter
	 * use_internal() with a stale zombie context.  Guard above at L552
	 * handles empty newval (exit_branch_cleanup path), but that path only
	 * fires when is_active && (bid>0 || name[] nonempty).  Here we catch
	 * the bid=0 / name[0]='' / is_active=true zombie shell and let the
	 * GUC proceed without ERROR. */
	if (CurrentBranchContext != NULL &&
		CurrentBranchContext->is_active &&
		CurrentBranchContext->branch_id == 0 &&
		CurrentBranchContext->branch_name[0] == '\0')
		return;

	/* LAYER-1 (transaction-end abort-restore guard):
	 *   On a PL/pgSQL subxact-abort GUC RESTORE, or any abort/commit GUC
	 *   replay, CurrentBranchContext may already be inactive because
	 *   throttle / discard / apply kicked us off the branch and ran
	 *   exit_cleanup.  The check hook ran BEFORE the abort started and
	 *   said the name is valid (exists+active at that time), but the
	 *   catalog state has moved on since — we MUST NOT call use_internal
	 *   (would call SPI inside an abort unwind → PANIC).  The GUC value
	 *   has already been written by PG; we simply skip the enter side-
	 *   effect.
	 *
	 *   Gated on !IsTransactionState(): with a live transaction this is a
	 *   plain user `SET overlay_branch.current = '<branch>'`, which MUST
	 *   enter the branch (see LAYER-1b below) — swallowing it here left the
	 *   GUC claiming a branch the session was not actually on. */
	if (!IsTransactionState() &&
		CurrentBranchContext != NULL && !CurrentBranchContext->is_active)
		return;

	/* LAYER-1b (transaction-end GUC RESTORE guard): see
	 * doc/internals.md §3.1 / §6.1.
	 *
	 * AtEOXact_GUC()/AtEOSubXact_GUC() restore a transaction's GUC changes
	 * when the transaction (or subtransaction) ends, on COMMIT and ABORT
	 * alike.  PG writes the restored value into overlay_branch_current_name
	 * and THEN calls this hook, while the surrounding transaction state is
	 * TRANS_COMMIT or TRANS_ABORT, so IsTransactionState() is FALSE.  Any
	 * SPI / relation access here (the mode-preserve lookup below and
	 * use_internal()) reaches RelationIdGetRelation(), whose
	 * Assert(IsTransactionState()) fires and SIGABRTs the backend.  We
	 * therefore cannot (re)enter a branch here.
	 *
	 * The empty-value path above is memory-only and still correct.  For a
	 * non-empty restore: if the session already sits on exactly this branch
	 * there is nothing to do; otherwise the context points at a different
	 * branch (a use_branch()/SET that ran inside the ended transaction), or
	 * has already been cleared by LAYER-1 above, so we drop to the safe "no
	 * branch" state rather than silently keep the wrong branch — the GUC
	 * value itself was already written by PG and cannot be undone from an
	 * assign hook. */
	if (!IsTransactionState())
	{
		if (CurrentBranchContext != NULL &&
			CurrentBranchContext->is_active &&
			strcmp(CurrentBranchContext->branch_name, newval) != 0)
		{
			ereport(WARNING,
					(errcode(ERRCODE_WARNING),
					 errmsg("overlay_branch: branch \"%s\" was not restored after a "
							"transaction rollback", newval),
					 errdetail("The session was on branch \"%s\" inside the aborted "
							   "(sub)transaction; the rollback could not re-enter a "
							   "branch, so the session is now outside any branch.",
							   CurrentBranchContext->branch_name),
					 errhint("Re-enter with SELECT overlay_branch.use_branch('%s').",
							 newval)));
			ob_exit_branch_cleanup(CurrentBranchContext);
		}
		return;
	}

	/* I8.2 mode-preserve: GUC 1-arg SET (no mode param) — reuse cached mode.
	 * Without this, assign hook always calls use_internal() → mode='live'
	 * unconditionally → PART D/E snapshot modes always regress to 'live' in
	 * list_branches.  With cache hit, re-enter prior mode exactly (matches
	 * prior catalog-WRITE semantics but w/o RowExclusiveLock per use). */
	{
		int32 bid_from_name = 0;
		char   *esc;
		StringInfoData sq;
		const char *mode_for_hook = NULL;

		/* Allocate in outer Caller context BEFORE SPI_connect to avoid
		 * double-free: SPI_finish() frees SPI_proc context, so any
		 * allocations made while SPI_proc is CurrentMemoryContext would
		 * already be released before we pfree them below. */
		esc = quote_literal_cstr(newval);
		initStringInfo(&sq);
		appendStringInfo(&sq,
			"SELECT branch_id FROM overlay_branch.pg_branch "
			"WHERE branch_name = %s LIMIT 1", esc);

		if (SPI_connect() == SPI_OK_CONNECT)
		{
			if (SPI_execute(sq.data, true, 1) == SPI_OK_SELECT &&
				SPI_processed == 1)
			{
				bool isnull = false;
				Datum d = SPI_getbinval(SPI_tuptable->vals[0],
									  SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
					bid_from_name = DatumGetInt32(d);
			}
			SPI_finish();
		}
		pfree(esc);
		pfree(sq.data);
		if (bid_from_name > 0)
			mode_for_hook = ob_mode_cache_lookup(bid_from_name);
		if (mode_for_hook != NULL)
		{
			overlay_branch_use_with_mode_internal(newval, mode_for_hook);
			return;
		}
	}

	/* Check hook says branch EXISTS + state='active' at the latest
	 * snapshot.  This call will not raise any ERROR. */
	overlay_branch_use_internal(newval);
}

/* ================================================================
 * ======= Step7: HARD GUARD ========
 * ================================================================ */

static const char *ob_guard_hint_tmpl =
	"Switch back to the MAIN database before running this statement:\n"
	"    SELECT overlay_branch.discard_branch('%s');\n"
	"  or\n"
	"    RESET overlay_branch.current;";

void
overlay_guard_ereport_fail(const char *operation, const char *relname,
						   const char *branch_name, const char *reason)
{
	const char *cur = overlay_branch_get_current_name();
	bool		in_branch_mode = (cur != NULL && *cur != '\0');
	const char *brname;
	char		hintbuf[1024];

	/* apply/discard run OUTSIDE branch mode (they are the very statements
	 * that leave it), so the GUC-based current name is empty there.  Those
	 * callers pass the target branch explicitly, otherwise a conflict error
	 * would name "<unknown>" instead of the branch it belongs to. */
	brname = (branch_name != NULL && *branch_name != '\0') ? branch_name :
		(in_branch_mode ? cur : "<unknown>");

	snprintf(hintbuf, sizeof(hintbuf), ob_guard_hint_tmpl,
			 in_branch_mode ? cur : "");
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("overlay_branch: cannot %s%s inside active branch \"%s\": %s",
					operation,
					relname ? psprintf(" relation \"%s\"", relname) : "",
					brname,
					reason ? reason : "operation is not supported in branch mode"),
			 relname ? errdetail("Target relation: %s.", relname) : 0,
			 /* The "switch back to MAIN" hint only makes sense when the
			  * failing statement actually ran INSIDE branch mode. */
			 in_branch_mode ? errhint("%s", hintbuf) : 0));
}

bool
overlay_guard_rel_ok(Relation rel, char **reason)
{
	Oid			nspoid;
	char	   *nspname;

	if (reason) *reason = NULL;
	if (rel == NULL) return true;
	if (!overlay_branch_is_active()) return true;

	if (rel->rd_rel->relisshared)
		return true;
	nspoid = RelationGetNamespace(rel);
	nspname = get_namespace_name(nspoid);
	if (nspname == NULL) return true;
	if (strcmp(nspname, "pg_catalog") == 0 ||
		strcmp(nspname, "information_schema") == 0 ||
		strncmp(nspname, "pg_toast", 8) == 0 ||
		strcmp(nspname, OBSCHEMA) == 0)
		return true;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION: break;
		case RELKIND_VIEW:
			if (reason) *reason = psprintf("views are not supported (branch mode only accepts plain heap tables with a PRIMARY KEY)");
			return false;
		case RELKIND_MATVIEW:
			if (reason) *reason = psprintf("materialized views are not supported");
			return false;
		case RELKIND_FOREIGN_TABLE:
			if (reason) *reason = psprintf("foreign tables are not supported");
			return false;
		case RELKIND_PARTITIONED_TABLE:
			if (reason) *reason = psprintf("partitioned root tables are not supported");
			return false;
		default:
			if (reason) *reason = psprintf("relkind='%c' is not supported in branch mode", rel->rd_rel->relkind);
			return false;
	}

	if (rel->rd_rel->relispartition)
	{
		if (reason) *reason = psprintf("partition child tables are not supported");
		return false;
	}

	if (rel->trigdesc != NULL && rel->trigdesc->numtriggers > 0)
	{
		int			i;
		for (i = 0; i < rel->trigdesc->numtriggers; i++)
		{
			if (!rel->trigdesc->triggers[i].tgisinternal)
			{
				if (reason) *reason = psprintf("user-defined triggers are not supported (found trigger \"%s\")",
											   rel->trigdesc->triggers[i].tgname);
				return false;
			}
		}
	}

	if (OidIsValid(RelationGetRelid(rel)))
	{
		int			nfk = 0, nref = 0;
		Oid			relid = RelationGetRelid(rel);
		bool		isnull;
		Datum		v;
		char	   *sql;
		int			ret;

		sql = psprintf("SELECT "
					   "(SELECT count(*) FROM pg_catalog.pg_constraint WHERE conrelid = %u AND contype='f'),"
					   "(SELECT count(*) FROM pg_catalog.pg_constraint WHERE confrelid = %u AND contype='f')",
					   relid, relid);
		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			elog(ERROR, "overlay_guard_rel_ok: SPI_connect failed %d", ret);
		ret = SPI_execute(sql, true, 1);
		pfree(sql);
		if (ret != SPI_OK_SELECT || SPI_processed != 1)
		{
			SPI_finish();
			elog(ERROR, "overlay_guard_rel_ok: SPI FK counts failed %d rows=%lu",
				 ret, SPI_processed);
		}
		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		nfk = isnull ? 0 : DatumGetInt64(v);
		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
		nref = isnull ? 0 : DatumGetInt64(v);
		SPI_finish();

		if (nfk > 0 || nref > 0)
		{
			if (reason) *reason = psprintf("FOREIGN KEYs are not supported (this table has %d outgoing and is referenced by %d FK constraints)",
										   nfk, nref);
			return false;
		}
	}

	if (!overlay_relation_has_pk(rel) && !overlay_keyless_tables)
	{
		if (reason) *reason = psprintf("table must have a PRIMARY KEY for branch-mode DML (enable overlay_branch.keyless_tables for the experimental ctid fallback)");
		return false;
	}

	return true;
}

void
overlay_guard_ensure_branch_or_main_active(void)
{
	/* no-op: overlay_branch_is_active() already composes the dual-source
	 * truth-check.  Callers use this for explicit semantic documentation
	 * before entering apply/discard code paths that mutate catalog state. */
}

/* ---------- ob_relid_is_user_table: helper for DDL guard ---------- */
static bool
ob_relid_is_user_table(Oid relid, Relation *outrel, char **relname_out)
{
	Relation	rel;
	Oid			nspoid;
	char	   *nspname;
	bool		result = false;

	if (outrel) *outrel = NULL;
	if (relname_out) *relname_out = NULL;
	if (!OidIsValid(relid))
		return false;

	rel = try_relation_open(relid, AccessShareLock);
	if (rel == NULL)
		return false;
	nspoid = RelationGetNamespace(rel);
	nspname = get_namespace_name(nspoid);
	if (nspname == NULL)
	{
		relation_close(rel, AccessShareLock);
		return false;
	}

	if (strcmp(nspname, "pg_catalog") == 0 ||
		strcmp(nspname, "information_schema") == 0 ||
		strncmp(nspname, "pg_toast", 8) == 0 ||
		strcmp(nspname, OBSCHEMA) == 0 ||
		rel->rd_rel->relisshared)
	{
		relation_close(rel, AccessShareLock);
		return false;
	}

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
		case RELKIND_MATVIEW:
		case RELKIND_FOREIGN_TABLE:
		case RELKIND_PARTITIONED_TABLE:
		case RELKIND_SEQUENCE:
			result = true;
			break;
		default:
			result = false;
			break;
	}

	if (result)
	{
		if (relname_out)
			*relname_out = psprintf("%s.%s", nspname,
									RelationGetRelationName(rel));
		if (outrel)
			*outrel = rel;
		else
			relation_close(rel, AccessShareLock);
	}
	else
	{
		relation_close(rel, AccessShareLock);
	}
	return result;
}

static const char *
ob_utility_opname(NodeTag tag)
{
	switch (tag)
	{
		case T_AlterTableStmt:	return "ALTER TABLE";
		case T_CreateStmt:		return "CREATE TABLE";
		case T_IndexStmt:		return "CREATE INDEX";
		case T_ReindexStmt:		return "REINDEX";
		case T_TruncateStmt:	return "TRUNCATE";
		case T_DropStmt:		return "DROP";
		case T_VacuumStmt:		return "VACUUM/ANALYZE";
		case T_ClusterStmt:		return "CLUSTER";
		case T_RenameStmt:		return "RENAME";
		case T_RuleStmt:		return "CREATE RULE";
		case T_CreatePolicyStmt:return "CREATE POLICY";
		case T_AlterPolicyStmt:	return "ALTER POLICY";
		default:				return "utility/DDL";
	}
}

/* ---------- overlay_guard_ddl_ok_for_branch ---------- */
bool
overlay_guard_ddl_ok_for_branch(Node *parsetree, char **operation,
								 char **objname, char **reason)
{
	if (operation) *operation = NULL;
	if (objname) *objname = NULL;
	if (reason) *reason = NULL;
	if (!overlay_branch_is_active()) return true;
	if (parsetree == NULL) return true;

	switch (nodeTag(parsetree))
	{
		case T_AlterTableStmt:
		case T_CreateStmt:
		{
			if (operation) *operation = pstrdup(ob_utility_opname(nodeTag(parsetree)));
			if (reason) *reason = psprintf("%s on MAIN user objects during an active branch would "
										  "modify underlying schema/tables in place and break MAIN/delta "
										  "join semantics after apply.  (§A7 DDL RESTRICTED 0A000 — schema-drift guard)",
										  operation ? *operation : "DDL");
			return false;
		}

		case T_DropStmt:
		{
			DropStmt *d = (DropStmt *) parsetree;
			if (operation) *operation = pstrdup("DROP");
			{
				ListCell *lc;
				foreach(lc, d->objects)
				{
					Node *n = (Node *) lfirst(lc);
					if (IsA(n, List))
					{
						List     *names = (List *) n;
						RangeVar *rv = makeRangeVarFromNameList(names);
						Oid       rid;
						char     *rn = NULL;
						rid = RangeVarGetRelid(rv, AccessShareLock, true);
						if (OidIsValid(rid) && ob_relid_is_user_table(rid, NULL, &rn))
						{
							if (objname) *objname = rn;
							if (reason) *reason = psprintf("DROP of MAIN user objects during an active branch "
														  "breaks MAIN/delta relid-based joins on apply; registry "
														  "fingerprint for this relid will no longer match on next "
														  "enter (§A7 DDL RESTRICTED 0A000 — schema-drift guard)");
							return false;
						}
						switch (d->removeType)
						{
							case OBJECT_TABLE: case OBJECT_VIEW: case OBJECT_MATVIEW:
							case OBJECT_SEQUENCE: case OBJECT_FOREIGN_TABLE: case OBJECT_COLUMN:
							case OBJECT_POLICY: case OBJECT_TRIGGER: case OBJECT_RULE:
							case OBJECT_TABCONSTRAINT: case OBJECT_DOMCONSTRAINT:
							case OBJECT_INDEX:
								if (reason && !*reason) *reason = psprintf("DROP of this class during an active "
																			"branch affects MAIN's catalog (pg_class / "
																			"pg_attribute / pg_index etc.) and will "
																			"cause a schema mismatch on apply (§A7 "
																			"DDL RESTRICTED 0A000 — schema-drift guard)");
								return false;
							default:
								break;
						}
					}
				}
			}
			return true;
		}

		case T_TruncateStmt:
		{
			TruncateStmt *t = (TruncateStmt *) parsetree;
			ListCell   *lc;
			if (operation) *operation = pstrdup("TRUNCATE");
			foreach(lc, t->relations)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);
				Oid			rid = RangeVarGetRelid(rv, AccessShareLock, true);
				char	   *rn = NULL;
				if (OidIsValid(rid) && ob_relid_is_user_table(rid, NULL, &rn))
				{
					if (objname) *objname = rn;
					if (reason) *reason = psprintf("TRUNCATE empties the MAIN heap immediately; subsequent "
												  "apply of branch-delta I/U/D against missing MAIN rows will "
												  "either produce wrong counts or loose PK integrity (§A7 "
												  "DDL RESTRICTED 0A000 — schema-drift guard)");
					return false;
				}
			}
			return true;
		}

		case T_IndexStmt:
		{
			IndexStmt  *i = (IndexStmt *) parsetree;
			Oid			rid = RangeVarGetRelid(i->relation, AccessShareLock, true);
			char	   *rn = NULL;
			if (operation) *operation = pstrdup("CREATE INDEX");
			if (OidIsValid(rid) && ob_relid_is_user_table(rid, NULL, &rn))
			{
				if (objname) *objname = rn;
				if (reason) *reason = psprintf("CREATE INDEX writes to MAIN's pg_index/pg_class catalogs "
											  "and modifies MAIN-side plan costs — planner inside branch uses "
											  "live MAIN costs but writes deltas whose apply-time plan might "
											  "choose different indexes — silent wrong-plan corruption risk "
											  "(§A7 DDL RESTRICTED 0A000 — schema-drift guard)");
				return false;
			}
			return true;
		}

		case T_ReindexStmt:
			if (operation) *operation = pstrdup("REINDEX");
			if (reason) *reason = psprintf("REINDEX rebuilds MAIN indexes in place; concurrently held branch "
										  "sessions with pending I/U/D may read stale MAIN tuples during "
										  "the rebuild window (§A7 DDL RESTRICTED 0A000 — schema-drift guard)");
			return false;
		case T_ClusterStmt:
			if (operation) *operation = pstrdup("CLUSTER");
			if (reason) *reason = psprintf("CLUSTER rewrites the MAIN heap in place under ACCESS EXCLUSIVE lock; "
										  "all active-branch sessions holding the same MAIN rows would lose "
										  "visibility continuity (§A7 DDL RESTRICTED 0A000 — schema-drift guard)");
			return false;
		case T_VacuumStmt:
			if (operation) *operation = pstrdup("VACUUM/ANALYZE");
			if (reason) *reason = psprintf("VACUUM/ANALYZE modifies MAIN visibility map and planner statistics "
										  "during an active branch — plancache replans inside the branch will "
										  "pick MAIN-based cost estimates that diverge from apply-time plans, "
										  "causing silent wrong-row reads (§A7 DDL RESTRICTED 0A000 — schema-drift guard)");
			return false;
		case T_RenameStmt:
			if (operation) *operation = pstrdup("RENAME");
			if (reason) *reason = psprintf("RENAME (column/table/constraint) mutates MAIN's pg_class/pg_attribute "
										  "catalogs in place — stored registry fingerprints for this branch "
										  "will fail to match on next enter/apply and raise 55000 (§A7 DDL "
										  "RESTRICTED 0A000 — schema-drift guard)");
			return false;
		case T_RuleStmt:
			if (operation) *operation = pstrdup("CREATE RULE");
			if (reason) *reason = psprintf("CREATE RULE installs ON DO/INSTEAD semantics directly on MAIN — "
										  "branch writes via write_redirect go directly to delta store WITHOUT "
										  "traversing the rule rewriter, causing MAJOR MAIN/delta divergence "
										  "on apply (§A7 DDL RESTRICTED 0A000 — schema-drift guard)");
			return false;
		case T_CreatePolicyStmt:
		case T_AlterPolicyStmt:
			if (operation) *operation = pstrdup("(ALTER) POLICY");
			if (reason) *reason = psprintf("CREATE/ALTER POLICY toggles MAIN-side row-level security permissions "
										  "in place — branch reads from MAIN via BranchScan happen against the "
										  "LIVE MAIN row-security bitmap and will diverge from the snapshot "
										  "captured at create_branch (§A7 DDL RESTRICTED 0A000 — schema-drift guard)");
			return false;
		case T_AlterObjectSchemaStmt:
			if (operation) *operation = pstrdup("SET SCHEMA / ALTER ... SET SCHEMA");
			if (reason) *reason = psprintf("SET SCHEMA moves a MAIN object between namespaces in place — "
										  "stored (schema_name, table_name) fingerprints for this branch will "
										  "fail to match on next enter/apply and raise 55000 (§A7 DDL "
										  "RESTRICTED 0A000 — schema-drift guard)");
			return false;
		case T_CommentStmt:
			/* COMMENT only modifies pg_description — safe for MAIN/delta
			 * join semantics (no columns, no pk, no schema drift). */
			return true;
		case T_AlterOwnerStmt:
			if (operation) *operation = pstrdup("ALTER ... OWNER TO");
			if (reason) *reason = psprintf("ALTER OWNER mutates pg_class.relowner in place on MAIN; writes "
										  "from the branch by original owner during apply may get ACL-rejected "
										  "after apply commits (§A7 DDL RESTRICTED 0A000 — schema-drift guard)");
			return false;

		default:
			return true;
	}
}

/* ================================================================
 * --- Executor + ProcessUtility hooks ---
 * ================================================================ */
static void
overlay_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	/* A1/A6: bind this statement's branch identity BEFORE the executor
	 * initialises plan nodes, so BranchScan BeginCustomScan (which runs
	 * inside standard_ExecutorStart) already sees the bound copy. */
	ob_exec_ctx_push(queryDesc);

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
overlay_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count, bool execute_once)
{
	/* Dispatch: if the intercept handled it (true return → handled +
	 * write-redirect exit already called internally), return
	 * immediately.  Otherwise fall through to chained / standard. */
	if (overlay_executor_run_intercept(queryDesc, direction, count,
									   execute_once, prev_ExecutorRun))
		return;

	if (prev_ExecutorRun)
		prev_ExecutorRun(queryDesc, direction, count, execute_once);
	else
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
}

static void
overlay_ExecutorFinish(QueryDesc *queryDesc)
{
	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

static void
overlay_ExecutorEnd(QueryDesc *queryDesc)
{
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	/* A1/A6: release the statement's bound identity.  ExecutorEnd is
	 * skipped on error abort; the transaction/subtransaction callbacks
	 * clean the stack up in that case. */
	ob_exec_ctx_unwind(queryDesc);
}

static void
overlay_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					   bool readOnlyTree, ProcessUtilityContext context,
					   ParamListInfo params, QueryEnvironment *queryEnv,
					   DestReceiver *dest, QueryCompletion *qc)
{
	char	   *op_name = NULL;
	char	   *obj_name = NULL;
	char	   *fail_reason = NULL;
	NodeTag		tag;

	/* --- FR4 kickout guard: do NOT run the generic
	 * overlay_branch_is_active() → throttled(DQL) check for pure
	 * session/utility commands that never touch user-table rows.
	 * Running it on, say, a SET command (T_VariableSetStmt) would cause
	 * the following pathological race:
	 *
	 *   1. Session S is on branch B (state ACTIVE in catalog).
	 *   2. S manually flips overlay_branch.pg_branch.state = 'applied'
	 *      (catalog-only write; WR protected-schema guard correctly
	 *      skips the DML kickout so the manual flip persists).
	 *   3. S runs SET overlay_branch.invalidation_check_threshold = 1
	 *      so the *next* user-table DML deterministically runs SPI.
	 *   4. Without the whitelist below: SET triggers ProcessUtility
	 *      → overlay_branch_is_active() → throttled(false) with
	 *      counter already >= new threshold=1 → DQL NOTICE kickout
	 *      → ctx->is_active = false (and ctx->counter reset, GUC
	 *      overlay_branch.current cleared).
	 *   5. The *actual* user-table UPDATE (the drift-write the test
	 *      deliberately wanted to ERROR-block) then sees
	 *      ctx->is_active=false → throttled fast-path return false
	 *      (no ERROR because no "on branch"), falls through to MAIN
	 *      heap, silent drift.
	 *
	 * Solution: only overlay_branch_is_active() + DDL-guard the
	 * statement categories that *might* mutate user state.  Every
	 * Tag below is provably session-only / catalog-maintenance only
	 * and is allowed to skip the check.
	 *
	 * The list is intentionally conservative: add to it only when
	 * you can prove the command type never writes MAIN user heap
	 * rows (no silent pollution possible). */
	if (pstmt != NULL && pstmt->utilityStmt != NULL)
	{
		tag = nodeTag(pstmt->utilityStmt);
		switch (tag)
		{
			case T_VariableSetStmt:
			case T_VariableShowStmt:
			case T_ListenStmt:
			case T_NotifyStmt:
			case T_UnlistenStmt:
			case T_TransactionStmt:
			case T_ConstraintsSetStmt:
			case T_DiscardStmt:
			case T_LockStmt:
			case T_CheckPointStmt:
			case T_ExecuteStmt:
			case T_PrepareStmt:
			case T_DeallocateStmt:
			case T_CreateSubscriptionStmt:
			case T_DropSubscriptionStmt:
			case T_AlterSubscriptionStmt:
			case T_CreatePublicationStmt:
			case T_AlterPublicationStmt:
			case T_SecLabelStmt:
				/* Pure session / maintenance utility — no user-data
				 * mutation possible, so skip the is_active() call and
				 * its throttled(DQL) side-effect.  Still run normal
				 * command below. */
				break;
			/* ============================================================
			 * REVIEW-260926 / R05: COPY FROM STDIN/FILE/PROGRAM bypasses
			 *   ExecutorRun entirely (handled by CopyFrom in
			 *   ProcessUtility).  Without this explicit block, a
			 *   drift-write lands directly on the MAIN heap with
			 *   ZERO write-redirect protection.  We intercept here
			 *   because this is the single choke point for ALL COPY
			 *   variants (COPY FROM view with INSTEAD OF trigger is
			 *   rejected separately, but raw heap COPY is the
			 *   hazard).  COPY TO (read-side) is deliberately not
			 *   blocked because a plain SELECT overlay-equivalent
			 *   read is safe; we block only writes. */
			case T_CopyStmt:
				{
					CopyStmt *cstmt = (CopyStmt *) pstmt->utilityStmt;
					if (!cstmt->is_from)
						break;			/* COPY TO = read-only; allow */
					if (overlay_branch_is_active())
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("overlay_branch does not support COPY FROM inside a branch (R05 drift-write blocked)"),
								 errhint("Use INSERT ... VALUES / INSERT ... SELECT instead.  "
										 "COPY FROM bypasses WriteRedirect and would silently write MAIN.")));
					break;
				}
			/* ============================================================
			 * REVIEW-260926 / R06: MERGE also routes through
			 *   ProcessUtility for dispatch and builds a ModifyTable
			 *   plan whose resultRelation we do not consistently
			 *   redirect.  Block at the outermost choke point so
			 *   users get a clear 0A000 instead of MAIN pollution. */
			case T_MergeStmt:
				if (overlay_branch_is_active())
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("overlay_branch does not support MERGE inside a branch (R06 drift-write blocked)"),
							 errhint("Rework MERGE as separate INSERT/UPDATE/DELETE "
									 "wrapped in a transaction; those DML are "
									 "properly redirected to the delta store.")));
				break;
			default:
				if (overlay_branch_is_active() &&
					!overlay_guard_ddl_ok_for_branch(pstmt->utilityStmt,
													  &op_name, &obj_name,
													  &fail_reason))
				{
					overlay_guard_ereport_fail(op_name ? op_name : "execute utility/DDL",
											   obj_name, NULL, fail_reason);
				}
				break;
		}
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/* ================================================================
 * SQL-callable thin-dispatch wrappers (real impl → branch_lifecycle.c)
 * ================================================================ */

Datum
overlay_branch_create(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);
	int32		new_branch_id;

	ob_acl_check_lifecycle(OB_OP_CREATE_BRANCH, NameStr(*branch_name));
	new_branch_id = overlay_branch_create_internal(NameStr(*branch_name));
	PG_RETURN_INT32(new_branch_id);
}

Datum
overlay_branch_use(PG_FUNCTION_ARGS)
{
	const char *branch_name;

	if (PG_ARGISNULL(0))
		branch_name = NULL;
	else
		branch_name = NameStr(*PG_GETARG_NAME(0));

	ob_acl_check_lifecycle(OB_OP_USE_BRANCH, branch_name);
	overlay_branch_use_internal(branch_name);
	PG_RETURN_VOID();
}

Datum
overlay_branch_use_with_mode(PG_FUNCTION_ARGS)
{
	const char *branch_name;
	const char *mode;

	/* REVIEW-260926 / R08: NULL branch_name → EXIT (same as 1-arg).
	 *   PG_ARGISNULL must be checked BEFORE PG_GETARG_NAME because
	 *   PG_GETARG_NAME on NULL datum dereferences to garbage memory
	 *   and would SIGSEGV. */
	if (PG_ARGISNULL(0))
		branch_name = NULL;
	else
		branch_name = NameStr(*PG_GETARG_NAME(0));

	if (PG_ARGISNULL(1))
		mode = BRANCH_MODE_LIVE;
	else
		mode = text_to_cstring(PG_GETARG_TEXT_PP(1));

	ob_acl_check_lifecycle(OB_OP_USE_BRANCH, branch_name);
	overlay_branch_use_with_mode_internal(branch_name, mode);
	PG_RETURN_VOID();
}

/* --------------------------------------------------------------------
 * overlay_branch_cached_mode — SQL-callable helper: return per-bid
 * session-local cached mode, or NULL if not cached (so caller can
 * COALESCE with catalog mode).  Used by list_branches() SQL body to
 * overlay the I8.2 per-bid HTAB onto the pure-catalog SELECT. */
Datum
overlay_branch_cached_mode(PG_FUNCTION_ARGS)
{
	int32 bid = PG_GETARG_INT32(0);
	const char *cached;

	cached = ob_mode_cache_lookup(bid);
	if (cached == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(cached));
}

/* --------------------------------------------------------------------
 * overlay_branch_is_active_by_name — SQL-callable: check catalog state
 * of a named branch, PLUS if the named branch is exactly the current
 * session's in-use branch also run Layer1/FR4 invalidation + kickout.
 *
 * SQL signature: overlay_branch_is_active(branch_name name) RETURNS bool
 * Synonym: public.is_active(name name) RETURNS bool
 * -------------------------------------------------------------------- */
Datum
overlay_branch_is_active_by_name(PG_FUNCTION_ARGS)
{
	Name        branch_name = PG_GETARG_NAME(0);
	const char *bn;
	BranchContext *ctx = CurrentBranchContext;
	bool        result = false;

	if (PG_ARGISNULL(0) || branch_name == NULL)
		PG_RETURN_BOOL(false);
	bn = NameStr(*branch_name);
	if (*bn == '\0')
		PG_RETURN_BOOL(false);

	/* If this is OUR current branch, FIRST run our own inv check so the
	 * call side-effect kicks us out.  is_active → throttled DQL path. */
	if (ctx != NULL && ctx->is_active &&
		ctx->branch_id != 0 &&
		strncmp(ctx->branch_name, bn, NAMEDATALEN) == 0)
	{
		/* is_current = true; (formerly kept for logging — removed to suppress -Wunused-but-set) */
		(void) ob_invalidate_check_throttled(false);
		/* Re-read ctx after possible kick-out */
		if (!CurrentBranchContext || !CurrentBranchContext->is_active)
			PG_RETURN_BOOL(false);
	}

	/* Always hit catalog so user gets ground-truth, not just in-memory.
	 * SPI lifecycle probe (same pattern as ob_invalidate_check_throttled):
	 * we may be reached from inside a nested SPI (e.g. via SQL-language
	 * synonym `public.is_active` whose body is a SELECT that invokes us
	 * through the function executor — outer SQL func may already hold an
	 * SPI connection).  PG does not expose SPI_connected() public API, so
	 * we probe via SPI_connect() return value. */
	{
		int         spi_cr2;
		bool        did_con2 = false;
		StringInfoData sql2;
		int         ret2;
		char       *esc2;

		spi_cr2 = SPI_connect();
		if (spi_cr2 == SPI_OK_CONNECT)
			did_con2 = true;
		else if (spi_cr2 != SPI_ERROR_CONNECT)
			elog(ERROR, "overlay_branch_is_active: SPI_connect ret=%d", spi_cr2);

		esc2 = quote_literal_cstr(bn);
		initStringInfo(&sql2);
		appendStringInfo(&sql2,
					 "SELECT state FROM " OBTABLE_BRANCH " "
					 "WHERE branch_name = %s LIMIT 1",
					 esc2);
		pfree(esc2);

		ret2 = SPI_execute(sql2.data, true, 1);
		pfree(sql2.data);
		if (ret2 != SPI_OK_SELECT)
		{
			if (did_con2)
				SPI_finish();
			elog(ERROR, "overlay_branch_is_active: SPI_execute ret=%d", ret2);
		}
		if (SPI_processed == 1 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
		{
			Datum   v2;
			bool    isnull2;
			char   *st2;

			v2 = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
							 1, &isnull2);
			if (!isnull2)
			{
				st2 = TextDatumGetCString(v2);
				if (st2 != NULL && strcmp(st2, BRANCH_STATE_ACTIVE) == 0)
					result = true;
				pfree(st2);
			}
		}
		if (did_con2)
			SPI_finish();
	}

	PG_RETURN_BOOL(result);
}

/* --------------------------------------------------------------------
 * overlay_branch_force_invalidation_check — SQL-callable debug helper
 * (no params): forces the Layer1/FR4 invalidation SPI recheck to run
 * NOW regardless of count/time throttles.  Returns true iff the session
 * is still on an ACTIVE branch after the check.  Mainly useful for
 * regression tests to exercise the kickout path without waiting N ms.
 *
 * SQL signature: overlay_branch_force_invalidation_check() RETURNS bool
 * Synonym: public.force_invalidation_check() RETURNS bool
 * -------------------------------------------------------------------- */
Datum
overlay_branch_force_invalidation_check(PG_FUNCTION_ARGS)
{
	BranchContext *ctx = CurrentBranchContext;

	if (ctx == NULL || !ctx->is_active)
		PG_RETURN_BOOL(false);

	/* Force both throttles to fire: counter += THRESHOLD, last_check = 0 */
	if (ob_invalidation_check_threshold > 0)
		ctx->invalidation_counter = (uint32) ob_invalidation_check_threshold + 1u;
	else
		ctx->invalidation_counter = 1u << 30;
	ctx->invalidation_last_check = 0;

	/* Run DQL-path check, but force_invalidation_check() is a deliberate
	 * user-visible "run the check NOW" request — unlike the "observer"
	 * callers (planner hook, rel_ok), force_check needs to actually raise
	 * the DQL NOTICE + clear ctx + reset GUC so the user can see it (J3,
	 * E4).  Opt-in to destructive kickout side-effects.  See branch_life
	 * cycle.c ob_throttled_allow_kickout comment. */
	{
		extern bool ob_throttled_allow_kickout;
		bool		saved_allow = ob_throttled_allow_kickout;
		bool		thr_ok;

		ob_throttled_allow_kickout = true;
		thr_ok = ob_invalidate_check_throttled(false);
		ob_throttled_allow_kickout = saved_allow;
		if (!thr_ok)
			PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(CurrentBranchContext != NULL &&
				   CurrentBranchContext->is_active);
}


Datum
overlay_branch_current(PG_FUNCTION_ARGS)
{
	const char *name;
	NameData   *result;

	/* Report the branch the session's writes actually land in — the live
	 * branch context — NOT the raw `overlay_branch.current` GUC string.
	 * The two can diverge: after a cross-branch ROLLBACK the GUC is
	 * restored to its pre-transaction value while the abort path cannot
	 * re-enter a branch (no transaction state), so the context is dropped
	 * to "no branch".  Reading the GUC there would claim a branch the
	 * session is not on, contradicting is_active() and the write path.
	 * See doc/internals.md §3.2 / §6.2. */
	name = overlay_branch_get_current_name();

	if (name == NULL || *name == '\0')
		PG_RETURN_NULL();

	result = (NameData *) palloc0(sizeof(NameData));
	namestrcpy(result, name);
	PG_RETURN_NAME(result);
}

Datum
overlay_branch_apply(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);

	ob_acl_check_lifecycle(OB_OP_APPLY_BRANCH, NameStr(*branch_name));
	overlay_branch_apply_internal(NameStr(*branch_name));
	PG_RETURN_VOID();
}

Datum
overlay_branch_discard(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);

	ob_acl_check_lifecycle(OB_OP_DISCARD_BRANCH, NameStr(*branch_name));
	overlay_branch_discard_internal(NameStr(*branch_name));
	PG_RETURN_VOID();
}

/* ================================================================
 * overlay_branch_list() SRF
 * ================================================================ */
Datum
overlay_branch_list(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int			call_cntr;
	int			max_calls;
	struct ob_list_state
	{
		int			nrows;
		Datum	   *values;
		bool	   *nulls;
	};
	struct ob_list_state *st;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		int			ret, i;
		TypeFuncClass tfc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tfc = get_call_result_type(fcinfo, NULL, &tupdesc);
		if (tfc != TYPEFUNC_COMPOSITE || tupdesc == NULL ||
			tupdesc->natts != 7)
		{
			elog(ERROR, "overlay_branch: list_branches() must be called "
				 "with an explicit 7-column composite return type "
				 "(RETURNS TABLE); got tfc=%d nattrs=%d",
				 (int) tfc,
				 tupdesc ? (int) tupdesc->natts : -1);
		}
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		st = (struct ob_list_state *) palloc0(sizeof(struct ob_list_state));
		funcctx->user_fctx = (void *) st;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "overlay_branch: SPI_connect failed in list_branches");

		/*
		 * SPI_connect() switches CurrentMemoryContext to its private procCxt,
		 * which SPI_finish() unconditionally deletes.  Everything we
		 * materialize below (the st->values/st->nulls arrays and every
		 * datumCopy() of a SPI result) must outlive the SPI connection, so
		 * move back to the SRF's multi-call context, where st itself already
		 * lives.  Without this the arrays dangle after SPI_finish() and the
		 * SRF_PERCALL_SETUP() reads below are a use-after-free.
		 */
		MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ret = SPI_execute("SELECT b.branch_id, b.branch_name, b.owner, "
						  "       b.created_at, b.mode, b.state, "
						  "       COALESCE(d.cnt, 0)::bigint AS delta_count "
						  "FROM " OBTABLE_BRANCH " b "
						  "LEFT JOIN (SELECT branch_id, count(*) AS cnt "
						  "           FROM " OBTABLE_DELTA " "
						  "           GROUP BY branch_id) d "
						  "  ON d.branch_id = b.branch_id "
						  "ORDER BY b.branch_id",
						  true, 0);
		if (ret != SPI_OK_SELECT)
		{
			SPI_finish();
			elog(ERROR, "overlay_branch: list_branches SELECT failed ret=%d", ret);
		}

		st->nrows = SPI_processed;
		if (st->nrows > 0)
		{
			TupleDesc	spi_td = SPI_tuptable->tupdesc;

			if (SPI_tuptable->vals == NULL)
			{
				SPI_finish();
				elog(ERROR, "overlay_branch: list_branches got vals=NULL despite nrows=%d",
					 (int) st->nrows);
			}

			st->values = (Datum *) palloc(sizeof(Datum) * 7 * st->nrows);
			st->nulls = (bool *) palloc(sizeof(bool) * 7 * st->nrows);
			for (i = 0; i < st->nrows; i++)
			{
				HeapTuple	tup = SPI_tuptable->vals[i];
				int			col;
				int32		this_bid = 0;
				bool		bid_isnull = true;

				if (tup == NULL)
				{
					for (col = 0; col < 7; col++)
					{
						st->values[i * 7 + col] = (Datum) 0;
						st->nulls[i * 7 + col] = true;
					}
					continue;
				}

				/* Peek bid (col 0) first for possible mode-cache overlay
				 * (FR2 I8.2: mode no longer written to catalog per-use;
				 * it lives in a session-local bid HTAB to avoid cross-
				 * session RowExclusiveLock contention). */
				{
					Datum bid_datum = SPI_getbinval(tup, spi_td, 1,
													&bid_isnull);
					if (!bid_isnull)
						this_bid = DatumGetInt32(bid_datum);
				}

				for (col = 0; col < 7; col++)
				{
					Datum		val;
					bool		isnull;
					Form_pg_attribute att;

					val = SPI_getbinval(tup, spi_td, col + 1, &isnull);

					/* mode column (5th output = col index 4): overlay the
					 * session-local cached mode for this bid, if any.
					 * Fallback = catalog mode (pure read, no lock). */
					if (col == 4 && !bid_isnull)
					{
						const char *cached_mode =
							ob_mode_cache_lookup(this_bid);
						if (cached_mode != NULL)
						{
							st->nulls[i * 7 + col] = false;
							st->values[i * 7 + col] =
								CStringGetTextDatum(cached_mode);
							continue;
						}
					}

					st->nulls[i * 7 + col] = isnull;
					if (isnull)
					{
						st->values[i * 7 + col] = (Datum) 0;
						continue;
					}
					att = TupleDescAttr(spi_td, col + 1);
					if (att == NULL)
					{
						st->values[i * 7 + col] = (Datum) 0;
						st->nulls[i * 7 + col] = true;
						continue;
					}
					st->values[i * 7 + col] = datumCopy(val,
													  att->attbyval,
													  att->attlen);
				}
			}
		}

		SPI_finish();
		funcctx->max_calls = st->nrows;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	st = (struct ob_list_state *) funcctx->user_fctx;

	if (call_cntr < max_calls)
	{
		Datum		values[7];
		bool		nulls[7];
		HeapTuple	tuple;
		int			col;

		for (col = 0; col < 7; col++)
		{
			values[col] = st->values[call_cntr * 7 + col];
			nulls[col] = st->nulls[call_cntr * 7 + col];
		}
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/* ================================================================
 * overlay_main_plus_delta SRF — manual Main+Delta view
 * ================================================================ */
Datum
overlay_main_plus_delta(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext	oldcxt;
		Oid				relid;
		Relation		rel;
		ReturnSetInfo  *rsinfo;
		int32			branch_id;
		void		   *handle;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (PG_ARGISNULL(0))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("overlay_main_plus_delta: table must not be NULL")));
		relid = PG_GETARG_OID(0);

		rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
			rsinfo->expectedDesc == NULL || rsinfo->expectedDesc->natts == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay_main_plus_delta: must be called with explicit column list"),
					 errhint("Use: FROM overlay_branch.overlay_main_plus_delta('t') AS x(id int, v text)")));

		if (!overlay_branch_is_active())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_main_plus_delta: no active branch"),
					 errhint("Call use_branch('name') first.")));

		/*
		 * The PK check needs the relation, but the streaming handle opens (and
		 * owns) its own copy — probe with a short-lived one.  The name must be
		 * copied out before the close, since RelationGetRelationName() points
		 * into relcache.
		 */
		rel = table_open(relid, AccessShareLock);
		if (!overlay_relation_has_pk(rel) && !overlay_keyless_tables)
		{
			char	   *relname = pstrdup(RelationGetRelationName(rel));

			table_close(rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay_main_plus_delta requires a primary key on %s",
							relname)));
		}
		table_close(rel, AccessShareLock);

		/* A1/A6: use the identity bound for this statement rather than
		 * re-reading the live global. */
		{
			BranchExecContext *ectx = ob_get_exec_context();

			branch_id = (ectx != NULL) ? ectx->bid
									   : overlay_branch_get_current_id();
		}

		overlay_overlay_helper_enter();
		PG_TRY();
		{
			handle = ob_overlay_srf_open(relid, branch_id,
										 funcctx->multi_call_memory_ctx);
		}
		PG_CATCH();
		{
			overlay_overlay_helper_exit();
			PG_RE_THROW();
		}
		PG_END_TRY();
		overlay_overlay_helper_exit();

		funcctx->user_fctx = handle;

		MemoryContextSwitchTo(oldcxt);
	}

	funcctx = SRF_PERCALL_SETUP();
	{
		TupleTableSlot *src_slot = ob_overlay_srf_next(funcctx->user_fctx);

		if (src_slot == NULL)
		{
			ob_overlay_srf_close(funcctx->user_fctx);
			funcctx->user_fctx = NULL;
			SRF_RETURN_DONE(funcctx);
		}
		else
		{
			ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
			TupleDesc		exp_desc = rsinfo->expectedDesc;
			TupleDesc		src_desc = src_slot->tts_tupleDescriptor;
			Datum		   *values;
			bool		   *isnulls;
			int				exp_natts = exp_desc->natts;
			HeapTuple		tuple;
			Datum			result;

			/*
			 * Do NOT switch to funcctx->multi_call_memory_ctx here.  That
			 * context is only deleted when the SRF ends, so allocating the
			 * per-row scratch (values/isnulls and their datumCopy()s) and the
			 * result tuple there would grow the backend's memory linearly
			 * with the number of rows produced.  The executor already runs us
			 * in ecxt_per_tuple_memory and resets it before every call
			 * (execSRF.c, value-per-call loop), and it copies the returned
			 * tuple into the tuplestore before the next reset -- so letting
			 * these transient objects land in CurrentMemoryContext is both
			 * correct and constant-memory.  Only the long-lived handle and
			 * tuple_desc stay in the multi-call context.
			 */
			values = (Datum *) palloc0(sizeof(Datum) * exp_natts);
			isnulls = (bool *) palloc0(sizeof(bool) * exp_natts);

			for (int e = 0; e < exp_natts; e++)
			{
				Form_pg_attribute eatt = TupleDescAttr(exp_desc, e);
				const char	   *ename = NameStr(eatt->attname);
				bool			found = false;

				for (int s = 0; s < src_desc->natts; s++)
				{
					Form_pg_attribute satt = TupleDescAttr(src_desc, s);
					if (satt->attisdropped)
						continue;
					if (strcmp(NameStr(satt->attname), ename) == 0)
					{
						bool	sisnull;
						Datum	sd = slot_getattr(src_slot,
												  (AttrNumber) (s + 1),
												  &sisnull);

						if (!sisnull)
						{
							int16	typlen;
							bool	typbyval;
							get_typlenbyval(satt->atttypid, &typlen, &typbyval);
							sd = datumCopy(sd, typbyval, typlen);
						}
						values[e] = sd;
						isnulls[e] = sisnull;
						found = true;
						break;
					}
				}
				if (!found)
				{
					values[e] = (Datum) 0;
					isnulls[e] = true;
				}
			}

			tuple = heap_form_tuple(exp_desc, values, isnulls);
			result = HeapTupleGetDatum(tuple);

			SRF_RETURN_NEXT(funcctx, result);
		}
	}
}
