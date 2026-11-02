/*-------------------------------------------------------------------------
 *
 * write_redirect.c
 *	  Overlay Branch Step 4a: ExecutorRun DML write-redirection
 *
 *	  Intercepts ModifyTable (INSERT / UPDATE / DELETE) inside an active
 *	  branch and redirects writes into pg_branch_delta via overlay_delta_*
 *	  helpers, leaving the MAIN heap completely untouched.
 *
 *	  Entry point: overlay_executor_run_intercept() — called from the
 *	  overlay_ExecutorRun hook in overlay_branch.c after hard-guard checks
 *	  pass.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "parser/parsetree.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "storage/lmgr.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/builtins.h"

#ifndef OBSCHEMA
#define OBSCHEMA        "overlay_branch"
#endif
#ifndef OBTABLE_DELTA
#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#endif

/*
 * P1 RETURNING support.
 *
 * The ModifyTable node with RETURNING works by calling ExecModifyTable() in a
 * loop; each call processes ONE row and returns the projected RETURNING slot
 * (or NULL when finished).  Our ExecutorRun hook sits ABOVE that loop, so the
 * cleanest way to deliver RETURNING rows to the destination (client / portal)
 * is to reproduce exactly what the top-level ExecutorRun() standard dispatch
 * does for nodeModifyTable: run ExecProcNode(mt) in a loop and feed each
 * returned slot into queryDesc->dest->receiveSlot().
 *
 * However we are the ones redirecting the writes, so instead of calling
 * ExecModifyTable() we:
 *   (1) run the subplan to exhaustion,
 *   (2) for each row, write a delta record (existing code),
 *   (3) if RETURNING is active, call ExecProcessReturning() using the
 *       appropriate in/out slot (INSERT new slot, UPDATE new merged slot,
 *       DELETE old-clean slot) and materialize the projected RETURNING
 *       slot into our own array allocated in the estate's per-query
 *       context,
 *   (4) AFTER the subplan loop we replay the collected RETURNING slots
 *       through dest->receiveSlot() (one call per row) so the client/
 *       portal sees the exact tuple stream a normal non-redirected DML
 *       would have produced.
 *
 * Steps (3)/(4) are skipped entirely when plan->returningLists == NIL so the
 * non-RETURNING fast-path keeps its exact old performance characteristics.
 *
 * ExecProcessReturning() is MODULE-local in nodeModifyTable.c so we cannot
 * call it directly.  Instead we re-implement the same projection here,
 * mirroring exactly ExecProcessReturning's pattern: set econtext->scantuple
 * + tableOid, then ExecProject(ri_projectReturning), then ExecMaterializeSlot
 * to copy pass-by-ref values out of the child slot's context.  The projection
 * itself was already built by ExecInitModifyTable() (via ExecBuildProjectionInfo)
 * during ExecutorStart() — we only use it, we never build it.
 */

#include "executor/executor.h"
#include "executor/nodeModifyTable.h"

static TupleTableSlot *
ob_project_returning(ResultRelInfo *rri, TupleTableSlot *scanSlot,
					 TupleTableSlot *planSlot, Relation rel)
{
	ProjectionInfo *proj = rri->ri_projectReturning;
	ExprContext *econtext;
	TupleTableSlot *out;
	TupleTableSlot *phys_out;

	if (proj == NULL) return NULL;
	econtext = proj->pi_exprContext;

	if (scanSlot && TTS_IS_VIRTUAL(scanSlot))
	{
		TupleDesc	std = scanSlot->tts_tupleDescriptor;
		if (std && scanSlot->tts_nvalid < std->natts)
			scanSlot->tts_nvalid = std->natts;
	}
	if (planSlot && TTS_IS_VIRTUAL(planSlot))
	{
		TupleDesc	ptd = planSlot->tts_tupleDescriptor;
		if (ptd && planSlot->tts_nvalid < ptd->natts)
			planSlot->tts_nvalid = ptd->natts;
	}

	if (scanSlot)
		econtext->ecxt_scantuple = scanSlot;
	econtext->ecxt_outertuple = planSlot;
	if (econtext->ecxt_scantuple)
		econtext->ecxt_scantuple->tts_tableOid = RelationGetRelid(rel);

	out = ExecProject(proj);
	if (TupIsNull(out))
		return out;

	{
		TupleDesc	otd = out->tts_tupleDescriptor;

		ExecMaterializeSlot(out);
		if (TTS_IS_VIRTUAL(out))
		{
			if (otd && out->tts_nvalid < otd->natts)
				out->tts_nvalid = otd->natts;
		}
		phys_out = MakeSingleTupleTableSlot(otd, &TTSOpsHeapTuple);
		ExecCopySlot(phys_out, out);
		ExecMaterializeSlot(phys_out);
	}
	return phys_out;
}

/* ----------------------------------------------------------------
 * ob_expr_has_ctid_var
 *
 *   True if the expression tree references the ctid system column
 *   (SelfItemPointerAttributeNumber) of any relation.
 *
 *   Used to keep the pure-delta write pass from evaluating a
 *   ctid-based DML predicate against reconstructed overlay rows.
 *   A pure-delta row has no physical ctid: its slot carries the
 *   invalid ItemPointer, and PG's slot_getsysattr() returns
 *   &slot->tts_tid for ctid unconditionally.  Evaluating e.g.
 *   `ctid = (SELECT ctid FROM t WHERE pk = 1)` would therefore
 *   match EVERY pure-delta row at once (data loss), and never a
 *   meaningful one.  ctid-addressed DML on pure-delta rows stays a
 *   documented no-op, symmetric with the ctid write path which
 *   skips them via slot_get_ctid_cstr() == NULL.
 * ----------------------------------------------------------------
 */
static bool
ob_ctid_var_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varattno == SelfItemPointerAttributeNumber)
			return true;
	}
	return expression_tree_walker(node, ob_ctid_var_walker, context);
}

static bool
ob_expr_has_ctid_var(Node *node)
{
	return node != NULL && ob_ctid_var_walker(node, NULL);
}

/* ----------------------------------------------------------------
 * mt_state_result_rel: small helper that extracts the i-th
 * ResultRelInfo from a ModifyTableState.
 * ----------------------------------------------------------------
 */
ResultRelInfo *
mt_state_result_rel(ModifyTableState *mt, int i)
{
	Assert(i >= 0 && i < mt->mt_nrels);
	return &mt->resultRelInfo[i];
}

/* ----------------------------------------------------------------
 * r21_check_insert_preconditions
 *
 *   R21 MVP pre-check pass for INSERT-path rows (called when the
 *   final promotion decision is INSERT, i.e. promo=1 would be set).
 *   Raises native PostgreSQL errors BEFORE any delta or MAIN write
 *   happens so the transaction has no partial modifications.
 *
 *   Checks performed (MVP scope – single PK table only):
 *     1. NOT NULL : every attnotnull=true (non-dropped, user) column
 *                   in `slot` must be non-NULL → ERRCODE_NOT_NULL_VIOLATION
 *     2. PK UNIQUE (MAIN side)  : same PK must not already exist in
 *                                 the MAIN heap → ERRCODE_UNIQUE_VIOLATION
 *     3. PK UNIQUE (delta side) : same PK must not already exist in
 *                                 the ACTIVE pure-delta (op I/U rows)
 *                                 → ERRCODE_UNIQUE_VIOLATION
 *
 *   Fail-open safe: if there is no single-column PK the MVP scope is
 *   not applicable and we return silently (the only supported scope of
 *   overlay_branch is single-PK heap tables anyway so this path only
 *   triggers for the catalog DML bypass in create_branch etc.)
 * ----------------------------------------------------------------
 */
static void
r21_check_insert_preconditions(Relation rel, TupleTableSlot *slot,
							   int32 bid, Oid relid,
							   AttrNumber pk_attno,
							   const char *pk)
{
	TupleDesc	reldesc;
	int			natts;
	int			i;

	if (rel == NULL || slot == NULL || TupIsNull(slot))
		return;

	reldesc = RelationGetDescr(rel);
	natts = reldesc->natts;

	if (TTS_IS_VIRTUAL(slot))
	{
		/* CRITICAL (R21 NOT NULL scan): TTSOpsVirtual tts_values/tts_isnull
		 * are NOT populated for columns beyond tts_nvalid; simply bumping
		 * tts_nvalid does NOT materialize them.  We MUST call
		 * ExecMaterializeSlot-less attribute extraction — slot_getsomeattrs()
		 * — which walks the virtual-slot "missing" bitmap and fills the
		 * arrays in-place.  This is safe here because R21 runs BEFORE any
		 * ExecProject/SET-merge mutation touches the slot, so no
		 * pass-by-ref contents can be corrupted. */
		if (slot->tts_nvalid < (AttrNumber) natts)
			slot_getsomeattrs(slot, natts);
	}

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(reldesc, i);
		bool		isnull;

		if (att->attisdropped)
			continue;
		if (att->attnum <= 0)
			continue;
		if (!att->attnotnull)
			continue;

		isnull = slot->tts_isnull[i];

		if (isnull)
		{
			ereport(ERROR,
					(errcode(ERRCODE_NOT_NULL_VIOLATION),
					 errmsg("null value in column \"%s\" of relation \"%s\" violates not-null constraint",
							NameStr(att->attname),
							RelationGetRelationName(rel)),
					 errdetail("Failing row contains an R21 MVP pre-check violation on INSERT path.")));
		}
	}

	if (pk_attno <= 0)
		return;

	{
		int			pk_attidx = pk_attno - 1;
		Oid			pk_type;
		Datum		pkdatum;
		bool		pk_isnull = false;
		Oid			pk_out_func;
		bool		pk_is_varlena;
		char	   *pkval_txt;
		const char *pk_colname;

		pk_type = TupleDescAttr(reldesc, pk_attidx)->atttypid;
		pk_colname = NameStr(TupleDescAttr(reldesc, pk_attidx)->attname);

		if (TTS_IS_VIRTUAL(slot))
		{
			/* R21 PK read: mirror Phase-II safe pattern — only access
			 * tts_values/tts_isnull AFTER slot_getsomeattrs has populated
			 * the prefix up to pk_attidx+1. */
			if (slot->tts_nvalid < (AttrNumber) (pk_attidx + 1))
				slot_getsomeattrs(slot, pk_attidx + 1);
			pkdatum = slot->tts_values[pk_attidx];
			pk_isnull = slot->tts_isnull[pk_attidx];
		}
		else
		{
			pkdatum = slot_getattr(slot, pk_attno, &pk_isnull);
		}

		if (pk_isnull)
			return;

		getTypeOutputInfo(pk_type, &pk_out_func, &pk_is_varlena);
		pkval_txt = OidOutputFunctionCall(pk_out_func, pkdatum);

		{
			Datum		vals_delta[3];
			static const Oid argtypes_delta[3] = {INT4OID, OIDOID, TEXTOID};
			int			ret_delta;
			char		delta_latest_op = 0; /* 0 = no entry; 'I'/'U'/'D' otherwise */

			/* WR-1a: branch_id/relid/key bound as $1..$3. */
			vals_delta[0] = Int32GetDatum(bid);
			vals_delta[1] = ObjectIdGetDatum(relid);
			vals_delta[2] = CStringGetTextDatum(pk ? pk : "");

			overlay_overlay_helper_enter();
			overlay_write_redirect_exit();
			ret_delta = ob_spi_exec_params(
				"SELECT d.op FROM " OBTABLE_DELTA " d "
				"WHERE d.branch_id = $1 AND d.relid = $2 AND d.key = $3 LIMIT 1",
				true, 1, 3, argtypes_delta, vals_delta, NULL);
			pfree(DatumGetPointer(vals_delta[2]));

			if (ret_delta == SPI_OK_SELECT && SPI_tuptable != NULL && SPI_processed >= 1)
			{
				char *op_txt = SPI_getvalue(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc, 1);
				if (op_txt != NULL && op_txt[0] != '\0')
					delta_latest_op = op_txt[0];
				if (op_txt) pfree(op_txt);
			}

			if (delta_latest_op == 'I' || delta_latest_op == 'U')
			{
				/* pkval_txt lives in the caller's memory context, not in the
				 * SPI procedure context, so it survives SPI_finish() below.
				 * (Copying it with pstrdup() here would place the copy in the
				 * SPI context, which SPI_finish() then frees -- the ereport
				 * would read freed memory.) */
				SPI_finish();
				overlay_write_redirect_enter();
				overlay_overlay_helper_exit();
				ereport(ERROR,
						(errcode(ERRCODE_UNIQUE_VIOLATION),
						 errmsg("duplicate key value violates unique constraint on relation \"%s\"",
								RelationGetRelationName(rel)),
						 errdetail("Key (%s)=(%s) already exists in branch delta (R21 pre-check).",
								   pk_colname, pkval_txt)));
			}

			overlay_write_redirect_enter();
			overlay_overlay_helper_exit();
			SPI_finish();

			/* ---------- MAIN-side conflict check: only if delta has NO
			 * tombstone covering the MAIN row.  If delta already has
			 * op='D' (DELETE tombstone), the MAIN row is logically
			 * invisible in this branch and re-INSERT (rebirth) is
			 * allowed. ---------- */
			if (delta_latest_op != 'D')
			{
				Datum		vals_main[1];
				static const Oid argtypes_main[1] = {TEXTOID};
				char	   *sql_main;
				int			ret_main;

				/* WR-1a: PK value bound as $1; relname/colname/type inline. */
				sql_main = psprintf("SELECT 1 FROM %s WHERE (%s) = $1::text::%s LIMIT 1",
									quote_qualified_identifier(
										get_namespace_name(RelationGetNamespace(rel)),
										RelationGetRelationName(rel)),
									quote_identifier(pk_colname),
									format_type_be(pk_type));
				vals_main[0] = CStringGetTextDatum(pkval_txt);

				overlay_overlay_helper_enter();
				overlay_write_redirect_exit();
				ret_main = ob_spi_exec_params(sql_main, true, 1, 1,
											  argtypes_main, vals_main, NULL);
				pfree(sql_main);
				pfree(DatumGetPointer(vals_main[0]));

				if (ret_main == SPI_OK_SELECT && SPI_tuptable != NULL && SPI_processed >= 1)
				{
					/* See the delta branch above: pkval_txt is allocated
					 * outside the SPI context and stays valid after
					 * SPI_finish(). */
					SPI_finish();
					overlay_write_redirect_enter();
					overlay_overlay_helper_exit();
					ereport(ERROR,
							(errcode(ERRCODE_UNIQUE_VIOLATION),
							 errmsg("duplicate key value violates unique constraint on relation \"%s\"",
									RelationGetRelationName(rel)),
							 errdetail("Key (%s)=(%s) already exists in MAIN heap (R21 pre-check).",
									   pk_colname, pkval_txt)));
				}
				overlay_write_redirect_enter();
				overlay_overlay_helper_exit();
				SPI_finish();
			}
		}

		pfree(pkval_txt);
	}
}

