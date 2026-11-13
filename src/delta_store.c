/*-------------------------------------------------------------------------
 *
 * delta_store.c
 *	  Overlay Branch extension — Delta Store SPI implementation + row serialization +
 *	  Write-Redirect bridge calling overlay_delta_* (overlay_modify_*)
 *
 *	  Responsibilities:
 *		- overlay_delta_insert/update/lookup/list_for_rel/count/delete_all
 *		  (real SPI UPSERT / SELECT / DELETE against overlay_branch.pg_branch_delta)
 *		- overlay_relation_has_pk(): whether a Relation has a Primary Key
 *		- rel_attno_to_slot_idx(): handle junk attrs of the UPDATE/DELETE subplan slot
 *		- slot_get_ctid_cstr() / fetch_tuple_by_ctid(): physical row → clean slot
 *		- overlay_serialize_pk() / overlay_serialize_tuple(): PK / row → JSON text
 *		- reconstruct_slot_from_delta(): bytea tuple_data → relation-descr slot
 *		- overlay_tuple_version(): ctid + xmin → "blk:off-x<xmin>" token
 *		- overlay_should_redirect(): 6-layer pre-check (write-redirect pre-gate)
 *		- overlay_modify_insert/update/delete(): write-redirect call bridge
 *		- overlay_debug_delta_insert/count/delete_all(): Step2c smoke debug wrappers
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "parser/parse_oper.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* ---------------------------------------------------------------
 * Bypass helpers: all internal SPI calls that must hit the MAIN
 * physical heap (NOT the overlay view) must push/pop
 * overlay_overlay_helper_enter() / exit() so the BranchScan planner
 * hook skips CustomScan injection for that internal query.
 * --------------------------------------------------------------- */
extern void overlay_overlay_helper_enter(void);
extern void overlay_overlay_helper_exit(void);

/* Fully qualified table names (control file pins schema = 'overlay_branch').
 * OBSCHEMA is defined in the common header overlay_branch.h */
#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

/* B1 §5.6: amortized delta-size nudge, called once per delta write. */
static void ob_delta_size_warn_check(int32 branch_id);

/* ================================================================
 * Delta Store (6 real SPI functions)
 * ================================================================ */
void
overlay_delta_insert(int32 branch_id, Oid relid, const char *key,
					 char op, const char *old_version, const char *tuple_json_cstr)
{
	int			ret;
	int			gret;
	LockAcquireResult lres;
	bool		check_state;
	bytea	   *td = NULL;
	Datum		state_values[1];
	Datum		values[6];
	char		nulls[6];
	char		opbuf[2];
	static const Oid state_argtypes[1] = {INT4OID};
	static const Oid upsert_argtypes[6] = {INT4OID, OIDOID, TEXTOID,
										   TEXTOID, TEXTOID, BYTEAOID};
	MemoryContext caller_cxt = CurrentMemoryContext;

	if (branch_id <= 0)
		elog(ERROR, "overlay_delta_insert: invalid branch_id=%d", branch_id);

	/* REPEATABLE READ / SERIALIZABLE unsupported (doc/internals.md §3.1-3.4).
	 * The cross-session freeze is an advisory lock plus a fresh state re-read;
	 * under transaction-snapshot isolation every read (even read_only=false)
	 * is served from the fixed transaction snapshot, so a concurrent
	 * apply/discard commit could not be observed and this write could append
	 * an orphan delta row to a now-terminal branch.  Reject up front. */
	if (IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_branch: cannot write to a branch from a "
						"REPEATABLE READ or SERIALIZABLE transaction"),
				 errdetail("Branch write freezing relies on re-reading the "
						   "branch state after an advisory-lock wait; in "
						   "transaction-snapshot isolation that read cannot "
						   "see a concurrent apply/discard commit."),
				 errhint("Use READ COMMITTED, or commit and retry the write "
						 "in a new READ COMMITTED transaction.")));

	/* FR2/FR3 freeze (doc/internals.md §3.3-3.4).  A delta writer holds a
	 * SHARE advisory lock on the branch for the rest of the transaction.
	 * apply/discard take the EXCLUSIVE form non-blocking, so this writer
	 * blocks behind an in-flight apply/discard and, conversely, makes a
	 * mid-flight apply fail fast.  This transaction-scoped lock is the only
	 * cross-session signal for the otherwise MVCC-invisible 'applying'
	 * state.  Re-acquiring SHARE in the same xact is a no-op. */
	lres = ob_try_branch_advisory_lock_ex(branch_id, ShareLock);
	if (lres == LOCKACQUIRE_NOT_AVAIL)
	{
		/* An apply/discard held the ExclusiveLock; block until it ends. */
		ob_take_branch_advisory_lock(branch_id, ShareLock);
		check_state = true;
	}
	else
	{
		/* LOCKACQUIRE_OK: first Share this xact, so the branch state must be
		 * re-read on a fresh snapshot (the statement snapshot may predate a
		 * concurrent apply commit; see below).  LOCKACQUIRE_ALREADY_HELD: a
		 * previous delta write this xact already verified the state while
		 * holding this lock, and the lock pins the state until xact end, so
		 * the per-row check can be skipped. */
		check_state = (lres == LOCKACQUIRE_OK);
	}

	/* The state check must NOT run on the statement's active snapshot: a long
	 * statement planned before a concurrent apply committed could reach here
	 * afterwards and still read the stale 'active'.  read_only=false selects
	 * GetTransactionSnapshot(), recomputed per call under READ COMMITTED. */
	if (check_state)
	{
		state_values[0] = Int32GetDatum(branch_id);
		gret = ob_spi_exec_params(
								 "SELECT branch_id, state FROM " OBTABLE_BRANCH " "
								 "WHERE branch_id = $1 LIMIT 1",
								 false, 1, 1, state_argtypes, state_values, NULL);
		if (gret != SPI_OK_SELECT)
		{
			SPI_finish();
			elog(ERROR, "overlay_delta_insert: bid=%d state lookup SPI ret=%d",
				 branch_id, gret);
		}
		if (SPI_processed == 0)
		{
			const char *bn;

			SPI_finish();
			bn = overlay_branch_get_current_name();
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("overlay_delta_insert: branch_id=%d does not exist",
							branch_id),
					 errhint("Current session branch: \"%s\". "
							 "Call create_branch() first.",
							 bn ? bn : "<none>")));
		}
		else
		{
			bool		isnull;
			Datum		v;
			char	   *st;
			MemoryContext oldmc;

			/* mirror apply_branch Step0 pattern: SELECT branch_id, state → state is col 2 */
			v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
			if (isnull)
			{
				SPI_finish();
				{
					const char *bn2 = overlay_branch_get_current_name();

					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("overlay_delta_insert: bid=%d has NULL state "
									"(corrupt pg_branch row)",
									branch_id),
							 errhint("Current session branch: \"%s\".",
									 bn2 ? bn2 : "<none>")));
				}
			}
			/* Copy the detoasted text C string into the caller's context, which
			 * outlives SPI_finish() (the raw datum lives in the SPI proc context
			 * that finish tears down).  Not TopMemoryContext: this string is used
			 * only here (freed on success, consumed by the ereport on failure),
			 * so a TopMC copy would leak on every rejected write. */
			oldmc = MemoryContextSwitchTo(caller_cxt);
			st = TextDatumGetCString(v);
			MemoryContextSwitchTo(oldmc);
			SPI_finish();
			if (st == NULL || strcmp(st, BRANCH_STATE_ACTIVE) != 0)
			{
				const char *bn;
				const char *saved_st = st ? st : "<NULL>";

				bn = overlay_branch_get_current_name();
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("overlay_branch: cannot write delta rows to branch "
								"\"%s\" (bid=%d state=%s). "
								"Writes are only allowed while branch is ACTIVE.",
								bn ? bn : "<unresolved name>",
								branch_id, saved_st),
						 (st && strcmp(st, BRANCH_STATE_APPLYING) == 0
						  ? errhint("Branch is currently applying. "
									"Retry apply once the current apply_branch() "
									"caller has finished.")
						  : 0),
						 (st && (strcmp(st, BRANCH_STATE_APPLIED) == 0
								 || strcmp(st, BRANCH_STATE_DISCARDED) == 0)
						  ? errhint("Branch is in a terminal state (%s); "
									"no further writes allowed. Create a new branch.",
									saved_st)
						  : 0)));
			}
			if (st) pfree(st);
		}
	}
	if (key == NULL)
		elog(ERROR, "overlay_delta_insert: key must not be NULL");
	if (op != DELTA_OP_INSERT && op != DELTA_OP_UPDATE && op != DELTA_OP_DELETE)
		elog(ERROR, "overlay_delta_insert: invalid op '%c'", op);

	opbuf[0] = op;
	opbuf[1] = '\0';

	/* Bind tuple_data as a BYTEA parameter: the raw bytes reach the executor
	 * verbatim and round-trip every byte.  (The previous code built a
	 * '\x..'::bytea hex literal for exactly this reason -- a plain text
	 * literal would be mangled by bytea's legacy escape input format, which
	 * rejects embedded double quotes and collapses backslashes.) */
	if (tuple_json_cstr != NULL)
	{
		Size		n = strlen(tuple_json_cstr);

		td = (bytea *) palloc(VARHDRSZ + n);
		SET_VARSIZE(td, VARHDRSZ + n);
		memcpy(VARDATA(td), tuple_json_cstr, n);
	}

	values[0] = Int32GetDatum(branch_id);
	values[1] = ObjectIdGetDatum(relid);
	values[2] = CStringGetTextDatum(key);
	values[3] = CStringGetTextDatum(opbuf);
	values[4] = old_version ? CStringGetTextDatum(old_version) : (Datum) 0;
	values[5] = tuple_json_cstr ? PointerGetDatum(td) : (Datum) 0;
	nulls[0] = ' ';
	nulls[1] = ' ';
	nulls[2] = ' ';
	nulls[3] = ' ';
	nulls[4] = old_version ? ' ' : 'n';
	nulls[5] = tuple_json_cstr ? ' ' : 'n';

	/* The branch delta is keyed by (branch_id, relid, key), so every
	 * statement for the same row overwrites the previous delta entry
	 * (latest-wins).  One collapse needs special handling: DELETE a row that
	 * exists in MAIN, then INSERT the same PK again (a "rebirth" -- the R21
	 * pre-check explicitly permits it while a tombstone is present).  Storing
	 * the resurrected image as op='I' with old_version=NULL would lose the
	 * fact that the row already existed in MAIN, and the apply INSERT pass
	 * would then report a spurious "MAIN row already present" conflict even
	 * though the branch itself deleted that row.
	 *
	 * When the surviving delta entry overwrites a DELETE that claimed a MAIN
	 * baseline (d.op='D' AND d.old_version IS NOT NULL) with a plain INSERT
	 * (EXCLUDED.op='I' AND EXCLUDED.old_version IS NULL), rewrite it to an
	 * UPDATE that keeps the deleted row's version token: the net branch effect
	 * is "replace that MAIN row", which is exactly what the UPDATE apply pass
	 * validates and replays.  A pure-delta tombstone (d.old_version IS NULL)
	 * is left as a plain INSERT, since the row never existed in MAIN. */
	ret = ob_spi_exec_params(
							 "INSERT INTO " OBTABLE_DELTA " AS d "
							 "(branch_id, relid, key, op, old_version, tuple_data) "
							 "VALUES ($1, $2, $3, ($4)::bpchar, $5, $6) "
							 "ON CONFLICT (branch_id, relid, key) DO UPDATE SET "
							 "op = CASE WHEN d.op = 'D' AND EXCLUDED.op = 'I' "
							 "               AND EXCLUDED.old_version IS NULL "
							 "               AND d.old_version IS NOT NULL "
							 "          THEN 'U'::bpchar ELSE EXCLUDED.op END, "
							 "old_version = CASE WHEN d.op = 'D' AND EXCLUDED.op = 'I' "
							 "               AND EXCLUDED.old_version IS NULL "
							 "               AND d.old_version IS NOT NULL "
							 "          THEN d.old_version ELSE EXCLUDED.old_version END, "
							 "tuple_data = EXCLUDED.tuple_data, "
							 "updated_at = now()",
							 false, 1, 6, upsert_argtypes, values, nulls);

	/* WR-1a: the bound parameter datums are transient.  Free them as soon as
	 * the plan has run so the per-row write path stays allocation-neutral in
	 * the caller's (statement-lifetime) memory context -- the previous
	 * literal-interpolation code free()d its StringInfo / quoted literals the
	 * same way. */
	if (td != NULL)
		pfree(td);
	pfree(DatumGetPointer(values[2]));
	pfree(DatumGetPointer(values[3]));
	if (old_version != NULL)
		pfree(DatumGetPointer(values[4]));

	ereport(DEBUG1,
			(errmsg_internal("ODI[dbg] SPI_execute_plan ret=%d processed=%lu",
							 ret, (unsigned long) SPI_processed)));

	if (ret != SPI_OK_INSERT && ret != SPI_OK_INSERT_RETURNING)
	{
		SPI_finish();
		elog(ERROR, "overlay_delta_insert: SPI_execute_plan failed ret=%d", ret);
	}

	SPI_finish();

	ob_delta_size_warn_check(branch_id);
}

void
overlay_delta_update(int32 branch_id, Oid relid, const char *key,
					 char op, const char *old_version, const char *tuple_json_cstr)
{
	overlay_delta_insert(branch_id, relid, key, op, old_version, tuple_json_cstr);
}

