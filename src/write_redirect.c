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
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/index.h"
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
#include "common/hashfn.h"
#include "utils/hsearch.h"

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
 * ob_resolve_upsert_arbiter
 *
 *   Resolve the ON CONFLICT arbiter of a keyless UPSERT.  Returns the
 *   column numbers of the single UNIQUE index the statement targets,
 *   or false when it does not resolve to exactly one plain-column
 *   UNIQUE index (several candidate indexes, or an expression /
 *   partial index key).  Callers treat false as "refuse with 0A000":
 *   the 2-phase conflict detector cannot be rebound to such a key.
 *   `nulls_not_distinct_out` reports the index's NULLS NOT DISTINCT
 *   property so the arbiter probe can pick the right comparison
 *   operator (a NULL arbiter value must not match under the default
 *   NULLS DISTINCT index, exactly like a native unique check).
 * ----------------------------------------------------------------
 */
static bool
ob_resolve_upsert_arbiter(ResultRelInfo *rri, AttrNumber **attnos_out,
						  int *n_out, bool *nulls_not_distinct_out)
{
	List	   *arbiters = rri->ri_onConflictArbiterIndexes;
	Oid			idxoid;
	Relation	idxrel;
	IndexInfo  *ii;
	AttrNumber *attnos;
	int			n;
	int			i;

	if (arbiters == NIL || list_length(arbiters) != 1)
		return false;

	idxoid = linitial_oid(arbiters);
	idxrel = index_open(idxoid, AccessShareLock);
	ii = BuildIndexInfo(idxrel);
	n = ii->ii_NumIndexAttrs;

	if (n <= 0)
	{
		index_close(idxrel, AccessShareLock);
		return false;
	}

	attnos = (AttrNumber *) palloc(sizeof(AttrNumber) * n);
	for (i = 0; i < n; i++)
	{
		AttrNumber	a = ii->ii_IndexAttrNumbers[i];

		if (a <= 0)
		{
			pfree(attnos);
			index_close(idxrel, AccessShareLock);
			return false;
		}
		attnos[i] = a;
	}

	*nulls_not_distinct_out = ii->ii_NullsNotDistinct;
	index_close(idxrel, AccessShareLock);
	*attnos_out = attnos;
	*n_out = n;
	return true;
}

/* ----------------------------------------------------------------
 * ob_slot_col_text
 *
 *   Render one column of a (possibly virtual) slot as text via the
 *   column's output function, and report NULL.  Mirrors the UPSERT
 *   Phase II slot read: a virtual slot must be read through
 *   tts_values/tts_isnull after widening tts_nvalid, never through
 *   slot_getattr (which would materialize and corrupt by-ref content).
 * ----------------------------------------------------------------
 */