/* ----------------------------------------------------------------
 * overlay_executor_run_intercept
 *
 *   Full write-redirection body extracted from overlay_ExecutorRun.
 *   Returns true if the call handled the DML (caller must return
 *   immediately, must NOT fall through to standard ExecutorRun).
 *   Returns false if caller should fall through (catalog tables,
 *   multi-target DML, non-redirectable relations etc.)
 *
 *   Semantics preserved from the original monolithic L824-L1208:
 *     • Step7 DML guard runs over mt_nrels + es_range_table entries
 *     • CMD_UPDATE / CMD_DELETE:
 *         – no RETURNING support (MVP)
 *         – extract ctid from junk slot → re-fetch clean MAIN slot
 *         – capture old_version (ctid+xmin token) BEFORE mutation
 *         – for UPDATE, merge SET-values by column NAME into clean slot
 *         – serialize_pk + serialize_tuple → overlay_delta_insert
 *     • CMD_INSERT:
 *         – no RETURNING support (MVP)
 *         – serialize_pk + serialize_tuple per row → overlay_delta_insert
 *     • ob_write_redirect_done label placed AFTER
 *       prev/standard_ExecutorRun so catalog/non-redirected DML falls
 *       through to standard (otherwise overlay_branch catalog INSERTS
 *       from SPI silently never persist).
 * ----------------------------------------------------------------
 */