bool
overlay_delta_lookup(int32 branch_id, Oid relid, const char *key,
					 DeltaTuple *out_tuple)
{
	Datum		values[3];
	static const Oid argtypes[3] = {INT4OID, OIDOID, TEXTOID};
	int			ret;
	bool		found = false;

	if (key == NULL || out_tuple == NULL)
		elog(ERROR, "overlay_delta_lookup: key and out_tuple must not be NULL");

	values[0] = Int32GetDatum(branch_id);
	values[1] = ObjectIdGetDatum(relid);
	values[2] = CStringGetTextDatum(key);

	/* The outer caller may be a cursor whose DECLARE-time snapshot freezes
	 * snapshot->curcid before this same-txn WR INSERT ran.  Push the latest
	 * secondary snapshot (GetSnapshotData refreshes curcid to the current
	 * command id) so delta rows inserted by later commands remain visible.
	 * Push/Pop uses FirstSnapshotSet; PopActiveSnapshot auto-frees snapshots
	 * with zero active_count+regd_count (see snapmgr.c L743-763). */
	PushActiveSnapshot(GetLatestSnapshot());
	ret = ob_spi_exec_params(
							 "SELECT branch_id, relid, key, op, old_version, tuple_data "
							 "FROM " OBTABLE_DELTA " "
							 "WHERE branch_id = $1 AND relid = $2 AND key = $3 "
							 "LIMIT 1",
							 true, 1, 3, argtypes, values, NULL);
	PopActiveSnapshot();

	/* WR-1a: release the transient key parameter (see overlay_delta_insert). */
	pfree(DatumGetPointer(values[2]));

	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		elog(ERROR, "overlay_delta_lookup: SPI_execute_plan failed ret=%d", ret);
	}

	if (SPI_processed > 0 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
	{
		HeapTuple			tup = SPI_tuptable->vals[0];
		TupleDesc			td  = SPI_tuptable->tupdesc;
		bool				isnull;
		Datum				d;
		MemoryContext		oldmc;

		found = true;

		/* Must be allocated in TopMemoryContext like list_for_rel,
		 * otherwise datumCopy / TextDatumGetCString palloc in
		 * the SPI proc context and all dangle once SPI_finish() is called! */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		memset(out_tuple, 0, sizeof(DeltaTuple));
		out_tuple->typed_pk = NULL;
		out_tuple->typed_pk_n = -1;

		d = SPI_getbinval(tup, td, 1, &isnull);
		out_tuple->branch_id = isnull ? 0 : DatumGetInt32(d);

		d = SPI_getbinval(tup, td, 2, &isnull);
		out_tuple->relid = isnull ? InvalidOid : DatumGetObjectId(d);

		d = SPI_getbinval(tup, td, 3, &isnull);
		if (!isnull)
		{
			/* TextDatumGetCString() already returns a fresh palloc in the
			 * current context (TopMemoryContext here), so it outlives
			 * SPI_finish on its own.  Do NOT feed it a datumCopy() copy:
			 * that intermediate varlena copy was never freed and leaked
			 * once per delta row (MF-1). */
			out_tuple->key = TextDatumGetCString(d);
		}
		else
			out_tuple->key = pstrdup("");

		d = SPI_getbinval(tup, td, 4, &isnull);
		if (isnull)
			out_tuple->op = '?';
		else
		{
			Oid			outoid;
			bool		outvarlena;
			char	   *opstr;

			getTypeOutputInfo(BPCHAROID, &outoid, &outvarlena);
			opstr = OidOutputFunctionCall(outoid, d);
			out_tuple->op = (opstr && opstr[0]) ? opstr[0] : '?';
			pfree(opstr);
		}

		d = SPI_getbinval(tup, td, 5, &isnull);
		if (!isnull)
		{
			/* TextDatumGetCString() output already lives in the current
			 * context; the extra datumCopy() copy leaked per row (MF-1). */
			out_tuple->old_version = TextDatumGetCString(d);
		}
		else
			out_tuple->old_version = NULL;

		d = SPI_getbinval(tup, td, 6, &isnull);
		if (!isnull)
		{
			/* SPI col 6 (tuple_data BYTEA) = attrs[5]. */
			Form_pg_attribute batt = TupleDescAttr(td, 5);
			int16		typlen;
			bool		typbyval;

			get_typlenbyval(batt->atttypid, &typlen, &typbyval);
			out_tuple->tuple_data = (bytea *) datumCopy(d,
										  typbyval, typlen);
		}
		else
			out_tuple->tuple_data = NULL;

		MemoryContextSwitchTo(oldmc);
	}

	SPI_finish();
	return found;
}

List *
overlay_delta_list_for_rel(int32 branch_id, Oid relid)
{
	StringInfoData sql;
	int			ret;
	List	   *result = NIL;
	uint64		i;
	MemoryContext oldmc;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT branch_id, relid, key, op, old_version, tuple_data "
					 "FROM " OBTABLE_DELTA " "
					 "WHERE branch_id = %d AND relid = %u "
					 "ORDER BY key",
					 branch_id, relid);

	/* Match the Push/Pop discipline used in overlay_delta_lookup: force the
	 * secondary latest snapshot so delta rows appended by a later command id
	 * inside the same transaction (e.g. same-txn WR INSERT after a DECLARE
	 * CURSOR) become visible even when the outer statement's active snapshot
	 * still has the older, frozen curcid. */
	PushActiveSnapshot(GetLatestSnapshot());
	ret = ob_spi_one_shot(sql.data, true, 0);
	PopActiveSnapshot();
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		pfree(sql.data);
		elog(ERROR, "overlay_delta_list_for_rel: SPI_execute failed ret=%d", ret);
	}

	if (SPI_processed > 0 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
	{
		TupleDesc	td = SPI_tuptable->tupdesc;

		/* All DeltaTuple + key/tuple_data must be allocated in TopMemoryContext to
		 * outlive SPI_finish() */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);

		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple			tup = SPI_tuptable->vals[i];
			DeltaTuple		   *dt;
			bool				isnull;
			Datum				d;

			dt = (DeltaTuple *) palloc0(sizeof(DeltaTuple));
			dt->typed_pk = NULL;
			dt->typed_pk_n = -1;

			d = SPI_getbinval(tup, td, 1, &isnull);
			dt->branch_id = isnull ? 0 : DatumGetInt32(d);

			d = SPI_getbinval(tup, td, 2, &isnull);
			dt->relid = isnull ? InvalidOid : DatumGetObjectId(d);

			d = SPI_getbinval(tup, td, 3, &isnull);
			if (!isnull)
			{
				/* TextDatumGetCString() output is a fresh palloc in the
				 * current (TopMemoryContext) context; the previous
				 * datumCopy() argument copy leaked per delta row (MF-1). */
				dt->key = TextDatumGetCString(d);
			}
			else
				dt->key = pstrdup("");

			d = SPI_getbinval(tup, td, 4, &isnull);
			if (isnull)
				dt->op = '?';
			else
			{
				Oid			outoid;
				bool		outvarlena;
				char	   *opstr;

				getTypeOutputInfo(BPCHAROID, &outoid, &outvarlena);
				opstr = OidOutputFunctionCall(outoid, d);
				dt->op = (opstr && opstr[0]) ? opstr[0] : '?';
				pfree(opstr);
			}

			d = SPI_getbinval(tup, td, 5, &isnull);
			if (!isnull)
			{
				/* TextDatumGetCString() output already lives in the
				 * current context; the datumCopy() copy leaked per row
				 * (MF-1). */
				dt->old_version = TextDatumGetCString(d);
			}
			else
				dt->old_version = NULL;

			d = SPI_getbinval(tup, td, 6, &isnull);
			if (!isnull)
			{
				Form_pg_attribute batt = TupleDescAttr(td, 5);
				int16		typlen;
				bool		typbyval;
				get_typlenbyval(batt->atttypid, &typlen, &typbyval);
				dt->tuple_data = (bytea *) datumCopy(d,
									   typbyval, typlen);
			}
			else
				dt->tuple_data = NULL;

			result = lappend(result, dt);
		}
		MemoryContextSwitchTo(oldmc);
	}

	SPI_finish();
	pfree(sql.data);
	return result;
}

int64
overlay_delta_count(int32 branch_id)
{
	StringInfoData sql;
	int			ret;
	int64		result = 0;
	volatile bool did_connect = false;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT count(*)::bigint FROM " OBTABLE_DELTA " "
					 "WHERE branch_id = %d",
					 branch_id);

	/* PG_TRY guarantees our own SPI_connect() is matched by an SPI_finish()
	 * even when SPI_execute()/SPI_getbinval() raise (e.g. a statement_timeout
	 * during the count).  Without it the connection frame is left on the SPI
	 * stack and every later SPI_connect() nests one level deeper, which
	 * corrupts the memory-context save/restore for the rest of the
	 * transaction.  did_connect keeps the cleanup scoped to our own level so
	 * a caller that already had SPI connected is left untouched. */
	PG_TRY();
	{
		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			elog(ERROR, "overlay_branch: SPI_connect failed: %d", ret);
		did_connect = true;

		/* Deterministic error injection for the SPI cleanup test: raise with
		 * SPI still connected so the PG_CATCH below is the only thing that
		 * can close the frame. */
		if (ob_debug_delta_count_error)
			elog(ERROR, "overlay_branch: injected delta-count error "
						"(_debug_delta_count_error)");

		ret = SPI_execute(sql.data, true, 1);
		if (ret != SPI_OK_SELECT)
			elog(ERROR, "overlay_delta_count: SPI_execute failed ret=%d", ret);

		if (SPI_processed > 0 && SPI_tuptable != NULL &&
			SPI_tuptable->vals != NULL)
		{
			bool	isnull;
			Datum	d = SPI_getbinval(SPI_tuptable->vals[0],
									  SPI_tuptable->tupdesc, 1, &isnull);
			if (!isnull)
				result = DatumGetInt64(d);
		}
	}
	PG_CATCH();
	{
		if (did_connect)
			SPI_finish();
		PG_RE_THROW();
	}
	PG_END_TRY();

	SPI_finish();
	pfree(sql.data);
	return result;
}

/*
 * ob_delta_size_warn_check
 *
 *	B1 (internals.md §5.6): amortized delta-size nudge.  Runs once per delta
 *	write, but does real work only when overlay_branch.delta_size_warn_rows > 0
 *	and the write counter lands on a multiple of
 *	overlay_branch.delta_size_check_interval.  It then counts the current
 *	branch's delta rows and emits a single WARNING per session on the first
 *	overflow, pointing the user at branch_delta_stats().
 *
 *	Fail-open: any SPI/catalog error is swallowed so a bookkeeping problem can
 *	never break user DML.  Both counters are session-local statics (never
 *	persisted); the default threshold of 0 short-circuits before any SPI.
 */
static void
ob_delta_size_warn_check(int32 branch_id)
{
	static uint64 s_writes = 0;
	static bool s_warned = false;
	int64		n;
	const char *branch_name;
	volatile bool snap_pushed = false;

	if (ob_delta_size_warn_rows <= 0 || s_warned)
		return;

	s_writes++;
	if (ob_delta_size_check_interval <= 0 ||
		(s_writes % (uint64) ob_delta_size_check_interval) != 0)
		return;

	PG_TRY();
	{
		/* Read the *current* delta size.  The caller's statement snapshot has a
		 * frozen curcid, so the row this very write just inserted would
		 * otherwise be invisible and the count would lag by one.  Push the
		 * latest snapshot (same idiom as overlay_delta_lookup) so the nudge
		 * reflects the write that triggered it. */
		PushActiveSnapshot(GetLatestSnapshot());
		snap_pushed = true;
		n = overlay_delta_count(branch_id);
		PopActiveSnapshot();
		snap_pushed = false;
	}
	PG_CATCH();
	{
		if (snap_pushed)
			PopActiveSnapshot();
		FlushErrorState();
		return;
	}
	PG_END_TRY();

	if (n <= (int64) ob_delta_size_warn_rows)
		return;

	s_warned = true;
	branch_name = overlay_branch_get_current_name();
	ereport(WARNING,
			(errmsg("overlay_branch: branch \"%s\" delta has reached " INT64_FORMAT
					" rows (threshold %d); consider apply_branch()/discard_branch() to bound growth",
					branch_name ? branch_name : "<unknown>",
					n, ob_delta_size_warn_rows),
			 errhint("Inspect per-relation delta size with "
					 "SELECT * FROM branch_delta_stats('%s').",
					 branch_name ? branch_name : "<unknown>")));
}

void
overlay_delta_delete_all(int32 branch_id)
{
	StringInfoData sql;
	int			ret;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM " OBTABLE_DELTA " WHERE branch_id = %d",
					 branch_id);

	ret = ob_spi_one_shot(sql.data, false, 1);
	if (ret != SPI_OK_DELETE)
	{
		SPI_finish();
		pfree(sql.data);
		elog(ERROR, "overlay_delta_delete_all: SPI_execute failed ret=%d", ret);
	}

	elog(DEBUG1, "overlay_delta_delete_all: branch=%d deleted=%lu rows",
		 branch_id, (unsigned long) SPI_processed);
	SPI_finish();
	pfree(sql.data);
}

/* ================================================================
 * Slot / junk-attribute helpers (needed by UPDATE/DELETE write redirect)
 * ================================================================ */