static char *
ob_slot_col_text(Relation rel, TupleTableSlot *slot, AttrNumber attno,
				 bool *isnull)
{
	TupleDesc	reldesc = RelationGetDescr(rel);
	int			attidx = attno - 1;
	Oid			typ;
	Oid			outfunc;
	bool		isvarlena;
	Datum		d;

	typ = TupleDescAttr(reldesc, attidx)->atttypid;
	getTypeOutputInfo(typ, &outfunc, &isvarlena);

	if (TTS_IS_VIRTUAL(slot))
	{
		if (slot->tts_nvalid < attno)
		{
			TupleDesc	std = slot->tts_tupleDescriptor;

			if (std)
				slot->tts_nvalid = std->natts;
		}
		d = slot->tts_values[attidx];
		*isnull = slot->tts_isnull[attidx];
	}
	else
	{
		d = slot_getattr(slot, attno, isnull);
	}

	if (*isnull)
		return NULL;
	return OidOutputFunctionCall(outfunc, d);
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

/*
 * ob_claimed_hash / ob_claimed_match / ob_claim_key
 *
 *	Same-statement duplicate-arbiter-key tracking for the INSERT redirect.
 *	PostgreSQL detects a constrained value proposed twice within one command
 *	at the heap/index level (plain INSERT / DO NOTHING) or via the "cannot
 *	affect row a second time" check (ON CONFLICT DO UPDATE).  The redirect
 *	writes every row into the delta under ON CONFLICT DO UPDATE, and a
 *	command-counter bump never happens between the rows of a single command,
 *	so the Phase I/II and R21 SPI probes cannot see an earlier row written by
 *	THIS command.  This small hash set, alive for the duration of one
 *	statement, lets the loop reproduce the native outcome: a later candidate
 *	whose arbiter key was already written errors (plain INSERT ->
 *	unique_violation, DO UPDATE -> cardinality_violation) or is skipped
 *	(DO NOTHING), exactly as upstream does.
 */
typedef struct ObClaimedKey
{
	char	   *key;			/* NUL-terminated serialized identity (in cxt) */
} ObClaimedKey;

static uint32
ob_claimed_hash(const void *key, Size keysize)
{
	const char *s = *((const char *const *) key);

	(void) keysize;
	return hash_bytes((const unsigned char *) s, (int) strlen(s));
}

static int
ob_claimed_match(const void *key1, const void *key2, Size keysize)
{
	const char *s1 = *((const char *const *) key1);
	const char *s2 = *((const char *const *) key2);

	(void) keysize;
	return strcmp(s1, s2);
}

static void
ob_claim_key(HTAB *claimed, MemoryContext cxt, const char *pk)
{
	ObClaimedKey *ent;
	char	   *probe = (char *) pk;
	bool		found;
	MemoryContext old;

	ent = (ObClaimedKey *) hash_search(claimed, &probe, HASH_ENTER, &found);
	if (!found)
	{
		old = MemoryContextSwitchTo(cxt);
		ent->key = pstrdup(pk);
		MemoryContextSwitchTo(old);
	}
}

/*
 * ob_pk_value_text
 *
 *	Return the textual output of the row's single-column PK, matching the
 *	`(col)=(value)` wording used by the R21 pre-check and by native
 *	PostgreSQL.  Returns NULL when the table has no PK or the value is NULL
 *	(so callers can fall back to the serialized identity).
 */
static char *
ob_pk_value_text(Relation rel, TupleTableSlot *slot, AttrNumber pk_attno)
{
	TupleDesc	reldesc;
	int			pk_attidx;
	Oid			pk_type;
	Datum		pkdatum;
	bool		pk_isnull = false;
	Oid			pk_out_func;
	bool		pk_is_varlena;

	if (rel == NULL || slot == NULL || TupIsNull(slot) || pk_attno <= 0)
		return NULL;

	reldesc = RelationGetDescr(rel);
	pk_attidx = pk_attno - 1;
	pk_type = TupleDescAttr(reldesc, pk_attidx)->atttypid;

	if (TTS_IS_VIRTUAL(slot))
	{
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
		return NULL;

	getTypeOutputInfo(pk_type, &pk_out_func, &pk_is_varlena);
	return OidOutputFunctionCall(pk_out_func, pkdatum);
}

/* ----------------------------------------------------------------
 * r21_check_insert_preconditions
 *
 *   R21 pre-check pass for INSERT-path rows (called when the
 *   final promotion decision is INSERT, i.e. promo=1 would be set).
 *   Raises native PostgreSQL errors BEFORE any delta or MAIN write
 *   happens so the transaction has no partial modifications.
 *
 *   Checks performed (any table with a PRIMARY KEY, single- or multi-column):
 *     1. NOT NULL : every attnotnull=true (non-dropped, user) column
 *                   in `slot` must be non-NULL → ERRCODE_NOT_NULL_VIOLATION
 *     2. PK UNIQUE (MAIN side)  : same PK must not already exist in
 *                                 the MAIN heap → ERRCODE_UNIQUE_VIOLATION
 *     3. PK UNIQUE (delta side) : same PK must not already exist in
 *                                 the ACTIVE pure-delta (op I/U rows)
 *                                 → ERRCODE_UNIQUE_VIOLATION
 *
 *   Fail-open safe: a keyless table has no uniqueness constraint, so the
 *   PK checks are skipped and we return silently.
 * ----------------------------------------------------------------
 */
static void
r21_check_insert_preconditions(Relation rel, TupleTableSlot *slot,
							   int32 bid, Oid relid,
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
					 errdetail("Failing row contains an R21 pre-check violation on INSERT path.")));
		}
	}

	/* PK scoped checks (single- or multi-column); keyless tables have no
	 * uniqueness constraint to enforce. */
	if (!overlay_relation_has_pk(rel))
		return;

	{
		List	   *pk_list = overlay_get_pk_attnos_list(rel);
		int			n_pk = list_length(pk_list);
		ListCell   *lc;
		int			ci;
		Oid		   *argtypes_main;
		Datum	   *vals_main;
		StringInfoData colnames;	/* "a, b" — for the errdetail message */
		StringInfoData valdesc;		/* "1, 2" — for the errdetail message */
		StringInfoData where_main;	/* "a = $1::text::int4 AND b = $2::text::int4" */
		bool		any_null = false;

		Assert(n_pk > 0);

		argtypes_main = (Oid *) palloc(sizeof(Oid) * n_pk);
		vals_main = (Datum *) palloc0(sizeof(Datum) * n_pk);
		initStringInfo(&colnames);
		initStringInfo(&valdesc);
		initStringInfo(&where_main);

		/* Collect every PK column's textual value (matching the write-side
		 * overlay_serialize_pk() ordering) and fold the MAIN-side WHERE. */
		ci = 0;
		foreach(lc, pk_list)
		{
			AttrNumber	attno = (AttrNumber) lfirst_int(lc);
			int			attidx = attno - 1;
			Form_pg_attribute att = TupleDescAttr(reldesc, attidx);
			Datum		d;
			bool		isnull = false;

			if (TTS_IS_VIRTUAL(slot))
			{
				/* R21 PK read: mirror the single-column safe pattern — only
				 * access tts_values/tts_isnull AFTER slot_getsomeattrs has
				 * populated the prefix up to this column. */
				if (slot->tts_nvalid < (AttrNumber) (attidx + 1))
					slot_getsomeattrs(slot, attidx + 1);
				d = slot->tts_values[attidx];
				isnull = slot->tts_isnull[attidx];
			}
			else
			{
				d = slot_getattr(slot, attno, &isnull);
			}

			if (ci > 0)
			{
				appendStringInfoString(&colnames, ", ");
				appendStringInfoString(&valdesc, ", ");
				appendStringInfoString(&where_main, " AND ");
			}
			appendStringInfoString(&colnames, NameStr(att->attname));
			appendStringInfo(&where_main, "%s = $%d::text::%s",
							 quote_identifier(NameStr(att->attname)),
							 ci + 1, format_type_be(att->atttypid));
			argtypes_main[ci] = TEXTOID;

			if (isnull)
			{
				any_null = true;
			}
			else
			{
				Oid			out_func;
				bool		out_varlena;
				char	   *valtxt;

				getTypeOutputInfo(att->atttypid, &out_func, &out_varlena);
				valtxt = OidOutputFunctionCall(out_func, d);
				appendStringInfoString(&valdesc, valtxt);
				vals_main[ci] = CStringGetTextDatum(valtxt);
				pfree(valtxt);
			}
			ci++;
		}
		list_free(pk_list);

		/* A NULL PK column cannot collide (NULL is not equal to anything in
		 * SQL), and PK columns are attnotnull so the scan above already
		 * rejected this.  Bail out defensively without any uniqueness check. */
		if (any_null)
		{
			for (ci = 0; ci < n_pk; ci++)
				if (DatumGetPointer(vals_main[ci]) != NULL)
					pfree(DatumGetPointer(vals_main[ci]));
			pfree(vals_main);
			pfree(argtypes_main);
			pfree(colnames.data);
			pfree(valdesc.data);
			pfree(where_main.data);
			return;
		}

		/* ---------- delta-side uniqueness: an op I/U row holds this key.
		 * ---------- */
		{
			Datum		vals_delta[3];
			static const Oid argtypes_delta[3] = {INT4OID, OIDOID, TEXTOID};
			int			ret_delta;
			char		delta_latest_op = 0; /* 0 = no entry; 'I'/'U'/'D' otherwise */

			/* WR-1a: branch_id/relid/key bound as $1..$3.  The delta key is
			 * the full serialized PK identity (composite PKs included). */
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
				/* colnames/valdesc live in the caller's memory context, not
				 * in the SPI procedure context, so they survive SPI_finish()
				 * below.  (Copying them with pstrdup() here would place the
				 * copy in the SPI context, which SPI_finish() then frees --
				 * the ereport would read freed memory.) */
				SPI_finish();
				overlay_write_redirect_enter();
				overlay_overlay_helper_exit();
				ereport(ERROR,
						(errcode(ERRCODE_UNIQUE_VIOLATION),
						 errmsg("duplicate key value violates unique constraint on relation \"%s\"",
								RelationGetRelationName(rel)),
						 errdetail("Key (%s)=(%s) already exists in branch delta (R21 pre-check).",
								   colnames.data, valdesc.data)));
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
				char	   *sql_main;
				int			ret_main;

				/* WR-1a: PK value(s) bound as $1..$n; relname/colnames/types inline. */
				sql_main = psprintf("SELECT 1 FROM %s WHERE %s LIMIT 1",
									quote_qualified_identifier(
										get_namespace_name(RelationGetNamespace(rel)),
										RelationGetRelationName(rel)),
									where_main.data);

				overlay_overlay_helper_enter();
				overlay_write_redirect_exit();
				ret_main = ob_spi_exec_params(sql_main, true, 1, n_pk,
											  argtypes_main, vals_main, NULL);
				pfree(sql_main);

				if (ret_main == SPI_OK_SELECT && SPI_tuptable != NULL && SPI_processed >= 1)
				{
					/* See the delta branch above: colnames/valdesc are
					 * allocated outside the SPI context and stay valid after
					 * SPI_finish(). */
					SPI_finish();
					overlay_write_redirect_enter();
					overlay_overlay_helper_exit();
					ereport(ERROR,
							(errcode(ERRCODE_UNIQUE_VIOLATION),
							 errmsg("duplicate key value violates unique constraint on relation \"%s\"",
									RelationGetRelationName(rel)),
							 errdetail("Key (%s)=(%s) already exists in MAIN heap (R21 pre-check).",
									   colnames.data, valdesc.data)));
				}
				overlay_write_redirect_enter();
				overlay_overlay_helper_exit();
				SPI_finish();
			}
		}

		for (ci = 0; ci < n_pk; ci++)
			pfree(DatumGetPointer(vals_main[ci]));
		pfree(vals_main);
		pfree(argtypes_main);
		pfree(colnames.data);
		pfree(valdesc.data);
		pfree(where_main.data);
	}
}

/* ----------------------------------------------------------------
 * ob_validate_redirect_constraints
 *
 *   Apply the target relation's native NOT NULL and CHECK constraints to a
 *   post-image tuple BEFORE it is written into the delta store, so a
 *   branch-local INSERT/UPDATE fails eagerly with the same SQLSTATE and
 *   message as the identical statement run against MAIN.
 *
 *   The write-redirect short-circuits standard_ExecutorRun(), so the executor
 *   never reaches its own ExecConstraints() call for these rows.  Without this
 *   check an invalid row is silently stored in the delta and only rejected
 *   later, at apply time.
 *
 *   Delegates to the executor's ExecConstraints() so the error detail, the
 *   failing-row description and the SQLSTATE match the native messages
 *   exactly.  No-op when the relation declares no constraints, which is also
 *   required because ExecConstraints() asserts on that case.
 *
 *   The per-tuple ExprContext's scan binding is saved and restored so the
 *   CHECK evaluation cannot leak the temporary post-image slot to callers that
 *   reuse that context (e.g. the UPSERT DO UPDATE WHERE predicate).
 * ----------------------------------------------------------------
 */