bool
overlay_executor_run_intercept(QueryDesc *queryDesc,
							   ScanDirection direction,
							   uint64 count, bool execute_once,
							   ExecutorRun_hook_type_fn prev_ExecutorRun)
{
	CmdType		cmd = queryDesc->operation;
	ModifyTableState *mt = NULL;
	ModifyTable      *plan = NULL;
	bool		handled = false;
	extern char *slot_get_ctid_cstr(TupleTableSlot *slot);
	extern TupleTableSlot *fetch_tuple_by_ctid(Relation rel, const char *ctid_cstr,
											   MemoryContext result_cxt);

	/* V3 Layer1 + FR4: DML path MUST raise ERROR if the branch has been
	 * applied/discarded in another session — DQL path gets NOTICE + MAIN
	 * fallback only, but drift-writes to MAIN are 100% disallowed, so we
	 * call with for_dml=true BEFORE the plain is_active() check so the
	 * ERROR split fires.
	 *
	 * CRITICAL CmdType guard: throttled(true) (DML ERROR split) is ONLY
	 * for INSERT/UPDATE/DELETE/MERGE.  For plain SELECTs (including
	 * SELECT force_invalidation_check(), SELECT is_active('name'), SELECT
	 * * FROM user_table), CmdType is CMD_SELECT and we just need the
	 * soft DQL kickout (NOTICE + fall back to MAIN reads).  Accidentally
	 * passing for_dml=true to a SELECT caused the regression in Section
	 * J3 where `SELECT force_check()` raised a DML ERROR.
	 *
	 * CRITICAL protected-schema guard (Bug #5): before invoking
	 * throttled() on a DML CmdType, inspect the PlannedStmt's
	 * resultRelations.  If EVERY target relation lives in a protected
	 * schema (pg_catalog / information_schema / pg_toast* / OBSCHEMA
	 * i.e. the extension's own catalog tables) then this statement is a
	 * purely internal / catalog DML (e.g.  UPDATE overlay_branch.pg_branch
	 * SET state='applied' simulating another session, or a SPI insert
	 * inside the extension helper), NOT a user-table drift-write.
	 * Running throttled() on such DMLs is wrong: it might SPI-query
	 * pg_branch, notice the just-written applied state, and KICK OUT
	 * CurrentBranchContext *before* the following user-table DML (D7)
	 * has a chance to run with for_dml=true.  Effect: D7 would see
	 * ctx->is_active already false, hit the throttled L565 fast-path,
	 * return false, and the drift-write silently falls through to MAIN.
	 * Guarding target-schema here (BEFORE the throttled() call) keeps
	 * the user-table branch context alive long enough for the next
	 * real user-table DML to correctly raise the DML split ERROR. */
	{
		bool		dml_split;

		dml_split = (cmd == CMD_INSERT ||
					 cmd == CMD_UPDATE ||
					 cmd == CMD_DELETE ||
					 cmd == CMD_MERGE);

		/* ========= REVIEW-260926 / R06 (belt-and-braces #2) ========
		 *   MERGE statements build a ModifyTable plan but we have NOT
		 *   validated that every result-relation path is properly
		 *   redirected.  To be safe, we explicitly REJECT CMD_MERGE
		 *   here too, even though ProcessUtility has a choke point.
		 *   This WR-level guard ensures DO-block inner SPI execution
		 *   (which may bypass the outer ProcessUtility hook entirely)
		 *   still refuses MERGE against a user table inside an active
		 *   branch.  The same gate applies to ModCTE (handled above). */
		if (cmd == CMD_MERGE && overlay_branch_is_active())
		{
			ListCell   *lc;
			bool		has_managed_rel = false;

			if (queryDesc->plannedstmt != NULL)
				foreach(lc, queryDesc->plannedstmt->resultRelations)
				{
					Index		rti = lfirst_int(lc);
					RangeTblEntry *rte;
					Oid			nspoid;
					char	   *nsp;

					if (rti <= 0 || queryDesc->plannedstmt->rtable == NULL ||
						rti > list_length(queryDesc->plannedstmt->rtable))
						continue;
					rte = rt_fetch(rti, queryDesc->plannedstmt->rtable);
					if (rte == NULL || rte->rtekind != RTE_RELATION ||
						!OidIsValid(rte->relid))
						continue;
					nspoid = get_rel_namespace(rte->relid);
					if (!OidIsValid(nspoid)) continue;
					nsp = get_namespace_name(nspoid);
					if (nsp == NULL) continue;
					if (strcmp(nsp, "pg_catalog") == 0 ||
						strcmp(nsp, OBSCHEMA) == 0 ||
						strncmp(nsp, "pg_toast", 8) == 0)
						continue;
					has_managed_rel = true;
					break;
				}
			if (has_managed_rel)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("overlay_branch MVP does not support MERGE inside a branch (R06 WR-stage blocked)"),
						 errhint("Rework MERGE as separate INSERT/UPDATE/DELETE statements.")));
		}

		if (dml_split && queryDesc->plannedstmt != NULL &&
			queryDesc->plannedstmt->resultRelations != NIL)
		{
			ListCell   *lc;
			bool		all_protected = true;

			foreach(lc, queryDesc->plannedstmt->resultRelations)
			{
				Index		rti = lfirst_int(lc);
				RangeTblEntry *rte;
				Oid			nspoid;
				char	   *nsp;

				if (rti <= 0 ||
					rti > list_length(queryDesc->plannedstmt->rtable))
				{
					all_protected = false;
					break;
				}
				rte = rt_fetch(rti, queryDesc->plannedstmt->rtable);
				if (rte == NULL || rte->rtekind != RTE_RELATION ||
					!OidIsValid(rte->relid))
				{
					all_protected = false;
					break;
				}
				nspoid = get_rel_namespace(rte->relid);
				if (!OidIsValid(nspoid))
				{
					all_protected = false;
					break;
				}
				nsp = get_namespace_name(nspoid);
				if (nsp == NULL)
				{
					all_protected = false;
					break;
				}
				if (strcmp(nsp, "pg_catalog") != 0 &&
					strcmp(nsp, "information_schema") != 0 &&
					strncmp(nsp, "pg_toast", 8) != 0 &&
					strcmp(nsp, OBSCHEMA) != 0)
				{
					all_protected = false;
					break;
				}
			}

			/* Pure catalog DML: skip throttled kickout entirely so an
			 * in-flight user-table branch context is not prematurely
			 * invalidated by our own catalog writes. */
			if (all_protected)
				return false;
		}

		/* --- Throttled invalidation check + CmdType-split dispatch ---
		 *
		 * CRITICAL CmdType-split: DESTRUCTIVE kickout side-effects
		 * (exit_branch + clear GUC + raise NOTICE/ERROR) are ONLY
		 * opted-in for REAL DML (dml_split=true: INSERT/UPDATE/
		 * DELETE/MERGE about to touch user-table rows).  For plain
		 * SELECT/DECLARE CURSOR etc. (dml_split=false), throttled()
		 * runs in OBSERVER-ONLY mode — returns boolean ground-truth
		 * WITHOUT mutating ctx.is_active or clearing the GUC, so a
		 * post-apply SELECT (which sees "branch dead" via SPI) does
		 * NOT silently consume the kickout via suppressed NOTICE
		 * when client_min_messages >= WARNING, which would cause the
		 * NEXT real DML to fall through to MAIN unprotected and
		 * silently drift-write (T8 L1 ob_state_inval P1 bug).
		 *
		 * Every other caller of throttled() — planner hook,
		 * is_active_by_name, rel_ok/ddl_ok guards, ProcessUtility
		 * branch-DDL whitelist, internal SPI heavy-check re-read —
		 * MUST also see the boolean ground-truth WITHOUT mutating
		 * global state.  Otherwise D7 (UPDATE t_inv after SET
		 * threshold=1) dies: an observer call clears ctx.is_active
		 * before the real ModifyTable ExecutorRun runs, the DML
		 * ERROR split never fires, and the drift write silently
		 * lands on MAIN.
		 *
		 * Note: protected-schema skip above (all catalog DML →
		 * return false) means the DML-only opt-in below never fires
		 * for catalog-only statements — exactly what we want because
		 * internal catalog writes must never cause a WRONG DML
		 * ERROR against the in-flight user branch. */
		{
			extern bool ob_throttled_allow_kickout;
			bool		saved_allow = ob_throttled_allow_kickout;
			bool		thr_ok;
			/* FIX-C: Dirty-only kickout gate.
			 *
			 * Destructive side-effects of throttled() (ERROR for DML,
			 * NOTICE for DQL, exit_branch_cleanup + GUC reset) are ONLY
			 * opted-in when BOTH:
			 *   (a) dml_split == true (real user INSERT/UPDATE/DELETE
			 *       about to write rows, not a catalog-only internal
			 *       statement)
			 *   (b) CurrentBranchContext is ACTUALLY DIRTY — i.e. it
			 *       exists, is_active, and carries a real branch_id>0.
			 *
			 * Without (b), a dml_split=true write that happens to run
			 * on a session with NO branch (or a zombie cleaned ctx with
			 * bid=0 / name='') would invoke the kickout machinery on a
			 * no-op ctx and produce a spurious ERROR / NOTICE.  In
			 * that case we simply observe thr_ok (which is always true
			 * for a NULL/clean ctx anyway) and skip the destructive
			 * mutation path.  This is the DIRTY-ONLY complement to
			 * FIX-A (ERROR vs NOTICE split). */
			bool		dirty_ctx = false;
			{
				extern BranchContext *CurrentBranchContext;
				if (CurrentBranchContext != NULL &&
					CurrentBranchContext->is_active &&
					CurrentBranchContext->branch_id > 0)
					dirty_ctx = true;
			}

			if (dml_split && dirty_ctx)
				ob_throttled_allow_kickout = true;
			thr_ok = ob_invalidate_check_throttled(dml_split && dirty_ctx);
			ob_throttled_allow_kickout = saved_allow;
			if (!thr_ok || queryDesc->planstate == NULL)
				return false;
		}

		/* FIX-I: ModifyingCTE MVP guard (MUST run BEFORE Non-DML return!)
		 *
		 * If plannedstmt has ModifyingCTE (INSERT/UPDATE/DELETE in WITH
		 * list wrapped in outer SELECT), then queryDesc->operation is
		 * CMD_SELECT → dml_split=false.  If the Non-DML `if (!dml_split)
		 * return false;` line below runs BEFORE this check, we silently
		 * fall through to standard_ExecutorRun which writes DIRECTLY to
		 * MAIN heap → silent MAIN pollution + data corruption!  E.g.:
		 *
		 *   WITH upd AS (UPDATE t SET v=v+1 WHERE pk=2 RETURNING *)
		 *   SELECT * FROM upd;
		 *
		 * hasModifyingCTE flag is set by the planner exactly for this
		 * case.  Proper fix requires intercepting *inner* ModifyTable
		 * nodes inside CTE subplans (non-trivial refactor of WR loop).
		 * MVP: explicit ERROR with helpful hint — silent corruption is
		 * NEVER acceptable, regardless of CmdType. */
		if (queryDesc->plannedstmt != NULL &&
			queryDesc->plannedstmt->hasModifyingCTE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay_branch MVP does not support data-modifying statements inside WITH (CTE) clauses"),
					 errhint("Rewrite WITH (UPDATE/DELETE/INSERT ... RETURNING) SELECT ... "
							 "using a TEMP TABLE to collect RETURNING rows:\n"
							 "  CREATE TEMP TABLE _r AS UPDATE t SET ... RETURNING ...;\n"
							 "  SELECT * FROM _r; DROP TABLE _r;")));
		}

		/* Non-DML CmdType: after throttled(false) DQL check + ModifyingCTE
		 * MVP guard we have nothing to redirect — fall through to standard
		 * executor so SELECTs, DECLARE CURSORs, etc. run normally on MAIN. */
		if (!dml_split)
			return false;
	}

	/* B2-2a DFS walker: locate the FIRST ModifyTableState descendant
	 * of queryDesc->planstate.
	 *
	 * For UPSERT (INSERT ... ON CONFLICT), PG planner often wraps the
	 * ModifyTableState under one or more projection / result nodes.
	 * The original `!IsA(ModifyTableState)` guard above would wrongly
	 * return false and fall through to standard ExecutorRun → MAIN
	 * drift.  We DFS the PlanState tree (64-deep manual stack to avoid
	 * recursion) and accept the FIRST ModifyTableState descendant we
	 * find.  This is safe because WR redirects DML only when
	 * mt_nrels==1 && resultRelations==1 anyway.  Scan descent stops
	 * early at baserel-type nodes (Seq/Index/BitmapHeapScan) per
	 * Scan_ps degradation invariant — prevents BitmapIndexScan trap. */
	if (!IsA(queryDesc->planstate, ModifyTableState))
	{
		PlanState  *stack[64];
		int			depth = 0;

		stack[depth++] = queryDesc->planstate;
		while (depth > 0)
		{
			PlanState  *ps = stack[--depth];
			NodeTag		tag;

			if (ps == NULL)
				continue;
			tag = nodeTag(ps);
			if (tag == T_ModifyTableState)
			{
				/* Found: adopt as our working ModifyTableState */
				queryDesc->planstate = ps;
				break;
			}
			/* Stop descent at baserel-type scan nodes: they never wrap
			 * ModifyTableState and continuing into BitmapIndexScan /
			 * IndexScan internals causes false-positive trap trips. */
			if (tag == T_SeqScanState ||
				tag == T_IndexScanState ||
				tag == T_IndexOnlyScanState ||
				tag == T_BitmapHeapScanState ||
				tag == T_TidScanState ||
				tag == T_SubqueryScanState ||
				tag == T_FunctionScanState ||
				tag == T_ValuesScanState ||
				tag == T_CteScanState ||
				tag == T_WorkTableScanState ||
				tag == T_ForeignScanState ||
				tag == T_CustomScanState)
				continue;
			/* Push children right-first so left is popped first (DFS
			 * order matches PG execution order; irrelevant for "first
			 * ModifyTableState" match but keeps traversal
			 * deterministic). */
			if (ps->righttree != NULL && depth < 64)
				stack[depth++] = ps->righttree;
			if (ps->lefttree != NULL && depth < 64)
				stack[depth++] = ps->lefttree;
		}
		/* If still not a ModifyTableState after DFS → no DML in tree */
		if (!IsA(queryDesc->planstate, ModifyTableState))
			return false;
	}

	overlay_write_redirect_enter();
	PG_TRY();
	{
		mt = (ModifyTableState *) queryDesc->planstate;
		plan = (ModifyTable *) mt->ps.plan;

		/* --- Step7 DML hard guard (result rels + rtable) --- */
		{
			int			i;
			int			nres = mt->mt_nrels;
			int			planres = list_length(plan->resultRelations);
			EState	   *estate = mt->ps.state;

			if (nres <= 0) nres = planres;
			for (i = 0; i < nres; i++)
			{
				ResultRelInfo *rri = mt_state_result_rel(mt, i);
				Relation	rel;
				const char *opstr;
				char	   *gr = NULL;

				if (rri == NULL) continue;
				rel = rri->ri_RelationDesc;
				if (rel == NULL) continue;

				opstr = cmd == CMD_INSERT ? "INSERT" :
						cmd == CMD_UPDATE ? "UPDATE" :
						cmd == CMD_DELETE ? "DELETE" : "DML";

				if (!overlay_guard_rel_ok(rel, &gr))
					overlay_guard_ereport_fail(opstr,
											   RelationGetRelationName(rel), NULL, gr);
			}

			if (estate != NULL)
			{
				int			rt_size = estate->es_range_table_size;
				List	   *rtable = estate->es_range_table;

				if (rtable != NULL)
				{
					for (i = 1; i <= rt_size; i++)
					{
						RangeTblEntry *rte = exec_rt_fetch(i, estate);
						Relation	rel;
						const char *opstr;
						char	   *gr = NULL;

						if (rte == NULL) continue;
						if (rte->rtekind != RTE_RELATION) continue;

						rel = ExecGetRangeTableRelation(estate, i);
						if (rel == NULL) continue;

						{
							Oid			nspoid = RelationGetNamespace(rel);
							char	   *nsp = get_namespace_name(nspoid);
							bool		skip_catalog = false;
							if (nsp && (strcmp(nsp, "pg_catalog") == 0 ||
										strcmp(nsp, OBSCHEMA) == 0 ||
										strncmp(nsp, "pg_toast", 8) == 0))
								skip_catalog = true;
							if (skip_catalog) continue;
						}

						opstr = cmd == CMD_INSERT ? "INSERT" :
								cmd == CMD_UPDATE ? "UPDATE" :
								cmd == CMD_DELETE ? "DELETE" : "DML";
						if (!overlay_guard_rel_ok(rel, &gr))
							overlay_guard_ereport_fail(opstr,
													   RelationGetRelationName(rel), NULL, gr);
					}
				}
			}
		}

		/* --- Single target only (MVP) --- */
		if (mt->mt_nrels == 1 && list_length(plan->resultRelations) == 1)
		{
			ResultRelInfo *rri = mt_state_result_rel(mt, 0);
			Relation	rel = rri->ri_RelationDesc;

			/* --- CMD_UPDATE / CMD_DELETE redirect --- */
			if ((cmd == CMD_UPDATE || cmd == CMD_DELETE) &&
				overlay_should_redirect(rel))
			{
				PlanState  *subplan;
				uint64		ndone = 0;
				TupleTableSlot *junk_slot;
				char		op = (cmd == CMD_UPDATE) ? DELTA_OP_UPDATE
													 : DELTA_OP_DELETE;
				bool		has_returning = (plan->returningLists != NIL);
				/* RETURNING rows collected during subplan loop and replayed
				 * to dest after ndone == processed.  Array grows in
				 * estate->es_query_cxt so slots survive to receiveSlot(). */
				TupleTableSlot **retslots = NULL;
				int			nretslots = 0;
				int			nretslots_alloc = 0;
				/* =============================================================
				 * REVIEW-260926 / R03: PK-column mutation guard state.
				 * Bitmap of PK attnos (bit index = 1-based attno - 1).
				 * Non-NULL only for CMD_UPDATE with a valid primary key on
				 * the target relation.  Declared HERE (top of block) to
				 * comply with ISO C90 mixed-declaration-code rule (-Werror
				 * would otherwise fail under -Wdeclaration-after-statement).
				 * ============================================================= */
				Bitmapset  *pk_attnums_set = NULL;

				subplan = outerPlanState(mt);

				if (cmd == CMD_UPDATE)
				{
					ListCell   *lcpk;
					List	   *pkidxs;

					pkidxs = overlay_get_pk_attnos_list(rel);
					if (pkidxs != NULL)
						foreach(lcpk, pkidxs)
						{
							int attno = lfirst_int(lcpk);
							pk_attnums_set =
								bms_add_member(pk_attnums_set, attno - 1);
						}
				}

				for (;;)
				{
					char	   *pk;
					char	   *tdata;
					char	   *oldver;
					TupleTableSlot *main_row;
					TupleTableSlot *overlay_slot;
					TupleTableSlot *old_clean_slot;
					char	   *ctid_cstr;
					bool		delta_deleted;

					CHECK_FOR_INTERRUPTS();

					junk_slot = ExecProcNode(subplan);
					if (TupIsNull(junk_slot))
						break;
					if (count != 0 && ndone >= count)
						break;

					ctid_cstr = slot_get_ctid_cstr(junk_slot);
					if (ctid_cstr == NULL)
					{
						/* ACC-1: a row that lives only in the delta store (a
						 * pure-delta INSERT) has no MAIN ctid; the DML-target
						 * BranchScan leaves its t_self invalid.  Those rows are
						 * NOT handled here -- the pure-delta ExecQual pass below
						 * replays them.  The ctid loop and that pass are
						 * disjoint by ctid presence (doc/internals.md §2.5), so
						 * skipping here never drops a row. */
						elog(DEBUG2,
							 "overlay WR: skipping pure-delta row (no MAIN ctid); pure-delta pass handles it");
						ExecClearTuple(junk_slot);
						continue;
					}

					/* ACC-1: fetch the MAIN row.  It anchors both the row
					 * identity (ctid) and the optimistic base-version token,
					 * and is the overlay pre-image fallback when this branch
					 * has no delta row for the key yet. */
					main_row = fetch_tuple_by_ctid(rel, ctid_cstr,
												   queryDesc->estate->es_query_cxt);
					pfree(ctid_cstr);

					/* ACC-1 pre-image = the OVERLAY row (MAIN (+) delta).  When
					 * this branch already holds a delta row for the identity,
					 * its serialized tuple_data IS the full post-image; using it
					 * (instead of the bare MAIN row) is what makes a second
					 * UPDATE in the same branch compound correctly:
					 * value-dependent SET expressions (G1) and non-SET columns
					 * (G3) then see earlier branch edits, and the DML subplan
					 * evaluated WHERE/SET against the overlay row too (G2). */
					overlay_slot = main_row;
					delta_deleted = false;
					{
						DeltaTuple	dtu;
						char	   *idkey;
						bool		have_delta;

						memset(&dtu, 0, sizeof(dtu));
						idkey = overlay_serialize_identity(rel, main_row, op);
						have_delta = (idkey != NULL &&
									  overlay_delta_lookup(overlay_branch_get_current_id(),
														   RelationGetRelid(rel),
														   idkey, &dtu));
						if (have_delta && dtu.op == DELTA_OP_DELETE)
						{
							/* BranchScan would not emit a row this branch has
							 * already deleted; guard defensively. */
							delta_deleted = true;
						}
						else if (have_delta &&
								 (dtu.op == DELTA_OP_INSERT ||
								  dtu.op == DELTA_OP_UPDATE) &&
								 dtu.tuple_data != NULL)
						{
							TupleTableSlot *ov;

							ov = reconstruct_slot_from_delta(rel, dtu.tuple_data,
															 queryDesc->estate->es_query_cxt);
							if (ov != NULL)
							{
								/* Keep the MAIN anchor ctid: the overlay row has
								 * no physical ctid of its own. */
								ov->tts_tid = main_row->tts_tid;
								overlay_slot = ov;
							}
						}
						if (idkey != NULL)
							pfree(idkey);
						/* overlay_delta_lookup() deep-copies key/old_version/
						 * tuple_data into TopMemoryContext so they outlive its
						 * SPI_finish(); release them now to stay allocation
						 * neutral (MF-1). */
						if (dtu.key != NULL)
							pfree(dtu.key);
						if (dtu.old_version != NULL)
							pfree(dtu.old_version);
						if (dtu.tuple_data != NULL)
							pfree(dtu.tuple_data);
					}

					if (delta_deleted)
					{
						ExecDropSingleTupleTableSlot(main_row);
						ExecClearTuple(junk_slot);
						continue;
					}

					/* old_clean_slot = the pre-image the SET-merge mutates and
					 * the serializer / token read.  It aliases main_row unless a
					 * reconstructed overlay slot replaced it; both are released
					 * at the end of the iteration. */
					old_clean_slot = overlay_slot;
					oldver = overlay_tuple_version(rel, main_row);

					if (cmd == CMD_UPDATE)
					{
						TupleDesc	reldesc = RelationGetDescr(rel);
						TupleDesc	junkdesc = junk_slot->tts_tupleDescriptor;
						MemoryContext oldmc;
						int			r;

						/* SET-merge copies belong to the same lifetime as
						 * old_clean_slot (fetched into es_query_cxt above), so
						 * allocate them there too.  TopMemoryContext leaked one
						 * datum per redirected row per varchar SET column
						 * (MF-1). */
						oldmc = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
						for (r = 0; r < reldesc->natts; r++)
						{
							Form_pg_attribute ratt = TupleDescAttr(reldesc, r);
							const char *rname = NameStr(ratt->attname);
							int			j;

							if (ratt->attisdropped)
								continue;

							for (j = 0; j < junkdesc->natts; j++)
							{
								Form_pg_attribute jatt = TupleDescAttr(junkdesc, j);
								if (jatt->attisdropped)
									continue;
								if (strcmp(NameStr(jatt->attname), rname) == 0)
								{
									bool		jisnull;
									Datum		jvalue = slot_getattr(junk_slot,
												  (AttrNumber) (j + 1),
												  &jisnull);

									/* ===== REVIEW-260926 / R03 =====
									 * If this column is a primary-key
									 * column, then the user has put the
									 * PK inside SET targetlist.  Forbid
									 * BEFORE writing any delta: the old
									 * PK key would be used for the delta
									 * row, but the post-image carries a
									 * DIFFERENT key — live view + apply
									 * both misbehave.  Note ratt->attnum
									 * is the LOGICAL 1-based attno of
									 * the column (per heap
									 * TupleDescAttr convention) so the
									 * bitmap offset is attnum-1. */
									if (pk_attnums_set != NULL &&
										bms_is_member(ratt->attnum - 1,
													  pk_attnums_set))
									{
										ereport(ERROR,
												(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
												 errmsg("overlay_branch MVP does not support UPDATE that modifies primary key columns (R03 pk-mutation blocked)"),
												 errdetail("Target relation has primary key column \"%s\"; attempting to SET it would desync the delta key from the post-image.",
														   rname),
												 errhint("To change a PK value inside a branch, DELETE the old row then INSERT a new row with the desired key.  Those two DML are both properly redirected to the delta store.")));
									}

									if (!jisnull)
									{
										int16		typlen;
										bool		typbyval;

										get_typlenbyval(ratt->atttypid, &typlen, &typbyval);
										jvalue = datumCopy(jvalue, typbyval, typlen);
										old_clean_slot->tts_values[r] = jvalue;
										old_clean_slot->tts_isnull[r] = false;
									}
									/* ===== REVIEW-260926 / R20 =====
									 * If `jisnull` is true we used to
									 * fall through, i.e. the caller
									 * requested SET col = NULL but the
									 * merge logic treated "NULL Datum
									 * in junk slot" the same as "this
									 * column wasn't mentioned in SET
									 * targetlist" — so the old value
									 * remained in the post-image.
									 * That is an MVP semantic bug.  Fix
									 * below: if the column IS in the
									 * SET targetlist (we already found
									 * it by name in the junkdesc),
									 * then we MUST copy the isnull
									 * flag regardless of whether the
									 * corresponding Datum is zero.  The
									 * value side already handles the
									 * non-null case above, so here we
									 * just force tts_isnull[r]=true. */
									else
									{
										old_clean_slot->tts_values[r] = (Datum) 0;
										old_clean_slot->tts_isnull[r] = true;
									}
									break;
								}
							}
						}
						MemoryContextSwitchTo(oldmc);
						tdata = overlay_serialize_tuple(rel, old_clean_slot);
					}
					else
					{
						tdata = NULL;
					}

					pk = overlay_serialize_identity(rel, old_clean_slot, op);

					overlay_delta_insert(overlay_branch_get_current_id(),
										RelationGetRelid(rel), pk, op,
										oldver, tdata);
					ndone++;

					/* P1 RETURNING: project the RETURNING list.
					 * For UPDATE the post-image is old_clean_slot (already
					 * merged with SET values above).  For DELETE the
					 * pre-image is also old_clean_slot (we merged nothing).
					 * Junk from the scan subplan is the planSlot. */
					if (has_returning)
					{
						TupleTableSlot *rslot;

						rslot = ob_project_returning(rri,
													 old_clean_slot,
													 junk_slot,
													 rel);
						if (rslot != NULL && !TupIsNull(rslot))
						{
							MemoryContext oldmcq;
							oldmcq = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
							if (nretslots >= nretslots_alloc)
							{
								int			newsz = nretslots_alloc == 0 ? 16 : nretslots_alloc * 2;
								if (retslots == NULL)
									retslots = palloc(sizeof(TupleTableSlot *) * newsz);
								else
									retslots = repalloc(retslots,
														sizeof(TupleTableSlot *) * newsz);
								nretslots_alloc = newsz;
							}
							/* Copy slot into query context. */
							{
								TupleTableSlot *cp;
								TupleDesc	tdesc = rslot->tts_tupleDescriptor;
								cp = MakeSingleTupleTableSlot(tdesc, &TTSOpsHeapTuple);
								ExecCopySlot(cp, rslot);
								ExecMaterializeSlot(cp);
								retslots[nretslots] = cp;
							}
							nretslots++;
							MemoryContextSwitchTo(oldmcq);
						}
					}

					pfree(pk);
					if (tdata) pfree(tdata);
					if (oldver) pfree(oldver);
					/* ACC-1: release the pre-image and, when a reconstructed
					 * overlay slot replaced it, the MAIN anchor row too. */
					ExecDropSingleTupleTableSlot(old_clean_slot);
					if (old_clean_slot != main_row)
						ExecDropSingleTupleTableSlot(main_row);
					ExecClearTuple(junk_slot);
				}

				/* NOTE: rows that live ONLY in the delta store (pure-delta
				 * INSERTs with no MAIN baseline) never enter the ctid loop
				 * above: their BranchScan output carries an invalid ctid, so
				 * they take the skip branch there.  They are instead replayed
				 * by the ExecQual pass below.  A previous attempt to handle
				 * them with a post-loop foreach over every INSERT delta row
				 * caused a severe regression: the code emitted a DELETE
				 * tombstone for *all* pure-delta INSERTs regardless of WHERE,
				 * silently wiping out rows the user had just inserted and not
				 * asked to delete (e.g. DELETE id=2 also wiped out
				 * newly-INSERTed id=4).  Correctly applying WHERE predicates to
				 * delta-only rows requires replaying the subplan qual against
				 * reconstructed delta slots, which is exactly what the ExecQual
				 * pass below does: for every pure-delta INSERT row still alive
				 * in this branch it reconstructs a TupleTableSlot and runs it
				 * through the plan's qual.  Qual-hits → write UPDATE/DELETE
				 * delta.  This eliminates the earlier regression where a
				 * no-qual foreach would tombstone ALL pure-delta rows. */
				/* MVP pure-delta UPDATE/DELETE ExecQual pass: for every
				 * pure-delta INSERT row still alive in this branch,
				 * reconstruct a TupleTableSlot and run it through
				 * subplan->qual (ExecQual).  Qual-hits → write DELETE delta
				 * (UPDATE pure-delta is MVP-out-of-scope for SET merge,
				 * skipped, documented below).  This eliminates the earlier
				 * regression where a no-qual foreach would tombstone ALL
				 * pure-delta rows.
				 *
				 * TODO: for UPDATE, we must apply ModifyTable's SET
				 * projection per reconstructed pure-delta row (via
				 * ExecTargetList evaluation) instead of relying on
				 * junk_slot which belongs to the last MAIN row.  Deferred.
				 *
				 * TEMP DEBUG: wrap pure-delta in a "enable/disable flag so we
				 * can isolate crash. */
/*
 * DEBUG BISect flags for pure-delta pass.
 * Set PURE_DELTA_STEP to the highest enabled step. Crash will be isolated
 * at the first step N that causes postmaster exit code 2.
 *   0 = fully disabled (baseline)
 *   1 = bid/relid only (no SPI, no alloc)
 *   2 = Phase A: initStringInfo + appendStringInfo + pfree(spiq)
 *   3 = Phase B: helper_enter + ob_spi_one_shot (SPI SELECT) + helper_exit +
 *               in_pure_delta=true, SPI_finish
 *   4 = Phase C: ret2 ok check + SPI_tuptable NULL guard
 *   5 = Phase D: deep copy pkstr+tdata into CurrentMemoryContext raw_rows[]
 *   6 = Phase E: overlay_delta_lookup + reconstruct_slot (cand_inserts)
 *   7 = Phase F: ExecQual walk over cand_inserts
 *   8 = FULL:     overlay_delta_insert DELETE write + RETURNING + cleanup
 */
#define PURE_DELTA_STEP 8

#if PURE_DELTA_STEP >= 1
				{
#if PURE_DELTA_STEP >= 5
					/* define early (cleanup block below uses it) */
					struct WR_PDR1 { char *s; bytea *t; char *ov; };
					struct WR_PDC1 { TupleTableSlot *slot; char *key; };
#endif
					int32 bid = overlay_branch_get_current_id();
					Oid relid = RelationGetRelid(rel);
#if PURE_DELTA_STEP >= 2
					StringInfoData spiq;
#endif
#if PURE_DELTA_STEP >= 3
					int ret2;
					bool in_pure_delta = false;
#endif
#if PURE_DELTA_STEP >= 5
					List *raw_rows = NIL;
#endif
#if PURE_DELTA_STEP >= 6
					List *cand_inserts = NIL;
#endif

					(void)bid; (void)relid; /* prevent unused warnings */

#if PURE_DELTA_STEP >= 2
					initStringInfo(&spiq);
					appendStringInfo(&spiq,
						"SELECT d.key, d.tuple_data, d.old_version FROM %s d "
						"WHERE d.branch_id = %u AND d.relid = %u AND d.op IN (%s, %s)",
						OBTABLE_DELTA, (unsigned) bid, (unsigned) relid,
						quote_literal_cstr("I"),
						quote_literal_cstr("U"));
#endif

#if PURE_DELTA_STEP >= 3
					overlay_overlay_helper_enter();
					ret2 = ob_spi_one_shot(spiq.data, true, 0);
					in_pure_delta = true;
#endif

#if PURE_DELTA_STEP >= 2
					pfree(spiq.data);
#endif

#if PURE_DELTA_STEP >= 3
					overlay_overlay_helper_exit();
#endif

#if PURE_DELTA_STEP >= 4
					if (ret2 == SPI_OK_SELECT && SPI_tuptable && SPI_tuptable->vals)
					{
#endif
#if PURE_DELTA_STEP >= 5
						{
							uint64 i;
							MemoryContext oldmc;
							/* Use executor's per-query context: CurrentMemoryContext
							 * may have been switched to a child of the subplan's
							 * per-tuple context which can disappear when subplan
							 * finishes.  Per-query is long-lived for entire
							 * query lifetime and always valid for raw_rows allocation. */
							oldmc = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
							for (i = 0; i < SPI_processed; i++)
							{
								bool isn_td;
								char *pkstr = SPI_getvalue(SPI_tuptable->vals[i],
								                           SPI_tuptable->tupdesc, 1);
								bytea *td = (bytea*) SPI_getbinval(
									SPI_tuptable->vals[i],
									SPI_tuptable->tupdesc, 2, &isn_td);
								char *ov = SPI_getvalue(SPI_tuptable->vals[i],
								                        SPI_tuptable->tupdesc, 3);
								if (pkstr == NULL || isn_td || td == NULL)
								{
									if (pkstr) pfree(pkstr);
									if (ov) pfree(ov);
									continue;
								}
								{
									struct WR_PDR1 *r;
									int sz = VARSIZE_ANY(td);

									r = (struct WR_PDR1*) palloc0(sizeof(struct WR_PDR1));
									/* Hand the SPI_getvalue() strings straight to
									 * the row (same es_query_cxt lifetime as the
									 * pstrdup()s they replace) so no per-row
									 * intermediate is left behind. */
									r->s = pkstr;
									r->ov = ov;
									r->t = (bytea*) palloc(sz);
									memcpy(r->t, td, sz);
									raw_rows = lappend(raw_rows, r);
								}
							}
							MemoryContextSwitchTo(oldmc);
						}
#endif
#if PURE_DELTA_STEP >= 4
					}
#endif

#if PURE_DELTA_STEP >= 3
					if (in_pure_delta) SPI_finish();
#endif

#if PURE_DELTA_STEP >= 6
					if (raw_rows != NIL)
					{
						ListCell *lc;
						List     *seen = NIL;
						foreach(lc, raw_rows)
						{
							struct WR_PDR1 *r = (struct WR_PDR1*) lfirst(lc);
							DeltaTuple dtu; bool rc;
							ListCell *sl;
							bool dup = false;
							if (r->ov != NULL)
								continue;
							foreach(sl, seen)
							{
								if (strcmp((char*) lfirst(sl), r->s) == 0)
								{ dup = true; break; }
							}
							if (dup)
								continue;
							seen = lappend(seen, r->s);
							memset(&dtu, 0, sizeof(dtu));
							rc = overlay_delta_lookup(bid, relid, r->s, &dtu);
							if (rc && (dtu.op == DELTA_OP_INSERT ||
							           dtu.op == DELTA_OP_UPDATE) &&
							    dtu.tuple_data != NULL)
							{
								TupleTableSlot *ps = reconstruct_slot_from_delta(rel,
								                                                 dtu.tuple_data,
								                                                 queryDesc->estate->es_query_cxt);
								if (ps)
								{
									struct WR_PDC1 *c1_ = (struct WR_PDC1 *) palloc0(sizeof(struct WR_PDC1));
									c1_->slot = ps;
									c1_->key = pstrdup(r->s);
									cand_inserts = lappend(cand_inserts, c1_);
								}
							}
							/* overlay_delta_lookup() deep-copies key /
							 * old_version / tuple_data into TopMemoryContext so
							 * they outlive its SPI_finish().  They are consumed
							 * above (reconstruct_slot_from_delta copies what it
							 * needs), so release them here — otherwise every
							 * pure-delta rewritten row leaks its whole
							 * serialized payload for the backend lifetime
							 * (MF-1).  Mirrors ob_overlay_free_delta_list()
							 * on the read path. */
							if (rc)
							{
								if (dtu.key != NULL)
									pfree(dtu.key);
								if (dtu.old_version != NULL)
									pfree(dtu.old_version);
								if (dtu.tuple_data != NULL)
									pfree(dtu.tuple_data);
							}
						}
						list_free(seen);
						foreach(lc, raw_rows)
						{
							struct WR_PDR1 *r = (struct WR_PDR1*) lfirst(lc);
							if (r->s) pfree(r->s);
							if (r->ov) pfree(r->ov);
							if (r->t) pfree(r->t);
							pfree(r);
						}
						list_free(raw_rows);
						raw_rows = NIL;
					}
#endif

#if PURE_DELTA_STEP >= 7
					if (cand_inserts != NIL)
					{
						PlanState *scan_ps;
						ExprState *qual_scan;
						ExprState *qual_extra;
						ExprContext *econtext;
						ListCell   *lc;
						bool		pass_refs_ctid;
						/* Walk down outerPlanState to the leaf scan node,
						 * stopping early if we hit any level with a non-NULL
						 * qual.  Nodes such as Result or ProjectSet wrap the
						 * actual baserel scan without carrying qual, so we
						 * have to descend. */
						scan_ps = subplan;
						while (outerPlanState(scan_ps) != NULL)
						{
							if (scan_ps->qual != NULL)
								break;
							scan_ps = outerPlanState(scan_ps);
						}
						/* Always grab the ScanState's own qual; most plan
						 * types put the post-recheck restrictions here. */
						qual_scan = scan_ps->qual;
						qual_extra = NULL;
						/* ACC-1 follow-up: a ctid-based DML predicate
						 * cannot address a pure-delta row (no physical
						 * ctid), so treat it as a no-op instead of
						 * letting it match every reconstructed row whose
						 * slot ctid is the invalid ItemPointer. */
						pass_refs_ctid = ob_expr_has_ctid_var(
							(Node *) scan_ps->plan->qual);
						/* For index-based scans, PG pushes the original
						 * restriction expressions into scan-type-specific
						 * fields and leaves ScanState.qual NULL for the
						 * index-reachable portion.  We must re-run those
						 * original quals for pure-delta rows that never
						 * entered the index. */
						if (nodeTag(scan_ps) == T_IndexScanState)
							qual_extra = ((IndexScanState *) scan_ps)->indexqualorig;
						else if (nodeTag(scan_ps) == T_BitmapHeapScanState)
							qual_extra = ((BitmapHeapScanState *) scan_ps)->bitmapqualorig;
						else if (nodeTag(scan_ps) == T_IndexOnlyScanState)
							qual_extra = ((IndexOnlyScanState *) scan_ps)->recheckqual;
						econtext = scan_ps->ps_ExprContext;
						foreach(lc, cand_inserts)
						{
							struct WR_PDC1 *c1_ = (struct WR_PDC1 *) lfirst(lc);
							TupleTableSlot *slot = c1_->slot;
							bool passes = true;
							if (qual_scan != NULL || qual_extra != NULL)
							{
								MemoryContext oldcxt;
								oldcxt = MemoryContextSwitchTo(
									econtext->ecxt_per_tuple_memory);
								if (econtext->ecxt_scantuple)
									ExecClearTuple(econtext->ecxt_scantuple);
								econtext->ecxt_scantuple = slot;
								econtext->ecxt_outertuple = slot;
								if (TTS_IS_VIRTUAL(slot))
								{
									TupleDesc	std = slot->tts_tupleDescriptor;
									if (std && slot->tts_nvalid < std->natts)
										slot->tts_nvalid = std->natts;
								}
								ResetExprContext(econtext);
								if (qual_scan != NULL)
								{
									bool p1 = ExecQual(qual_scan, econtext);
									if (!p1) passes = false;
								}
								if (passes && qual_extra != NULL)
								{
									bool p2 = ExecQual(qual_extra, econtext);
									if (!p2) passes = false;
								}
								MemoryContextSwitchTo(oldcxt);
							}
#if PURE_DELTA_STEP >= 8
							/* MVP safety guard: only run pure-delta write path if
							 * there is an explicit WHERE (scan qual OR scan-type
							 * specific extra qual non-NULL).  Full-table DML
							 * (DELETE FROM t / UPDATE t SET ...) is MVP-out-of-scope
							 * for pure-delta rows; an unqualified DML has no quals
							 * anywhere in the plan tree, so the naive pass would
							 * tombstone every pure-delta INSERT causing silent
							 * data loss. */
							if (qual_scan == NULL && qual_extra == NULL) continue;
							if (pass_refs_ctid) continue;
							if (!passes) continue;
							if (cmd == CMD_DELETE)
							{
								char *pk = pstrdup(c1_->key);
								ndone++;
								/* Run overlay_delta_insert on per-query context to
								 * avoid UB: CurrentMemoryContext at this point may
								 * belong to the now-drained subplan's per-tuple
								 * context.  Force the allocations to live on
								 * es_query_cxt which is guaranteed stable for
								 * entire query lifetime. */
								{
									MemoryContext oldmcdi;
									oldmcdi = MemoryContextSwitchTo(
										queryDesc->estate->es_query_cxt);
									overlay_delta_insert(bid, relid, pk,
									                     DELTA_OP_DELETE, NULL, NULL);
									MemoryContextSwitchTo(oldmcdi);
								}
								if (has_returning)
								{
									TupleTableSlot *rslot =
										ob_project_returning(rri, slot, slot, rel);
									if (rslot && !TupIsNull(rslot))
									{
										MemoryContext oldmcq;
										oldmcq = MemoryContextSwitchTo(
											queryDesc->estate->es_query_cxt);
										if (nretslots >= nretslots_alloc)
										{
											int newsz = (nretslots_alloc == 0)
												? 16 : nretslots_alloc * 2;
											if (retslots == NULL)
												retslots = palloc(
													sizeof(TupleTableSlot*) * newsz);
											else
												retslots = repalloc(retslots,
													sizeof(TupleTableSlot*) * newsz);
											nretslots_alloc = newsz;
										}
										{
											TupleTableSlot *cp;
											TupleDesc td =
												rslot->tts_tupleDescriptor;
											cp = MakeSingleTupleTableSlot(td,
												&TTSOpsHeapTuple);
											ExecCopySlot(cp, rslot);
											ExecMaterializeSlot(cp);
											retslots[nretslots++] = cp;
										}
										MemoryContextSwitchTo(oldmcq);
									}
								}
								pfree(pk);
							}
							else if (cmd == CMD_UPDATE)
							{
								TupleTableSlot *set_plan_slot;
								TupleTableSlot *post_image;
								TupleTableSlot *proj_slot;
								ProjectionInfo *proj_info;
								ExprContext *proj_ecxt;
								char        *pk;
								char        *tdata;
								TupleDesc    reldesc;
								TupleDesc    jdesc;
								TupleTableSlot *save_scantuple;
								TupleTableSlot *save_outertuple;
								int			jnatts;
								int			natts;
								int			k;
								int			r;
								HeapTuple	post_htup;
								PlanState  *subchild;
								TupleDesc	in_desc;
								TupleTableSlot *proj_input;
								List		*child_tlist;
								TargetEntry *cte;
								int			i;

								reldesc = RelationGetDescr(rel);
								natts = reldesc->natts;

								jdesc = CreateTupleDescCopy(
									subplan->ps_ResultTupleSlot->tts_tupleDescriptor);
								jnatts = jdesc->natts;

								set_plan_slot = MakeSingleTupleTableSlot(jdesc,
									&TTSOpsHeapTuple);

								proj_info = subplan->ps_ProjInfo;
								if (proj_info == NULL)
								{
									ExecClearTuple(set_plan_slot);
									if (jnatts != natts)
									{
										ereport(WARNING,
											(errmsg("overlay_branch: pure-delta UPDATE: no projection info and natts mismatch")));
										ExecDropSingleTupleTableSlot(set_plan_slot);
										FreeTupleDesc(jdesc);
										continue;
									}
									for (k = 0; k < jnatts; k++)
									{
										Datum		d;
										bool		isnull;
										Form_pg_attribute patt;
										if (TTS_IS_VIRTUAL(slot) &&
										    slot->tts_nvalid < (AttrNumber)(k+1))
											slot->tts_nvalid = natts;
										d = slot->tts_values[k];
										isnull = slot->tts_isnull[k];
										patt = TupleDescAttr(jdesc, k);
										if (!isnull)
										{
											int16 typlen;
											bool  typbyval;
											get_typlenbyval(patt->atttypid,
												&typlen, &typbyval);
											d = datumCopy(d, typbyval, typlen);
										}
										set_plan_slot->tts_values[k] = d;
										set_plan_slot->tts_isnull[k] = isnull;
									}
									set_plan_slot->tts_nvalid = jnatts;
								}
								else
								{
									/* ACC-1: the DML subplan is typically a
									 * Result node (planner B2) whose
									 * projection consumes the SCAN output
									 * (e.g. {num, ctid}), NOT the full
									 * base-relation row.  Feeding the
									 * reconstructed pre-image straight into
									 * that projection read attribute
									 * positions of the wrong layout and
									 * segfaulted inside ExecProject().
									 *
									 * Synthesize a projection input shaped
									 * like the child's output and fill it
									 * from the pre-image.  The child's
									 * output descriptor cannot be matched by
									 * attribute NAME: a scan node's
									 * targetlist TLEs carry resname == NULL,
									 * so ExecTypeFromTL() yields empty
									 * attnames.  Instead walk the child's
									 * plan targetlist and resolve each
									 * output position through its Var, which
									 * still carries the base relation's
									 * attnum. */
									subchild = outerPlanState(subplan);
									child_tlist = subchild != NULL
										? subchild->plan->targetlist : NIL;
									if (child_tlist != NIL &&
										subchild->ps_ResultTupleSlot != NULL)
										in_desc = subchild->ps_ResultTupleSlot->tts_tupleDescriptor;
									else
										in_desc = reldesc;

									proj_input = MakeSingleTupleTableSlot(in_desc,
										&TTSOpsVirtual);
									for (i = 0; i < in_desc->natts; i++)
									{
										bool		found = false;

										if (child_tlist == NIL)
										{
											/* Child passes the base relation
											 * row through unchanged. */
											if (i < natts)
											{
												Form_pg_attribute ratt =
													TupleDescAttr(reldesc, i);

												if (!ratt->attisdropped)
												{
													bool		isnull;
													Datum		d;

													d = slot_getattr(slot,
														(AttrNumber) (i + 1),
														&isnull);
													if (!isnull)
													{
														int16	typlen;
														bool	typbyval;

														get_typlenbyval(
															ratt->atttypid,
															&typlen, &typbyval);
														d = datumCopy(d, typbyval,
															typlen);
													}
													proj_input->tts_values[i] = d;
													proj_input->tts_isnull[i] = isnull;
													found = true;
												}
											}
										}
										else if (i < list_length(child_tlist))
										{
											Node	   *cexpr;

											cte = (TargetEntry *) list_nth(child_tlist, i);
											cexpr = (Node *) cte->expr;
											while (cexpr != NULL &&
												   IsA(cexpr, RelabelType))
												cexpr = (Node *) ((RelabelType *) cexpr)->arg;
											if (cexpr != NULL && IsA(cexpr, Var))
											{
												Var		   *cv = (Var *) cexpr;

												/* System columns (ctid, ...)
												 * have varattno < 0 and are junk
												 * for our purposes; leave them
												 * NULL.  A positive varattno is
												 * the base relation attnum of the
												 * pre-image. */
												if (cv->varattno > 0 &&
													cv->varattno <= natts)
												{
													bool		isnull;
													Datum		d;

													d = slot_getattr(slot,
														cv->varattno,
														&isnull);
													if (!isnull)
													{
														int16	typlen;
														bool	typbyval;

														get_typlenbyval(
															TupleDescAttr(in_desc, i)->atttypid,
															&typlen, &typbyval);
														d = datumCopy(d, typbyval,
															typlen);
													}
													proj_input->tts_values[i] = d;
													proj_input->tts_isnull[i] = isnull;
													found = true;
												}
											}
										}
										if (!found)
										{
											proj_input->tts_values[i] = (Datum) 0;
											proj_input->tts_isnull[i] = true;
										}
									}
									ExecStoreVirtualTuple(proj_input);

									proj_ecxt = proj_info->pi_exprContext;
									save_scantuple = proj_ecxt->ecxt_scantuple;
									save_outertuple = proj_ecxt->ecxt_outertuple;
									{
										MemoryContext oldcxt;
										oldcxt = MemoryContextSwitchTo(
											proj_ecxt->ecxt_per_tuple_memory);
										ResetExprContext(proj_ecxt);
										proj_ecxt->ecxt_scantuple = proj_input;
										proj_ecxt->ecxt_outertuple = proj_input;
										proj_ecxt->ecxt_innertuple = NULL;
										proj_slot = ExecProject(proj_info);
										ExecClearTuple(set_plan_slot);
										if (TTS_IS_VIRTUAL(proj_slot) &&
										    proj_slot->tts_nvalid < jnatts)
											proj_slot->tts_nvalid = jnatts;
										for (k = 0; k < jnatts; k++)
										{
											Datum		d;
											bool		isnull;
											Form_pg_attribute patt;
											d = proj_slot->tts_values[k];
											isnull = proj_slot->tts_isnull[k];
											patt = TupleDescAttr(jdesc, k);
											if (!isnull)
											{
												int16 typlen;
												bool  typbyval;
												get_typlenbyval(patt->atttypid,
													&typlen, &typbyval);
												d = datumCopy(d, typbyval, typlen);
											}
											set_plan_slot->tts_values[k] = d;
											set_plan_slot->tts_isnull[k] = isnull;
										}
										set_plan_slot->tts_nvalid = jnatts;
										MemoryContextSwitchTo(oldcxt);
									}
									proj_ecxt->ecxt_scantuple = save_scantuple;
									proj_ecxt->ecxt_outertuple = save_outertuple;
									ExecDropSingleTupleTableSlot(proj_input);
								}

								post_image = MakeSingleTupleTableSlot(reldesc,
									&TTSOpsHeapTuple);
								{
									MemoryContext oldmc;
									/* The post-image copies are consumed by
									 * heap_form_tuple() immediately below and only
									 * need statement lifetime; TopMemoryContext
									 * leaked one datum per pure-delta rewritten row
									 * (MF-1). */
									oldmc = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
									for (k = 0; k < natts; k++)
									{
										Datum		d;
										bool		isnull;
										Form_pg_attribute patt;
										if (TTS_IS_VIRTUAL(slot) &&
										    slot->tts_nvalid < (AttrNumber)(k+1))
											slot->tts_nvalid = natts;
										d = slot->tts_values[k];
										isnull = slot->tts_isnull[k];
										patt = TupleDescAttr(reldesc, k);
										if (!isnull)
										{
											int16 typlen;
											bool  typbyval;
											get_typlenbyval(patt->atttypid,
												&typlen, &typbyval);
											d = datumCopy(d, typbyval, typlen);
										}
										post_image->tts_values[k] = d;
										post_image->tts_isnull[k] = isnull;
									}
									post_image->tts_nvalid = natts;

									for (r = 0; r < natts; r++)
									{
										Form_pg_attribute ratt =
											TupleDescAttr(reldesc, r);
										const char *rname = NameStr(ratt->attname);
										int j;
										if (ratt->attisdropped) continue;
										for (j = 0; j < jnatts; j++)
										{
											Form_pg_attribute jatt =
												TupleDescAttr(jdesc, j);
											if (jatt->attisdropped) continue;
											if (strcmp(NameStr(jatt->attname),
											           rname) == 0)
											{
												bool jisnull;
												Datum jvalue;
												/* ===== REVIEW-260926 / R03
												 * (pure-delta path) =====
												 * Same PK-mutation guard as
												 * MAIN-hit set-merge path: if
												 * the SET target column is a
												 * primary-key column, forbid
												 * BEFORE any delta_insert.
												 * Without this guard the
												 * pure-delta post-image
												 * would carry a DIFFERENT key
												 * from the delta row's key
												 * (the original INSERT key)
												 * so live-view lookups and
												 * apply both see desynced
												 * keys. */
												if (pk_attnums_set != NULL &&
													bms_is_member(ratt->attnum - 1,
																  pk_attnums_set))
												{
													ereport(ERROR,
														(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
														 errmsg("overlay_branch MVP does not support UPDATE that modifies primary key columns on pure-delta rows (R03 pk-mutation blocked)"),
														 errdetail("SET target column \"%s\" is part of the relation primary key.",
														           rname),
														 errhint("To change a PK on a pure-delta row, DELETE it, then INSERT a new row with the desired new primary key.  Both DELETE and INSERT DML are fully supported for pure-delta rows.")));
												}
												if (set_plan_slot->tts_nvalid <
												    (AttrNumber)(j+1))
													set_plan_slot->tts_nvalid =
														jnatts;
												jvalue = slot_getattr(
													set_plan_slot,
													(AttrNumber)(j+1), &jisnull);
												if (!jisnull)
												{
													int16 typlen;
													bool  typbyval;
													get_typlenbyval(ratt->atttypid,
														&typlen, &typbyval);
													jvalue = datumCopy(jvalue,
														typbyval, typlen);
													post_image->tts_values[r] =
														jvalue;
													post_image->tts_isnull[r] =
														false;
												}
												/* ===== REVIEW-260926 / R20
												 * (pure-delta path) =====
												 * When SET col = NULL on a
												 * pure-delta row, write the
												 * isnull flag.  The MAIN-hit
												 * path already does this;
												 * we mirror it here so both
												 * code paths behave
												 * identically for NULL
												 * assignments. */
												else
												{
													post_image->tts_values[r] = (Datum) 0;
													post_image->tts_isnull[r] = true;
												}
												break;
											}
										}
									}
									MemoryContextSwitchTo(oldmc);
								}

								post_htup = heap_form_tuple(reldesc,
									post_image->tts_values,
									post_image->tts_isnull);
								ExecClearTuple(post_image);
								ExecStoreHeapTuple(post_htup, post_image, true);

								pk = pstrdup(c1_->key);
								tdata = overlay_serialize_tuple(rel, post_image);

								{
									MemoryContext oldmcdi2;
									oldmcdi2 = MemoryContextSwitchTo(
										queryDesc->estate->es_query_cxt);
									overlay_delta_insert(bid, relid, pk,
									                     DELTA_OP_UPDATE, NULL, tdata);
									MemoryContextSwitchTo(oldmcdi2);
								}
								ndone++;

								if (has_returning)
								{
									TupleTableSlot *rslot;
									rslot = ob_project_returning(rri,
									                             post_image,
									                             set_plan_slot,
									                             rel);
									if (rslot && !TupIsNull(rslot))
									{
										MemoryContext oldmcq;
										oldmcq = MemoryContextSwitchTo(
											queryDesc->estate->es_query_cxt);
										if (nretslots >= nretslots_alloc)
										{
											int newsz = (nretslots_alloc == 0)
												? 16 : nretslots_alloc * 2;
											if (retslots == NULL)
												retslots = palloc(
													sizeof(TupleTableSlot*) * newsz);
											else
												retslots = repalloc(retslots,
													sizeof(TupleTableSlot*) * newsz);
											nretslots_alloc = newsz;
										}
										{
											TupleTableSlot *cp;
											TupleDesc td =
												rslot->tts_tupleDescriptor;
											cp = MakeSingleTupleTableSlot(td,
												&TTSOpsHeapTuple);
											ExecCopySlot(cp, rslot);
											ExecMaterializeSlot(cp);
											retslots[nretslots++] = cp;
										}
										MemoryContextSwitchTo(oldmcq);
									}
								}

								pfree(pk);
								if (tdata) pfree(tdata);
								ExecDropSingleTupleTableSlot(post_image);
								ExecDropSingleTupleTableSlot(set_plan_slot);
								FreeTupleDesc(jdesc);
							}
#endif /* >=8 */
						}
						foreach(lc, cand_inserts)
						{
							struct WR_PDC1 *c1_ = (struct WR_PDC1 *) lfirst(lc);
							if (c1_->slot) ExecDropSingleTupleTableSlot(c1_->slot);
							if (c1_->key) pfree(c1_->key);
							pfree(c1_);
						}
						list_free(cand_inserts);
						cand_inserts = NIL;
					}
#endif /* >=7 */

#if PURE_DELTA_STEP >= 5
					if (raw_rows != NIL)
					{
						ListCell *lc;
						foreach(lc, raw_rows)
						{
							struct WR_PDR1 *r = (struct WR_PDR1*) lfirst(lc);
							if (r->s) pfree(r->s);
							if (r->ov) pfree(r->ov);
							if (r->t) pfree(r->t);
							pfree(r);
						}
						list_free(raw_rows);
					}
#endif
				}
#endif /* >=1 */

				/* Replay collected RETURNING rows into dest. */
				if (has_returning && queryDesc->dest != NULL)
				{
					int			k;
					for (k = 0; k < nretslots; k++)
					{
						(*queryDesc->dest->receiveSlot) (retslots[k], queryDesc->dest);
					}
				}
				/* Slots are owned by estate->es_query_cxt → no free needed here. */

				queryDesc->estate->es_processed = ndone;
				handled = true;
				goto ob_write_redirect_done;
			}

			/* --- CMD_INSERT redirect (including UPSERT ON CONFLICT) --- */
			if (cmd == CMD_INSERT && overlay_should_redirect(rel))
			{
				PlanState  *subplan;
				uint64		ninserted = 0;
				TupleTableSlot *slot;
				bool		has_returning = (plan->returningLists != NIL);
				TupleTableSlot **retslots = NULL;
				int			nretslots = 0;
				int			nretslots_alloc = 0;
				int32		bid = overlay_branch_get_current_id();
				Oid			relid = RelationGetRelid(rel);
				int			pk_attno = 0;
				OnConflictAction oc_action = ONCONFLICT_NONE;
				ProjectionInfo *oc_proj = NULL;
				ExprState  *oc_where = NULL;

				{
					ResultRelInfo *rri0 = rri;
					/* Primary derivation: use ModifyTable plan fields
					 * because PG17 does NOT always populate
					 * plan->onConflictAction for DO NOTHING (stays 0),
					 * and rri->ri_onConflict can be NULL for DO NOTHING
					 * when no SET targetlist is needed.
					 * Rule:
					 *   plan->arbiterIndexes == NIL → NONE (0, plain INSERT)
					 *   arbiterIndexes set + (onConflictSet set OR
					 *     rri ri_onConflict has ProjInfo) → UPDATE (2)
					 *   arbiterIndexes set + no SET → NOTHING (1)
					 */
					if (plan->arbiterIndexes == NIL)
					{
						oc_action = ONCONFLICT_NONE;
					}
					else if (plan->onConflictSet != NIL ||
							 (rri0->ri_onConflict != NULL &&
							  rri0->ri_onConflict->oc_ProjInfo != NULL))
					{
						oc_action = ONCONFLICT_UPDATE;
						if (rri0->ri_onConflict != NULL)
						{
							oc_proj = rri0->ri_onConflict->oc_ProjInfo;
							oc_where = rri0->ri_onConflict->oc_WhereClause;
						}
					}
					else
					{
						oc_action = ONCONFLICT_NOTHING;
					}
				}
				/* keyless ctid mode has no PK to arbitrate ON CONFLICT against;
				 * the 2-phase UPSERT conflict detector is PK-only.  Reject
				 * explicitly (0A000) rather than silently mis-promoting. */
				if (!overlay_relation_has_pk(rel) && oc_action != ONCONFLICT_NONE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("overlay_branch: INSERT ... ON CONFLICT (UPSERT) is not supported on a table without a PRIMARY KEY (overlay_branch.keyless_tables=on)"),
							 errhint("Add a PRIMARY KEY, or rewrite the statement as a plain INSERT / UPDATE / DELETE.")));

				{
					const char *pk_colname_dummy = NULL;
					if (!overlay_get_pk_single_attno(rel, (AttrNumber*) &pk_attno,
												 &pk_colname_dummy))
						pk_attno = 0;
				}

				subplan = outerPlanState(mt);

			for (;;)
			{
				char	   *pk;
				char	   *tdata;
				char	   *old_version = NULL;
				int			promotion = 0; /* 0=skip 1=INSERT 2=UPDATE(main) 3=UPDATE(pure) */
				TupleTableSlot *pre_image_slot = NULL;
				bytea	   *pure_tuple_bytes = NULL;
				char	   *main_ctid = NULL;

				CHECK_FOR_INTERRUPTS();

				slot = ExecProcNode(subplan);
				if (TupIsNull(slot))
					break;
				if (count != 0 && ninserted >= count)
					break;

				pk = overlay_serialize_identity(rel, slot, DELTA_OP_INSERT);
				tdata = overlay_serialize_tuple(rel, slot);

				/* ================================================================
				 * === B2 UPSERT: 2-Phase Conflict Detection (MAIN-first) ===
				 * ================================================================
				 * Per B1/B2 mandate, conflict detection is strictly 2-phase:
				 *   Phase I  → pure delta SPI lookup (op=I OR op=U per
				 *              BranchScan Pass2 invariant, so INSERT→UPDATE
				 *              collapsed rows do not vanish)
				 *   Phase II → MAIN heap ctid SPI lookup (MAIN-first per
				 *              hard functional constraint)
				 *
				 * promo dispatch matrix after both phases:
				 *   main_hit  pure_hit  oc_action  WHERE  promotion
				 *   false     false     DO_UPD     -      1 (INSERT)
				 *   false     false     DO_NOTHING -      1 (INSERT)
				 *   true      false     DO_UPD     true   2 (UPDATE main)
				 *   true      false     DO_UPD     false  0 (skip WHERE)
				 *   true      false     DO_NOTHING -      0 (skip NOTHING)
				 *   false     true      DO_UPD     true   3 (UPDATE pure)
				 *   false     true      DO_UPD     false  0 (skip WHERE)
				 *   false     true      DO_NOTHING -      0 (skip NOTHING)
				 *   true      true      DO_UPD     true   2 (MAIN wins tie)
				 * ================================================================ */

				/* ---------- Phase I: Pure delta conflict lookup ---------- */
				if (oc_action != ONCONFLICT_NONE)
				{
					Datum		vals_pure[3];
					static const Oid argtypes_pure[3] = {INT4OID, OIDOID, TEXTOID};
					int			ret2;
					bool		in_pure_delta = false;
					MemoryContext oldmc;

					oldmc = NULL;
					/* WR-1a: branch_id/relid/key bound as $1..$3. */
					vals_pure[0] = Int32GetDatum(bid);
					vals_pure[1] = ObjectIdGetDatum(relid);
					vals_pure[2] = CStringGetTextDatum(pk);

					overlay_overlay_helper_enter();
					overlay_write_redirect_exit();
					ret2 = ob_spi_exec_params(
						"SELECT d.op, d.old_version, d.tuple_data FROM " OBTABLE_DELTA " d "
						"WHERE d.branch_id = $1 AND d.relid = $2 AND d.key = $3 "
						"AND (d.op = 'I' OR d.op = 'U') LIMIT 1",
						true, 0, 3, argtypes_pure, vals_pure, NULL);
					pfree(DatumGetPointer(vals_pure[2]));
					in_pure_delta = (ret2 == SPI_OK_SELECT);
					overlay_write_redirect_enter();
					overlay_overlay_helper_exit();

					if (in_pure_delta && SPI_tuptable != NULL && SPI_processed >= 1)
					{
						char	   *op_txt;
						char	   *oldver_txt;
						char	   *td_hex;
						int			hexlen;
						int			i;

						/* FIX-H1: use SPI_getvalue (not SPI_getbinval) for
						 * all columns.  For op/old_version (TEXT) this is
						 * the only correct path; for tuple_data (BYTEA)
						 * SPI returns it as hex-encoded TEXT string, we
						 * run manual hex_decode below instead of
						 * SPI_getbinval which would misinterpret the
						 * server-side wire-format bytea header. */
						op_txt = SPI_getvalue(SPI_tuptable->vals[0],
											  SPI_tuptable->tupdesc, 1);
						oldver_txt = SPI_getvalue(SPI_tuptable->vals[0],
												  SPI_tuptable->tupdesc, 2);
						td_hex = SPI_getvalue(SPI_tuptable->vals[0],
											  SPI_tuptable->tupdesc, 3);

						if (op_txt != NULL && td_hex != NULL &&
							(strcmp(op_txt, "I") == 0 || strcmp(op_txt, "U") == 0))
						{
							/* FIX-H3 pure: allocate pure_tuple_bytes in
							 * LONG-LIVED es_query_cxt (NOT per-tuple
							 * ExprContext which is ResetExprContext'd at
							 * loop tail — would poison pointer → 0x7f
							 * garbage → SIGSEGV during promo=3
							 * reconstruct). */
							int			hstart = 0;
							hexlen = (int) strlen(td_hex);
							/* FIX-H1-2: PG SPI_getvalue bytea uses \x
							 * prefix.  Skip leading 2 chars \x so hex
							 * decode does not misinterpret '\' and 'x'
							 * as nibbles (produces spurious 0x00 byte
							 * prefix corrupting convert_from/UTF8). */
							if (hexlen >= 2 && td_hex[0] == '\\' && td_hex[1] == 'x')
							{
								hstart = 2;
								hexlen -= 2;
							}
							if (hexlen % 2 != 0)
							{
								elog(WARNING, "overlay_branch Phase I pure tuple_data odd hexlen=%d, truncating", hexlen);
								hexlen--;
							}
							oldmc = MemoryContextSwitchTo(
								queryDesc->estate->es_query_cxt);
							pure_tuple_bytes = (bytea *)
								palloc(VARHDRSZ + (hexlen / 2));
							SET_VARSIZE(pure_tuple_bytes,
										VARHDRSZ + (hexlen / 2));
							MemoryContextSwitchTo(oldmc);
							oldmc = NULL;

							/* manual hex decode: td_hex[hstart + i*2]
							 * td_hex[hstart + i*2+1] → one byte VARDATA. */
							for (i = 0; i < hexlen / 2; i++)
							{
								char		c1 = td_hex[hstart + i * 2];
								char		c2 = td_hex[hstart + i * 2 + 1];
								unsigned	b = 0;
								if (c1 >= '0' && c1 <= '9') b = (c1 - '0') << 4;
								else if (c1 >= 'a' && c1 <= 'f') b = ((c1 - 'a') + 10) << 4;
								else if (c1 >= 'A' && c1 <= 'F') b = ((c1 - 'A') + 10) << 4;
								if (c2 >= '0' && c2 <= '9') b |= (c2 - '0');
								else if (c2 >= 'a' && c2 <= 'f') b |= ((c2 - 'a') + 10);
								else if (c2 >= 'A' && c2 <= 'F') b |= ((c2 - 'A') + 10);
								((unsigned char *) VARDATA(pure_tuple_bytes))[i] =
									(unsigned char) b;
							}

							pre_image_slot = reconstruct_slot_from_delta(
								rel, pure_tuple_bytes,
								queryDesc->estate->es_query_cxt);
							old_version = oldver_txt ? pstrdup(oldver_txt) : NULL;
							promotion = 0; /* pure-only mark; combined with
											* Phase II below */

						}
						if (op_txt) pfree(op_txt);
						if (oldver_txt) pfree(oldver_txt);
						if (td_hex) pfree(td_hex);
					}
					SPI_finish();
				}

				/* ---------- Phase II: MAIN heap ctid lookup (MAIN-FIRST) --- */
				if (oc_action != ONCONFLICT_NONE && pk_attno > 0)
				{
					TupleDesc	reldesc;
					int			pk_attidx = pk_attno - 1;
					Oid			pk_type;
					int			ret3;
					char	   *sql2;
					bool		pk_isnull = false;
					Datum		pkdatum;
					Datum		vals_main2[1];
					static const Oid argtypes_main2[1] = {TEXTOID};
					MemoryContext oldmc;

					reldesc = RelationGetDescr(rel);
					pk_type = TupleDescAttr(reldesc, pk_attidx)->atttypid;

					/* TTS safe read: direct tts_values access.  UPSERT
					 * VALUES subplan returns TTSOpsVirtual slots; we
					 * MUST use tts_values[attidx] / tts_isnull[attidx]
					 * directly (after ensuring tts_nvalid >= attno)
					 * instead of slot_getattr / slot_getallattrs which
					 * would MATERIALIZE the virtual slot and corrupt
					 * its by-ref contents. */
					if (TTS_IS_VIRTUAL(slot))
					{
						if (slot->tts_nvalid < (AttrNumber) (pk_attidx + 1))
						{
							TupleDesc	std = slot->tts_tupleDescriptor;
							if (std)
								slot->tts_nvalid = std->natts;
						}
						pkdatum = slot->tts_values[pk_attidx];
						pk_isnull = slot->tts_isnull[pk_attidx];
					}
					else
					{
						pkdatum = slot_getattr(slot, pk_attno, &pk_isnull);
					}

					if (pk_isnull)
					{
						/* PK cannot be NULL for valid conflict detection.
						 * If user passes PK=NULL, this is a plain insert.
						 * promotion remains 0/1 from Phase I. */
					}
					else
					{
						Oid			pk_out_func;
						bool		pk_is_varlena;
						char   *pkval_txt;
						getTypeOutputInfo(pk_type, &pk_out_func, &pk_is_varlena);
						pkval_txt = OidOutputFunctionCall(pk_out_func, pkdatum);

						/* WR-1a: PK value bound as $1; relname/colname/type inline. */
						sql2 = psprintf("SELECT ctid FROM %s WHERE (%s) = $1::text::%s LIMIT 1",
										quote_qualified_identifier(
											get_namespace_name(RelationGetNamespace(rel)),
											RelationGetRelationName(rel)),
										quote_identifier(NameStr(
											TupleDescAttr(reldesc, pk_attidx)->attname)),
										format_type_be(pk_type));
						vals_main2[0] = CStringGetTextDatum(pkval_txt);
						pfree(pkval_txt);

						overlay_overlay_helper_enter();
						overlay_write_redirect_exit();
						ret3 = ob_spi_exec_params(sql2, true, 0, 1,
												  argtypes_main2, vals_main2, NULL);
						overlay_write_redirect_enter();
						overlay_overlay_helper_exit();
						pfree(sql2);
						pfree(DatumGetPointer(vals_main2[0]));

						if (ret3 == SPI_OK_SELECT && SPI_tuptable != NULL &&
							SPI_processed >= 1)
						{
							char *ctid_txt = SPI_getvalue(SPI_tuptable->vals[0],
														  SPI_tuptable->tupdesc, 1);

							if (ctid_txt != NULL)
							{
								char *s;
								char *e;
								/* FIX-H2 ctid sanitize: ctid arrives as
								 * "(blk,off)" textual form.  Strip only
								 * leading/trailing whitespace and validate
								 * parentheses are present — the tid type
								 * REQUIRES the '(X,Y)' string form.
								 * FIX-H3 main: allocate long-lived copy in
								 * es_query_cxt to survive per-tuple
								 * ExprContext reset at loop tail. */
								s = ctid_txt;
								while (*s == ' ' || *s == '\t') s++;
								e = s + strlen(s);
								while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
								{
									int clen = (int) (e - s);
									oldmc = MemoryContextSwitchTo(
										queryDesc->estate->es_query_cxt);
									main_ctid = palloc(clen + 1);
									memcpy(main_ctid, s, clen);
									main_ctid[clen] = '\0';
									MemoryContextSwitchTo(oldmc);
								}
								pfree(ctid_txt);
							}
						}
						SPI_finish();
					}
				}

				/* ================================================================
				 * === Promo dispatch (4-way) ===
				 * ================================================================ */
				{
					bool main_hit = (main_ctid != NULL);
					bool pure_hit = (pre_image_slot != NULL);
					if (oc_action == ONCONFLICT_NONE)
					{
						r21_check_insert_preconditions(rel, slot, bid, relid,
													   pk_attno, pk);
						promotion = 1;
					}
					else if (!main_hit && !pure_hit)
					{
						/* No conflict → promo=1 INSERT (both DO NOTHING /
						 * DO UPDATE fall through to insert) */
						promotion = 1;
					}
					else if (oc_action == ONCONFLICT_NOTHING)
					{
						/* Any conflict + DO NOTHING → promo=0 skip */
						promotion = 0;
					}
					else /* ONCONFLICT_UPDATE + at least one hit */
					{
						/* ========== WHERE cond (for DO UPDATE only) ========== */
						bool where_ok = true;
						if (oc_where != NULL)
						{
							ExprContext *ecx;
							EState *estate = mt->ps.state;
							TupleTableSlot *t_where_scan = NULL;
							bool		main_where_fetched = false;
							ecx = GetPerTupleExprContext(estate);
							if (ecx->ecxt_scantuple)
								ExecClearTuple(ecx->ecxt_scantuple);
							if (ecx->ecxt_innertuple)
								ExecClearTuple(ecx->ecxt_innertuple);
							if (main_hit)
							{
								t_where_scan = fetch_tuple_by_ctid(rel, main_ctid,
																   queryDesc->estate->es_query_cxt);
								main_where_fetched = true;
							}
							else
							{
								t_where_scan = pre_image_slot;
								main_where_fetched = false;
							}
							ecx->ecxt_scantuple = t_where_scan;
							ecx->ecxt_innertuple = slot; /* EXCLUDED */
							if (ecx->ecxt_scantuple && TTS_IS_VIRTUAL(ecx->ecxt_scantuple))
							{
								TupleDesc std = ecx->ecxt_scantuple->tts_tupleDescriptor;
								if (std && ecx->ecxt_scantuple->tts_nvalid < std->natts)
									ecx->ecxt_scantuple->tts_nvalid = std->natts;
							}
							if (ecx->ecxt_innertuple && TTS_IS_VIRTUAL(ecx->ecxt_innertuple))
							{
								TupleDesc itd = ecx->ecxt_innertuple->tts_tupleDescriptor;
								if (itd && ecx->ecxt_innertuple->tts_nvalid < itd->natts)
									ecx->ecxt_innertuple->tts_nvalid = itd->natts;
							}
							ResetExprContext(ecx);
							where_ok = ExecQual(oc_where, ecx);
							ExecClearTuple(ecx->ecxt_scantuple);
							ExecClearTuple(ecx->ecxt_innertuple);
							if (main_where_fetched && t_where_scan)
								ExecDropSingleTupleTableSlot(t_where_scan);
						}
						if (!where_ok)
						{
							promotion = 0;
						}
						else
						{
							/* MAIN-first tie-break: both hit → MAIN wins promo=2 */
							if (main_hit)
							{
								promotion = 2;
							}
							else
							{
								promotion = 3;
							}
						}
					}
				}

				/* ================================================================
				 * === ExecProject SET merge (promo=2/3) + delta write ===
				 * ================================================================ */
				if (promotion == 2 || promotion == 3)
				{
					TupleTableSlot *post_image;
					EState	   *estate = mt->ps.state;
					ExprContext *ecx;
					MemoryContext oldprojctx;
					bool		proj_switched = false;

					/* ========== Build pre_image (scantuple) ========== */
					if (promotion == 2)
					{
						/* promo=2: MAIN row → fetch by ctid */
						pre_image_slot = fetch_tuple_by_ctid(rel, main_ctid,
															 queryDesc->estate->es_query_cxt);
						if (old_version == NULL)
							old_version = overlay_tuple_version(rel, pre_image_slot);
					}
					else
					{
						/* promo=3: pure delta.  If pre_image_slot was NOT
						 * built (pure_tuple_bytes was NULL due to Phase I
						 * read failure or op mismatch), fall back to
						 * INSERT pass: treat same as promo=1 to avoid
						 * false concurrent-delete conflict on NULL
						 * old_version.  Per engineering spec "Apply
						 * logic" invariant. */
						if (pre_image_slot == NULL)
						{
							/* promo=3 but no pure pre-image → treat as fresh INSERT */
							promotion = 1;
						}
					}

					if (promotion == 2 || promotion == 3)
					{
						if (tdata != NULL)
						{
							pfree(tdata);
							tdata = NULL;
						}

						if (oc_proj == NULL || estate == NULL ||
							pre_image_slot == NULL || TupIsNull(pre_image_slot) ||
							slot == NULL || TupIsNull(slot))
						{
							/* Fallback: no projection info or any critical
							 * input NULL → bypass ExecProject, directly use
							 * EXCLUDED slot as post-image (matches "DO
							 * UPDATE SET = EXCLUDED.*" semantic which is
							 * the common case).  This also handles the
							 * pathological edge case where ri_onConflict is
							 * present but oc_ProjInfo was not built by the
							 * planner. */
							post_image = slot;
						}
						else
						{
							ecx = oc_proj->pi_exprContext;
							if (ecx == NULL)
							{
								elog(ERROR, "overlay_branch: oc_proj pi_exprContext is NULL for UPSERT projection");
							}
							oldprojctx = MemoryContextSwitchTo(ecx->ecxt_per_tuple_memory);
							proj_switched = true;
							if (ecx->ecxt_scantuple)
								ExecClearTuple(ecx->ecxt_scantuple);
							if (ecx->ecxt_innertuple)
								ExecClearTuple(ecx->ecxt_innertuple);
							ecx->ecxt_scantuple = pre_image_slot;
							ecx->ecxt_innertuple  = slot; /* EXCLUDED */
							if (ecx->ecxt_scantuple && TTS_IS_VIRTUAL(ecx->ecxt_scantuple))
							{
								TupleDesc	std = ecx->ecxt_scantuple->tts_tupleDescriptor;
								if (std && ecx->ecxt_scantuple->tts_nvalid < std->natts)
									ecx->ecxt_scantuple->tts_nvalid = std->natts;
							}
							if (ecx->ecxt_innertuple && TTS_IS_VIRTUAL(ecx->ecxt_innertuple))
							{
								TupleDesc	itd = ecx->ecxt_innertuple->tts_tupleDescriptor;
								if (itd && ecx->ecxt_innertuple->tts_nvalid < itd->natts)
									ecx->ecxt_innertuple->tts_nvalid = itd->natts;
							}
							ResetExprContext(ecx);

							post_image = ExecProject(oc_proj);
							if (post_image != NULL && !TupIsNull(post_image))
							{
								ExecMaterializeSlot(post_image);
								if (TTS_IS_VIRTUAL(post_image))
								{
									TupleDesc	ptd = post_image->tts_tupleDescriptor;
									if (ptd && post_image->tts_nvalid < ptd->natts)
										post_image->tts_nvalid = ptd->natts;
								}
							}

							ExecClearTuple(ecx->ecxt_scantuple);
							ExecClearTuple(ecx->ecxt_innertuple);
						}

						tdata = overlay_serialize_tuple(rel, post_image);

						{
							MemoryContext oldmc;
							oldmc = MemoryContextSwitchTo(
								queryDesc->estate->es_query_cxt);
							overlay_delta_insert(bid, relid, pk,
												 DELTA_OP_UPDATE, old_version, tdata);
							MemoryContextSwitchTo(oldmc);
						}
						ninserted++;

						/* RETURNING */
						if (has_returning)
						{
							TupleTableSlot *rslot =
								ob_project_returning(rri, post_image, slot, rel);
							if (rslot != NULL && !TupIsNull(rslot))
							{
								MemoryContext oldmcq;
								oldmcq = MemoryContextSwitchTo(
									queryDesc->estate->es_query_cxt);
								if (nretslots >= nretslots_alloc)
								{
									int newsz = nretslots_alloc == 0 ? 16
										: nretslots_alloc * 2;
									if (retslots == NULL)
										retslots = palloc(sizeof(TupleTableSlot *) * newsz);
									else
										retslots = repalloc(retslots,
													sizeof(TupleTableSlot *) * newsz);
									nretslots_alloc = newsz;
								}
								{
								TupleTableSlot *cp;
								TupleDesc	tdesc = rslot->tts_tupleDescriptor;
								ExecMaterializeSlot(rslot);
								if (TTS_IS_VIRTUAL(rslot) && rslot->tts_nvalid < tdesc->natts)
									rslot->tts_nvalid = tdesc->natts;
								cp = MakeSingleTupleTableSlot(tdesc, &TTSOpsHeapTuple);
								ExecCopySlot(cp, rslot);
								ExecMaterializeSlot(cp);
								retslots[nretslots] = cp;
							}
							nretslots++;
							MemoryContextSwitchTo(oldmcq);
						}
					}
					if (proj_switched)
						MemoryContextSwitchTo(oldprojctx);
					}

					if (pre_image_slot != NULL)
					{
						ExecDropSingleTupleTableSlot(pre_image_slot);
						pre_image_slot = NULL;
					}
				}

				if (promotion == 1)
				{
					/* promo=1: fresh INSERT pass */
					{
						MemoryContext oldmc;
						oldmc = MemoryContextSwitchTo(
							queryDesc->estate->es_query_cxt);
						overlay_delta_insert(bid, relid, pk,
											 DELTA_OP_INSERT, NULL, tdata);
						MemoryContextSwitchTo(oldmc);
					}
					ninserted++;

					if (has_returning)
					{
						TupleTableSlot *rslot =
							ob_project_returning(rri, slot, slot, rel);
						if (rslot != NULL && !TupIsNull(rslot))
						{
							MemoryContext oldmcq;
							oldmcq = MemoryContextSwitchTo(
								queryDesc->estate->es_query_cxt);
							if (nretslots >= nretslots_alloc)
							{
								int newsz = nretslots_alloc == 0 ? 16
									: nretslots_alloc * 2;
								if (retslots == NULL)
									retslots = palloc(sizeof(TupleTableSlot *) * newsz);
								else
									retslots = repalloc(retslots,
												sizeof(TupleTableSlot *) * newsz);
								nretslots_alloc = newsz;
							}
							{
							TupleTableSlot *cp;
							TupleDesc	tdesc = rslot->tts_tupleDescriptor;
							ExecMaterializeSlot(rslot);
							if (TTS_IS_VIRTUAL(rslot) && rslot->tts_nvalid < tdesc->natts)
								rslot->tts_nvalid = tdesc->natts;
							cp = MakeSingleTupleTableSlot(tdesc, &TTSOpsHeapTuple);
							ExecCopySlot(cp, rslot);
							ExecMaterializeSlot(cp);
							retslots[nretslots] = cp;
						}
						nretslots++;
						MemoryContextSwitchTo(oldmcq);
					}
				}
			}

				/* promotion == 0 → no-op skip (WHERE false or DO NOTHING hit) */

				pfree(pk);
				if (tdata) pfree(tdata);
				if (old_version) pfree(old_version);
				ExecClearTuple(slot);
			}

				if (has_returning && queryDesc->dest != NULL)
				{
					int			k;
					for (k = 0; k < nretslots; k++)
						(*queryDesc->dest->receiveSlot) (retslots[k], queryDesc->dest);
					for (k = 0; k < nretslots; k++)
						ExecDropSingleTupleTableSlot(retslots[k]);
				}

				queryDesc->estate->es_processed = ninserted;
				handled = true;
				goto ob_write_redirect_done;
			}
		}

		/* Fall through to standard (catalog tables etc.) */
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		handled = true;