AttrNumber
rel_attno_to_slot_idx(TupleDesc slotdesc, AttrNumber rel_attno)
{
	int			natts = slotdesc->natts;

	for (int j = 0; j < natts; j++)
	{
		Form_pg_attribute satt = TupleDescAttr(slotdesc, j);
		if (satt->attisdropped)
			continue;
		if (satt->attnum == rel_attno)
			return (AttrNumber) (j + 1);
	}
	return 0;
}

char *
slot_get_ctid_cstr(TupleTableSlot *slot)
{
	TupleDesc	td = slot->tts_tupleDescriptor;
	int			natts = td->natts;
	int			j;

	for (j = 0; j < natts; j++)
	{
		Form_pg_attribute satt = TupleDescAttr(td, j);

		if (satt->attisdropped)
			continue;
		if (strcmp(NameStr(satt->attname), "ctid") == 0)
		{
			bool		isnull;
			Datum		d = slot_getattr(slot, (AttrNumber) (j + 1), &isnull);
			ItemPointerData ip;

			if (isnull)
				elog(ERROR, "slot_get_ctid_cstr: ctid IS NULL (cannot locate row)");

			ip = *DatumGetItemPointer(d);
			/* ACC-1: a pure-delta overlay row (no MAIN counterpart) carries an
			 * invalid ItemPointer (block/offset 0) OR, when emitted by a
			 * DML-target BranchScan, a SYNTHETIC ctid (block
			 * OB_SYNTH_CTID_BLOCK).  Neither is a physical row location, so
			 * report "no ctid": the write path resolves the row's delta
			 * identity instead, and the keyless anchor path must never build
			 * "A:..." from a synthetic block. */
			if (!ItemPointerIsValid(&ip))
				return NULL;
			if (ItemPointerGetBlockNumber(&ip) == OB_SYNTH_CTID_BLOCK)
				return NULL;
			{
				char	   *out = (char *) palloc(32);

				snprintf(out, 32, "(%u,%u)",
						 ItemPointerGetBlockNumber(&ip),
						 ItemPointerGetOffsetNumber(&ip));
				return out;
			}
		}
	}
	return NULL;
}

/*
 * Fetch a clean relation-shaped slot for the physical row at ctid_cstr.
 *
 * result_cxt owns the returned slot, its CreateTupleDescCopy()'d tupdesc and
 * every datumCopy() of the fetched column values, mirroring
 * reconstruct_slot_from_delta().  NULL means CurrentMemoryContext.  This used
 * to allocate everything in TopMemoryContext, so every UPDATE/DELETE row
 * redirected through the write path leaked one tupdesc + slot shell + toasted
 * value copies for the whole backend lifetime (MF-1): ExecDropSingleTupleTableSlot()
 * only pfree()s the slot and ReleaseTupleDesc() is a no-op for the -1-refcount
 * copy, so nothing reclaimed those chunks.
 */
TupleTableSlot *
fetch_tuple_by_ctid(Relation rel, const char *ctid_cstr,
					MemoryContext result_cxt)
{
	int			ret;
	TupleTableSlot *out_slot;
	TupleDesc	reldesc = RelationGetDescr(rel);
	char	   *sql;
	Datum		values[1];
	static const Oid argtypes[1] = {TEXTOID};

	overlay_overlay_helper_enter();

	/* WR-1a: relation name stays inline (the cached plan is keyed on the
	 * full statement text); ctid is bound as $1 rather than interpolated.
	 * The ctid system column is selected explicitly: SPI materialises the
	 * result tuple with a fresh heap_form_tuple(), which leaves t_self
	 * invalid, so htup->t_self cannot be used as the row identity (it only
	 * looks valid when the plan needs no projection, e.g. a table with no
	 * dropped columns). */
	sql = psprintf("SELECT ctid, * FROM %s WHERE ctid = $1::tid LIMIT 1",
				   quote_qualified_identifier(
					   get_namespace_name(RelationGetNamespace(rel)),
					   RelationGetRelationName(rel)));
	values[0] = CStringGetTextDatum(ctid_cstr);

	ret = ob_spi_exec_params(sql, true, 1, 1, argtypes, values, NULL);
	pfree(sql);
	pfree(DatumGetPointer(values[0]));

	if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
		SPI_tuptable == NULL || SPI_tuptable->vals == NULL)
	{
		SPI_finish();
		overlay_overlay_helper_exit();
		elog(ERROR, "fetch_tuple_by_ctid: SPI SELECT-by-ctid failed (ret=%d rows=%lu) ctid=%s",
			 ret, SPI_processed != 0 ? (unsigned long) SPI_processed : 0UL,
			 ctid_cstr);
	}

	out_slot = NULL;
	{
		HeapTuple	htup = SPI_tuptable->vals[0];
		TupleDesc	td_spi = SPI_tuptable->tupdesc;
		MemoryContext oldmc;

		oldmc = MemoryContextSwitchTo(result_cxt != NULL ? result_cxt
														 : CurrentMemoryContext);
		{
			TupleDesc	slot_desc = CreateTupleDescCopy(reldesc);

			out_slot = MakeSingleTupleTableSlot(slot_desc, &TTSOpsVirtual);
		}
		ExecClearTuple(out_slot);
		/* td_spi layout is (ctid, <non-dropped attributes in attnum order>).
		 * Map each physical attribute to its result column so a dropped
		 * column cannot shift the columns that follow it. */
		{
			int			spi_col_idx = 1;	/* result column 1 is the ctid */

			for (int a = 0; a < reldesc->natts; a++)
			{
				bool			isnull;
				bool			dropped = TupleDescAttr(reldesc, a)->attisdropped;
				Datum			d;

				if (dropped)
				{
					out_slot->tts_isnull[a] = true;
					out_slot->tts_values[a] = (Datum) 0;
					continue;
				}

				spi_col_idx++;
				d = SPI_getbinval(htup, td_spi, spi_col_idx, &isnull);
				out_slot->tts_isnull[a] = isnull;
				if (!isnull)
				{
					Form_pg_attribute ratt = TupleDescAttr(reldesc, a);
					int16		typlen;
					bool		typbyval;

					get_typlenbyval(ratt->atttypid, &typlen, &typbyval);
					d = datumCopy(d, typbyval, typlen);
				}
				out_slot->tts_values[a] = isnull ? (Datum) 0 : d;
			}
		}
		ExecStoreVirtualTuple(out_slot);
		out_slot->tts_nvalid = reldesc->natts;
		{
			/* Row identity from the projected ctid column, NOT htup->t_self. */
			bool		ctid_isnull;
			Datum		ctid_d = SPI_getbinval(htup, td_spi, 1, &ctid_isnull);

			if (!ctid_isnull)
				out_slot->tts_tid = *DatumGetItemPointer(ctid_d);
		}
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	overlay_overlay_helper_exit();
	return out_slot;
}

/*
 * Rebuild a relation-shaped slot from a delta row's JSON tuple_data blob.
 *
 * result_cxt owns the returned slot, its CreateTupleDescCopy()'d tupdesc and
 * every datumCopy() of the column values.  Callers pick a context whose
 * lifetime matches the result set (see overlay_branch.h); NULL means
 * CurrentMemoryContext, i.e. the caller manages it.  Previously this always
 * allocated in TopMemoryContext, which leaked one tupdesc + all toasted
 * value copies per reconstructed row for the whole backend lifetime (MF-1).
 */
TupleTableSlot *
reconstruct_slot_from_delta(Relation rel, bytea *tuple_data,
							MemoryContext result_cxt)
{
	StringInfoData sql;
	StringInfoData cols;
	TupleDesc	reldesc = RelationGetDescr(rel);
	int			ret;
	TupleTableSlot *out_slot;
	int			natts;
	MemoryContext oldmc;
	HeapTuple	htup;
	TupleDesc	td_spi;
	Oid			bytea_out_func;
	bool		bytea_out_varlena;
	char	   *bytea_sql_lit;
	char	   *convert_from_expr;

	Assert(rel != NULL && tuple_data != NULL);
	natts = reldesc->natts;

	/* build column spec colname sqltype,... */
	initStringInfo(&cols);
	for (int a = 0; a < natts; a++)
	{
		Form_pg_attribute ratt = TupleDescAttr(reldesc, a);

		if (ratt->attisdropped)
			continue;
		if (cols.len > 0)
			appendStringInfoChar(&cols, ',');
		appendStringInfo(&cols, "%s %s",
						 quote_identifier(NameStr(ratt->attname)),
						 format_type_with_typemod(ratt->atttypid,
												  ratt->atttypmod));
	}

	/* use the built-in PG bytea output to build a '\xHHHH' literal, compatible with all varlena formats */
	getTypeOutputInfo(BYTEAOID, &bytea_out_func, &bytea_out_varlena);
	bytea_sql_lit = OidOutputFunctionCall(bytea_out_func,
										  PointerGetDatum(tuple_data));
	convert_from_expr = psprintf("convert_from('%s'::bytea, 'UTF8')::jsonb",
								 bytea_sql_lit);
	pfree(bytea_sql_lit);

	/* one-shot SPI turns the JSON blob → typed single-column result.
	 * Must be `SELECT *`, not `SELECT (x).*`: the latter references the
	 * range-table alias `x` by name, so a user column literally named `x`
	 * shadows it and aborts with "type <T> is not composite".  `*` expands
	 * to the record columns in declaration order with no name lookup, so the
	 * index-based mapping below is unchanged. */
	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT * FROM jsonb_to_record(%s) AS x(%s) LIMIT 1",
					 convert_from_expr, cols.data);
	pfree(convert_from_expr);
	pfree(cols.data);

	ret = ob_spi_one_shot(sql.data, true, 1);
	pfree(sql.data);

	if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
		SPI_tuptable == NULL || SPI_tuptable->vals == NULL)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("overlay_branch: failed to reconstruct tuple from delta JSON (SPI ret=%d rows=%lu)",
						ret, (unsigned long) SPI_processed)));
	}

	oldmc = MemoryContextSwitchTo(result_cxt != NULL ? result_cxt
													 : CurrentMemoryContext);
	{
		TupleDesc	slot_desc = CreateTupleDescCopy(reldesc);

		out_slot = MakeSingleTupleTableSlot(slot_desc, &TTSOpsVirtual);
	}
	ExecClearTuple(out_slot);
	htup = SPI_tuptable->vals[0];
	td_spi = SPI_tuptable->tupdesc;

	/* td_spi layout is (non-dropped attributes in attnum order), matching the
	 * colspec built above -- dropped columns were skipped there too.  Map each
	 * physical attribute to its result column so a dropped column cannot shift
	 * the columns that follow it (SPI_getbinval returns NULL, not an error, for
	 * an out-of-range fnumber, so an unmapped read would silently blank the
	 * tail columns instead of failing loudly).  Same convention as
	 * fetch_tuple_by_ctid(). */
	{
		int			spi_col_idx = 0;

		for (int a = 0; a < natts; a++)
		{
			bool			isnull;
			Form_pg_attribute ratt = TupleDescAttr(reldesc, a);
			Datum			d;

			if (ratt->attisdropped)
			{
				out_slot->tts_isnull[a] = true;
				out_slot->tts_values[a] = (Datum) 0;
				continue;
			}

			spi_col_idx++;
			d = SPI_getbinval(htup, td_spi, spi_col_idx, &isnull);
			out_slot->tts_isnull[a] = isnull;
			if (!isnull)
			{
				/* RD-3: reuse the attribute's own attlen/attbyval (kept in sync
				 * with pg_type) instead of a per-column get_typlenbyval(). */
				d = datumCopy(d, ratt->attbyval, ratt->attlen);
			}
			out_slot->tts_values[a] = isnull ? (Datum) 0 : d;
		}
	}
	ExecStoreVirtualTuple(out_slot);
	out_slot->tts_nvalid = natts;
	MemoryContextSwitchTo(oldmc);

	SPI_finish();
	return out_slot;
}

/* ================================================================
 * RD-4 / RD-5: cached PRIMARY KEY metadata and PK comparison functions
 * ----------------------------------------------------------------
 * Every hot-path helper used to rediscover the relation's PRIMARY KEY
 * index on EVERY call (RelationGetIndexList() + index_open() per row),
 * and overlay_typed_pk_cmp() re-ran get_sort_group_operators() plus two
 * OPEROID syscache lookups on EVERY comparison.  Both results are a pure
 * function of the catalog state, so they are memoised here:
 *
 *   ob_pk_rel_hash  : relid  -> {PK attnos, per-column type metadata,
 *                                per-column lt/eq FmgrInfo pointers}
 *   ob_pk_type_hash : typeid -> {lt/eq FmgrInfo}
 *
 * Lifetime / invalidation rules:
 *   - Both hash tables live in a dedicated child of TopMemoryContext
 *     created LAZILY on first use.  Never in _PG_init: that also runs in
 *     the postmaster when the library is preloaded, and a postmaster
 *     owned hash inherited by every fork would be shared state.
 *   - ob_pk_type_hash entries are keyed by TYPE oid and never removed.
 *     The FmgrInfo objects themselves are deliberately allocated in
 *     TopMemoryContext so a TypedKey that hands out pointers to them can
 *     never observe them being freed mid-scan.
 *   - ob_pk_rel_hash entries are dropped by a relcache invalidation
 *     callback: ADD/DROP PRIMARY KEY invalidates the TABLE's relcache
 *     entry, and a "flush everything" event arrives as InvalidOid.
 * ================================================================ */

#define OB_MAX_PK_COLS		32	/* == MAX_INDEX_COLUMNS, PG hard limit */