static void
ob_validate_redirect_constraints(ResultRelInfo *rri, Relation rel,
								 TupleTableSlot *slot, EState *estate)
{
	TupleDesc	tupdesc;
	ExprContext *ecx;
	TupleTableSlot *save_scantuple;

	if (rri == NULL || rel == NULL || slot == NULL || estate == NULL)
		return;
	if (TupIsNull(slot))
		return;

	tupdesc = RelationGetDescr(rel);
	if (tupdesc->constr == NULL)
		return;

	ecx = GetPerTupleExprContext(estate);
	save_scantuple = ecx->ecxt_scantuple;
	ExecConstraints(rri, slot, estate);
	ecx->ecxt_scantuple = save_scantuple;
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
 *         – no RETURNING support
 *         – extract ctid from junk slot → re-fetch clean MAIN slot
 *         – capture old_version (ctid+xmin token) BEFORE mutation
 *         – for UPDATE, merge SET-values by column NAME into clean slot
 *         – serialize_pk + serialize_tuple → overlay_delta_insert
 *     • CMD_INSERT:
 *         – no RETURNING support
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

	/* Layer1 + FR4: DML path MUST raise ERROR if the branch has been
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
						 errmsg("overlay_branch does not support MERGE inside a branch (R06 WR-stage blocked)"),
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

		/* FIX-I: ModifyingCTE guard (MUST run BEFORE Non-DML return!)
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
		 * Explicit ERROR with helpful hint — silent corruption is
		 * NEVER acceptable, regardless of CmdType. */
		if (queryDesc->plannedstmt != NULL &&
			queryDesc->plannedstmt->hasModifyingCTE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay_branch does not support data-modifying statements inside WITH (CTE) clauses"),
					 errhint("Rewrite WITH (UPDATE/DELETE/INSERT ... RETURNING) SELECT ... "
							 "using a TEMP TABLE to collect RETURNING rows:\n"
							 "  CREATE TEMP TABLE _r AS UPDATE t SET ... RETURNING ...;\n"
							 "  SELECT * FROM _r; DROP TABLE _r;")));
		}

		/* Non-DML CmdType: after throttled(false) DQL check + ModifyingCTE
		 * guard we have nothing to redirect — fall through to standard
		 * executor so SELECTs, DECLARE CURSORs, etc. run normally on MAIN. */
		if (!dml_split)
			return false;
	}

	/* B2-2a DFS walker: locate the FIRST ModifyTableState descendant
	 * of queryDesc->planstate.
	 *
	 * Defensive fallback: for every top-level DML form we support
	 * (INSERT / UPSERT / UPDATE / DELETE, incl. prepared statements and
	 * SPI/plpgsql execution) the PG planner places ModifyTable at the
	 * plan root, so the branch below is normally NOT taken.  It exists
	 * only so that a hypothetical plan shape that wraps the
	 * ModifyTableState under projection / result nodes still gets
	 * redirected instead of silently falling through to
	 * standard_ExecutorRun → MAIN drift.  We DFS the PlanState tree
	 * (64-deep manual stack to avoid recursion) and accept the FIRST
	 * ModifyTableState descendant we find.  This is safe because WR
	 * redirects DML only when mt_nrels==1 && resultRelations==1 anyway.
	 * Scan descent stops early at baserel-type nodes
	 * (Seq/Index/BitmapHeapScan) per Scan_ps degradation invariant —
	 * prevents BitmapIndexScan trap. */
	if (!IsA(queryDesc->planstate, ModifyTableState))
	{
		PlanState  *stack[64];
		PlanState  *mt_ps = NULL;
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
				/* Found: keep it in a LOCAL, never overwrite
				 * queryDesc->planstate.  That field owns the whole
				 * planstate tree for ExecutorEnd/ExecutorFinish; pointing
				 * it at an interior node would skip ExecEndNode of the
				 * wrapper nodes above and leak their resources.  All
				 * redirect logic below works off the local `mt` only. */
				mt_ps = ps;
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
		/* If DFS found no ModifyTableState → no DML in tree */
		if (mt_ps == NULL)
			return false;
		mt = (ModifyTableState *) mt_ps;
	}
	else
		mt = (ModifyTableState *) queryDesc->planstate;

	overlay_write_redirect_enter();
	PG_TRY();
	{
		plan = (ModifyTable *) mt->ps.plan;

		/* --- Step7 DML hard guard (result rels only) ---
		 *
		 * Validate ONLY the DML TARGET relations (the ModifyTable result
		 * relations): those are the rows this statement writes, and each must
		 * be a plain PK heap table (or a keyless table under the
		 * overlay_branch.keyless_tables GUC).
		 *
		 * A range-table entry that is NOT a target is read-only here (the
		 * FROM/USING side of INSERT ... SELECT / UPDATE ... FROM / DELETE
		 * ... USING).  Such a read follows the same DQL policy as a standalone
		 * SELECT: the executor reads MAIN directly (BranchScan's B2 pre-check
		 * injects the overlay only for SELECT and for the UPDATE/DELETE target
		 * baserel), so a no-PK read source must NOT abort the statement.
		 * Walking the full es_range_table here wrongly rejected legitimate
		 * statements such as
		 *   UPDATE pk_t SET v = s.w FROM nopk_s s WHERE pk_t.id = s.k;
		 * whose source is read from MAIN by design. */
		{
			int			i;
			int			nres = mt->mt_nrels;
			int			planres = list_length(plan->resultRelations);

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
		}

		/* --- Single target only --- */
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
				/* ACC-1: branch id + target relid, resolved once for the
				 * redirect loop (both the MAIN-anchored and the pure-delta
				 * branches key their delta lookups / inserts on them). */
				int32		bid;
				Oid			relid;

				subplan = outerPlanState(mt);
				bid = overlay_branch_get_current_id();
				relid = RelationGetRelid(rel);

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

				/* EXPLAIN ANALYZE parity: the redirect short-circuits
				 * standard_ExecutorRun(), so nothing ever drives the
				 * ModifyTable node's own instrumentation.  Bracket the replay
				 * with a start/stop pair so the root node reports real
				 * rows/loops instead of "(never executed)".  No-op when
				 * instrumentation is off (instrument == NULL). */
				if (mt->ps.instrument != NULL)
					InstrStartNode(mt->ps.instrument);

				for (;;)
				{
					char	   *pk;
					char	   *tdata;
					char	   *oldver;
					TupleTableSlot *main_row = NULL;
					TupleTableSlot *overlay_slot;
					TupleTableSlot *old_clean_slot;
					char	   *ctid_cstr = NULL;
					char	   *pure_delta_key = NULL;
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
						 * pure-delta INSERT) has no MAIN ctid.  A DML-target
						 * BranchScan stamps such a row with a SYNTHETIC ctid
						 * and records its delta key, so resolve the key here and
						 * process the row INLINE (below), exactly like a
						 * MAIN-anchored one: the executor has already applied
						 * the statement's join/qual filtering to this subplan
						 * output, so every row that reaches us is one the DML
						 * actually targets (doc/internals.md §2.5). */
						pure_delta_key = overlay_dml_target_pure_delta_key(subplan,
																		   junk_slot);
						if (pure_delta_key == NULL)
						{
							/* Invalid ctid and not a synthetic one: nothing to
							 * redirect (defensive; a DML-target scan never
							 * emits such a row). */
							ExecClearTuple(junk_slot);
							continue;
						}
					}

					if (ctid_cstr != NULL)
					{
						/* ---- MAIN-anchored row: fetch the MAIN heap row and
						 * build the OVERLAY pre-image (MAIN (+) delta).  The
						 * MAIN row anchors both the row identity (ctid) and the
						 * optimistic base-version token, and is the pre-image
						 * fallback when this branch has no delta row for the
						 * key yet. ---- */
						main_row = fetch_tuple_by_ctid(rel, ctid_cstr,
													   queryDesc->estate->es_query_cxt);
						pfree(ctid_cstr);
						ctid_cstr = NULL;

						/* ACC-1 pre-image = the OVERLAY row (MAIN (+) delta).
						 * When this branch already holds a delta row for the
						 * identity, its serialized tuple_data IS the full
						 * post-image; using it (instead of the bare MAIN row)
						 * is what makes a second UPDATE in the same branch
						 * compound correctly: value-dependent SET expressions
						 * (G1) and non-SET columns (G3) then see earlier branch
						 * edits, and the DML subplan evaluated WHERE/SET against
						 * the overlay row too (G2). */
						overlay_slot = main_row;
						delta_deleted = false;
						{
							DeltaTuple	dtu;
							char	   *idkey;
							bool		have_delta;

							memset(&dtu, 0, sizeof(dtu));
							idkey = overlay_serialize_identity(rel, main_row, op);
							have_delta = (idkey != NULL &&
										  overlay_delta_lookup(bid, relid,
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
									/* Keep the MAIN anchor ctid: the overlay row
									 * has no physical ctid of its own. */
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

						/* old_clean_slot = the pre-image the SET-merge mutates
						 * and the serializer / token read.  It aliases main_row
						 * unless a reconstructed overlay slot replaced it; both
						 * are released at the end of the iteration. */
						old_clean_slot = overlay_slot;
						oldver = overlay_tuple_version(rel, main_row);
					}
					else
					{
						/* ---- pure-delta row: no MAIN baseline.  Reconstruct
						 * the pre-image from the delta payload and reuse the
						 * row's stored delta key as its identity. ---- */
						DeltaTuple	dtu;
						bool		have_delta;

						memset(&dtu, 0, sizeof(dtu));
						have_delta = overlay_delta_lookup(bid, relid,
														  pure_delta_key, &dtu);
						if (!have_delta || dtu.op == DELTA_OP_DELETE ||
							dtu.tuple_data == NULL)
						{
							if (dtu.key != NULL)
								pfree(dtu.key);
							if (dtu.old_version != NULL)
								pfree(dtu.old_version);
							if (dtu.tuple_data != NULL)
								pfree(dtu.tuple_data);
							pfree(pure_delta_key);
							ExecClearTuple(junk_slot);
							continue;
						}
						overlay_slot = reconstruct_slot_from_delta(rel,
																   dtu.tuple_data,
																   queryDesc->estate->es_query_cxt);
						if (dtu.key != NULL)
							pfree(dtu.key);
						if (dtu.old_version != NULL)
							pfree(dtu.old_version);
						if (dtu.tuple_data != NULL)
							pfree(dtu.tuple_data);
						if (overlay_slot == NULL)
						{
							pfree(pure_delta_key);
							ExecClearTuple(junk_slot);
							continue;
						}
						old_clean_slot = overlay_slot;
						oldver = NULL;
					}

					if (cmd == CMD_UPDATE)
					{
						TupleDesc	reldesc = RelationGetDescr(rel);
						TupleDesc	junkdesc = junk_slot->tts_tupleDescriptor;
						MemoryContext oldmc;
						int			r;
						/* R03: the pre-image PK, captured lazily the first time
						 * the SET-merge touches a PK column (still unmutated at
						 * that point).  After the merge the post-image PK is
						 * compared against it so only a REAL key change is a
						 * desync; a no-op SET (id = id) is legal, matching
						 * native PostgreSQL and the UPSERT path. */
						TypedKey   *pre_pk = NULL;
						int			pre_pk_n = 0;
						bool		pk_check_active = false;
						const char *pk_col_name = NULL;

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
									 * The SET targetlist mentions a PK
									 * column.  A no-op SET (id = id) is
									 * legal, so do not reject here: capture
									 * the pre-image PK ONCE and defer the
									 * real mutation check to after the
									 * merge.  This is the first PK column
									 * seen, and the loop walks reldesc in
									 * ascending attno order, so no PK column
									 * has been overwritten yet (safe for
									 * composite keys too).  Note
									 * ratt->attnum is the LOGICAL 1-based
									 * attno (heap TupleDescAttr convention)
									 * so the bitmap offset is attnum-1. */
									if (pk_attnums_set != NULL &&
										bms_is_member(ratt->attnum - 1,
													  pk_attnums_set) &&
										!pk_check_active &&
										old_clean_slot != NULL &&
										!TupIsNull(old_clean_slot))
									{
										pre_pk = overlay_build_typed_pk_from_slot(
											rel, old_clean_slot, &pre_pk_n);
										pk_check_active = true;
										pk_col_name = rname;
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
									 * That is a semantic bug.  Fix
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

						/* ===== REVIEW-260926 / R03 (mutation check) =====
						 * Only a REAL PK change desyncs the delta key from the
						 * post-image: the delta row is keyed by the post-image
						 * PK while apply matches MAIN by that same key.  A
						 * no-op SET (id = id) leaves both equal and is
						 * harmless, so it must be allowed (native PostgreSQL
						 * allows it too).  Compare with the type-safe TypedKey
						 * equality (never as text) so BPCHAR rtrim / NUMERIC
						 * precision / TIMESTAMPTZ zone differences cannot
						 * alias.  Mirrors the UPSERT DO UPDATE path. */
						if (pk_check_active)
						{
							int			post_pk_n = 0;
							TypedKey   *post_pk;
							bool		pk_mutated;

							post_pk = overlay_build_typed_pk_from_slot(
								rel, old_clean_slot, &post_pk_n);
							pk_mutated = (overlay_typed_pk_cmp(pre_pk, pre_pk_n,
															   post_pk, post_pk_n) != 0);
							overlay_typed_pk_free(post_pk, post_pk_n);

							if (pk_mutated)
							{
								overlay_typed_pk_free(pre_pk, pre_pk_n);
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("overlay_branch does not support UPDATE that modifies primary key columns (R03 pk-mutation blocked)"),
										 errdetail("Target relation has primary key column \"%s\"; attempting to SET it would desync the delta key from the post-image.",
												   pk_col_name ? pk_col_name : "?"),
										 errhint("To change a PK value inside a branch, DELETE the old row then INSERT a new row with the desired key.  Those two DML are both properly redirected to the delta store.")));
							}
							overlay_typed_pk_free(pre_pk, pre_pk_n);
						}

						/* Eager native NOT NULL + CHECK check on the merged
						 * post-image, so this branch-local UPDATE rejects an
						 * invalid row immediately (23502 / 23514) instead of
						 * storing it in the delta and failing at apply. */
						ob_validate_redirect_constraints(rri, rel, old_clean_slot,
														 queryDesc->estate);

						tdata = overlay_serialize_tuple(rel, old_clean_slot);
					}
					else
					{
						tdata = NULL;
					}

					/* A pure-delta row keeps its stored delta key as identity
					 * (its slot has no physical ctid to re-serialize); a
					 * MAIN-anchored row (re)serializes from the merged
					 * post-image. */
					if (pure_delta_key != NULL)
						pk = pstrdup(pure_delta_key);
					else
						pk = overlay_serialize_identity(rel, old_clean_slot, op);

					overlay_delta_insert(bid, relid, pk, op, oldver, tdata);
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
					 * overlay slot replaced it, the MAIN anchor row too.  A
					 * pure-delta iteration has no main_row. */
					ExecDropSingleTupleTableSlot(old_clean_slot);
					if (main_row != NULL && old_clean_slot != main_row)
						ExecDropSingleTupleTableSlot(main_row);
					if (pure_delta_key != NULL)
						pfree(pure_delta_key);
					ExecClearTuple(junk_slot);
				}

				/* Replay collected RETURNING rows into dest.
				 *
				 * Because the write path short-circuits standard_ExecutorRun()
				 * (we goto ob_write_redirect_done below), the executor never
				 * drives the DestReceiver lifecycle.  We must do it ourselves:
				 * SPI's DestSPI allocates its SPITupleTable in rStartup, and
				 * its receiveSlot (spi_printtup) raises "improper call to
				 * spi_printtup" when that table is missing.  Mirrors the
				 * sendTuples handling in standard_ExecutorRun(). */
				if (has_returning && queryDesc->dest != NULL)
				{
					int			k;

					queryDesc->dest->rStartup(queryDesc->dest,
											  queryDesc->operation,
											  queryDesc->tupDesc);
					for (k = 0; k < nretslots; k++)
					{
						(*queryDesc->dest->receiveSlot) (retslots[k], queryDesc->dest);
					}
					queryDesc->dest->rShutdown(queryDesc->dest);
				}
				/* Slots are owned by estate->es_query_cxt → no free needed here. */

				/* Close the ModifyTable instrumentation opened before the
				 * replay loop; nretslots is the number of RETURNING rows the
				 * node would have emitted (0 without RETURNING), matching the
				 * stock "actual rows" figure. */
				if (mt->ps.instrument != NULL)
					InstrStopNode(mt->ps.instrument, (double) nretslots);

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
				AttrNumber *arb_attnos = NULL;
				int			arb_natts = 0;
				bool		arb_nulls_not_distinct = false;
				OnConflictAction oc_action = ONCONFLICT_NONE;
				ProjectionInfo *oc_proj = NULL;
				ExprState  *oc_where = NULL;
				HTAB	   *claimed = NULL;
				MemoryContext claimed_cxt = NULL;

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
				/* keyless ctid mode has no PK to arbitrate ON CONFLICT
				 * against; resolve the statement's UNIQUE arbiter instead
				 * and rebind the 2-phase detector to its columns.  A
				 * keyless UPSERT whose arbiter is not exactly one
				 * plain-column UNIQUE index is refused (0A000) rather than
				 * silently mis-promoted. */
				if (!overlay_relation_has_pk(rel) && oc_action != ONCONFLICT_NONE)
				{
					if (!ob_resolve_upsert_arbiter(rri, &arb_attnos, &arb_natts,
												   &arb_nulls_not_distinct))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("overlay_branch: INSERT ... ON CONFLICT (UPSERT) on a table without a PRIMARY KEY requires exactly one UNIQUE arbiter index"),
								 errhint("Add a PRIMARY KEY, or target exactly one UNIQUE constraint.")));
				}

				{
					const char *pk_colname_dummy = NULL;
					if (!overlay_get_pk_single_attno(rel, (AttrNumber*) &pk_attno,
												 &pk_colname_dummy))
						pk_attno = 0;
				}

				subplan = outerPlanState(mt);

				/* Per-statement "claimed arbiter keys" set (see ob_claim_key).
				 * Only meaningful for a table with a PK (single- or
				 * multi-column; keyed by serialized identity): a keyless table
				 * has no constraint to violate, so upstream inserts every row.
				 * Parented to the query context so an error longjmp frees it. */
				if (overlay_relation_has_pk(rel))
				{
					HASHCTL		ctl;

					claimed_cxt =
						AllocSetContextCreate(queryDesc->estate->es_query_cxt,
											  "overlay_branch claimed keys",
											  ALLOCSET_SMALL_SIZES);
					memset(&ctl, 0, sizeof(ctl));
					ctl.keysize = sizeof(char *);
					ctl.entrysize = sizeof(ObClaimedKey);
					ctl.hcxt = claimed_cxt;
					ctl.hash = ob_claimed_hash;
					ctl.match = ob_claimed_match;
					claimed = hash_create("overlay_branch claimed keys", 64, &ctl,
										  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
				}

			/* EXPLAIN ANALYZE parity: mirror the UPDATE/DELETE path — open
			 * the ModifyTable node's instrumentation before the replay loop
			 * so the root node is not reported as "(never executed)". */
			if (mt->ps.instrument != NULL)
				InstrStartNode(mt->ps.instrument);

			for (;;)
			{
				char	   *pk;
				char	   *tdata;
				char	   *old_version = NULL;
				char	   *pure_delta_key = NULL;
				int			promotion = 0; /* 0=skip 1=INSERT 2=UPDATE(main) 3=UPDATE(pure) */
				TupleTableSlot *pre_image_slot = NULL;
				bytea	   *pure_tuple_bytes = NULL;
				char	   *main_ctid = NULL;
				bool	   delta_tombstone = false; /* delta holds op='D' for this key */
				bool	   key_claimed = false; /* arbiter key written by an earlier row of this command */

				CHECK_FOR_INTERRUPTS();

				slot = ExecProcNode(subplan);
				if (TupIsNull(slot))
					break;
				if (count != 0 && ninserted >= count)
					break;

				pk = overlay_serialize_identity(rel, slot, DELTA_OP_INSERT);
				tdata = overlay_serialize_tuple(rel, slot);

				/* Eager native NOT NULL + CHECK check on the candidate row,
				 * BEFORE conflict detection, mirroring ExecInsert(): the
				 * executor validates the proposed tuple's constraints first
				 * and only afterwards resolves ON CONFLICT.  Running it here
				 * (rather than only on the rows that end up written) keeps a
				 * branch-local INSERT ... ON CONFLICT in line with MAIN even
				 * when the candidate is skipped (DO NOTHING on conflict) or
				 * its invalid columns would be overwritten by DO UPDATE. */
				ob_validate_redirect_constraints(rri, rel, slot,
												 queryDesc->estate);

				/* Same-statement duplicate check: was this arbiter key already
				 * written by an earlier row of THIS command?  (PK tables only;
				 * `claimed` stays NULL for keyless tables.) */
				if (claimed != NULL)
				{
					char	   *pk_probe = pk;

					(void) hash_search(claimed, &pk_probe, HASH_FIND, &key_claimed);
				}

				/* ================================================================
				 * === B2 UPSERT: 2-Phase Conflict Detection (delta shadows
				 * === MAIN) ===
				 * ================================================================
				 * Per B1/B2 mandate, conflict detection is strictly 2-phase:
				 *   Phase I  → pure delta SPI lookup (op=I OR op=U per
				 *              BranchScan Pass2 invariant, so INSERT→UPDATE
				 *              collapsed rows do not vanish)
				 *   Phase II → MAIN heap ctid SPI lookup (only consulted when
				 *              the delta has no row for the key)
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
				 *   true      true      DO_UPD     true   3 (delta shadows MAIN)
				 *
				 * A delta tombstone (op='D') for the key suppresses the Phase
				 * II MAIN probe entirely, so both main_hit and pure_hit stay
				 * false and any oc_action takes promotion=1 (rebirth INSERT).
				 * ================================================================ */

				/* ---------- Phase I: Pure delta conflict lookup ---------- */
				if (oc_action != ONCONFLICT_NONE)
				{
					Datum	   *vals_pure;
					Oid		   *argtypes_pure;
					char	   *nulls_pure = NULL;
					char	   *sql_pure;
					int			nargs_pure;
					int			ret2;
					bool		in_pure_delta = false;
					MemoryContext oldmc;
					int			i;

					oldmc = NULL;

					if (arb_natts > 0)
					{
						/* Keyless UPSERT: probe a delta row's stored image
						 * on the arbiter columns (the ctid key is not known
						 * yet) and carry d.key back for the promo=3 reuse.
						 * A NULL arbiter column must be bound as a real SQL
						 * NULL ($n IS NULL via the 'n' null-string), not as
						 * a (Datum) 0 payload with a NULL nulls array — the
						 * latter makes SPI dereference a bogus text pointer
						 * and crash. */
						StringInfoData wbuf;

						initStringInfo(&wbuf);
						nargs_pure = 2 + arb_natts;
						argtypes_pure = (Oid *) palloc(sizeof(Oid) * nargs_pure);
						vals_pure = (Datum *) palloc(sizeof(Datum) * nargs_pure);
						nulls_pure = (char *) palloc(sizeof(char) * nargs_pure);
						argtypes_pure[0] = INT4OID;
						vals_pure[0] = Int32GetDatum(bid);
						nulls_pure[0] = ' ';
						argtypes_pure[1] = OIDOID;
						vals_pure[1] = ObjectIdGetDatum(relid);
						nulls_pure[1] = ' ';
						for (i = 0; i < arb_natts; i++)
						{
							AttrNumber	attno = arb_attnos[i];
							Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel),
																  attno - 1);
							bool		cn = false;
							char	   *ct = ob_slot_col_text(rel, slot, attno, &cn);

							appendStringInfo(&wbuf,
											 " AND (jsonb_populate_record(NULL::%s, "
											 "convert_from(d.tuple_data, 'UTF8')::jsonb)).%s "
											 "%s $%d::text::%s",
											 quote_qualified_identifier(
												 get_namespace_name(RelationGetNamespace(rel)),
												 RelationGetRelationName(rel)),
											 quote_identifier(NameStr(att->attname)),
											 arb_nulls_not_distinct
												? "IS NOT DISTINCT FROM" : "=",
											 i + 3,
											 format_type_be(att->atttypid));
							argtypes_pure[i + 2] = TEXTOID;
							if (cn)
							{
								vals_pure[i + 2] = (Datum) 0;
								nulls_pure[i + 2] = 'n';
							}
							else
							{
								vals_pure[i + 2] = CStringGetTextDatum(ct);
								nulls_pure[i + 2] = ' ';
							}
							if (ct != NULL)
								pfree(ct);
						}
						sql_pure = psprintf(
							"SELECT d.op, d.old_version, d.tuple_data, d.key FROM "
							OBTABLE_DELTA " d WHERE d.branch_id = $1 AND d.relid = $2 "
							"AND (d.op = 'I' OR d.op = 'U')%s LIMIT 1", wbuf.data);
						pfree(wbuf.data);
					}
					else
					{
						/* PK path: probe on the serialized PK (unchanged). */
						nargs_pure = 3;
						argtypes_pure = (Oid *) palloc(sizeof(Oid) * 3);
						vals_pure = (Datum *) palloc(sizeof(Datum) * 3);
						argtypes_pure[0] = INT4OID;
						argtypes_pure[1] = OIDOID;
						argtypes_pure[2] = TEXTOID;
						vals_pure[0] = Int32GetDatum(bid);
						vals_pure[1] = ObjectIdGetDatum(relid);
						vals_pure[2] = CStringGetTextDatum(pk);
						sql_pure = pstrdup(
							"SELECT d.op, d.old_version, d.tuple_data FROM "
							OBTABLE_DELTA " d WHERE d.branch_id = $1 AND d.relid = $2 "
							"AND d.key = $3 LIMIT 1");
					}

					overlay_overlay_helper_enter();
					overlay_write_redirect_exit();
					ret2 = ob_spi_exec_params(sql_pure, true, 0, nargs_pure,
											  argtypes_pure, vals_pure, nulls_pure);
					overlay_write_redirect_enter();
					overlay_overlay_helper_exit();
					pfree(sql_pure);
					/* $1/$2 are pass-by-value; only the text payloads (and
					 * NULL arbiter columns passed as (Datum) 0) remain. */
					for (i = 2; i < nargs_pure; i++)
						if (DatumGetPointer(vals_pure[i]) != NULL)
							pfree(DatumGetPointer(vals_pure[i]));
					pfree(vals_pure);
					pfree(argtypes_pure);
					if (nulls_pure != NULL)
						pfree(nulls_pure);
					in_pure_delta = (ret2 == SPI_OK_SELECT);

					if (in_pure_delta && SPI_tuptable != NULL && SPI_processed >= 1)
					{
						char	   *op_txt;
						char	   *oldver_txt;
						char	   *td_hex;
						int			hexlen;

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
							/* old_version / pure_delta_key MUST outlive the
							 * SPI_finish() below: the current context here is
							 * the SPI proc context, which SPI_finish tears
							 * down, and the loop tail later pfree()s both.
							 * Anchor them in es_query_cxt, exactly like
							 * pure_tuple_bytes above; otherwise the tail
							 * frees a dangling pointer (invalid-pfree PANIC
							 * once a pure-delta row carries a real
							 * old_version, i.e. it was written by an UPDATE). */
							oldmc = MemoryContextSwitchTo(
								queryDesc->estate->es_query_cxt);
							old_version = oldver_txt ? pstrdup(oldver_txt) : NULL;
							if (arb_natts > 0)
							{
								/* Keep the existing delta key so promo=3
								 * updates that row rather than inserting a
								 * fresh synthetic-key duplicate. */
								char	   *k_txt = SPI_getvalue(SPI_tuptable->vals[0],
																  SPI_tuptable->tupdesc, 4);

								if (k_txt != NULL)
								{
									pure_delta_key = pstrdup(k_txt);
									pfree(k_txt);
								}
							}
							MemoryContextSwitchTo(oldmc);
							oldmc = NULL;
							promotion = 0; /* pure-only mark; combined with
											* Phase II below */

						}
						else if (op_txt != NULL && strcmp(op_txt, "D") == 0)
						{
							/* The delta holds a tombstone (op='D') for this key: the
							 * branch has logically deleted the row, so an INSERT ... ON
							 * CONFLICT must rebirth it exactly like the plain-INSERT R21
							 * path (which permits rebirth after a tombstone).  Flag it so
							 * Phase II does NOT re-find the still-present MAIN row and
							 * misclassify DO NOTHING / WHERE-false as a conflict; the promo
							 * dispatch then falls through to promotion = 1 (INSERT). */
							delta_tombstone = true;
						}
						if (op_txt) pfree(op_txt);
						if (oldver_txt) pfree(oldver_txt);
						if (td_hex) pfree(td_hex);
					}
					SPI_finish();
				}

				/* ---------- Phase II: MAIN heap ctid lookup (delta-first
				 * ---------- shadow: only consulted on a delta miss) --- */
				/* Enter for any PK table (single- AND multi-column) and for a
				 * keyless table with a resolved UNIQUE arbiter.  A composite
				 * PK leaves pk_attno == 0 (overlay_get_pk_single_attno is a
				 * single-column fast path only), so gating on `pk_attno > 0`
				 * would silently skip the MAIN probe and misclassify every
				 * ON CONFLICT as a fresh INSERT. */
				if (oc_action != ONCONFLICT_NONE && !delta_tombstone &&
					(overlay_relation_has_pk(rel) || arb_natts > 0))
				{
					TupleDesc	reldesc = RelationGetDescr(rel);
					int			ret3;
					char	   *sql2 = NULL;
					Datum	   *vals_main2 = NULL;
					Oid		   *argtypes_main2 = NULL;
					char	   *nulls_main2 = NULL;
					int			nargs_main2 = 0;
					bool		probe_ok = true;
					int			i;
					MemoryContext oldmc;

					if (arb_natts > 0)
					{
						/* Keyless UPSERT: match a MAIN row on the arbiter
						 * columns (the ctid key is not known up front).  As
						 * in Phase I, a NULL arbiter column must be bound as
						 * a real SQL NULL via the 'n' null-string, never as
						 * a (Datum) 0 text payload. */
						StringInfoData wbuf;

						initStringInfo(&wbuf);
						nargs_main2 = arb_natts;
						argtypes_main2 = (Oid *) palloc(sizeof(Oid) * nargs_main2);
						vals_main2 = (Datum *) palloc(sizeof(Datum) * nargs_main2);
						nulls_main2 = (char *) palloc(sizeof(char) * nargs_main2);
						for (i = 0; i < arb_natts; i++)
						{
							AttrNumber	attno = arb_attnos[i];
							Form_pg_attribute att = TupleDescAttr(reldesc, attno - 1);
							bool		cn = false;
							char	   *ct = ob_slot_col_text(rel, slot, attno, &cn);

							appendStringInfo(&wbuf,
											 "%s%s %s $%d::text::%s",
											 (i == 0) ? " WHERE " : " AND ",
											 quote_identifier(NameStr(att->attname)),
											 arb_nulls_not_distinct
												? "IS NOT DISTINCT FROM" : "=",
											 i + 1,
											 format_type_be(att->atttypid));
							argtypes_main2[i] = TEXTOID;
							if (cn)
							{
								vals_main2[i] = (Datum) 0;
								nulls_main2[i] = 'n';
							}
							else
							{
								vals_main2[i] = CStringGetTextDatum(ct);
								nulls_main2[i] = ' ';
							}
							if (ct != NULL)
								pfree(ct);
						}
						sql2 = psprintf("SELECT ctid FROM %s%s LIMIT 1",
										quote_qualified_identifier(
											get_namespace_name(RelationGetNamespace(rel)),
											RelationGetRelationName(rel)),
										wbuf.data);
						pfree(wbuf.data);
					}
					else if (pk_attno > 0)
					{
						int			pk_attidx = pk_attno - 1;
						Oid			pk_type = TupleDescAttr(reldesc, pk_attidx)->atttypid;
						bool		pk_isnull = false;
						Datum		pkdatum;

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
							probe_ok = false;
						}
						else
						{
							Oid			pk_out_func;
							bool		pk_is_varlena;
							char	   *pkval_txt;

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
							nargs_main2 = 1;
							argtypes_main2 = (Oid *) palloc(sizeof(Oid));
							vals_main2 = (Datum *) palloc(sizeof(Datum));
							argtypes_main2[0] = TEXTOID;
							vals_main2[0] = CStringGetTextDatum(pkval_txt);
							pfree(pkval_txt);
						}
					}
					else
					{
						/* Composite PRIMARY KEY (2+ columns): probe MAIN on
						 * every key column, folding the same
						 * "col = $n::text::<type>" predicate as the R21
						 * write-side precheck.  Single-column PKs never reach
						 * here (pk_attno > 0 above). */
						List	   *pk_list = overlay_get_pk_attnos_list(rel);
						int			n_pk = list_length(pk_list);
						ListCell   *lc;
						StringInfoData wbuf;
						int			ci;

						Assert(n_pk > 1);
						initStringInfo(&wbuf);
						nargs_main2 = n_pk;
						argtypes_main2 = (Oid *) palloc(sizeof(Oid) * n_pk);
						/* palloc0 so an early bail (NULL PK column) leaves
						 * every slot a real NULL the cleanup can skip. */
						vals_main2 = (Datum *) palloc0(sizeof(Datum) * n_pk);

						ci = 0;
						foreach(lc, pk_list)
						{
							AttrNumber	attno = (AttrNumber) lfirst_int(lc);
							int			attidx = attno - 1;
							Form_pg_attribute att = TupleDescAttr(reldesc, attidx);
							Datum		d;
							bool		isnull = false;

							/* TTS safe read: virtual slots must be addressed
							 * through tts_values after ensuring the prefix is
							 * populated (same rule as the single-column path
							 * and R21). */
							if (TTS_IS_VIRTUAL(slot))
							{
								if (slot->tts_nvalid < (AttrNumber) (attidx + 1))
									slot_getsomeattrs(slot, attidx + 1);
								d = slot->tts_values[attidx];
								isnull = slot->tts_isnull[attidx];
							}
							else
							{
								d = slot_getattr(slot, attno, &isnull);
							}

							if (isnull)
							{
								/* A NULL PK column cannot conflict; fall back
								 * to a plain INSERT (probe_ok stays false). */
								probe_ok = false;
								break;
							}

							appendStringInfo(&wbuf, "%s%s = $%d::text::%s",
											 (ci == 0) ? " WHERE " : " AND ",
											 quote_identifier(NameStr(att->attname)),
											 ci + 1,
											 format_type_be(att->atttypid));
							argtypes_main2[ci] = TEXTOID;
							{
								Oid			out_func;
								bool		out_varlena;
								char	   *valtxt;

								getTypeOutputInfo(att->atttypid, &out_func,
												  &out_varlena);
								valtxt = OidOutputFunctionCall(out_func, d);
								vals_main2[ci] = CStringGetTextDatum(valtxt);
								pfree(valtxt);
							}
							ci++;
						}
						list_free(pk_list);

						if (probe_ok)
							sql2 = psprintf("SELECT ctid FROM %s%s LIMIT 1",
											quote_qualified_identifier(
												get_namespace_name(RelationGetNamespace(rel)),
												RelationGetRelationName(rel)),
											wbuf.data);
						pfree(wbuf.data);
					}

					if (probe_ok)
					{
						overlay_overlay_helper_enter();
						overlay_write_redirect_exit();
						ret3 = ob_spi_exec_params(sql2, true, 0, nargs_main2,
												  argtypes_main2, vals_main2, nulls_main2);
						overlay_write_redirect_enter();
						overlay_overlay_helper_exit();

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

					/* Release the probe arrays (NULL arbiter columns were
					 * passed as (Datum) 0 and must not be freed). */
					if (sql2 != NULL)
						pfree(sql2);
					if (vals_main2 != NULL)
					{
						for (i = 0; i < nargs_main2; i++)
							if (DatumGetPointer(vals_main2[i]) != NULL)
								pfree(DatumGetPointer(vals_main2[i]));
						pfree(vals_main2);
					}
					if (argtypes_main2 != NULL)
						pfree(argtypes_main2);
					if (nulls_main2 != NULL)
						pfree(nulls_main2);
				}

				/* ================================================================
				 * === Promo dispatch (4-way) ===
				 * ================================================================ */
				{
					bool main_hit = (main_ctid != NULL);
					bool pure_hit = (pre_image_slot != NULL);
					const char *dup_colname = (pk_attno > 0)
						? NameStr(TupleDescAttr(RelationGetDescr(rel), pk_attno - 1)->attname)
						: "key";
					if (oc_action == ONCONFLICT_NONE)
					{
						if (key_claimed)
						{
							char	   *dup_pk_txt = ob_pk_value_text(rel, slot, pk_attno);

							ereport(ERROR,
								(errcode(ERRCODE_UNIQUE_VIOLATION),
								 errmsg("duplicate key value violates unique constraint on relation \"%s\"",
										RelationGetRelationName(rel)),
								 errdetail("Key (%s)=(%s) already exists in this branch (same-statement duplicate).",
										   dup_colname, dup_pk_txt ? dup_pk_txt : pk)));
						}
						r21_check_insert_preconditions(rel, slot, bid, relid, pk);
						promotion = 1;
					}
					else if (!main_hit && !pure_hit && !key_claimed)
					{
						/* No conflict → promo=1 INSERT (both DO NOTHING /
						 * DO UPDATE fall through to insert) */
						promotion = 1;
					}
					else if (oc_action == ONCONFLICT_NOTHING)
					{
						/* Any conflict + DO NOTHING → promo=0 skip */
						promotion = 0;
						/* EXPLAIN ANALYZE: a skipped conflict still counts
						 * as a conflicting tuple, mirroring the DO NOTHING
						 * conflict path in nodeModifyTable.c. */
						InstrCountTuples2(&mt->ps, 1);
					}
					else /* ONCONFLICT_UPDATE + at least one hit */
					{
						/* ========== WHERE cond (for DO UPDATE only) ========== */
						bool where_ok = true;
						if (key_claimed)
							ereport(ERROR,
								(errcode(ERRCODE_CARDINALITY_VIOLATION),
								 errmsg("ON CONFLICT DO UPDATE command cannot affect row a second time"),
								 errhint("Ensure that no rows proposed for insertion within the same command have duplicate constrained values.")));
						if (oc_where != NULL)
						{
							ExprContext *ecx;
							EState *estate = mt->ps.state;
							TupleTableSlot *t_where_scan = NULL;
							TupleTableSlot *save_scantuple;
							TupleTableSlot *save_innertuple;
							bool		main_where_fetched = false;
							ecx = GetPerTupleExprContext(estate);
							/* FIX-UPSERT-WHERE (shared-slot preservation):
							 * ecxt_innertuple below is bound to the caller's
							 * EXCLUDED slot (`slot`) and, on the pure-delta
							 * path, ecxt_scantuple is bound to the caller's
							 * live pre-image slot (`pre_image_slot`).  Both
							 * are consumed again by the promo dispatch and the
							 * ExecProject SET merge that follow.  Calling
							 * ExecClearTuple on them would mark those live
							 * slots empty, after which TupIsNull() misreports
							 * a populated row as NULL and the promo=2/3 guard
							 * silently takes the fallback path against a
							 * cleared slot (getsomeattrs / bogus palloc
							 * errors).  Save and restore the incoming pointers
							 * instead of clearing them. */
							save_scantuple = ecx->ecxt_scantuple;
							save_innertuple = ecx->ecxt_innertuple;
							/* The branch's own delta row shadows MAIN
							 * (self-written deltas are always visible), so
							 * the WHERE predicate must be evaluated against
							 * the delta value whenever one exists.  Testing
							 * the stale MAIN row here would let a second
							 * UPSERT / UPDATE on a row the branch already
							 * touched mis-evaluate its predicate. */
							if (pure_hit)
							{
								t_where_scan = pre_image_slot;
								main_where_fetched = false;
							}
							else
							{
								t_where_scan = fetch_tuple_by_ctid(rel, main_ctid,
																   queryDesc->estate->es_query_cxt);
								main_where_fetched = true;
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
							ecx->ecxt_scantuple = save_scantuple;
							ecx->ecxt_innertuple = save_innertuple;
							if (main_where_fetched && t_where_scan)
								ExecDropSingleTupleTableSlot(t_where_scan);
						}
						if (!where_ok)
						{
							promotion = 0;
							/* The conflicting tuple is counted both as a
							 * conflict and as removed by the DO UPDATE ...
							 * WHERE filter, matching nodeModifyTable.c. */
							InstrCountTuples2(&mt->ps, 1);
							InstrCountFiltered1(&mt->ps, 1);
						}
						else
						{
							/* Both-hit tie-break: the delta row is the latest
							 * visible version -- it shadows MAIN -- so the
							 * pure pre-image wins.  Choosing MAIN here would
							 * make a second UPSERT / UPDATE on a row the
							 * branch already wrote silently revert every
							 * untouched column to its MAIN value. */
							if (pure_hit)
							{
								promotion = 3;
							}
							else
							{
								promotion = 2;
							}
							/* DO UPDATE applied to a conflicting tuple. */
							InstrCountTuples2(&mt->ps, 1);
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
					TupleTableSlot *save_scantuple = NULL;
					TupleTableSlot *save_innertuple = NULL;

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
						TypedKey   *pre_pk = NULL;
						int			pre_n = 0;
						bool		pk_check_active = false;

						if (tdata != NULL)
						{
							pfree(tdata);
							tdata = NULL;
						}

						/* R03 (UPSERT): capture the pre-image PK BEFORE the
						 * ExecProject below calls ExecClearTuple() on
						 * ecxt_scantuple (= pre_image_slot).  That clear marks
						 * the slot empty, after which TupIsNull() reports a
						 * fully populated pre-image as empty and the mutation
						 * check would be silently skipped.  Build the TypedKey
						 * here while the slot is still live. */
						if (overlay_relation_has_pk(rel) &&
							pre_image_slot != NULL && !TupIsNull(pre_image_slot))
						{
							pre_pk = overlay_build_typed_pk_from_slot(
								rel, pre_image_slot, &pre_n);
							pk_check_active = true;
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

							/* FIX-UPSERT-MULTIROW (shared-slot preservation):
							 * Save the ExprContext bindings and restore them
							 * after the projection; never ExecClearTuple() a
							 * slot we do not own.  ecxt_innertuple may still
							 * alias `slot` -- the caller's live subplan slot,
							 * bound on the previous loop iteration for a
							 * preceding promo=2/3 row -- and ecxt_scantuple
							 * may alias a pre_image_slot that was already
							 * dropped.  Clearing the caller's slot would free
							 * the tuple behind its EXCLUDED values, so the
							 * next ExcludedColumn read faults or reads freed
							 * varlena memory (observed as "compression method
							 * lz4 not supported").  See doc/internals.md
							 * "slot aliasing". */
							save_scantuple = ecx->ecxt_scantuple;
							save_innertuple = ecx->ecxt_innertuple;

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

							/* Restore the caller's ExprContext bindings
							 * (FIX-UPSERT-MULTIROW): undo our temporary
							 * scantuple/innertuple aliases instead of
							 * clearing them, so the live subplan slot is left
							 * intact for the next row. */
							ecx->ecxt_scantuple = save_scantuple;
							ecx->ecxt_innertuple = save_innertuple;
						}

						/* ===== REVIEW-260926 / R03 (UPSERT) =====
						 * The delta row is keyed by the conflict identity
						 * (the pre-image's PK); the post-image carries the
						 * DO UPDATE SET result.  If SET touched a PK column
						 * the two desync exactly as on the plain UPDATE
						 * path.  Compare with the type-safe TypedKey equality
						 * (never as text) so BPCHAR rtrim / NUMERIC precision
						 * / TIMESTAMPTZ zone differences cannot alias.
						 * pre_pk was captured above (before the projection
						 * cleared the pre-image slot); when the pre-image was
						 * absent/unusable pk_check_active is false and we skip
						 * -- that branch only uses the EXCLUDED row, whose PK
						 * is the arbiter key by construction. */
						if (pk_check_active)
						{
							int			post_n = 0;
							TypedKey   *post_pk;
							bool		pk_mutated;

							post_pk = overlay_build_typed_pk_from_slot(
								rel, post_image, &post_n);
							pk_mutated =
								(overlay_typed_pk_cmp(pre_pk, pre_n,
													  post_pk, post_n) != 0);
							overlay_typed_pk_free(post_pk, post_n);

							if (pk_mutated)
							{
								overlay_typed_pk_free(pre_pk, pre_n);
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("overlay_branch does not support UPSERT ON CONFLICT DO UPDATE that modifies primary key columns (R03 pk-mutation blocked)"),
										 errdetail("The DO UPDATE SET targetlist changes a primary key column, which would desync the delta key from the post-image."),
										 errhint("To change a PK value inside a branch, DELETE the old row then INSERT a new row with the desired key.")));
							}
							overlay_typed_pk_free(pre_pk, pre_n);
						}

						/* Eager native NOT NULL + CHECK check on the UPSERT
						 * DO UPDATE post-image, matching the plain INSERT
						 * path so a branch-local UPSERT rejects an invalid
						 * row immediately instead of at apply. */
						ob_validate_redirect_constraints(rri, rel, post_image,
														 queryDesc->estate);

						tdata = overlay_serialize_tuple(rel, post_image);

						{
							char	   *delta_key = pk;
							char	   *anchored_key = NULL;
							MemoryContext oldmc;

							/* Keyless UPSERT writes under the conflicted
							 * row's identity, not the VALUES slot's (which
							 * would mint a fresh synthetic key and duplicate
							 * the row on read): a pure-delta hit reuses the
							 * existing delta key + version, a MAIN hit
							 * re-anchors to that row's ctid. */
							if (pure_delta_key != NULL)
								delta_key = pure_delta_key;
							else if (!overlay_relation_has_pk(rel) &&
									 main_ctid != NULL)
							{
								anchored_key =
									overlay_anchor_key_from_ctid(main_ctid);
								if (anchored_key != NULL)
									delta_key = anchored_key;
							}

							oldmc = MemoryContextSwitchTo(
								queryDesc->estate->es_query_cxt);
							if (claimed != NULL)
								ob_claim_key(claimed, claimed_cxt, pk);
							overlay_delta_insert(bid, relid, delta_key,
												 DELTA_OP_UPDATE, old_version, tdata);
							MemoryContextSwitchTo(oldmc);

							if (anchored_key != NULL)
								pfree(anchored_key);
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
					/* promo=1: fresh INSERT pass.  The candidate row was
					 * already constraint-checked before conflict detection
					 * above, so it is not re-checked here. */
					{
						MemoryContext oldmc;
						oldmc = MemoryContextSwitchTo(
							queryDesc->estate->es_query_cxt);
						if (claimed != NULL)
							ob_claim_key(claimed, claimed_cxt, pk);
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
				if (pure_delta_key) pfree(pure_delta_key);
				ExecClearTuple(slot);
			}

				/* Statement-scoped arbiter column list (keyless UPSERT). */
				if (arb_attnos != NULL)
					pfree(arb_attnos);

				if (claimed_cxt != NULL)
				{
					MemoryContextDelete(claimed_cxt);
					claimed_cxt = NULL;
					claimed = NULL;
				}

				/* Replay collected RETURNING rows into dest.  As on the
				 * UPDATE/DELETE path, we bypassed standard_ExecutorRun(), so
				 * the DestReceiver lifecycle must be driven here — notably
				 * SPI's DestSPI needs rStartup to allocate its SPITupleTable
				 * before spi_printtup can accept rows. */
				if (has_returning && queryDesc->dest != NULL)
				{
					int			k;

					queryDesc->dest->rStartup(queryDesc->dest,
											  queryDesc->operation,
											  queryDesc->tupDesc);
					for (k = 0; k < nretslots; k++)
						(*queryDesc->dest->receiveSlot) (retslots[k], queryDesc->dest);
					queryDesc->dest->rShutdown(queryDesc->dest);
					for (k = 0; k < nretslots; k++)
						ExecDropSingleTupleTableSlot(retslots[k]);
				}

				/* Close the ModifyTable instrumentation opened before the
				 * replay loop; nretslots is the number of RETURNING rows the
				 * node would have emitted (0 without RETURNING). */
				if (mt->ps.instrument != NULL)
					InstrStopNode(mt->ps.instrument, (double) nretslots);

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
								 "by overlay_branch.  Please rewrite as a plain "
								 "INSERT/UPDATE/DELETE against one heap relation with "
								 "an explicit PRIMARY KEY.")));
		}
	}

	return handled;
}