ob_write_redirect_done:
		/* If we explicitly handled DML via goto above, we MUST NOT
		 * fall through to standard.  If we fell through to standard,
		 * handled is already set true and we just exit guard. */
	}
	PG_CATCH();
	{
		overlay_write_redirect_exit();
		PG_RE_THROW();
	}
	PG_END_TRY();
	overlay_write_redirect_exit();

	/* ================================================================
	 * REVIEW-260926 / A3+A5 — TOP-LEVEL PANIC CHOKE (belt-and-braces #N):
	 *
	 * If we reach here with handled==false BUT the branch is still ACTIVE
	 * AND the PlannedStmt declares at least one result relation that is
	 * NOT a protected catalog/internal schema, then a non-standard plan
	 * shape (e.g.  CustomScan ModifyTable, trigger-re-entrant DML,
	 * incorrectly-tagged CmdType) has BYPASSED every WR redirect gate
	 * above AND is about to fall through to the caller's
	 * standard_ExecutorRun → SILENT DRIFT-WRITE DIRECTLY TO MAIN.
	 *
	 * This is treated as a DATA-CORRUPTION LEVEL EMERGENCY: we raise
	 * ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE (55000) and force an
	 * ERROR abort of the entire transaction rather than risk even a
	 * single row landing on MAIN outside of the delta machinery.
	 *
	 * The check runs AFTER the PG_TRY block because handled is only
	 * flipped to true inside each CMD_*_redirect branch via the
	 * goto ob_write_redirect_done path.  If we are here with handled
	 * still false after all gates, we are in the bypass danger zone.
	 * ================================================================ */
	if (!handled &&
		overlay_branch_is_active() &&
		(cmd == CMD_INSERT || cmd == CMD_UPDATE ||
		 cmd == CMD_DELETE  || cmd == CMD_MERGE))
	{
		if (queryDesc->plannedstmt != NULL &&
			queryDesc->plannedstmt->resultRelations != NIL)
		{
			ListCell   *lc;
			bool		has_managed = false;

			foreach(lc, queryDesc->plannedstmt->resultRelations)
			{
				Index		rti = lfirst_int(lc);
				RangeTblEntry *rte;
				Oid			nspoid;
				char	   *nsp;

				if (rti <= 0 || queryDesc->plannedstmt->rtable == NULL ||
					rti > list_length(queryDesc->plannedstmt->rtable))
					continue;
				rte = rt_fetch(rti, queryDesc->plannedstmt->rtable);
				if (rte == NULL || rte->rtekind != RTE_RELATION ||
					!OidIsValid(rte->relid))
					continue;
				nspoid = get_rel_namespace(rte->relid);
				if (!OidIsValid(nspoid)) continue;
				nsp = get_namespace_name(nspoid);
				if (nsp == NULL) continue;
				if (strcmp(nsp, "pg_catalog") == 0 ||
					strcmp(nsp, "information_schema") == 0 ||
					strncmp(nsp, "pg_toast", 8) == 0 ||
					strcmp(nsp, OBSCHEMA) == 0)
					continue;
				has_managed = true;
				break;
			}

			if (has_managed)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("overlay_branch PANIC: write-redirection bypass "
								"detected for a managed table inside an active branch "
								"(A3/A5 WR-choke abort; drift-write to MAIN prevented)"),
						 errhint("This statement uses a plan shape not certified "
								 "by overlay_branch MVP.  Please rewrite as a plain "
								 "INSERT/UPDATE/DELETE against one heap relation with "
								 "an explicit PRIMARY KEY.")));
		}
	}

	return handled;
}