typedef struct ObPkColMeta
{
	AttrNumber	attno;			/* 1-based real attno; 0 = expression column */
	Oid			typid;
	int32		typmod;
	Oid			collid;
	int16		typlen;			/* att->attlen of the PK column */
	bool		typbyval;		/* att->attbyval of the PK column */
	FmgrInfo   *eq_finfo;		/* TopMemoryContext, NULL = no btree ops */
	FmgrInfo   *lt_finfo;		/* TopMemoryContext, NULL = no btree ops */
} ObPkColMeta;

typedef struct ObPkRelMeta
{
	Oid			relid;			/* hash key = table Oid */
	bool		has_pk;
	bool		guc_sensitive;	/* any PK col's output depends on a session GUC */
	int			n_pk;
	ObPkColMeta cols[OB_MAX_PK_COLS];
} ObPkRelMeta;

typedef struct ObTypeCmpMeta
{
	Oid			typid;			/* hash key */
	FmgrInfo   *eq_finfo;
	FmgrInfo   *lt_finfo;
} ObTypeCmpMeta;

static MemoryContext ob_pk_cache_cxt = NULL;
static HTAB *ob_pk_rel_hash = NULL;
static HTAB *ob_pk_type_hash = NULL;

static void ob_pk_cache_invalidate_cb(Datum arg, Oid relid);
static bool ob_identity_type_is_guc_sensitive(Oid typid);

static void
ob_pk_cache_init(void)
{
	HASHCTL		ctl;

	if (ob_pk_cache_cxt != NULL)
		return;

	ob_pk_cache_cxt = AllocSetContextCreate(TopMemoryContext,
											"overlay_branch pk meta cache",
											ALLOCSET_SMALL_SIZES);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(ObPkRelMeta);
	ctl.hcxt = ob_pk_cache_cxt;
	ob_pk_rel_hash = hash_create("overlay_branch pk per-relation",
								 32, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(ObTypeCmpMeta);
	ctl.hcxt = ob_pk_cache_cxt;
	ob_pk_type_hash = hash_create("overlay_branch pk per-type cmp",
								  16, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	CacheRegisterRelcacheCallback(ob_pk_cache_invalidate_cb, (Datum) 0);
}

static void
ob_pk_cache_invalidate_cb(Datum arg, Oid relid)
{
	(void) arg;

	if (ob_pk_rel_hash == NULL)
		return;

	if (relid != InvalidOid)
	{
		(void) hash_search(ob_pk_rel_hash, &relid, HASH_REMOVE, NULL);
		return;
	}

	/* Full relcache reset: flush the whole per-relation map.  Deleting
	 * the element just returned by hash_seq_search() is allowed. */
	{
		HASH_SEQ_STATUS status;
		ObPkRelMeta *entry;

		hash_seq_init(&status, ob_pk_rel_hash);
		while ((entry = (ObPkRelMeta *) hash_seq_search(&status)) != NULL)
		{
			Oid			key = entry->relid;

			(void) hash_search(ob_pk_rel_hash, &key, HASH_REMOVE, NULL);
		}
	}
}

/*
 * ob_pk_type_cmp_fmgrs
 *
 *	Memoised btree "<" / "=" support functions for a PK column type.
 *	Returns NULL pointers when the type has no btree sort operators; the
 *	callers then keep their pre-RD-5 fallback behaviour.
 */
static void
ob_pk_type_cmp_fmgrs(Oid typid, FmgrInfo **out_eq, FmgrInfo **out_lt)
{
	ObTypeCmpMeta *entry;
	bool		found;

	*out_eq = NULL;
	*out_lt = NULL;

	if (!OidIsValid(typid))
		return;

	entry = (ObTypeCmpMeta *) hash_search(ob_pk_type_hash, &typid,
										  HASH_ENTER, &found);
	if (found)
	{
		*out_eq = entry->eq_finfo;
		*out_lt = entry->lt_finfo;
		return;
	}

	entry->typid = typid;
	entry->eq_finfo = NULL;
	entry->lt_finfo = NULL;

	{
		Oid			ltOpr = InvalidOid;
		Oid			eqOpr = InvalidOid;
		Oid			gtOpr = InvalidOid;
		Oid			fn_eq;
		Oid			fn_lt;
		MemoryContext oldmc;

		get_sort_group_operators(typid, true, true, false,
								 &ltOpr, &eqOpr, &gtOpr, NULL);
		if (!OidIsValid(eqOpr) || !OidIsValid(ltOpr))
			return;

		fn_eq = get_opcode(eqOpr);
		fn_lt = get_opcode(ltOpr);
		if (!OidIsValid(fn_eq) || !OidIsValid(fn_lt))
			return;

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		entry->eq_finfo = (FmgrInfo *) palloc(sizeof(FmgrInfo));
		fmgr_info_cxt(fn_eq, entry->eq_finfo, TopMemoryContext);
		entry->lt_finfo = (FmgrInfo *) palloc(sizeof(FmgrInfo));
		fmgr_info_cxt(fn_lt, entry->lt_finfo, TopMemoryContext);
		MemoryContextSwitchTo(oldmc);
	}

	*out_eq = entry->eq_finfo;
	*out_lt = entry->lt_finfo;
}

/*
 * ob_pk_rel_meta
 *
 *	Memoised PRIMARY KEY description of `rel`; never returns NULL.
 *	Relations WITHOUT a primary key are cached as well (has_pk = false),
 *	so the keyless path does not re-walk the index list per row either.
 */
static ObPkRelMeta *
ob_pk_rel_meta(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	ObPkRelMeta *entry;
	bool		found;
	List	   *indexoids;
	ListCell   *lc;
	Oid			pk_index_oid = InvalidOid;
	Relation	pk_rel = NULL;

	ob_pk_cache_init();

	entry = (ObPkRelMeta *) hash_search(ob_pk_rel_hash, &relid,
										HASH_ENTER, &found);
	if (found)
		return entry;

	/* (Re)build in place.  relid is the hash key and is written back with
	 * the identical value it was entered with, so the bucket stays valid. */
	memset(entry, 0, sizeof(ObPkRelMeta));
	entry->relid = relid;

	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		Relation	idxrel;

		idxrel = index_open(idxoid, AccessShareLock);
		if (idxrel->rd_index != NULL && idxrel->rd_index->indisprimary)
		{
			pk_index_oid = idxoid;
			pk_rel = idxrel;
			break;
		}
		index_close(idxrel, AccessShareLock);
	}
	list_free(indexoids);

	if (OidIsValid(pk_index_oid) && pk_rel != NULL)
	{
		TupleDesc	reldesc = RelationGetDescr(rel);
		int			n = pk_rel->rd_index->indnatts;
		int			i;

		if (n > OB_MAX_PK_COLS)
		{
			index_close(pk_rel, AccessShareLock);
			elog(ERROR,
				 "overlay_branch: primary key of \"%s\" has %d columns, "
				 "exceeding the supported maximum of %d",
				 RelationGetRelationName(rel), n, OB_MAX_PK_COLS);
		}

		entry->has_pk = true;
		entry->n_pk = n;

		for (i = 0; i < n; i++)
		{
			ObPkColMeta *col = &entry->cols[i];
			AttrNumber	attno = pk_rel->rd_index->indkey.values[i];
			Form_pg_attribute att;

			col->attno = attno;
			if (attno <= 0 || attno > reldesc->natts)
			{
				/* Expression column: no type metadata, and the unknown
				 * output function may be GUC-dependent — pin the
				 * canonical identity GUCs defensively. */
				entry->guc_sensitive = true;
				continue;
			}
			att = TupleDescAttr(reldesc, attno - 1);
			col->typid = att->atttypid;
			col->typmod = att->atttypmod;
			col->collid = OidIsValid(att->attcollation)
				? att->attcollation : InvalidOid;
			col->typlen = att->attlen;
			col->typbyval = att->attbyval;
			ob_pk_type_cmp_fmgrs(col->typid, &col->eq_finfo, &col->lt_finfo);
			if (ob_identity_type_is_guc_sensitive(col->typid))
				entry->guc_sensitive = true;
		}

		index_close(pk_rel, AccessShareLock);
	}

	return entry;
}

bool
overlay_relation_has_pk(Relation rel)
{
	if (rel == NULL)
		return false;

	return ob_pk_rel_meta(rel)->has_pk;
}


/* ================================================================
 * Keyless (experimental) ctid row-identity helpers
 * ----------------------------------------------------------------
 * Only used when overlay_branch.keyless_tables = on AND the target
 * relation has no PRIMARY KEY.  Row identity is either the physical
 * MAIN anchor "A:<blk>:<off>" (for rows that exist in MAIN) or a
 * synthetic per-(branch,rel) sequence "S:<seq>" (rows born inside the
 * branch).  The tag prefix keeps these keys disjoint from the PK JSON
 * keys (which always start with '[').  See doc/internals.md §4.3.
 * ================================================================ */

/* Convert a ctid cstring "(blk,off)" to an anchor key "A:<blk>:<off>".
 * Returns NULL when the input is NULL or not a parseable ctid. */
char *
overlay_anchor_key_from_ctid(const char *ctid_cstr)
{
	unsigned int blk;
	unsigned int off;

	if (ctid_cstr == NULL)
		return NULL;
	if (sscanf(ctid_cstr, "(%u,%u)", &blk, &off) != 2)
		return NULL;
	return psprintf("A:%u:%u", blk, off);
}

/* Allocate the next synthetic key "S:<seq>" for a keyless row born
 * inside the branch.  seq = 1 + max(existing S: seq for this
 * (branch,rel)).  Because delta rows are never physically removed until
 * discard, this is monotonic within the branch lifetime. */
char *
overlay_synthetic_key(int32 branch_id, Oid relid)
{
	StringInfoData sql;
	int			ret;
	int64		next = 1;
	char	   *sqlstr;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT COALESCE(MAX(substring(d.key from 3)::bigint), 0) + 1 "
					 "FROM %s d WHERE d.branch_id = %d AND d.relid = %u "
					 "AND d.key LIKE 'S:%%'",
					 OBTABLE_DELTA, branch_id, (unsigned int) relid);
	sqlstr = sql.data;

	overlay_overlay_helper_enter();
	ret = ob_spi_one_shot(sqlstr, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed == 1 &&
		SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
	{
		bool		isnull;
		Datum		v = SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, 1, &isnull);

		if (!isnull)
			next = DatumGetInt64(v);
	}
	SPI_finish();
	overlay_overlay_helper_exit();
	pfree(sqlstr);

	return psprintf("S:" INT64_FORMAT, (int64) next);
}

/* Emit the keyless-identity WARNING once per (session, relid). */
void
overlay_keyless_warn_once(Relation rel)
{
	static List *warned = NIL;
	ListCell   *lc;
	Oid			relid;
	char	   *nspname;
	MemoryContext oldcxt;

	if (rel == NULL)
		return;
	relid = RelationGetRelid(rel);
	foreach(lc, warned)
	{
		if (lfirst_oid(lc) == relid)
			return;
	}

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	warned = lappend_oid(warned, relid);
	MemoryContextSwitchTo(oldcxt);

	nspname = get_namespace_name(RelationGetNamespace(rel));
	ereport(WARNING,
			(errmsg("overlay_branch: table \"%s.%s\" has no PRIMARY KEY; using experimental ctid-based row identity (overlay_branch.keyless_tables=on)",
					nspname ? nspname : "?", RelationGetRelationName(rel)),
			 errdetail("Row identity is the physical tuple location (ctid, xmin).  Concurrent UPDATE/DELETE on this table by other sessions may change a row's ctid and cause the branch to silently miss that row's delta (detected only at apply time as a conflict error)."),
			 errhint("Add a PRIMARY KEY (or ALTER TABLE ... ADD COLUMN ... GENERATED ALWAYS AS IDENTITY PRIMARY KEY) to get durable row identity.")));
}

/* Unified write-side identity resolver.  Dispatches to the PK serializer
 * when the relation has a PRIMARY KEY, otherwise falls back to the
 * experimental ctid anchor / synthetic sequence (keyless mode). */
char *
overlay_serialize_identity(Relation rel, TupleTableSlot *slot, char op)
{
	char	   *ctid_cstr;
	char	   *key;

	Assert(rel != NULL && slot != NULL);

	if (overlay_relation_has_pk(rel))
		return overlay_serialize_pk(rel, slot);

	if (!overlay_keyless_tables)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_serialize_identity: table %s has no PRIMARY KEY and overlay_branch.keyless_tables is off",
						RelationGetRelationName(rel))));

	/* Physical anchor first: UPDATE/DELETE and any MAIN-anchored row
	 * carry a ctid either as a junk column or in tts_tid. */
	ctid_cstr = slot_get_ctid_cstr(slot);
	if (ctid_cstr == NULL)
	{
		ItemPointer ip = &(slot->tts_tid);

		if (ItemPointerIsValid(ip) &&
			ItemPointerGetBlockNumber(ip) != InvalidBlockNumber &&
			ItemPointerGetOffsetNumber(ip) != 0)
			ctid_cstr = psprintf("(%u,%u)",
								 ItemPointerGetBlockNumber(ip),
								 ItemPointerGetOffsetNumber(ip));
	}

	if (ctid_cstr != NULL)
	{
		key = overlay_anchor_key_from_ctid(ctid_cstr);

		pfree(ctid_cstr);
		if (key != NULL)
			return key;
	}

	/* No physical origin: a brand-new row born inside the branch. */
	if (op == DELTA_OP_INSERT)
		return overlay_synthetic_key(overlay_branch_get_current_id(),
									 RelationGetRelid(rel));

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("overlay_serialize_identity: keyless %s on %s has no ctid anchor",
					op == DELTA_OP_UPDATE ? "UPDATE" : "DELETE",
					RelationGetRelationName(rel))));
	return NULL;				/* unreachable */
}

/* ================================================================
 * Canonical output GUC context (doc/internals.md §1.5)
 *
 * Two serializers render a value through its column type's output
 * function into text that outlives the writing session:
 *
 *   - the delta identity key (overlay_serialize_pk /
 *     overlay_serialize_pk_from_single_datum), whose text MUST be
 *     byte-identical for '='-equal values across sessions, and
 *   - the delta tuple payload (overlay_serialize_tuple), whose text is
 *     re-parsed by the reading session's input functions.
 *
 * A handful of output functions are NOT pure functions of the value:
 * they read session GUCs — TimeZone / DateStyle for the date-time
 * family, IntervalStyle for interval, extra_float_digits for
 * float4/float8, bytea_output for bytea.  Left alone, two sessions with
 * different settings either fail to match on the SAME value (identity
 * key → the delta PK (branch_id, relid, key) stores two rows and every
 * text-key probe misses) or re-parse the payload into a DIFFERENT value
 * (silent corruption).  Whenever a column is such a type we pin those
 * GUCs to a canonical value for the duration of the serialization only,
 * using GUC_ACTION_SAVE so guc.c unwinds the change automatically on
 * error (and because SAVE is the one action permitted inside a parallel
 * worker).  GUC-insensitive types (int, text, uuid, …) skip the pin
 * entirely, so the hot common path pays nothing.
 * ================================================================ */
static bool
ob_identity_type_is_guc_sensitive(Oid typid)
{
	Oid			elem;

	/* A domain renders through its base type's output function but has its
	 * own OID, so peel it first — a domain over timestamptz must be pinned
	 * exactly like timestamptz.  getBaseType also collapses nested domains
	 * and leaves an array's own OID (it is not a domain) intact. */
	typid = getBaseType(typid);

	/* An array column renders through its element output function, so
	 * recurse once (arrays are not nested as distinct types). */
	elem = get_element_type(typid);
	if (OidIsValid(elem))
		return ob_identity_type_is_guc_sensitive(elem);

	switch (typid)
	{
		case TIMESTAMPTZOID:
		case TIMESTAMPOID:
		case DATEOID:
		case TIMETZOID:
		case INTERVALOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case BYTEAOID:
			return true;
		default:
			return false;
	}
}

/*
 * ob_payload_type_roundtrip_unsafe
 *
 *	The subset of ob_identity_type_is_guc_sensitive whose output ALSO fails
 *	to survive a round trip through a differently-configured reader: the
 *	type carries no self-describing marker, so DateStyle / IntervalStyle /
 *	extra_float_digits can make timestamp_in / date_in / timestamptz_in /
 *	interval_in / float8in re-parse the stored payload text into a different
 *	value.
 *
 *	timestamptz belongs here: even though it always emits a zone offset (or
 *	name), that only pins the instant's zone — DateStyle still reorders the
 *	date fields, so `2024-06-01 00:00:00 UTC` written under SQL,DMY as
 *	`01/06/2024 00:00:00 UTC` is read back under ISO,MDY as 6 January.
 *	`timetz` (no date field) and `bytea` (two mutually-accepted spellings)
 *	do round-trip, so they skip the pin.
 */
static bool
ob_payload_type_roundtrip_unsafe(Oid typid)
{
	Oid			elem;

	/* Peel a domain first (own OID, base type's I/O functions). */
	typid = getBaseType(typid);

	elem = get_element_type(typid);
	if (OidIsValid(elem))
		return ob_payload_type_roundtrip_unsafe(elem);

	switch (typid)
	{
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case DATEOID:
		case INTERVALOID:
		case FLOAT4OID:
		case FLOAT8OID:
			return true;
		default:
			return false;
	}
}

static int
ob_canonical_guc_pin(void)
{
	int			nl = NewGUCNestLevel();

	(void) set_config_option("TimeZone", "UTC",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("DateStyle", "ISO, MDY",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("IntervalStyle", "postgres",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("extra_float_digits", "1",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("bytea_output", "hex",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	return nl;
}

static void
ob_canonical_guc_unpin(int nestlevel)
{
	AtEOXact_GUC(true, nestlevel);
}

/* ================================================================
 * PK / row serialization (JSON)
 * ================================================================ */

/*
 * ob_serialize_pk_body
 *	Build the canonical PK identity text.  The caller has already
 *	established the canonical GUC context when the PK is GUC-sensitive
 *	(see overlay_serialize_pk), so this helper is context-agnostic.
 */
static char *
ob_serialize_pk_body(Relation rel, TupleTableSlot *slot, ObPkRelMeta *pm)
{
	StringInfoData json;
	char	   *result;
	MemoryContext oldmc;

	Assert(rel != NULL && slot != NULL && pm != NULL);

	/* RD-8: build the JSON array text in pure C instead of a
	 * `SELECT to_jsonb(ARRAY[...])::text` SPI round-trip (~8us/row).
	 * jsonb renders a text array with elements joined by ", " and escapes
	 * every string through escape_json() (jsonb.c:jsonb_put_escaped_value),
	 * so this reproduces the previous output byte-for-byte. */
	initStringInfo(&json);
	appendStringInfoChar(&json, '[');

	for (int i = 0; i < pm->n_pk; i++)
	{
		AttrNumber	attno = pm->cols[i].attno;
		AttrNumber	slot_idx;
		Oid			typid;
		Datum		val;
		bool		isnull;

		if (i > 0)
			appendStringInfoString(&json, ", ");

		slot_idx = rel_attno_to_slot_idx(slot->tts_tupleDescriptor, attno);
		if (slot_idx == 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("overlay_serialize_pk: cannot find PK column (attno=%d) inside UPDATE/DELETE junk-column slot (natts=%d)",
							attno, slot->tts_tupleDescriptor->natts)));

		typid = getBaseType(pm->cols[i].typid);
		val = slot_getattr(slot, slot_idx, &isnull);
		if (isnull)
		{
			appendStringInfoString(&json, "null");
			continue;
		}

		{
			Oid			outfuncoid;
			bool		typeIsVarlena;
			char	   *valstr;
			Datum		outval = val;
			bool		have_trimmed = false;

			/* Canonicalize NUMERIC to a scale-invariant rendering so that
			 * two PK values that are '='-equal but print with a different
			 * dscale (e.g. 1.0 vs 1.00 on a NUMERIC column WITHOUT a
			 * typmod) collapse to ONE identity key.  Without this the delta
			 * PK (branch_id, relid, key) would hold two rows for a single
			 * logical row and every text-key probe -- R21 delta pre-check,
			 * UPSERT Phase I pure-delta probe, the delta O(1) lookup in
			 * BranchScan and overlay_delta_lookup() -- would miss the
			 * shadowing row, so a branch INSERT of an '='-equal PK would
			 * silently succeed instead of raising the native 23505.
			 * BPCHAR gets an equivalent r-trim below; both mirror the
			 * `CAST(... AS <pktype>)::text` normalization the const-side
			 * serializer (overlay_serialize_pk_from_single_datum) applies,
			 * so the two producers stay byte-identical. */
			if (typid == NUMERICOID)
			{
				outval = DirectFunctionCall1(numeric_trim_scale, val);
				have_trimmed = true;
			}
			else if (typid == FLOAT8OID)
			{
				/* -0.0 and +0.0 are '='-equal (float8eq) but render "-0"
				 * and "0"; collapse -0.0 to +0.0 so the two spellings
				 * share one identity key. */
				if (DatumGetFloat8(val) == 0.0)
					outval = Float8GetDatum((float8) 0.0);
			}
			else if (typid == FLOAT4OID)
			{
				if (DatumGetFloat4(val) == 0.0)
					outval = Float4GetDatum((float4) 0.0);
			}

			getTypeOutputInfo(typid, &outfuncoid, &typeIsVarlena);
			valstr = OidOutputFunctionCall(outfuncoid, outval);
			/* BPCHAR r-trim: PG bpchar→text cast silently strips trailing
			 * blanks, so P0 fast-path (which casts via ::text) would
			 * produce 'AB12' for a char(8) value 'AB12    '.  To match we
			 * r-trim the WRITE-side output string (bpcharout keeps the
			 * full N-blank-padded length).  Since bpchar equality ignores
			 * trailing blanks anyway, this preserves PK semantics. */
			if (typid == BPCHAROID)
			{
				int			len = strlen(valstr);

				while (len > 0 && valstr[len - 1] == ' ')
					valstr[--len] = '\0';
			}
			escape_json(&json, valstr);
			pfree(valstr);
			if (have_trimmed)
				pfree(DatumGetPointer(outval));
		}
	}
	appendStringInfoChar(&json, ']');

	oldmc = MemoryContextSwitchTo(TopMemoryContext);
	result = pstrdup(json.data);
	MemoryContextSwitchTo(oldmc);
	pfree(json.data);
	return result;
}

/*
 * overlay_serialize_pk
 *
 *	Canonical PK identity text.  When any PK column's output function
 *	reads a session GUC, pin the canonical identity GUCs around the
 *	serialization so every session renders byte-identical keys (see the
 *	"Canonical identity GUC context" note above).  PG_FINALLY restores
 *	the GUCs on any error path, so an error swallowed by an inner
 *	PG_CATCH can never leave the session pinned to UTC/ISO.
 */
char *
overlay_serialize_pk(Relation rel, TupleTableSlot *slot)
{
	ObPkRelMeta *pm;
	char	   *result = NULL;
	int			guc_nl;

	Assert(rel != NULL && slot != NULL);

	/* RD-4: one cached catalog lookup instead of RelationGetIndexList()
	 * + index_open() on every row. */
	pm = ob_pk_rel_meta(rel);
	if (!pm->has_pk || pm->n_pk <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_serialize_pk requires a primary key on %s",
						RelationGetRelationName(rel))));

	if (!pm->guc_sensitive)
		return ob_serialize_pk_body(rel, slot, pm);

	guc_nl = ob_canonical_guc_pin();
	PG_TRY();
	{
		result = ob_serialize_pk_body(rel, slot, pm);
	}
	PG_FINALLY();
	{
		ob_canonical_guc_unpin(guc_nl);
	}
	PG_END_TRY();

	return result;
}

/*
 * ob_serialize_tuple_body
 *
 *	Relation-shape → JSON payload text.  Pure with respect to GUCs only
 *	when every column type is GUC-insensitive; overlay_serialize_tuple
 *	below pins the canonical GUCs otherwise.
 */
static char *
ob_serialize_tuple_body(Relation rel, TupleTableSlot *slot)
{
	TupleDesc	reldesc;
	int			natts;
	StringInfoData jsonbuf;
	StringInfoData scratch;
	bool		first;
	int			len;
	const char *data;
	char	   *result;

	Assert(rel != NULL && slot != NULL);
	ExecMaterializeSlot(slot);
	if (TTS_IS_VIRTUAL(slot))
	{
		TupleDesc	std = slot->tts_tupleDescriptor;
		if (std && slot->tts_nvalid < std->natts)
			slot->tts_nvalid = std->natts;
	}

	reldesc = RelationGetDescr(rel);
	natts = reldesc->natts;

	initStringInfo(&jsonbuf);
	initStringInfo(&scratch);
	appendStringInfoChar(&jsonbuf, '{');
	first = true;

	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(reldesc, i);
		Datum		d;
		bool		isnull_col;

		if (att->attisdropped)
			continue;

		if (!first)
			appendStringInfoChar(&jsonbuf, ',');
		first = false;

		{
			const char *aname = NameStr(att->attname);

			appendStringInfoChar(&jsonbuf, '"');
			for (const char *p = aname; *p != '\0'; p++)
			{
				if (*p == '"' || *p == '\\')
					appendStringInfoChar(&jsonbuf, '\\');
				appendStringInfoChar(&jsonbuf, *p);
			}
			appendStringInfoChar(&jsonbuf, '"');
			appendStringInfoChar(&jsonbuf, ':');
		}

		/* use NameStr to match att→attnum → slot index, avoiding junk-attr misalignment */
		{
			AttrNumber	slot_idx = rel_attno_to_slot_idx(
									  slot->tts_tupleDescriptor,
									  att->attnum);

			if (slot_idx == 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("overlay_serialize_tuple: cannot find user column attno=%d name=%s inside UPDATE/DELETE junk-column slot (slot_natts=%d)",
								att->attnum, NameStr(att->attname),
								slot->tts_tupleDescriptor->natts)));

			d = slot_getattr(slot, slot_idx, &isnull_col);
		}
		if (isnull_col)
		{
			appendStringInfoString(&jsonbuf, "null");
		}
		else
		{
			Oid			typid = att->atttypid;
			Oid			outfuncoid;
			bool		typeIsVarlena;
			char	   *valstr;
			const char *vp;

			getTypeOutputInfo(typid, &outfuncoid, &typeIsVarlena);
			valstr = OidOutputFunctionCall(outfuncoid, d);

			resetStringInfo(&scratch);
			appendStringInfoChar(&scratch, '"');
			for (vp = valstr; *vp != '\0'; vp++)
			{
				unsigned char c = (unsigned char) *vp;

				switch (c)
				{
					case '"':	appendStringInfoString(&scratch, "\\\""); break;
					case '\\':	appendStringInfoString(&scratch, "\\\\"); break;
					case '\n':	appendStringInfoString(&scratch, "\\n");  break;
					case '\r':	appendStringInfoString(&scratch, "\\r");  break;
					case '\t':	appendStringInfoString(&scratch, "\\t");  break;
					case '\b':	appendStringInfoString(&scratch, "\\b");  break;
					case '\f':	appendStringInfoString(&scratch, "\\f");  break;
					default:
						if (c < 0x20)
							appendStringInfo(&scratch, "\\u%04x", c);
						else
							appendStringInfoChar(&scratch, (char) c);
				}
			}
			appendStringInfoChar(&scratch, '"');
			appendStringInfoString(&jsonbuf, scratch.data);
			pfree(valstr);
		}
	}
	appendStringInfoChar(&jsonbuf, '}');
	pfree(scratch.data);

	data = jsonbuf.data;
	len  = jsonbuf.len;
	{
		MemoryContext oldmc;

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		result = (char *) palloc((Size) len + 1);
		if (len > 0)
			memcpy(result, data, (Size) len);
		result[len] = '\0';
		MemoryContextSwitchTo(oldmc);
	}

	ereport(DEBUG1,
			(errmsg_internal("OST[dbg] json=%s len=%d",
							 result, len)));

	pfree(jsonbuf.data);
	return result;
}

/*
 * overlay_serialize_tuple
 *
 *	Serialized payload text is re-parsed by the reading session's input
 *	functions, so when any column type's output fails to round-trip across a
 *	differently-configured session (see ob_payload_type_roundtrip_unsafe) we
 *	pin the canonical GUCs around the body.  PG_FINALLY restores them on any
 *	error path.  Tables with only GUC-safe columns skip the pin entirely.
 */
char *
overlay_serialize_tuple(Relation rel, TupleTableSlot *slot)
{
	TupleDesc	reldesc;
	int			i;
	bool		sensitive = false;
	char	   *result = NULL;
	int			guc_nl;

	Assert(rel != NULL && slot != NULL);

	reldesc = RelationGetDescr(rel);
	for (i = 0; i < reldesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(reldesc, i);

		if (att->attisdropped)
			continue;
		if (ob_payload_type_roundtrip_unsafe(att->atttypid))
		{
			sensitive = true;
			break;
		}
	}

	if (!sensitive)
		return ob_serialize_tuple_body(rel, slot);

	guc_nl = ob_canonical_guc_pin();
	PG_TRY();
	{
		result = ob_serialize_tuple_body(rel, slot);
	}
	PG_FINALLY();
	{
		ob_canonical_guc_unpin(guc_nl);
	}
	PG_END_TRY();

	return result;
}

/* ================================================================
 * Base-version token codec (A1/A6 stage 2) — see include/overlay_branch.h
 * for the contract.  This is the single source of the "<blk>:<off>-x<xmin>"
 * CAS token used by R15/R18: the write path captures it through
 * overlay_tuple_version() below, apply re-derives it through
 * ob_fetch_main_version() (branch_lifecycle.c), and the read path parses it
 * back through ob_anchor_is_stale() (branch_scan.c).  Keeping format and
 * parse in one place is what stops the producer and the consumers drifting
 * apart.
 * ================================================================ */

bool
overlay_base_ver_parse_ctid(const char *ctid_txt, uint32 *blk, uint32 *off)
{
	if (ctid_txt == NULL || blk == NULL || off == NULL)
		return false;

	return sscanf(ctid_txt, "(%u,%u)", blk, off) == 2;
}

void
overlay_base_ver_format(char *buf, size_t buflen, uint32 blk, uint32 off,
						const char *xmin_txt)
{
	/* xmin_txt is NULL only when the caller could not read xmin; "0" keeps
	 * the token shape parseable so the mismatch is still detected. */
	snprintf(buf, buflen, "%u:%u-x%s", blk, off, xmin_txt ? xmin_txt : "0");
}

bool
overlay_base_ver_parse_xmin(const char *token, TransactionId *xmin_out)
{
	const char *dash;
	unsigned long parsed;

	if (token == NULL || xmin_out == NULL)
		return false;

	dash = strrchr(token, '-');
	if (dash == NULL || dash[1] != 'x' || dash[2] == '\0')
		return false;
	if (sscanf(dash + 2, "%lu", &parsed) != 1)
		return false;

	*xmin_out = (TransactionId) parsed;
	return true;
}

/* ================================================================
 * overlay_tuple_version: build a comparable version string from physical ctid + xmin
 * ================================================================ */
char *
overlay_tuple_version(Relation rel, TupleTableSlot *slot)
{
	int			ret;
	char	   *sql;
	char	   *ctid_cstr;
	char	   *result = NULL;
	Datum		values[1];
	static const Oid argtypes[1] = {TEXTOID};

	if (rel == NULL || slot == NULL)
		return NULL;

	ctid_cstr = slot_get_ctid_cstr(slot);
	if (ctid_cstr == NULL)
	{
		ItemPointer ip = &(slot->tts_tid);

		if (!ItemPointerIsValid(ip) ||
			ItemPointerGetBlockNumber(ip) == InvalidBlockNumber ||
			ItemPointerGetOffsetNumber(ip) == 0)
			return NULL;
		ctid_cstr = (char *) palloc(32);
		snprintf(ctid_cstr, 32, "(%u,%u)",
				 ItemPointerGetBlockNumber(ip),
				 ItemPointerGetOffsetNumber(ip));
	}

	overlay_overlay_helper_enter();

	/* WR-1a: ctid bound as $1; relation name stays inline (plan-cache key). */
	sql = psprintf("SELECT ctid::text, xmin::text FROM %s WHERE ctid = $1::tid LIMIT 1",
				   quote_qualified_identifier(
					   get_namespace_name(RelationGetNamespace(rel)),
					   RelationGetRelationName(rel)));
	values[0] = CStringGetTextDatum(ctid_cstr);

	ret = ob_spi_exec_params(sql, true, 1, 1, argtypes, values, NULL);
	pfree(sql);
	pfree(DatumGetPointer(values[0]));

	if (ret == SPI_OK_SELECT && SPI_processed == 1 &&
		SPI_tuptable && SPI_tuptable->vals && SPI_tuptable->vals[0])
	{
		bool		isnull1, isnull2;
		char	   *ctid_txt;
		char	   *xmin_txt;
		char		token[OB_BASE_VER_MAX];
		uint32		blk, off;

		ctid_txt = SPI_getvalue(SPI_tuptable->vals[0],
								SPI_tuptable->tupdesc, 1);
		xmin_txt = SPI_getvalue(SPI_tuptable->vals[0],
								SPI_tuptable->tupdesc, 2);
		isnull1 = (ctid_txt == NULL);
		isnull2 = (xmin_txt == NULL);

		if (!isnull1 && !isnull2 &&
			overlay_base_ver_parse_ctid(ctid_txt, &blk, &off))
		{
			MemoryContext oldmc3;

			overlay_base_ver_format(token, sizeof(token), blk, off, xmin_txt);

			oldmc3 = MemoryContextSwitchTo(TopMemoryContext);
			result = pstrdup(token);
			MemoryContextSwitchTo(oldmc3);
		}
	}

	SPI_finish();
	overlay_overlay_helper_exit();
	if (ctid_cstr)
		pfree(ctid_cstr);
	return result;
}

/* ================================================================
 * overlay_should_redirect: fast decision gate for write redirect
 * ================================================================ */

/* A1/A6: branch id in effect for the current statement (bound once in
 * overlay_ExecutorStart).  Falls back to the live global when no statement
 * context is bound. */
static int32
ob_stmt_branch_id(void)
{
	BranchExecContext *ectx = ob_get_exec_context();

	return (ectx != NULL) ? ectx->bid : overlay_branch_get_current_id();
}

bool
overlay_should_redirect(Relation rel)
{
	Oid			nspoid;
	char	   *nspname;

	/* Layer 1: apply's internal real MAIN write replay must always be bypassed, otherwise the write is consumed back into delta in an infinite loop */
	if (overlay_in_apply_operation())
		return false;
	/* is_active() stays live: it also runs the FR4 kickout check, which must
	 * be able to disable redirection mid-statement. */
	if (!overlay_branch_is_active())
		return false;
	/* A1/A6: the branch written to is the one bound for this statement. */
	if (ob_stmt_branch_id() <= 0)
		return false;
	if (rel == NULL)
		return false;
	if (rel->rd_rel->relisshared)
		return false;

	/* Layer 2: never store writes to overlay_branch.* catalogs as delta again */
	nspoid = RelationGetNamespace(rel);
	nspname = get_namespace_name(nspoid);
	if (nspname != NULL && strcmp(nspname, OBSCHEMA) == 0)
		return false;

	/* Layer 3: only regular heap tables */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		return false;

	/* Layer 4: reject by default when there is no PK (Guard G5 already ERRORs at the ExecutorRun entry; a conservative belt-and-braces here).
	 * Allow under the experimental keyless mode, and emit the one-shot §2.3 WARNING the first time. */
	if (!overlay_relation_has_pk(rel))
	{
		if (!overlay_keyless_tables)
			return false;
		overlay_keyless_warn_once(rel);
	}

	return true;
}

/* ================================================================
 * overlay_modify_insert/update/delete — Write Redirect → DeltaStore bridges
 * ================================================================ */
void
overlay_modify_insert(Relation rel, TupleTableSlot *slot)
{
	int32		bid = ob_stmt_branch_id();
	char	   *pk;
	char	   *tdata;

	pk = overlay_serialize_pk(rel, slot);
	tdata = overlay_serialize_tuple(rel, slot);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_INSERT, NULL, tdata);
	pfree(pk);
	pfree(tdata);
}

/* ob_enrich_tuple_with_base_image: attach "_base" top-level key to the
 * new-image JSON produced by overlay_serialize_tuple().  Used for
 * UPDATE/DELETE deltas so apply can compare the persisted MAIN base
 * image with the currently-locked MAIN row.  INSERTs carry no base
 * image (they have no MAIN baseline to compare against).
 *
 * Produces output like:  {"c1":"v1","c2":"v2","_base":{"c1":"old1","c2":"old2"}}
 * Caller is responsible for pfree()'ing the returned palloc'd cstring. */
static char *
ob_enrich_tuple_with_base_image(const char *new_image_json,
								const char *base_image_json)
{
	StringInfoData out;
	const char *p;
	bool		inside_string;
	bool		escaped;
	bool		object_closed;
	int			depth;

	if (new_image_json == NULL)
		return NULL;
	if (base_image_json == NULL || *base_image_json == '\0')
		return pstrdup(new_image_json);

	initStringInfo(&out);

	/* Walk the new-image JSON up to its final '}', strip a trailing '}',
	 * append ,"_base":<base_json>, then close with '}'.  We verify strict
	 * object syntax by tracking brace depth and string/escape state. */
	inside_string = false;
	escaped = false;
	depth = 0;
	object_closed = false;
	for (p = new_image_json; *p != '\0'; p++)
	{
		char c = *p;

		if (inside_string)
		{
			appendStringInfoChar(&out, c);
			if (escaped)
				escaped = false;
			else if (c == '\\')
				escaped = true;
			else if (c == '"')
				inside_string = false;
			continue;
		}
		switch (c)
		{
			case '"':
				inside_string = true;
				appendStringInfoChar(&out, c);
				break;
			case '{':
				depth++;
				appendStringInfoChar(&out, c);
				break;
			case '}':
				depth--;
				if (depth == 0)
				{
					/* Replace the final closing '}' with our enriched tail. */
					appendStringInfoString(&out, ",\"_base\":");
					appendStringInfoString(&out, base_image_json);
					appendStringInfoChar(&out, '}');
					object_closed = true;
				}
				else
					appendStringInfoChar(&out, c);
				break;
			default:
				appendStringInfoChar(&out, c);
				break;
		}
	}

	if (!object_closed)
	{
		/* Malformed input — fall back to returning original without base. */
		pfree(out.data);
		return pstrdup(new_image_json);
	}
	return out.data;
}

void
overlay_modify_update(Relation rel, TupleTableSlot *oldslot,
					  TupleTableSlot *newslot)
{
	int32		bid = ob_stmt_branch_id();
	char	   *pk;
	char	   *oldver;
	char	   *tdata_new;
	char	   *tdata_base;
	char	   *tdata_enriched;

	pk = overlay_serialize_pk(rel, newslot);
	oldver = overlay_tuple_version(rel, oldslot);
	tdata_new = overlay_serialize_tuple(rel, newslot);
	tdata_base = overlay_serialize_tuple(rel, oldslot);
	tdata_enriched = ob_enrich_tuple_with_base_image(tdata_new, tdata_base);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_UPDATE, oldver, tdata_enriched);
	pfree(pk);
	if (oldver) pfree(oldver);
	pfree(tdata_new);
	pfree(tdata_base);
	pfree(tdata_enriched);
}

void
overlay_modify_delete(Relation rel, TupleTableSlot *slot)
{
	int32		bid = ob_stmt_branch_id();
	char	   *pk;
	char	   *oldver;
	char	   *base_json;
	char	   *sentinel_new;
	char	   *tdata_enriched;

	pk = overlay_serialize_pk(rel, slot);
	oldver = overlay_tuple_version(rel, slot);
	base_json = overlay_serialize_tuple(rel, slot);

	/* DELETEs carry no new-image columns.  We still want the base image
	 * attached for apply conflict detection, so we synthesise an empty
	 * new-image object {"_deleted":true} and enrich it with _base.  The
	 * apply INSERT fallback path already guards on dt->op; jsonb_populate_
	 * record() ignores extra keys it doesn't recognise, so the sentinel
	 * will not corrupt existing reconstructions. */
	sentinel_new = pstrdup("{\"_deleted\":true}");
	tdata_enriched = ob_enrich_tuple_with_base_image(sentinel_new, base_json);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_DELETE, oldver, tdata_enriched);
	pfree(pk);
	if (oldver) pfree(oldver);
	pfree(base_json);
	pfree(sentinel_new);
	pfree(tdata_enriched);
}

/* ================================================================
 * overlay_debug_delta_*: Step 2c smoke debug wrappers
 * ================================================================ */

#include "fmgr.h"
PG_FUNCTION_INFO_V1(overlay_debug_delta_insert);
PG_FUNCTION_INFO_V1(overlay_debug_delta_count);
PG_FUNCTION_INFO_V1(overlay_debug_delta_delete_all);

Datum
overlay_debug_delta_insert(PG_FUNCTION_ARGS)
{
	int32		branch_id   = PG_GETARG_INT32(0);
	Oid			relid       = PG_GETARG_OID(1);
	text	   *key_txt     = PG_GETARG_TEXT_PP(2);
	char		op          = PG_GETARG_CHAR(3);
	const char *old_version = NULL;
	bytea	   *tuple_data  = NULL;
	char	   *key;

	elog(DEBUG1, "ODI_ENTRY: branch=%d rel=%u op=%c", branch_id, relid, op);

	if (!PG_ARGISNULL(4))
		old_version = text_to_cstring(PG_GETARG_TEXT_PP(4));

	if (!PG_ARGISNULL(5))
		tuple_data = PG_GETARG_BYTEA_PP(5);

	key = text_to_cstring(key_txt);
	if (key == NULL || *key == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("overlay_debug_delta_insert: key must not be empty")));

	elog(DEBUG1, "overlay_debug_delta_insert: branch=%d rel=%u key='%s' op='%c' oldver=%s tdatalen=%d",
		 branch_id, relid, key, op,
		 old_version ? old_version : "(null)",
		 tuple_data ? (int) VARSIZE_ANY_EXHDR(tuple_data) : -1);

	{
		const char *tuple_json_cstr = NULL;

		if (tuple_data != NULL)
		{
			Size payload_len = VARSIZE_ANY_EXHDR(tuple_data);
			const unsigned char *payload = (const unsigned char *) VARDATA_ANY(tuple_data);

			tuple_json_cstr = (const char *) palloc(payload_len + 1);
			if (payload_len > 0)
				memcpy(unconstify(char *, tuple_json_cstr), payload, payload_len);
			unconstify(char *, tuple_json_cstr)[payload_len] = '\0';
		}

		overlay_delta_insert(branch_id, relid, key, op, old_version, tuple_json_cstr);
		PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
	}
}

Datum
overlay_debug_delta_count(PG_FUNCTION_ARGS)
{
	int32		branch_id = PG_GETARG_INT32(0);
	int64		n;

	n = overlay_delta_count(branch_id);
	PG_RETURN_INT64(n);
}

Datum
overlay_debug_delta_delete_all(PG_FUNCTION_ARGS)
{
	int32		branch_id = PG_GETARG_INT32(0);

	overlay_delta_delete_all(branch_id);
	PG_RETURN_VOID();
}

/* ================================================================
 * Planner-side PK helper functions (added for BranchScan PK IndexScan adaptation)
 * ================================================================ */

/*
 * overlay_get_pk_single_attno
 *
 *   Fast return (attno + column name of the single-column PRIMARY KEY.
 *   Composite PK (indnatts>1) returns false and the Planner takes the full path.
 *   out_pk_attno is the 1-based real attno (matching rd_index->indkey.values[i]);
 *   out_pk_colname points to NameData inside the relcache (caller must not pfree).
 */
bool
overlay_get_pk_single_attno(Relation rel,
							AttrNumber *out_pk_attno,
							const char **out_pk_colname)
{
	ObPkRelMeta *pm;
	AttrNumber	pk_attno;
	TupleDesc	reldesc;
	Form_pg_attribute pkatt;

	if (rel == NULL)
		return false;
	if (out_pk_attno) *out_pk_attno = 0;
	if (out_pk_colname) *out_pk_colname = NULL;

	pm = ob_pk_rel_meta(rel);

	/* only single-column PK supported */
	if (!pm->has_pk || pm->n_pk != 1)
		return false;

	pk_attno = pm->cols[0].attno;
	if (pk_attno <= 0)
		return false;

	reldesc = RelationGetDescr(rel);
	if (pk_attno > reldesc->natts)
		return false;

	pkatt = TupleDescAttr(reldesc, pk_attno - 1);
	if (out_pk_attno)
		*out_pk_attno = pk_attno;
	if (out_pk_colname)
		*out_pk_colname = NameStr(pkatt->attname);
	return true;
}

/*
 * overlay_get_pk_attnos_list
 *
 *   Returns a list of 1-based attno integers for every column in the
 *   relation's PRIMARY KEY (supports composite PKs).  Returns NIL if
 *   the relation has no primary key.
 *
 *   Caller is responsible for list_free_deep() / pfree-ing the result
 *   (a List in CurrentMemoryContext).
 *
 *   This is used by write_redirect.c R03 PK-mutation guard to detect
 *   when a SET targetlist includes ANY of the PK columns. */
List *
overlay_get_pk_attnos_list(Relation rel)
{
	List	   *result = NIL;
	ObPkRelMeta *pm;
	int			i;

	if (rel == NULL)
		return NIL;

	pm = ob_pk_rel_meta(rel);
	if (!pm->has_pk)
		return NIL;

	for (i = 0; i < pm->n_pk; i++)
	{
		AttrNumber	attno = pm->cols[i].attno;

		if (attno > 0)
			result = lappend_int(result, attno);
	}

	return result;
}
/*
 * overlay_serialize_pk_from_single_datum
 *
 *   From a single Datum (single-column PK case) build, like overlay_serialize_pk,
 *   a fully byte-compatible JSON array ::text serialization string.
 *
 *   CRITICAL TYPE COERCION NOTE (see doc/internals.md §2.3):
 *   The caller passes `con->constvalue` with its OWN declared type
 *   `consttype` (as the parser saw it), which is almost never equal to
 *   `pkatt->atttypid` in real queries:
 *
 *     CREATE TABLE t(id bigint PRIMARY KEY);
 *     SELECT * FROM t WHERE id = 2;  -- Const.consttype = INT4OID !!
 *
 *   Calling getTypeOutputInfo(INT8OID) + OidOutputFunctionCall on a
 *   4-byte int4 Datum makes int8out deref 8 bytes, where the upper 4
 *   bytes are stack garbage → key becomes "[\"140703...\"]" while the
 *   slot-side overlay_serialize_pk() correctly produces "[\"2\"]" →
 *   O(1) delta lookup misses ALL UPDATE/DELETE tombstones for the row.
 *
 *   FIX (2 stages, coercion-safe):
 *     Stage 1: stringify the literal with ITS OWN type output fn
 *       4-byte int4 Datum → "2"; varchar Datum → "alpha"; etc.
 *       Dereference size always matches consttype's layout → no garbage.
 *     Stage 2: SQL-level
 *       CAST ('<literal_str>' AS <pk_coltype>) ::text
 *       Let PG's parser itself perform the canonical coercion (int4 →
 *       int8, bpchar blank-padding, numeric rounding, custom type casts
 *       …).  The resulting ::text representation is byte-identical to
 *       what the WRITE side emits for the same stored-in-table value.
 */
static char *
ob_serialize_pk_single_datum_body(Relation rel,
								  AttrNumber pk_attno,
								  Datum pk_val,
								  bool pk_isnull,
								  Oid consttype)
{
	TupleDesc	reldesc;
	Form_pg_attribute pkatt;
	Oid			pk_typid;
	StringInfoData arr_sql;
	int			ret;
	char	   *result;
	MemoryContext oldmc;

	Assert(rel != NULL);
	reldesc = RelationGetDescr(rel);
	pkatt = TupleDescAttr(reldesc, pk_attno - 1);
	pk_typid = pkatt->atttypid;

	initStringInfo(&arr_sql);
	appendStringInfoString(&arr_sql, "SELECT to_jsonb(ARRAY[");

	if (pk_isnull)
	{
		appendStringInfoString(&arr_sql, "NULL::text");
	}
	else
	{
		Oid			lit_outfuncoid;
		bool		lit_typeIsVarlena;
		char	   *litstr;
		char	   *q;
		const char *pktypname;
		Oid			base_typid;

		/* Stage 1: use the CONST'S OWN type to stringify its Datum. */
		getTypeOutputInfo(consttype, &lit_outfuncoid, &lit_typeIsVarlena);
		litstr = OidOutputFunctionCall(lit_outfuncoid, pk_val);

		/* Stage 2: SQL-level literal quote + CAST onto PK type.
		 *
		 * IMPORTANT: we MUST cast to ::text after the CAST onto the real
		 * PK type, so that to_jsonb(ARRAY[...]) treats every element as
		 * TEXT — matching overlay_serialize_pk() (the WRITE side), which
		 * always uses `quote_literal_cstr(OidOutputFunctionCall(...))
		 * ::text` per element.  Without this trailing `::text`, numeric
		 * types would be JSON *numbers* (`[10]`) vs WRITE-side JSON
		 * *strings* (`["10"]`) → byte mismatch → every P0 O(1) lookup
		 * misses!
		 *
		 * BUG#1 BPCHAR CAVEAT: PG's bpchar→text cast silently r-trims
		 * trailing blanks, so CAST('AB12' AS char(8))::text yields
		 * 'AB12' (4 chars).  To match, the WRITE-side datum_out-based
		 * bpchar value is also r-trimmed before jsonb-ification (see
		 * overlay_serialize_pk's "BPCHAR r-trim" comment).  Since PG
		 * treats 'AB12'::char(8) = 'AB12    '::char(8) as true anyway,
		 * r-trimming on both sides preserves PK equality semantics. */
		q = quote_literal_cstr(litstr);
		pktypname = format_type_with_typemod(pk_typid, pkatt->atttypmod);

		/* The branch selection below must look through a domain: a domain over
		 * numeric / float8 carries its own OID but the base type's semantics.
		 * pktypname stays the domain name so the CAST still applies the
		 * domain's own typmod rounding before the normalization. */
		base_typid = getBaseType(pk_typid);

		/* NUMERIC: apply the same scale-invariant normalization the WRITE
		 * side performs (numeric_trim_scale in overlay_serialize_pk), so a
		 * const probe key for 1.0 / 1.00 matches the stored identity key on
		 * a typmod-less NUMERIC PK.  CAST(q AS numeric(10,2)) still applies
		 * any column typmod rounding/padding FIRST; trim_scale then drops
		 * the trailing zeros. */
		if (base_typid == NUMERICOID)
			appendStringInfo(&arr_sql,
							 "CAST (pg_catalog.trim_scale(CAST (%s AS %s)) AS text)",
							 q, pktypname);
		else if (base_typid == FLOAT8OID || base_typid == FLOAT4OID)
			/* Collapse -0.0 to +0.0: adding +0.0 does exactly that in
			 * IEEE, matching the WRITE-side float normalization so the
			 * "-0"/"0" spellings share one identity key. */
			appendStringInfo(&arr_sql,
							 "CAST ((CAST (%s AS %s) + %s '0') AS text)",
							 q, pktypname, pktypname);
		else
			appendStringInfo(&arr_sql, "CAST (%s AS %s)::text",
							 q, pktypname);
		pfree(q);
		pfree(litstr);
	}
	appendStringInfoString(&arr_sql, "])::text");

	ret = ob_spi_one_shot(arr_sql.data, true, 1);
	pfree(arr_sql.data);

	if (ret != SPI_OK_SELECT || SPI_processed != 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_serialize_pk_from_single_datum: SPI to_jsonb failed (ret=%d rows=%lu)",
						ret, (unsigned long) SPI_processed)));
	}
	{
		const char *spival = SPI_getvalue(SPI_tuptable->vals[0],
										  SPI_tuptable->tupdesc, 1);

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		result = pstrdup(spival != NULL ? spival : "");
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	return result;
}

/*
 * overlay_serialize_pk_from_single_datum
 *
 *	Canonical identity text for a single-column const probe.  When the PK
 *	column type's output depends on a session GUC (date/time family,
 *	float, bytea) the CAST(...)::text below would render differently per
 *	session, so pin the canonical identity GUCs around the SPI call (see
 *	the "Canonical identity GUC context" note).  PG_FINALLY restores them
 *	on any error path.
 */
char *
overlay_serialize_pk_from_single_datum(Relation rel,
									   AttrNumber pk_attno,
									   Datum pk_val,
									   bool pk_isnull,
									   Oid consttype)
{
	Oid			pk_typid;
	char	   *result = NULL;
	int			guc_nl;

	Assert(rel != NULL);
	pk_typid = TupleDescAttr(RelationGetDescr(rel), pk_attno - 1)->atttypid;

	if (!ob_identity_type_is_guc_sensitive(pk_typid))
		return ob_serialize_pk_single_datum_body(rel, pk_attno, pk_val,
												 pk_isnull, consttype);

	guc_nl = ob_canonical_guc_pin();
	PG_TRY();
	{
		result = ob_serialize_pk_single_datum_body(rel, pk_attno, pk_val,
												   pk_isnull, consttype);
	}
	PG_FINALLY();
	{
		ob_canonical_guc_unpin(guc_nl);
	}
	PG_END_TRY();

	return result;
}

/* ====================================================================
 * ===== Typed PK helpers — REVIEW-260926/R14
 *
 * Perform semantic PostgreSQL-native equality/ordering on the real
 * primary-key column types instead of JSON-text strcmp on dt->key.
 * strcmp is WRONG for BPCHAR (r-trim semantics), NUMERIC precision
 * (12.3 vs 12.30 are numerically equal), and TIMESTAMPTZ UTC absolute
 * time vs textual-offset representations.  The typed-pk path always
 * wins when available; callers fall back to legacy strcmp ONLY when
 * the cache has not been populated (old/direct-call paths).
 *
 * All helpers follow ISO C90: variables declared at block top, no
 * mixed declarations, no goto across initialisations.
 * ====================================================================
 */

TypedKey *
overlay_build_typed_pk_from_slot(Relation rel,
								 TupleTableSlot *slot,
								 int *out_n_pk)
{
	ObPkRelMeta *pm;
	int			n_pk;
	int			i;
	TypedKey  *result;
	MemoryContext oldmc;

	Assert(rel != NULL && slot != NULL && out_n_pk != NULL);

	pm = ob_pk_rel_meta(rel);
	if (!pm->has_pk)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_build_typed_pk_from_slot requires a primary key on %s",
						RelationGetRelationName(rel))));
	n_pk = pm->n_pk;
	if (n_pk <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_build_typed_pk_from_slot found 0-column PK on %s",
						RelationGetRelationName(rel))));

	/* ---------------------------------------------------------------
	 * R14 hardening: all allocations (TypedKey array + per-column
	 * datum deep copies) MUST live in TopMemoryContext, otherwise
	 * they could end up inside a SPI_proc context (when this helper
	 * is called from within a SPI-connected code path) and get
	 * freed on SPI_finish — producing the classic
	 * "pfree called with invalid pointer 0x... (header 0x7f..7f)"
	 * crash when the caller later releases the returned array.
	 * --------------------------------------------------------------- */
	oldmc = MemoryContextSwitchTo(TopMemoryContext);
	result = (TypedKey *) palloc0(sizeof(TypedKey) * n_pk);
	MemoryContextSwitchTo(oldmc);

	for (i = 0; i < n_pk; i++)
	{
		ObPkColMeta *col = &pm->cols[i];
		AttrNumber	attno = col->attno;
		TypedKey  *tk;
		bool		attisnull;
		Datum		d;

		if (attno == 0)
		{
			overlay_typed_pk_free(result, i);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay typed pk does not support expression index "
							"attno=0 on column %d of relation \"%s\"",
							i, RelationGetRelationName(rel))));
		}
		tk = &result[i];
		tk->typid = col->typid;
		tk->typmod = col->typmod;
		tk->collid = col->collid;
		tk->typlen = col->typlen;
		tk->typbyval = col->typbyval;
		tk->eq_finfo = col->eq_finfo;
		tk->lt_finfo = col->lt_finfo;

		/* Project memory rule (see project_memory.md §2.3): strictly
		 * forbid calling slot_getattr on a TTSOpsVirtual slot whose
		 * tts_nvalid trails the requested attno.  Either materialise
		 * or ensure the virtual slot has been fully validated first.
		 * Here we use the portable slot_getsomeattrs() pre-condition
		 * (works for all TTSOps variants, not only virtual). */
		slot_getsomeattrs(slot, attno);
		d = slot_getattr(slot, attno, &attisnull);
		tk->isnull = attisnull;
		if (!attisnull)
		{
			/* datumCopy() into TopMemoryContext so the payload outlives
			 * any enclosing SPI / expression-eval context that may be
			 * active during this call. */
			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			tk->value = datumCopy(d, tk->typbyval, tk->typlen);
			MemoryContextSwitchTo(oldmc);
		}
		else
		{
			tk->value = (Datum) 0;
		}
	}

	*out_n_pk = n_pk;
	return result;
}

TypedKey *
overlay_build_typed_pk_from_key_text(Relation rel,
									 const char *key_json,
									 int *out_n_pk)
{
	ObPkRelMeta *pm;
	int			n_pk;
	int			i;
	TypedKey  *result;
	char	   *q_key;
	MemoryContext oldmc;

	Assert(rel != NULL && key_json != NULL && out_n_pk != NULL);

	pm = ob_pk_rel_meta(rel);
	if (!pm->has_pk)
	{
		/* Keyless (experimental ctid identity) relation: there is no
		 * typed PK representation to build.  Return NULL (n=0) so
		 * callers fall back to the text-key (strcmp) comparator,
		 * which is the correct order for the "A:"/"S:" anchor keys. */
		*out_n_pk = 0;
		return NULL;
	}
	n_pk = pm->n_pk;
	if (n_pk <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_build_typed_pk_from_key_text 0-col PK on %s",
						RelationGetRelationName(rel))));

	/* Build the TypedKey skeleton in TopMemoryContext so the returned
	 * payload outlives any SPI context (see build_from_slot rationale). */
	oldmc = MemoryContextSwitchTo(TopMemoryContext);
	result = (TypedKey *) palloc0(sizeof(TypedKey) * n_pk);
	MemoryContextSwitchTo(oldmc);

	q_key = quote_literal_cstr(key_json);

	for (i = 0; i < n_pk; i++)
	{
		ObPkColMeta *col = &pm->cols[i];
		AttrNumber	attno = col->attno;
		TypedKey  *tk;
		const char *pktypname;
		Datum		binval;
		bool		isnull;
		StringInfoData valsql;
		int			ret;

		if (attno == 0)
		{
			pfree(q_key);
			overlay_typed_pk_free(result, i);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("typed key expression-index PK not supported at col %d of %s",
							i, RelationGetRelationName(rel))));
		}
		tk = &result[i];
		tk->typid = col->typid;
		tk->typmod = col->typmod;
		tk->collid = col->collid;
		tk->typlen = col->typlen;
		tk->typbyval = col->typbyval;
		tk->eq_finfo = col->eq_finfo;
		tk->lt_finfo = col->lt_finfo;
		pktypname = format_type_with_typemod(tk->typid, tk->typmod);

		initStringInfo(&valsql);
		appendStringInfo(&valsql,
						 "SELECT (((%s::jsonb)->%d) #>> '{}'::text[])::%s",
						 q_key, i, pktypname);
		ret = ob_spi_one_shot(valsql.data, true, 1);
		pfree(valsql.data);
		if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
			SPI_tuptable == NULL || SPI_tuptable->vals == NULL)
		{
			/* NB: overlay_typed_pk_free already pfree's the TypedKey array
			 * itself after releasing the per-column datum payloads, so we
			 * MUST NOT call pfree(result) separately (would double-free
			 * the palloc chunk and corrupt the memory context). */
			overlay_typed_pk_free(result, i);
			SPI_finish();
			pfree(q_key);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("typed pk from key_text: SPI failed at col %d", i)));
		}
		binval = SPI_getbinval(SPI_tuptable->vals[0],
							   SPI_tuptable->tupdesc, 1, &isnull);
		tk->isnull = isnull;
		if (!isnull)
		{
			/* datumCopy into TopMemoryContext: the SPI_tuptable pool is
			 * released at SPI_finish below so any non-typbyval payload
			 * must be copied to a stable memory context NOW. */
			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			tk->value = datumCopy(binval, tk->typbyval, tk->typlen);
			MemoryContextSwitchTo(oldmc);
		}
		else
		{
			tk->value = (Datum) 0;
		}
		SPI_finish();
	}
	pfree(q_key);
	*out_n_pk = n_pk;
	return result;
}

/*
 * ob_typed_pk_cmp_legacy
 *
 *	Fallback single-column comparison used only when the memoised btree
 *	support functions are unavailable (RD-5: TypedKey.eq_finfo / lt_finfo
 *	== NULL, i.e. the PK type exposed no btree sort operators when the
 *	relation metadata was built).  This preserves the pre-RD-5 behaviour,
 *	including the ERROR raised for types without sort operators.
 */
static int
ob_typed_pk_cmp_legacy(const TypedKey *ta, const TypedKey *tb, int colidx)
{
	Oid			ltOpr = InvalidOid;
	Oid			eqOpr = InvalidOid;
	Oid			gtOpr = InvalidOid;
	HeapTuple	tup_eq;
	HeapTuple	tup_lt;
	Datum		d_eqfn;
	Datum		d_ltfn;
	bool		isnull_fn;
	Oid			fn_eq;
	Oid			fn_lt;

	get_sort_group_operators(ta->typid,
							 true, true, false,
							 &ltOpr, &eqOpr, &gtOpr,
							 NULL);
	if (!OidIsValid(eqOpr) || !OidIsValid(ltOpr))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_typed_pk_cmp: no btree sort operators "
						"for PK column type %u at col %d",
						ta->typid, colidx)));

	tup_eq = SearchSysCache1(OPEROID, ObjectIdGetDatum(eqOpr));
	if (!HeapTupleIsValid(tup_eq))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_typed_pk_cmp: eqOpr %u not in OPEROID cache at col %d",
						eqOpr, colidx)));
	d_eqfn = SysCacheGetAttr(OPEROID, tup_eq, 13, &isnull_fn);
	if (isnull_fn)
	{
		ReleaseSysCache(tup_eq);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_typed_pk_cmp: eqOpr %u has NULL oprcode at col %d",
						eqOpr, colidx)));
	}
	fn_eq = DatumGetObjectId(d_eqfn);
	ReleaseSysCache(tup_eq);

	tup_lt = SearchSysCache1(OPEROID, ObjectIdGetDatum(ltOpr));
	if (!HeapTupleIsValid(tup_lt))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_typed_pk_cmp: ltOpr %u not in OPEROID cache at col %d",
						ltOpr, colidx)));
	d_ltfn = SysCacheGetAttr(OPEROID, tup_lt, 13, &isnull_fn);
	if (isnull_fn)
	{
		ReleaseSysCache(tup_lt);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_typed_pk_cmp: ltOpr %u has NULL oprcode at col %d",
						ltOpr, colidx)));
	}
	fn_lt = DatumGetObjectId(d_ltfn);
	ReleaseSysCache(tup_lt);

	/* Equality short-circuit first. */
	if (DatumGetBool(OidFunctionCall2Coll(fn_eq, ta->collid,
										  ta->value, tb->value)))
		return 0;

	/* Not equal: strict less-than → -1, otherwise → +1. */
	return DatumGetBool(OidFunctionCall2Coll(fn_lt, ta->collid,
											 ta->value, tb->value))
		? -1 : 1;
}

int
overlay_typed_pk_cmp(const TypedKey *a, int a_n,
					 const TypedKey *b, int b_n)
{
	int			i;
	int			colcmp;

	if (a_n != b_n)
		return a_n < b_n ? -1 : 1;
	if (a_n == 0)
		return 0;

	for (i = 0; i < a_n; i++)
	{
		const TypedKey *ta;
		const TypedKey *tb;

		ta = &a[i];
		tb = &b[i];

		if (ta->isnull || tb->isnull)
		{
			if (!(ta->isnull && tb->isnull))
				return ta->isnull ? 1 : -1;
			continue;
		}
		if (ta->typid != tb->typid)
			return ta->typid < tb->typid ? -1 : 1;

		/* ------------------------------------------------------------
		 * RD-5: the per-column btree equality ("=") and less-than ("<")
		 * support functions are memoised once per PK column type and
		 * handed to us through TypedKey.eq_finfo / lt_finfo (owned by the
		 * PK metadata cache for the whole backend lifetime).  The hot
		 * path is therefore a pair of direct function calls with no
		 * catalog lookups, which is what makes the O((N+M) log M)
		 * bsearch/qsort comparisons affordable.
		 *
		 * Collation is threaded through so text/bpchar/varchar follow the
		 * column's real collation rather than a default C locale.
		 *
		 * When the memoised functions are absent (type had no btree sort
		 * operators), fall back to the legacy per-comparison catalog
		 * lookup so behaviour is unchanged for those edge types.
		 *
		 * A composite key is equal ONLY when every column is equal: an
		 * equal column must advance to the next one via `continue`.  A
		 * bare `return 0` here would falsely declare two keys equal as
		 * soon as their first column matched, which for a multi-column PK
		 * makes delta matching alias distinct rows (and silently defeats
		 * the R03 pk-mutation guard).
		 * ------------------------------------------------------------ */
		if (ta->eq_finfo != NULL && ta->lt_finfo != NULL)
		{
			if (DatumGetBool(FunctionCall2Coll(ta->eq_finfo, ta->collid,
											   ta->value, tb->value)))
				continue;
			return DatumGetBool(FunctionCall2Coll(ta->lt_finfo, ta->collid,
												  ta->value, tb->value))
				? -1 : 1;
		}

		colcmp = ob_typed_pk_cmp_legacy(ta, tb, i);
		if (colcmp != 0)
			return colcmp;
	}
	return 0;
}

void
overlay_typed_pk_free(TypedKey *pk, int n_pk)
{
	int			i;

	if (pk == NULL || n_pk <= 0)
		return;
	for (i = 0; i < n_pk; i++)
	{
		if (!pk[i].isnull && !pk[i].typbyval &&
			pk[i].value != (Datum) 0)
			pfree(DatumGetPointer(pk[i].value));
	}
	pfree(pk);
}

