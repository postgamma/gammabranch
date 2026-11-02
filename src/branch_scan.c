/*-------------------------------------------------------------------------
 *
 * branch_scan.c
 *    Transparent overlay read using PG CustomScan / set_rel_pathlist_hook.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"
#include "branch_scan.h"

#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "executor/nodeCustom.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/typcache.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

static Plan *ob_branchscan_plan_custom_path(PlannerInfo *root,
                                            RelOptInfo *rel,
                                            CustomPath *cpath,
                                            List *tlist,
                                            List *clauses,
                                            List *custom_plans);
static Node *ob_branchscan_create_custom_scan_state(CustomScan *cscan);
static void  ob_branchscan_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *ob_branchscan_exec(CustomScanState *node);
static void  ob_branchscan_end(CustomScanState *node);
static void  ob_branchscan_rescan(CustomScanState *node);

static const CustomPathMethods  ob_branchscan_path_methods = {
    "overlay_branch_branchscan",
    ob_branchscan_plan_custom_path,
    NULL,
};

static const CustomScanMethods  ob_branchscan_scan_methods = {
    "overlay_branch_branchscan",
    ob_branchscan_create_custom_scan_state,
};

static const CustomExecMethods ob_branchscan_exec_methods = {
    .CustomName         = "overlay_branch_branchscan",
    .BeginCustomScan    = ob_branchscan_begin,
    .ExecCustomScan     = ob_branchscan_exec,
    .EndCustomScan      = ob_branchscan_end,
    .ReScanCustomScan   = ob_branchscan_rescan,
    /* MarkPos/RestorePos intentionally left NULL.  We never advertise
     * CUSTOMPATH_SUPPORT_MARK_RESTORE, so the planner inserts a Material node
     * above us whenever an upper node (MergeJoin) needs mark/restore; a
     * forward heap scan cannot be rewound in a streaming iterator, so
     * nodeCustom.c's own "does not support MarkPos" error is the correct
     * failure mode if that assumption ever breaks. */
};

/* ================================================================
 * RD-6 — streaming 2-pass overlay iterator.
 *
 * The old helper returned a fully materialised List<TupleTableSlot*>:
 * the FIRST ExecCustomScan call paid for the whole overlay (MAIN heap
 * scan + delta merge + every reconstructed row) before yielding tuple
 * one, so LIMIT / cursors could not stop early.  The iterator below
 * produces ONE merged row per overlay_scan_next() call instead.
 *
 * Iteration ORDER is byte-for-byte the old one (Pass 1 walks the MAIN
 * baseline in heap/SPI order, Pass 2 walks delta_list in append order),
 * so every result set is unchanged — only *when* the work happens moves.
 *
 * The state is allocated in the caller's result context (BranchScan →
 * ebs->scan_cxt, child of es_query_cxt; SRF → multi_call_memory_ctx).  The
 * single merged row handed back by next() is built in a child "row scratch"
 * context that next() resets on entry, so a scan's footprint does NOT grow
 * with the number of rows it emits; the caller must finish with a row before
 * calling next() again (the plain ExecProcNode / SRF per-call contract).
 * close() releases the delta payload (TopMemoryContext), the heap scan
 * descriptor and the probe slot; the relation and the state itself are owned
 * by the caller (the relation is borrowed, the state is reclaimed with the
 * context).
 * ================================================================ */
typedef struct OverlayScanState
{
    MemoryContext   result_cxt;   /* owns the iterator + long-lived (P0) slots */
    MemoryContext   row_cxt;      /* one row's slot + datum copy; reset per call */
    Relation        rel;          /* borrowed: the caller keeps it open */
    TupleDesc       reldesc;
    TupleDesc       slot_desc;    /* shared TTSOpsVirtual desc for emitted rows */
    int             natts;
    bool            rel_has_pk;

    List           *delta_list;   /* owned; payload lives in TopMemoryContext */
    DeltaTuple    **arr;          /* delta_list pointers, sorted by key */
    int             M_total;
    bool            delta_all_typed;

    /* ---- Pass 1: MAIN baseline source ---- */
    bool            has_pk_pred;
    /* non-P0: live forward heap scan + buffer-heap probe slot */
    TableScanDesc   tscan;
    TupleTableSlot *probe;
    bool            do_prefilter;
    ExprState      *qual;
    ExprContext    *qual_econtext;
    /* P0 point query: SPI produced at most one pre-materialised MAIN row
     * (ObMainRow) here.  Its slot->tts_tid carries the keyless ctid anchor
     * and raw_xmin the anchor freshness check (ob_anchor_is_stale). */
    List           *p0_rows;
    ListCell       *p0_cur;
    bool            pass1_done;

    /* ---- Pass 2: pure-delta orphans (no MAIN counterpart) ---- */
    bool            pass2_started;
    ListCell       *pass2_cur;
} OverlayScanState;

static OverlayScanState *overlay_scan_open(Relation rel, int32 branch_id,
                                           bool has_pk_pred,
                                           const char *pk_where_sql,
                                           const char *pk_serialized_key,
                                           bool has_general_where,
                                           const char *general_where_sql,
                                           ExprState *qual,
                                           ExprContext *qual_econtext,
                                           bool dml_target,
                                           MemoryContext result_cxt);
static TupleTableSlot *overlay_scan_next(OverlayScanState *st);
static void overlay_scan_close(OverlayScanState *st);

/* Search key bundle for the Pass 1 bsearch: carries BOTH the typed pk
 * (preferred) and the legacy JSON text key (fallback) for a single MAIN
 * row, so the fixed-signature comparator needs no per-call global state. */
typedef struct ObSearchKey
{
    TypedKey   *typed_pk;      /* NULL if the MAIN-side build failed */
    int         typed_n;       /* columns in typed_pk, 0 if invalid */
    const char *text_key;      /* serialised JSON text (may be NULL) */
} ObSearchKey;

/* qsort comparator for DeltaTuple** by key: typed pk when BOTH sides carry
 * one (R14 native type semantics for BPCHAR / NUMERIC / TIMESTAMPTZ),
 * legacy strcmp(dt->key) otherwise.  NULL keys sort last. */
static int
ob_cmp_dtp_by_key(const void *a, const void *b)
{
    DeltaTuple *const *da = (DeltaTuple *const *) a;
    DeltaTuple *const *db = (DeltaTuple *const *) b;
    DeltaTuple *dta = (da) ? *da : NULL;
    DeltaTuple *dtb = (db) ? *db : NULL;
    const char *ka = (dta) ? dta->key : NULL;
    const char *kb = (dtb) ? dtb->key : NULL;

    if (ka == NULL && kb == NULL) return 0;
    if (ka == NULL) return +1;
    if (kb == NULL) return -1;
    if (dta != NULL && dtb != NULL &&
        dta->typed_pk_n > 0 && dtb->typed_pk_n > 0 &&
        dta->typed_pk != NULL && dtb->typed_pk != NULL)
        return overlay_typed_pk_cmp(dta->typed_pk, dta->typed_pk_n,
                                    dtb->typed_pk, dtb->typed_pk_n);
    return strcmp(ka, kb);
}

/* bsearch comparator: const ObSearchKey* (key) vs DeltaTuple** (element). */
static int
ob_cmp_searchkey_vs_dtp(const void *vkey, const void *velem)
{
    const ObSearchKey *skb = (const ObSearchKey *) vkey;
    DeltaTuple *const *pdt = (DeltaTuple *const *) velem;
    DeltaTuple *dt = (pdt) ? *pdt : NULL;
    const char *ka;
    const char *kd;

    if (skb == NULL && dt == NULL) return 0;
    if (skb == NULL) return +1;
    if (dt == NULL) return -1;
    if (skb->typed_pk != NULL && skb->typed_n > 0 &&
        dt->typed_pk != NULL && dt->typed_pk_n > 0)
        return overlay_typed_pk_cmp(skb->typed_pk, skb->typed_n,
                                    dt->typed_pk, dt->typed_pk_n);
    ka = skb->text_key;
    kd = dt->key;
    if (ka == NULL && kd == NULL) return 0;
    if (ka == NULL) return +1;
    if (kd == NULL) return -1;
    return strcmp(ka, kd);
}

/* ----- ExtendedCustomScanState: embed CustomScanState as 1st field per PG
 * design rule (ExecInitCustomScan nodeCustom.c L36-40).  BranchScanState
 * follows immediately so we can recover it with a simple container_of.
 * The PG-owned CustomScanState.custom_ps field MUST remain NIL (it is
 * a List of child PlanStates for plan nodes with subplans — we have none).
 * Never set custom_ps = private pointer: ExecShutdownNode_walker will
 * try to traverse it as List and SIGSEGV. */
typedef struct ExtendedCustomScanState
{
    CustomScanState css;          /* MUST be first field for IsA/castNode */
    /* our private state follows */
    OverlayScanState *scan;       /* streaming iterator; NULL until first Exec */
    MemoryContext scan_cxt;       /* owns the iterator + its per-row scratch */
    Relation      rel;
    Oid           relid;
    int32         branch_id;
    /* P0 PK-IndexScan MVP: fast-path info passed through from the Planner hook */
    bool          has_pk_pred;
    char         *pk_where_sql;
    char         *pk_serialized_key;
    /* P2 general non-PK qual pushdown: the deparsed SQL WHERE fragment */
    bool          has_general_where;
    char         *general_where_sql;
    /* ACC-1: true when this BranchScan feeds an UPDATE/DELETE DML target
     * (custom_private[7]).  Drives the heap-backed scan slot below so the
     * ctid system column (row identity) is retrievable (see doc/internals.md
     * §2.5).  Set in CreateCustomScanState from the plan, not in Begin. */
    bool          dml_target;
} ExtendedCustomScanState;

/* css MUST be a valid CustomScanState* whose embedding is ExtendedCustomScanState.
 * Since css is field offset 0 we could simply cast, but explicit arithmetic
 * makes the container_of pattern visible. */
#define CSS2BS(css)  ((ExtendedCustomScanState *)(css))


/* ================================================================
 * KEYLESS anchor freshness (doc/internals.md §4.5)
 * ================================================================
 * A keyless delta is matched to a MAIN row by ctid ALONE (Pass 1 below),
 * but dt->old_version also records the physical version the branch first
 * anchored: overlay_tuple_version() renders it as "<blk>:<off>-x<xmin>".
 *
 * If VACUUM recycled the anchored line pointer (LP_UNUSED) and a later
 * INSERT reused that very ctid, the ctid-only match would silently graft
 * the branch edit onto a completely unrelated row — the "silent misread"
 * case.  The xmin half of the token detects that: a tuple still living at
 * the anchored ctid but written by a different transaction is a DIFFERENT
 * logical row.
 *
 * `raw_xmin` is the MAIN reader's captured HeapTupleHeaderGetRawXmin for
 * the matched row (ObMainRow.raw_xmin): the physical scan slot itself is
 * reused/popped before Pass 1 runs, so only the xmin value is carried
 * forward.
 *
 * Returns true when the anchor is provably stale (delta must NOT be
 * matched; the Pass 2 orphan rule then drops it, exactly like the
 * concurrently-UPDATEd case).  A frozen tuple is accepted as-is: VACUUM
 * can freeze a tuple in place, so a non-normal xmin proves nothing.
 * ================================================================ */
static bool
ob_anchor_is_stale(const DeltaTuple *dt, TransactionId raw_xmin)
{
    TransactionId anchor_xmin;

    if (dt == NULL || dt->key == NULL || dt->old_version == NULL)
        return false;
    if (!(dt->key[0] == 'A' && dt->key[1] == ':'))
        return false;

    /* Shared base-version codec (A1/A6 stage 2): parse the token with the
     * same helper that formats it on the write path and at apply time. */
    if (!overlay_base_ver_parse_xmin(dt->old_version, &anchor_xmin))
        return false;

    if (!TransactionIdIsNormal(raw_xmin))
        return false;

    return anchor_xmin != raw_xmin;
}

/* One MAIN baseline row as produced by the MAIN reader below: the merge
 * input slot plus the physical half of the keyless anchor.  The ctid half
 * lives in slot->tts_tid; raw_xmin is kept separately because the anchor
 * freshness check (ob_anchor_is_stale) is only meaningful for keyless
 * tables and must survive past the end of the heap scan. */
typedef struct ObMainRow
{
    TupleTableSlot *slot;
    TransactionId   raw_xmin;
} ObMainRow;

/* ================================================================
 * SHARED streaming 2-pass overlay iterator (used by BOTH the SRF and the
 * CustomScan executor).
 *
 *   PK-predicate fast-path (P0 MVP):
 *     has_pk_pred = true
 *     pk_where_sql = non-empty SQL fragment like "id = 10::integer"
 *     pk_serialized_key = non-empty JSON array string like '["10"]'
 *   -> MAIN uses SPI SELECT ... WHERE pk_where_sql (<=1 row point lookup)
 *   -> Delta uses overlay_delta_lookup exact match (not list_for_rel full scan)
 *
 *   RD-7 — everything else reads the MAIN heap directly:
 *     qual / qual_econtext = executor-owned ExprState (= baserestrictinfo)
 *                            + the ExprContext to evaluate it in.  NULL
 *                            (SRF path) disables the prefilter.
 *   -> table_beginscan/table_scan_getnextslot replaces SPI `SELECT *`
 *   -> qual pre-filters MAIN rows (PK tables only; for keyless the Pass2 fallback does not apply)
 *
 *   General non-PK qual pushdown (P2 MVP)  SQL text now only serves the P0
 *   point lookup; equivalent non-P0 filtering is handled by the qual prefilter.
 *
 * RD-6 turns the old helper from “materialize the full List<slot> up front” into an iterator:
 *   overlay_scan_open()  one-shot setup (delta load + sort + read-side priming)
 *   overlay_scan_next()  emits one row per call (Pass1 walks the MAIN baseline, Pass2 re-emits delta orphans)
 *   overlay_scan_close() frees temporary resources (heap scan descriptor / probe slot / delta payload)
 * Iteration order is byte-identical to the old implementation → result set unchanged, only “when work happens” changed,
 * so LIMIT / cursors can stop early.
 * ================================================================ */

/* Free the delta payload loaded by overlay_scan_open(): every DeltaTuple plus
 * its key / old_version / tuple_data strings (all palloc'd in TopMemoryContext
 * by delta_store.c) and the lazily-built typed_pk.  R13 S07: leaking these
 * grew the backend RSS without bound on long sessions (MF-1). */
static void
ob_overlay_free_delta_list(List *delta_list)
{
    ListCell *lc;

    foreach(lc, delta_list)
    {
        DeltaTuple *dt = (DeltaTuple *) lfirst(lc);

        if (dt == NULL)
            continue;
        /* key always exists (list_for_rel always pstrdup("") for isnull).
         * tuple_data is NULL for op=DELETE entries; non-NULL for I/U.
         * old_version is NULL for pure-delta INSERTs, else the MAIN base
         * version copy (also TopMemoryContext).  typed_pk is the R14 add-on
         * built lazily in overlay_scan_open(). */
        if (dt->key != NULL) pfree(dt->key);
        if (dt->old_version != NULL) pfree(dt->old_version);
        if (dt->tuple_data != NULL) pfree(dt->tuple_data);
        if (dt->typed_pk != NULL && dt->typed_pk_n > 0)
            overlay_typed_pk_free(dt->typed_pk, dt->typed_pk_n);
        pfree(dt);
    }
    list_free(delta_list);
}

/*
 * One-time setup for a streaming overlay scan over `rel` (the caller keeps
 * the relation open and owns the lock).  result_cxt owns the iterator, the
 * shared slot descriptor and every slot handed out by overlay_scan_next();
 * NULL means CurrentMemoryContext (caller-managed, same contract as before).
 */
static OverlayScanState *
overlay_scan_open(Relation rel, int32 branch_id,
                  bool has_pk_pred,
                  const char *pk_where_sql,
                  const char *pk_serialized_key,
                  bool has_general_where,
                  const char *general_where_sql,
                  ExprState *qual,
                  ExprContext *qual_econtext,
                  bool dml_target,
                  MemoryContext result_cxt)
{
    OverlayScanState *st;
    TupleDesc   reldesc;
    int         natts;
    Oid         relid;
    MemoryContext oldcxt;
    BranchExecContext *ectx;
    Snapshot    stmt_snap;      /* A1/A6 bound MAIN read snapshot; NULL = live */

    Assert(rel != NULL);
    reldesc = RelationGetDescr(rel);
    natts = reldesc->natts;
    relid = RelationGetRelid(rel);

    /* A1/A6: consume the statement-bound identity instead of re-reading the
     * mutable global, so the branch id and the MAIN read snapshot come from
     * one bind point.  Fall back to the live global when no statement context
     * is bound (defensive only: reads always run inside an executor). */
    ectx = ob_get_exec_context();
    stmt_snap = (ectx != NULL)
        ? ectx->stmt_snap
        : (CurrentBranchContext != NULL ? CurrentBranchContext->branch_main_snapshot
                                        : NULL);

    oldcxt = MemoryContextSwitchTo(result_cxt != NULL ? result_cxt
                                                      : CurrentMemoryContext);

    st = (OverlayScanState *) palloc0(sizeof(OverlayScanState));
    st->result_cxt    = CurrentMemoryContext;
    /* Per-row scratch, a child of result_cxt reset at the top of every
     * overlay_scan_next() call.  Keeping the emitted slot (and its datum
     * copies) here instead of in result_cxt is what stops a large scan from
     * growing the backend linearly with the row count. */
    st->row_cxt       = AllocSetContextCreate(CurrentMemoryContext,
                                              "overlay row scratch",
                                              ALLOCSET_SMALL_SIZES);
    st->rel           = rel;
    st->reldesc       = reldesc;
    st->natts         = natts;
    st->rel_has_pk    = overlay_relation_has_pk(rel);
    st->has_pk_pred   = has_pk_pred;
    st->qual          = qual;
    st->qual_econtext = qual_econtext;
    /* Prefilter MAIN rows with the executor's baserestrictinfo qual ONLY for
     * PK tables.  A MAIN row filtered out here whose branch edit still lives
     * in the delta falls to the Pass 2 orphan rule: Pass 2 reconstructs a *PK*
     * orphan from tuple_data, but DROPS an unmatched keyless ("A:") anchor
     * (resurrecting it by ctid would be unsafe).  So keyless must scan MAIN
     * unfiltered and rely on the executor's post-merge qual re-check.
     *
     * ACC-1: a DML-target scan must NEVER filter on MAIN, because the
     * predicate is defined over the OVERLAY row.  If MAIN dropped a row whose
     * branch edit satisfies the predicate, that delta would be demoted to a
     * Pass 2 orphan -- and an orphan has no MAIN ctid, so the write path would
     * skip it.  Disabling the prefilter keeps every MAIN row paired with its
     * own ctid; the post-merge qual (which sees the overlay values) then makes
     * the correct keep/kill call. */
    st->do_prefilter   = (qual != NULL && qual_econtext != NULL &&
                          st->rel_has_pk && !dml_target);

    /* RD-3: one shared tupdesc for every emitted slot.  CreateTupleDescCopy()
     * is NOT reference-counted (tdrefcount == -1), so MakeSingleTupleTableSlot
     * / slot drop PinTupleDesc/ReleaseTupleDesc are no-ops and the descriptor
     * simply lives in result_cxt until the caller resets it. */
    st->slot_desc = CreateTupleDescCopy(reldesc);

    /* ---- delta side: P0 singleton lookup vs full (branch_id, relid) list ---- */
    if (has_pk_pred && pk_serialized_key != NULL && pk_serialized_key[0] != '\0')
    {
        DeltaTuple *singleton = (DeltaTuple *) palloc0(sizeof(DeltaTuple));

        if (overlay_delta_lookup(branch_id, relid, pk_serialized_key, singleton))
            st->delta_list = list_make1(singleton);
        else
        {
            pfree(singleton);
            st->delta_list = NIL;
        }
    }
    else
        st->delta_list = overlay_delta_list_for_rel(branch_id, relid);

    st->M_total = list_length(st->delta_list);

    /* ---- sort the delta pointers by key + warm the typed PKs ----
     * O(N x M) strcmp per MAIN row would stall the executor on realistic
     * workloads, so the pre-flight builds every delta key ONCE and qsort()s the
     * pointer array; Pass 1 then bsearch()es each MAIN row: O(M log M + N log M).
     * Build failures (no PK / relation changed) leave typed_pk_n == 0 so the
     * comparators fall back to the legacy strcmp(dt->key).  No post-sort dedup:
     * pg_branch_delta is keyed by PRIMARY KEY (branch_id, relid, key) and this
     * list is loaded with an exact (branch_id, relid) filter, so keys are
     * already unique (RD-1/RD-2) — the old O(M^2) fold was identity. */
    if (st->M_total > 0)
    {
        int       i;
        ListCell *lc;

        st->arr = (DeltaTuple **) palloc(sizeof(DeltaTuple*) * (st->M_total + 1));
        i = 0;
        foreach(lc, st->delta_list)
            st->arr[i++] = (DeltaTuple *) lfirst(lc);

        st->delta_all_typed = true;
        for (i = 0; i < st->M_total; i++)
        {
            DeltaTuple *dtp = st->arr[i];

            if (dtp != NULL && dtp->typed_pk_n < 0 && dtp->key != NULL)
            {
                int       nout = 0;
                TypedKey *out  = overlay_build_typed_pk_from_key_text(rel, dtp->key, &nout);

                if (out != NULL && nout > 0)
                {
                    dtp->typed_pk   = out;
                    dtp->typed_pk_n = nout;
                }
                else
                {
                    dtp->typed_pk   = NULL;
                    dtp->typed_pk_n = 0;
                }
            }
            /* RD-8: one entry without a typed key forces the whole scan back
             * onto the JSON-text comparison path. */
            if (dtp == NULL || dtp->typed_pk_n <= 0)
                st->delta_all_typed = false;
        }
        qsort(st->arr, st->M_total, sizeof(DeltaTuple*), ob_cmp_dtp_by_key);
    }

    /* ================================================================
     * Pass 1 source: MAIN baseline reader
     * ================================================================
     * • has_pk_pred (P0): SPI `SELECT * FROM rel WHERE pk = val` (<=1 row).
     *   We deliberately keep SPI here to reuse the planner's PK index scan:
     *   hand-rolling a btree ScanKey would require strict literal->PK-type
     *   coercion (BPCHAR / NUMERIC / collation) and a mismatch would silently
     *   return 0 rows.
     * • everything else: a direct forward heap scan (table_beginscan), pulled
     *   one row at a time by overlay_scan_next().  No SPI, no whole-result-set
     *   materialisation.
     * ================================================================ */
    if (has_pk_pred)
    {
        const char   *q_nspname = get_namespace_name(RelationGetNamespace(rel));
        const char   *q_relname = RelationGetRelationName(rel);
        char         *q_qualified;
        StringInfoData sql;
        bool          any_where;
        bool          pushed_snap_;
        bool          pop_done_;
        int           ret;

        q_qualified = quote_qualified_identifier(q_nspname, q_relname);
        initStringInfo(&sql);
        any_where = false;

        /* ACC-1: ask for the physical ctid explicitly.  SPI materialises each
         * result tuple with a fresh heap_form_tuple(), which leaves t_self
         * invalid -- so htup->t_self is NOT a reliable row identity.  It only
         * looks valid when the plan is a bare relation scan with no projection
         * (a table with no dropped columns); any projection (e.g. after
         * ALTER TABLE ... ADD/DROP COLUMN) makes the executor build a virtual
         * tuple and the ctid is lost.  Selecting the ctid system column forces
         * the executor to project the real physical ctid into a datum that
         * survives materialisation. */
        appendStringInfo(&sql, "SELECT ctid, * FROM %s", q_qualified);
        if (pk_where_sql != NULL && pk_where_sql[0] != '\0')
        {
            appendStringInfo(&sql, " WHERE (%s)", pk_where_sql);
            any_where = true;
        }
        /* ACC-1: a DML-target scan must not push the non-PK WHERE to MAIN
         * either -- the predicate is over the OVERLAY row, and over-filtering
         * MAIN would strand a branch-updated row as a ctid-less Pass 2
         * orphan.  Keep only the PK point predicate (safe: PK is immutable in
         * a branch, R03) and let scan.plan.qual re-check on the overlay. */
        if (!dml_target &&
            has_general_where && general_where_sql != NULL && general_where_sql[0] != '\0')
        {
            if (any_where)
                appendStringInfo(&sql, " AND (%s)", general_where_sql);
            else
            {
                appendStringInfo(&sql, " WHERE (%s)", general_where_sql);
                any_where = true;
            }
        }
        pfree(q_qualified);

        /* ----- Guard: run the internal MAIN query *outside* the Planner hook
         * overlay (otherwise infinite recursion: helper -> SPI SELECT ->
         * Planner hook -> CustomScan -> helper ...).  PG_TRY restores the flag
         * on ANY error path — otherwise one failed call leaves it stuck true
         * for the rest of the session and disables transparent overlay reads.
         *
         * Push/Pop Snapshot Balance Rules: in SNAPSHOT mode the MAIN baseline
         * read must use the frozen snapshot captured at use_branch() time.
         * Exactly one push brackets the single ob_spi_one_shot() call and
         * exactly one pop follows it; pop_done_ lets the CATCH path avoid a
         * double pop. */
        pushed_snap_ = (stmt_snap != NULL);
        pop_done_ = false;

        overlay_overlay_helper_enter();
        PG_TRY();
        {
            if (pushed_snap_)
                PushActiveSnapshot(stmt_snap);
            ret = ob_spi_one_shot(sql.data, true, 0);
            if (pushed_snap_)
            {
                PopActiveSnapshot();
                pop_done_ = true;
            }
            pfree(sql.data);

            if (ret != SPI_OK_SELECT)
            {
                SPI_finish();
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("overlay_scan_open: SPI main point query failed ret=%d", ret)));
            }
        }
        PG_CATCH();
        {
            if (pushed_snap_ && !pop_done_)
                PopActiveSnapshot();
            overlay_overlay_helper_exit();
            PG_RE_THROW();
        }
        PG_END_TRY();
        overlay_overlay_helper_exit();

        /* ob_spi_one_shot() left SPI connected and reparented the current
         * memory context onto the SPI proc context — which SPI_finish()
         * destroys.  The row built below MUST survive that, so re-enter
         * result_cxt first (doc RD-7 pitfall #2). */
        MemoryContextSwitchTo(st->result_cxt);

        if (SPI_processed > 0 && SPI_tuptable != NULL &&
            SPI_tuptable->vals != NULL)
        {
            TupleDesc       td_spi = SPI_tuptable->tupdesc;
            uint64          row;
            int            *spi_col;    /* SPI result column per rel attribute; 0 = dropped */
            int             spi_idx;

            /* td_spi layout is (ctid, <non-dropped attributes in attnum
             * order>).  Build the attribute->result-column map ONCE so a
             * dropped column anywhere in the table cannot shift the columns
             * that follow it. */
            spi_col = (int *) palloc(sizeof(int) * natts);
            spi_idx = 1;                /* result column 1 is the ctid */
            for (row = 0; row < (uint64) natts; row++)
            {
                int att_i = (int) row;

                if (TupleDescAttr(reldesc, att_i)->attisdropped)
                    spi_col[att_i] = 0;
                else
                    spi_col[att_i] = ++spi_idx;
            }

            /* The PK pushdown is no longer a point query: IN / range / OR
             * can match many MAIN rows, so materialise ALL of them (each in
             * result_cxt) and let the Pass1 cursor drain the list. */
            for (row = 0; row < SPI_processed; row++)
            {
                HeapTuple       htup = SPI_tuptable->vals[row];
                TupleTableSlot *mslot;
                ObMainRow      *mrow;
                int             a;
                bool            ctid_isnull;
                Datum           ctid_d;

                mslot = MakeSingleTupleTableSlot(st->slot_desc, &TTSOpsVirtual);
                for (a = 0; a < natts; a++)
                {
                    bool                isnull;
                    Form_pg_attribute   ratt = TupleDescAttr(reldesc, a);
                    Datum               d;

                    if (spi_col[a] == 0)    /* dropped attribute */
                    {
                        mslot->tts_isnull[a] = true;
                        mslot->tts_values[a] = (Datum) 0;
                        continue;
                    }
                    d = SPI_getbinval(htup, td_spi, spi_col[a], &isnull);
                    mslot->tts_isnull[a] = isnull;
                    mslot->tts_values[a] = isnull
                                               ? (Datum) 0
                                               : datumCopy(d, ratt->attbyval, ratt->attlen);
                }
                ExecStoreVirtualTuple(mslot);
                mslot->tts_nvalid = natts;

                /* ACC-1: row identity comes from the projected ctid column,
                 * NOT htup->t_self (which SPI leaves invalid). */
                ctid_d = SPI_getbinval(htup, td_spi, 1, &ctid_isnull);
                if (!ctid_isnull)
                    mslot->tts_tid = *DatumGetItemPointer(ctid_d);

                mrow = (ObMainRow *) palloc(sizeof(ObMainRow));
                mrow->slot = mslot;
                mrow->raw_xmin = (htup->t_data != NULL)
                                     ? HeapTupleHeaderGetRawXmin(htup->t_data)
                                     : InvalidTransactionId;
                st->p0_rows = lappend(st->p0_rows, mrow);
            }
        }
        st->p0_cur = list_head(st->p0_rows);
        SPI_finish();
    }
    else
    {
        Snapshot snap;

        if (stmt_snap != NULL)
            snap = stmt_snap;
        else
            snap = GetActiveSnapshot();

        st->probe = MakeSingleTupleTableSlot(reldesc, &TTSOpsBufferHeapTuple);
        st->tscan = table_beginscan(rel, snap, 0, NULL);
    }

    MemoryContextSwitchTo(oldcxt);
    return st;
}

/*
 * Produce the next merged overlay row, or NULL once both passes are done.
 * Pass 1 walks the MAIN baseline (heap scan / P0 point row) and applies the
 * matching delta side-effect (keep / tombstone / reconstruct); Pass 2 then
 * emits the delta INSERT/UPDATE orphans that had no MAIN counterpart.
 */
static TupleTableSlot *
overlay_scan_next(OverlayScanState *st)
{
    MemoryContext oldcxt;

    if (st == NULL)
        return NULL;

    /* Reclaim the scratch of the row handed back by the previous call (its
     * consumer already copied what it needed) and build the next row there,
     * so the footprint stays constant over an arbitrarily long scan. */
    MemoryContextReset(st->row_cxt);
    oldcxt = MemoryContextSwitchTo(st->row_cxt);

    /* ---------- Pass 1: MAIN baseline row merged with its delta ---------- */
    while (!st->pass1_done)
    {
        TupleTableSlot *main_slot = NULL;
        TransactionId   raw_xmin = InvalidTransactionId;

        if (st->has_pk_pred)
        {
            ObMainRow *mrow;

            if (st->p0_cur == NULL)
            {
                st->pass1_done = true;
                break;
            }
            mrow = (ObMainRow *) lfirst(st->p0_cur);
            st->p0_cur = lnext(st->p0_rows, st->p0_cur);
            main_slot = mrow->slot;
            raw_xmin  = mrow->raw_xmin;
        }
        else
        {
            int a;

            if (!table_scan_getnextslot(st->tscan, ForwardScanDirection, st->probe))
            {
                st->pass1_done = true;
                break;
            }

            if (st->do_prefilter)
            {
                TupleTableSlot *saved_scan;
                bool            ok;

                /* The qual was compiled against a TTSOpsVirtual scan slot, so
                 * ExecComputeSlotInfo() elided the EEOP_SCAN_FETCHSOME de-form
                 * step: EEOP_SCAN_VAR then reads scanslot->tts_values[] directly
                 * and asserts attnum < tts_nvalid.  The buffer-heap probe slot
                 * is NOT de-formed (tts_nvalid == 0), so materialise its
                 * attributes here to satisfy that contract. */
                slot_getsomeattrs(st->probe, st->natts);
                saved_scan = st->qual_econtext->ecxt_scantuple;
                st->qual_econtext->ecxt_scantuple = st->probe;
                ResetExprContext(st->qual_econtext);
                ok = ExecQual(st->qual, st->qual_econtext);
                st->qual_econtext->ecxt_scantuple = saved_scan;
                if (!ok)
                    continue;
            }

            main_slot = MakeSingleTupleTableSlot(st->slot_desc, &TTSOpsVirtual);
            for (a = 0; a < st->natts; a++)
            {
                bool                isnull;
                Form_pg_attribute   ratt = TupleDescAttr(st->reldesc, a);
                Datum               d;

                d = slot_getattr(st->probe, (AttrNumber)(a + 1), &isnull);
                main_slot->tts_isnull[a] = isnull;
                main_slot->tts_values[a] = isnull
                                               ? (Datum) 0
                                               : datumCopy(d, ratt->attbyval, ratt->attlen);
            }
            ExecStoreVirtualTuple(main_slot);
            main_slot->tts_nvalid = st->natts;
            main_slot->tts_tid = st->probe->tts_tid;
            /* BufferHeapTupleTableSlot: the probe points straight at a heap
             * tuple living in the page buffer, so read the raw xmin now, before
             * the slot is reused by the next table_scan_getnextslot() call. */
            raw_xmin = HeapTupleHeaderGetRawXmin(
                ((HeapTupleTableSlot *) st->probe)->tuple->t_data);
        }

        /* ---- merge this MAIN row against the sorted delta array ---- */
        {
            char            *pk_key = NULL;
            DeltaTuple      *match = NULL;
            TypedKey        *main_typed_pk = NULL;
            int              main_typed_n = 0;
            TupleTableSlot  *output_slot;
            int              tpk_i;

            /* RD-8: no key work when the delta side is empty. */
            if (st->M_total > 0)
            {
                if (st->rel_has_pk)
                {
                    /* The typed PK (pure C, no SPI) is the comparison key
                     * whenever both sides have one (R14).  The JSON text key is
                     * only the strcmp fallback and costs a full SPI round trip
                     * per row — build it ONLY when some delta entry lacks a
                     * typed key, or when this row's typed build failed (then the
                     * comparator must fall back to text, otherwise the delta row
                     * would be silently unmatched and its side-effect dropped). */
                    main_typed_pk = overlay_build_typed_pk_from_slot(st->rel, main_slot,
                                                                     &main_typed_n);
                    if (!st->delta_all_typed || main_typed_pk == NULL)
                        pk_key = overlay_serialize_pk(st->rel, main_slot);
                }
                else
                {
                    /* Keyless: anchor key from the MAIN row's physical ctid,
                     * captured by the reader into slot->tts_tid. */
                    ItemPointerData ip_ = main_slot->tts_tid;

                    if (ItemPointerIsValid(&ip_) &&
                        ItemPointerGetBlockNumber(&ip_) != InvalidBlockNumber &&
                        ItemPointerGetOffsetNumber(&ip_) != 0)
                        pk_key = psprintf("A:%u:%u",
                                          ItemPointerGetBlockNumber(&ip_),
                                          ItemPointerGetOffsetNumber(&ip_));
                }
            }

            if (st->arr != NULL && st->M_total > 0 &&
                (main_typed_pk != NULL || pk_key != NULL))
            {
                ObSearchKey   skb;
                DeltaTuple  **found;

                skb.typed_pk = main_typed_pk;
                skb.typed_n  = main_typed_n;
                skb.text_key = pk_key;
                found = (DeltaTuple **) bsearch(&skb, st->arr, st->M_total,
                                                sizeof(DeltaTuple*),
                                                ob_cmp_searchkey_vs_dtp);
                if (found != NULL)
                    match = *found;
            }

            /* keyless: a ctid-only match can hit a RECYCLED line pointer
             * (VACUUM freed it, a later INSERT reused it).  Reject the anchor
             * then, so the branch edit is not grafted onto an unrelated row;
             * the delta falls to the Pass 2 orphan rule and is dropped. */
            if (!st->rel_has_pk && match != NULL &&
                ob_anchor_is_stale(match, raw_xmin))
                match = NULL;

            if (match == NULL)
                output_slot = main_slot;
            else if (match->op == DELTA_OP_DELETE)
            {
                ExecDropSingleTupleTableSlot(main_slot);
                match->emitted = true;
                output_slot = NULL;
            }
            else
            {
                if (match->tuple_data == NULL)
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("overlay_scan_next: delta op=%c key=%s has NULL tuple_data",
                                    match->op, match->key)));
                output_slot = reconstruct_slot_from_delta(st->rel, match->tuple_data,
                                                          st->row_cxt);
                /* ACC-1: keep the MAIN anchor ctid on the merged (delta) row.
                 * A DML-target heap slot carries it into t_self so the ctid
                 * system column resolves to the MAIN row (the overlay row has
                 * no physical ctid of its own). */
                output_slot->tts_tid = main_slot->tts_tid;
                ExecDropSingleTupleTableSlot(main_slot);
                match->emitted = true;
            }

            if (main_typed_pk != NULL && main_typed_n > 0)
            {
                for (tpk_i = 0; tpk_i < main_typed_n; tpk_i++)
                {
                    TypedKey *mtk = &main_typed_pk[tpk_i];

                    if (!mtk->isnull && !mtk->typbyval && mtk->value != (Datum) 0)
                        pfree(DatumGetPointer(mtk->value));
                }
                pfree(main_typed_pk);
            }
            if (pk_key != NULL)
                pfree(pk_key);

            if (output_slot != NULL)
            {
                MemoryContextSwitchTo(oldcxt);
                return output_slot;
            }

            /* This MAIN row was tombstoned (output_slot == NULL).  Free its
             * scratch before building the next one, so a branch that deletes a
             * large fraction of MAIN stays constant-memory within one call too
             * (the slot itself was already dropped above, but its datum copies
             * are only reclaimed by the reset). */
            MemoryContextReset(st->row_cxt);
        }
    }

    /* ---------- Pass 2: pure-delta orphans (no MAIN counterpart) ----------
     * Rows created inside the branch with no MAIN baseline row.  Must run even
     * when Pass 1 matched nothing (e.g. "WHERE pk = <newkey>"): MAIN returns 0
     * rows and the orphan INSERT is the whole answer.  Keys are unique, so an
     * un-emitted INSERT/UPDATE is emitted as-is and a DELETE stays dropped; an
     * unmatched keyless ("A:") anchor is an orphan whose MAIN row was
     * concurrently changed or removed, so it is dropped too (never resurrected
     * by ctid — doc/internals.md §4.5). */
    if (!st->pass2_started)
    {
        st->pass2_started = true;
        st->pass2_cur = list_head(st->delta_list);
    }
    while (st->pass2_cur != NULL)
    {
        DeltaTuple *dt = (DeltaTuple *) lfirst(st->pass2_cur);

        st->pass2_cur = lnext(st->delta_list, st->pass2_cur);
        if (dt == NULL)
            continue;
        if (!dt->emitted &&
            (dt->op == DELTA_OP_INSERT || dt->op == DELTA_OP_UPDATE))
        {
            TupleTableSlot *new_slot;

            if (!st->rel_has_pk && dt->key != NULL &&
                dt->key[0] == 'A' && dt->key[1] == ':')
                continue;
            if (dt->tuple_data == NULL)
                continue;   /* defensive: already guarded */

            new_slot = reconstruct_slot_from_delta(st->rel, dt->tuple_data,
                                                   st->row_cxt);
            dt->emitted = true;
            MemoryContextSwitchTo(oldcxt);
            return new_slot;
        }
    }

    MemoryContextSwitchTo(oldcxt);
    return NULL;
}

/*
 * Release the transient resources of an iterator: the heap scan descriptor,
 * the buffer-heap probe slot and the delta payload (TopMemoryContext).  The
 * state itself, the shared slot descriptor and the pre-materialised P0 rows
 * live in result_cxt and are reclaimed by whoever owns it (ebs->scan_cxt
 * reset/delete, or the SRF's multi_call context); the per-row scratch is a
 * child of result_cxt and dies with it.  The relation is NOT closed here — it
 * is borrowed from the caller.
 */
static void
overlay_scan_close(OverlayScanState *st)
{
    if (st == NULL)
        return;

    if (st->tscan != NULL)
    {
        table_endscan(st->tscan);
        st->tscan = NULL;
    }
    if (st->probe != NULL)
    {
        ExecDropSingleTupleTableSlot(st->probe);
        st->probe = NULL;
    }
    if (st->arr != NULL)
    {
        pfree(st->arr);
        st->arr = NULL;
    }
    if (st->delta_list != NIL)
    {
        ob_overlay_free_delta_list(st->delta_list);
        st->delta_list = NIL;
    }
}

/* ----------------------------------------------------------------
 * ob_expr_has_outer_ref
 *   True if the expression references state that lives outside the
 *   relation being deparsed: a PARAM_EXEC Param or a Var with
 *   varlevelsup > 0.
 *
 *   deparse_expression() with a bare deparse_context_for() namespace
 *   cannot render such clauses: get_parameter() walks
 *   dpns->plan->initPlan for a PARAM_EXEC, and deparse_context_for()
 *   leaves dpns->plan == NULL, so ruleutils dereferences NULL.  A
 *   correlated operand is also useless as a static pushdown fragment
 *   (its value is only known at execution time), so callers drop it
 *   and let scan.plan.qual re-check the row instead.
 * ---------------------------------------------------------------- */
static bool
ob_expr_has_outer_ref(Node *node, void *context)
{
    (void) context;

    if (node == NULL)
        return false;

    if (IsA(node, Param))
    {
        if (((Param *) node)->paramkind == PARAM_EXEC)
            return true;
    }
    else if (IsA(node, Var))
    {
        if (((Var *) node)->varlevelsup > 0)
            return true;
    }

    return expression_tree_walker(node, ob_expr_has_outer_ref, NULL);
}

/* ----------------------------------------------------------------
 * ob_pk_btree_op_strategy
 *   Return the btree strategy number of opno inside vartype's default
 *   btree opfamily (1=<  2=<=  3==  4=>=  5=>), or 0 when opno is not
 *   an ordering comparison of that type.  This is what lets P0 accept
 *   the five range/equality operators on a PK column without hard-coding
 *   per-type operator OIDs.
 * ---------------------------------------------------------------- */
static int
ob_pk_btree_op_strategy(Oid opno, Oid vartype)
{
    TypeCacheEntry *tc;
    Oid             opfamily;
    int             strat;

    if (!OidIsValid(opno))
        return 0;
    tc = lookup_type_cache(vartype, TYPECACHE_BTREE_OPFAMILY);
    if (tc == NULL)
        return 0;
    opfamily = tc->btree_opf;
    if (!OidIsValid(opfamily))
        return 0;
    strat = get_op_opfamily_strategy(opno, opfamily);
    if (strat < BTLessStrategyNumber || strat > BTGreaterStrategyNumber)
        return 0;
    return strat;
}

/* ----------------------------------------------------------------
 * ob_is_pk_pushable_clause
 *   True when clause is built ONLY from comparisons against the single PK
 *   column of rel rti:  pk = c / pk < c / pk <= c / pk > c / pk >= c,
 *   pk IN (...) / pk = ANY(...), and any AND/OR combination of those.
 *   Such a clause can be deparsed into a self-contained SQL fragment and
 *   pushed to the MAIN SPI query.
 *
 *   Anything else (IS NULL, NOT, ALL(...), a non-Const array element, a
 *   mixed OR arm, ...) returns false: the caller then leaves the clause to
 *   the P2 / plan.qual path.
 * ---------------------------------------------------------------- */
static bool
ob_is_pk_pushable_clause(Node *clause, Index rti, AttrNumber pk_attno)
{
    if (clause == NULL)
        return false;

    if (IsA(clause, OpExpr))
    {
        OpExpr *op = (OpExpr *) clause;
        Node   *left;
        Node   *right;
        Var    *var;

        if (list_length(op->args) != 2)
            return false;
        left  = (Node *) linitial(op->args);
        right = (Node *) lsecond(op->args);

        if (IsA(left, Var) && IsA(right, Const))
            var = (Var *) left;
        else if (IsA(left, Const) && IsA(right, Var))
            var = (Var *) right;
        else
            return false;

        if (var->varno != rti || var->varattno != pk_attno ||
            var->varlevelsup != 0)
            return false;

        return ob_pk_btree_op_strategy(op->opno, var->vartype) != 0;
    }

    if (IsA(clause, ScalarArrayOpExpr))
    {
        ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) clause;
        Node      *left;
        Node      *right;
        Var       *var;
        ListCell  *lc;

        /* IN semantics only: `= ANY(...)` (useOr).  `ALL(...)` is not an
         * IN list and is not index-pushable as a single fragment. */
        if (!sa->useOr || list_length(sa->args) != 2)
            return false;
        left  = (Node *) linitial(sa->args);
        right = (Node *) lsecond(sa->args);

        if (!IsA(left, Var))
            return false;
        var = (Var *) left;
        if (var->varno != rti || var->varattno != pk_attno ||
            var->varlevelsup != 0)
            return false;
        if (ob_pk_btree_op_strategy(sa->opno, var->vartype)
            != BTEqualStrategyNumber)
            return false;

        if (IsA(right, Const))
            return !((Const *) right)->constisnull;
        if (IsA(right, ArrayExpr))
        {
            foreach(lc, ((ArrayExpr *) right)->elements)
            {
                if (!IsA((Node *) lfirst(lc), Const))
                    return false;
            }
            return true;
        }
        return false;
    }

    if (IsA(clause, BoolExpr))
    {
        BoolExpr *b = (BoolExpr *) clause;
        ListCell *lc;

        if (b->boolop != AND_EXPR && b->boolop != OR_EXPR)
            return false;
        foreach(lc, b->args)
        {
            if (!ob_is_pk_pushable_clause((Node *) lfirst(lc), rti, pk_attno))
                return false;
        }
        return true;
    }

    return false;
}

/* ----------------------------------------------------------------
 * ob_pk_point_const
 *   True only for a lone `pk = Const` / `Const = pk` equality; *out_con
 *   receives the Const.  Only this shape yields a single serialized key
 *   for the DELTA O(1) lookup (overlay_delta_lookup).
 * ---------------------------------------------------------------- */
static bool
ob_pk_point_const(Node *clause, Index rti, AttrNumber pk_attno,
                  Const **out_con)
{
    OpExpr *op;
    Node   *left;
    Node   *right;
    Var    *var;
    Const  *con;

    *out_con = NULL;
    if (clause == NULL || !IsA(clause, OpExpr))
        return false;
    op = (OpExpr *) clause;
    if (list_length(op->args) != 2)
        return false;
    left  = (Node *) linitial(op->args);
    right = (Node *) lsecond(op->args);

    if (IsA(left, Var) && IsA(right, Const))
    {
        var = (Var *) left;
        con = (Const *) right;
    }
    else if (IsA(left, Const) && IsA(right, Var))
    {
        con = (Const *) left;
        var = (Var *) right;
    }
    else
        return false;

    if (var->varno != rti || var->varattno != pk_attno ||
        var->varlevelsup != 0)
        return false;
    if (ob_pk_btree_op_strategy(op->opno, var->vartype)
        != BTEqualStrategyNumber)
        return false;

    *out_con = con;
    return true;
}

/* ================================================================
 * Planner integration
 * ================================================================ */
static void
ob_branchscan_planner_hook(PlannerInfo *root, RelOptInfo *rel,
                           Index rti, RangeTblEntry *rte)
{
    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);

    /* ================================================================
     * PRE-CHECK LAYER (fast-path exits BEFORE any C function call
     * into branch_lifecycle.c that may itself invoke SPI / planner,
     * which would re-enter this hook → stack overflow.
     *
     * ORDERING RULE (CRITICAL, violation ⇒ infinite recursion SIGABRT):
     *   B0 (schema skip, O(1) relcache)
     *   → B1 (recursion guards: apply / write_redirect / helper)
     *   → B2 (command-type bypass: non-SELECT CMD)
     *   → ONLY THEN call C wrappers is_active/CurrentBranchContext etc.
     *
     * Rationale:
     *   overlay_branch_is_active() (L?) ≝
     *       ob_invalidate_check_throttled(false)  [SPI-execute: SELECT state
     *                                                FROM overlay_branch.pg_branch]
     *       + return ctx->is_active;
     *   If we call is_active() BEFORE filtering RTE's whose namespace ==
     *   OBSCHEMA ("overlay_branch"), the inner SPI query against
     *   pg_branch (which IS in OBSCHEMA but wasn't yet skipped because
     *   B0 was AFTER is_active()) will re-plan → re-enter this hook →
     *   is_active() again → infinite recursion →
     *   "stack depth limit exceeded" in 50-100 calls (2MB stack).
     *   See Section J3 of test/sql/overlay_branch_invalidation.sql
     *   (J3 Bug) for the regression guard that exercises this exact path.
     * ================================================================ */

    /* B0 PRE-CHECK: Skip system / overlay_branch catalog schemas.
     * MUST be the very FIRST per-RTE filter (before ANY C function into
     * the overlay module that could SPI-plan anything). */
    if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION)
    {
        Oid            nspoid;
        const char    *nspname;

        nspoid = get_rel_namespace(rte->relid);
        nspname = get_namespace_name(nspoid);
        if (nspname != NULL &&
            (strcmp(nspname, "pg_catalog") == 0 ||
             strcmp(nspname, "information_schema") == 0 ||
             strncmp(nspname, "pg_toast", 8) == 0 ||
             strcmp(nspname, OBSCHEMA) == 0))
            return;
    }

    /* B1 PRE-CHECK: recursion guards for bypass layers */
    if (overlay_in_apply_operation())
        return;
    if (overlay_in_write_redirect())
        return;
    if (overlay_in_overlay_helper())
        return;

    /* ================================================================
     * REVIEW-260926 / R16: MVP guard for ModifyingCTE (Data-Modifying
     *   statements inside WITH clauses).
     *
     * Why THREE separate choke points (belt-and-braces, redundancy by
     * design — silent MAIN pollution from ModCTE is a P0 data
     * corruption bug):
     *   1. [THIS BLOCK — Planner hook, NEW] — catches ALL entry points
     *      including EXPLAIN ANALYZE / prepared / SPI / DO / function
     *      body SQL.  The outer query is CMD_SELECT so the B2 check
     *      below would LIE and return early.  hasModifyingCTE is set
     *      by the rewriter on the Query node exactly when WITH-list
     *      contains I/U/D/MERGE.  We refuse to plan this.
     *   2. [ExecutorRun intercept in write_redirect.c — EXISTING] —
     *      catches at runtime plannedstmt->hasModifyingCTE (the
     *      PlannedStmt copy).  Acts as last line of defense.
     *   3. [PlannedStmt dispatch — not needed; #1/#2 sufficient]
     *
     * Placement:  between B1 and B2, BEFORE the CmdType check.  If we
     * placed this after B2 (`ct != CMD_SELECT -> return`) then a
     * ModCTE whose outer query is SELECT would pass B2 (ct==SELECT
     * ok) and proceed to custom planning → the planned
     * ModifyTable nodes inside CTE subplans run with zero WR
     * protection = write MAIN directly.  NOT acceptable. */
    if (root != NULL && root->parse != NULL &&
        nodeTag(root->parse) == T_Query)
    {
        Query *pq = (Query *) root->parse;
        if (pq->hasModifyingCTE)
        {
            /* Raise 0A000 here in the planner context.  This is
             * intentionally NOT a no-op / skip / elog(DEBUG); if we
             * silently returned without injecting CustomScan the
             * planner would continue, build a plan with ModifyTable
             * inside CTE subplans, and ExecutorRun's outer CmdType is
             * CMD_SELECT → dml_split=false → WR checks do NOT opt-in
             * kickout → the ModifyTable nodes execute against MAIN
             * heap.  Catastrophic drift-write.  So we ERROR HERE,
             * BEFORE any plan shape can be constructed.
             *
             * NOTE: ereport inside a planner hook IS safe (PG core
             * does this all the time for constraint errors during
             * planning; PG_TRY/CATCH in caller handles it). */
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("overlay_branch MVP does not support data-modifying statements inside WITH (CTE) clauses (R16 planner-stage blocked)"),
                     errhint("Rewrite WITH (UPDATE/DELETE/INSERT ... RETURNING) SELECT ... "
                             "using a TEMP TABLE to collect RETURNING rows:\n"
                             "  CREATE TEMP TABLE _r AS UPDATE t SET ... RETURNING ...;\n"
                             "  SELECT * FROM _r; DROP TABLE _r;")));
        }
    }

    /* B2 PRE-CHECK: MVP injects CustomScan for pure SELECT, and (ACC-1) for
     * the DML target baserel of an UPDATE/DELETE so the DML subplan reads
     * overlay rows.  Every other non-SELECT command (INSERT, and the FROM/USING
     * side of UPDATE ... FROM / DELETE ... USING, which is a non-target RTI)
     * still bails out here.  Guarded by the same reason as before: this runs
     * above the is_active() call to avoid an unnecessary helper C entry. */
    if (root->parse != NULL && root->parse->type == T_Query)
    {
        CmdType ct = ((Query *) root->parse)->commandType;
        if (ct != CMD_SELECT &&
            ((ct != CMD_UPDATE && ct != CMD_DELETE) ||
             rti != ((Query *) root->parse)->resultRelation))
            return;
    }

    /* ---- End of pre-check layer; it is now SAFE to call into the
     * overlay module's C helpers because:
     *   (1) any SPI inside the helper would target *another* user table,
     *       not OBSCHEMA — we've already filtered those out,
     *   (2) helper recursion guards are set on their respective internal
     *       helper-enter/exit functions.
     * ---- */

    /* OLD BLOCK L367-431's order (now PROHIBITED, see J3 bug):
     *   if (in_apply) return;            // B1 → ok
     *   if (in_write_redirect) return;   // B1 → ok
     *   if (in_overlay_helper) return;   // B1 → ok
     *   if (ct != SELECT) return;        // B2 → ok
     *   if (!overlay_branch_is_active()) // ← ⚠ CALLED BEFORE B0!
     *       return;                          stack overflow ⇐ B0 was at ~L418
     *   ... B0 ...                        ← too late, recursion already on
     * DO NOT revert to that order without re-running J3 + J threshold=1. */

    /* --- Planner-hook throttle-suppression wrap ---
     * We call the C wrapper overlay_branch_is_active() here, which in turn
     * calls ob_invalidate_check_throttled(for_dml=false).  The throttled()
     * helper, when its heavy SPI check fires, has SIDE-EFFECTS: it runs
     * ob_exit_branch_cleanup() → ctx->is_active=false +
     * SetConfigOption("overlay_branch.current","") + ereport DQL NOTICE.
     *
     * That is the CORRECT behaviour during *executor* processing (a plain
     * SELECT that discovers its branch is stale MUST revert and announce
     * it).  It is the WRONG behaviour during planner processing, for a
     * non-SELECT CmdType whose plan we will not even generate a
     * CustomScan for anyway (B2 returns immediately after).  The ordering
     * race that kills D7 if we don't wrap this:
     *
     *   D6: manual catalog flip state=applied
     *   D6b: SET threshold=1 (protected by ProcessUtility whitelist; safe)
     *   D7: UPDATE t_inv SET v = ... WHERE pk=11
     *     → planner hook runs for t_inv RTE (UPDATE target)
     *       B0 (schema public: NOT protected) → B1 ok → B2 non-SELECT →
     *       (B2 already returns; fine.  BUT: if the CmdType were SELECT,
     *        or if B2 were somehow after the is_active() call, the wrap
     *        below is still safe: a stale-branch SELECT plan should NOT
     *        mutate user state in the *planner*, only during *executor*.)
     *     → executor_run_intercept runs with dml_split=true, is_active()
     *       was NOT prematurely cleared by the planner, throttled(true)
     *       runs heavy SPI → DML split ERROR → D7 PASS.
     *
     * Concretely: set ob_in_planner_hook around the C call.  Inside
     * throttled(), when this flag is set and the heavy SPI check would
     * kick out, we jump straight to returning `false` / `true` with NO
     * side-effects (no ctx cleanup, no GUC reset, no NOTICE) and let the
     * subsequent executor-stage call do the real enforcement.
     *
     * Scope of the flag: it wraps ONLY the is_active() call, because:
     *   - everything before (B0/B1/B2) is pure O(1) relcache + guard bools
     *     and cannot SPI-plan anything;
     *   - everything AFTER the is_active() call (CustomPath construction)
     *     does NOT call back into throttled/is_active helpers, so keeping
     *     the flag live there would be pointless and add unnecessary
     *     recursion-guard depth.
     */
    {
        bool active_snapshot;

        ob_in_planner_hook = true;
        active_snapshot = overlay_branch_is_active();
        ob_in_planner_hook = false;
        if (!active_snapshot)
            return;
    }
    if (rte->rtekind != RTE_RELATION)
        return;
    if (rte->relkind != RELKIND_RELATION)
        return;
    /* OLD B0 block WAS HERE (L418-431, after is_active).
     * Removed to avoid double-schema-check; B0 has been moved above. */
    {
        /* Widen reln's lifetime to the whole CustomPath construction block: both the preceding
         * has_pk check and the new PK equality predicate detection need to access the relcache. */
        Relation    reln = table_open(rte->relid, NoLock);
        CustomPath *cpath;
        Cost        min_cost = 1.0e-6;
        ListCell   *lc;
        /* P0: PK-IndexScan MVP — try to recognize from baserestrictinfo
         * a single-column PK = Const equality condition. On a hit, the fast path: the SPI MAIN lookup uses
         * WHERE pk=val, delta uses overlay_delta_lookup (O(1)).
         * custom_private transfer protocol (List length 6):
         *   [0] String  → rte->relid decimal string (no longer parsed, kept only as a compatibility placeholder)
         *   [1] Integer → has_pk_pred_flag (0 or 1)
         *   [2] String  → pk_where_sql_cstr (empty string means flag=0)
         *   [3] String  → serialized_pk_key (empty string means flag=0)
         *   [4] Integer → has_general_where_flag (0 or 1, P2 non-PK pushdown)
         *   [5] String  → general_where_sql (output of deparse_expression, empty string means flag=0)
         * Reader side: first take the standard RT-index→rte→relid path, then unpack by index. */
        AttrNumber  pk_attno = 0;
        const char *pk_colname = NULL;
        bool        has_pk_pred = false;
        char       *pk_where_sql = NULL;
        char       *pk_serialized_key = NULL;
        bool        has_general_where = false;
        char       *general_where_sql = NULL;

        if (!overlay_relation_has_pk(reln) && !overlay_keyless_tables)
        {
            table_close(reln, NoLock);
            return;
        }

        if (overlay_get_pk_single_attno(reln, &pk_attno, &pk_colname))
        {
            List       *pk_clauses = NIL;
            ListCell   *rc;

            /* Collect every top-level clause that is composed ONLY of
             * PK-column btree comparisons: equality, range (</<=/>/>=),
             * IN / = ANY(...), and any AND/OR combination of those.  They
             * are ANDed into a single fragment and deparsed ONCE, so the
             * MAIN SPI SELECT below still hands the predicate to the
             * planner's PK index scan. */
            foreach(rc, rel->baserestrictinfo)
            {
                RestrictInfo *rinfo = (RestrictInfo *) lfirst(rc);

                if (!IsA(rinfo, RestrictInfo))
                    continue;
                if (ob_expr_has_outer_ref((Node *) rinfo->clause, NULL))
                    continue;
                if (ob_is_pk_pushable_clause((Node *) rinfo->clause,
                                             rti, pk_attno))
                    pk_clauses = lappend(pk_clauses, rinfo->clause);
            }

            if (pk_clauses != NIL)
            {
                Node       *top;
                List       *dpctx;
                char       *deparsed;

                top = (Node *) ((list_length(pk_clauses) == 1)
                                    ? (Node *) linitial(pk_clauses)
                                    : (Node *) makeBoolExpr(AND_EXPR,
                                                            pk_clauses,
                                                            -1));
                /* V2 strategy (doc/internals.md §2.1): hand the whole
                 * predicate node to ruleutils instead of assembling SQL by
                 * hand, so the literal's consttype drives the output
                 * function and no static format_type_be buffer leaks in. */
                dpctx = deparse_context_for(RelationGetRelationName(reln),
                                            rte->relid);
                deparsed = NULL;
                PG_TRY();
                {
                    deparsed = deparse_expression(top, dpctx, false, false);
                }
                PG_CATCH();
                {
                    FlushErrorState();
                    deparsed = NULL;
                }
                PG_END_TRY();

                if (deparsed != NULL && *deparsed != '\0' &&
                    strlen(deparsed) < 65536)
                {
                    has_pk_pred = true;
                    pk_where_sql = pstrdup(deparsed);

                    /* L2 DELTA O(1): only a lone *point equality* yields one
                     * serialized key.  IN / range / OR have no single key,
                     * so pk_serialized_key stays empty and the delta side
                     * falls back to the full (branch_id, relid) list (see
                     * overlay_scan_open). */
                    if (list_length(pk_clauses) == 1)
                    {
                        Const *con = NULL;

                        if (ob_pk_point_const((Node *) linitial(pk_clauses),
                                              rti, pk_attno, &con))
                            pk_serialized_key =
                                overlay_serialize_pk_from_single_datum(
                                    reln, pk_attno, con->constvalue,
                                    con->constisnull, con->consttype);
                    }
                }
                /* deparse FAILED: has_pk_pred stays false so the P2 pass
                 * below re-collects every PK clause into general_where —
                 * never silently drop a predicate from the MAIN filter. */
            }
        }

        /* P2 general (non-PK) qual pushdown: collect from baserestrictinfo the remaining clauses
         * that are not PK=Const itself, combine them with AND and deparse into
         * SQL text passed through to the Executor, which the MAIN SPI SELECT uses directly
         * to filter (reducing rows pulled back from disk on the MAIN side).
         * Fallback: a deparse error, a non-depsable node, or an empty result string
         * → has_general_where=false, Main keeps the original query.
         *
         * Correlated references (correlated PARAM_EXEC / outer Var) are removed before
         * entering deparse: see ob_expr_has_outer_ref(). Such nodes can neither be deparsed into
         * a standalone SQL fragment, nor passed to ruleutils, which would dereference dpns->plan as NULL
         * and crash; after removal they are re-filtered row by row via scan.plan.qual. */
        if (rel->baserestrictinfo != NIL)
        {
            List       *remain = NIL;
            ListCell   *rc;

            foreach(rc, rel->baserestrictinfo)
            {
                RestrictInfo *rinfo = (RestrictInfo *) lfirst(rc);
                Node        *clause;
                bool        is_pk_clause = false;

                if (!IsA(rinfo, RestrictInfo)) continue;
                clause = (Node *) rinfo->clause;

                /* A PK clause already folded into pk_where_sql must not be
                 * pushed a second time through general_where.  Skip it ONLY
                 * when P0 actually consumed it (has_pk_pred); if P0's
                 * deparse failed, the clause must be re-collected here so
                 * the MAIN filter is not silently lost. */
                if (has_pk_pred && pk_attno != 0 &&
                    ob_is_pk_pushable_clause(clause, rti, pk_attno))
                    is_pk_clause = true;
                if (!is_pk_clause && !ob_expr_has_outer_ref(clause, NULL))
                    remain = lappend(remain, clause);
            }
            if (remain != NIL)
            {
                Node       *top;
                List       *dpctx;
                char       *deparsed;

                top = (Node *) ((list_length(remain) == 1)
                                    ? (Node *) linitial(remain)
                                    : (Node *) makeBoolExpr(AND_EXPR,
                                                            remain,
                                                            -1));
                dpctx = deparse_context_for(RelationGetRelationName(reln), rte->relid);
                PG_TRY();
                {
                    deparsed = deparse_expression(top, dpctx, false, false);
                }
                PG_CATCH();
                {
                    FlushErrorState();
                    deparsed = NULL;
                }
                PG_END_TRY();
                if (deparsed != NULL && *deparsed != '\0' &&
                    strlen(deparsed) < 65536)
                {
                    has_general_where = true;
                    general_where_sql = pstrdup(deparsed);
                }
            }
        }

        cpath = makeNode(CustomPath);
        foreach(lc, rel->pathlist)
        {
            Path *p = (Path *) lfirst(lc);
            if (p->total_cost < min_cost) min_cost = p->total_cost;
        }
        cpath->path.pathtype   = T_CustomScan;
        cpath->path.parent     = rel;
        cpath->path.pathtarget = rel->reltarget;
        cpath->path.param_info = NULL;
        cpath->path.parallel_aware = false;
        cpath->path.parallel_safe  = false;
        cpath->path.parallel_workers = 0;

        /* =====================================================================
         * REVIEW-260926 / A8 — cost_branchscan() MVP-level cost model
         * ---------------------------------------------------------------
         * Background (R11/R12 review):
         *   Pass1 MAIN merge loop used to be O(N×M) naive double-scan.
         *   After R11 fix: Phase A = copy + qsort → O(M log M)
         *                   Phase B = unique latest-wins → O(M) in practice
         *                   Pass1 = MAIN merge × bsearch → O(N log M_unique)
         *                   Pass2 = delta_list 2 linear walks → O(M) in practice
         *   Total complexity = O(M log M + N log M).
         *
         * Planner stage constraint (MVP):
         *   M (#delta rows for current_bid + relid) is NOT trivially known
         *   here (planner has no direct SPI access to pg_branch_delta w/o
         *   a heavy call).  We therefore estimate M using a realistic
         *   "editing branch" ratio: typical in-branch rewrite ratio for a
         *   100% acceptance MVP ∈ [1%, 10%] of MAIN rows.  We use a middle
         *   estimate of 5% (= 0.05), clamped to a sane interval to avoid
         *   pathological estimates for very small/large tables.
         *
         *   M = clamp( rel->tuples × 0.05 , 1, 1000000 )
         *
         * Cost decomposition (follows PG cost.c conventions):
         *   startup_cost = Phase A + Phase B
         *                = cpu_tuple_cost × (M × log2(M+1))        [sort]
         *                + cpu_tuple_cost × M                       [dedup]
         *   run_cost     = Pass1 + Pass2
         *                = cpu_tuple_cost × (N × log2(M+1))        [bsearch]
         *                + cpu_tuple_cost × N                       [MAIN emit]
         *                + cpu_tuple_cost × M                       [Pass2 linear]
         *   total_cost   = startup_cost + run_cost
         *
         *   PK-pred fast path (override):
         *     If has_pk_pred=true, Pass1 bsearch lookup is 1× not N×, so
         *     we squash N→1 in run_cost.  Rows = 1.
         *
         * Safety / fallback:
         *   If the resulting total_cost would be LARGER than the cheapest
         *   native path (unlikely, given our 5% M ratio is conservative),
         *   we still keep our computed cost (truthful, EXPLAIN output is
         *   correct for DBA inspection) — MVP correctness does NOT depend
         *   on planner choosing us, because the L1350-1360 block below
         *   forcibly deletes all non-CustomPath paths from rel->pathlist
         *   in active-branch mode (BranchScan becomes the ONLY candidate).
         *
         * Historical note (was):
         *   cpath->path.startup_cost = 0.0;
         *   cpath->path.total_cost   = min_cost * 1.0e-5; (artificially zero)
         * =================================================================== */
        {
            double      N = (rel->tuples > 0) ? rel->tuples : 100.0;
            double      M_est_raw = N * 0.05;
            double      M = (M_est_raw < 1.0) ? 1.0 :
                            (M_est_raw > 1000000.0) ? 1000000.0 : M_est_raw;
            double      log_M = 1.0;
            double      startup, run, total;

            if (M > 1.0)
            {
                double  ln_M = 0.0;
                double  m_tmp = M;
                while (m_tmp > 1.0) { ln_M += 1.0; m_tmp /= 2.0; }
                log_M = ln_M;
            }

            startup = cpu_tuple_cost * (M * log_M + M);

            if (has_pk_pred)
            {
                run = cpu_tuple_cost * (1.0 * log_M + 1.0)       /* Pass1: 1 bsearch */
                    + cpu_tuple_cost * M;                        /* Pass2 linear */
            }
            else
            {
                run = cpu_tuple_cost * (N * log_M + N)           /* Pass1: N bsearch */
                    + cpu_tuple_cost * M;                        /* Pass2 linear */
            }
            total = startup + run;

            cpath->path.startup_cost = startup;
            cpath->path.total_cost   = total;

            if (has_pk_pred)
                cpath->path.rows = 1.0;
            else
                cpath->path.rows = rel->rows;
        }
        cpath->path.pathkeys   = NIL;
        cpath->flags           = 0;
        cpath->custom_paths    = NIL;
        cpath->custom_restrictinfo = NIL;

        /* Pack custom_private (uniform Integer/String wrapping).
         * OID is passed as a decimal string (a PG List Integer cell is int32,
         * so a large OID >INT32_MAX truncates to a negative number and the Executor's open rel fails).
         * custom_private is extended to 8 elements for backward compatibility: if any element is missing
         * the Executor safely falls back to the corresponding full path. */
        {
            List       *cpriv = NIL;
            int         pkflag;
            int         gwflag;
            int32       plan_bid;
            int         dml_target;
            char       *oid_cstr;
            pkflag = has_pk_pred ? 1 : 0;
            gwflag = has_general_where ? 1 : 0;
            /* ACC-1: mark plans built for an UPDATE/DELETE DML target so the
             * executor exposes a heap-backed scan slot (ctid system column
             * must be retrievable).  Same predicate as the B2 pre-check. */
            dml_target = 0;
            if (root->parse != NULL && root->parse->type == T_Query)
            {
                Query *pq = (Query *) root->parse;
                if ((pq->commandType == CMD_UPDATE ||
                     pq->commandType == CMD_DELETE) &&
                    rti == pq->resultRelation)
                    dml_target = 1;
            }
            /* A1/A6 Stage 3: record the branch identity this plan was built
             * for.  BeginCustomScan asserts that the executing statement is
             * bound to the same branch (see ob_branchscan_begin). */
            plan_bid = overlay_branch_get_current_id();
            oid_cstr = psprintf("%u", (unsigned) rte->relid);
            /* CRITICAL: makeString() stores the passed char* pointer
             * BY REFERENCE — it does NOT copy.  We MUST NOT pfree()
             * the strings we hand to makeString(), otherwise the
             * String* nodes inside custom_private end up pointing to
             * freed/reused memory (→ Executor reads garbage bytes
             * like 0x0111 or random pointers).
             * Strategy: always pstrdup() on the way in, then free our
             * locals as usual.  copyObject() (which happens when
             * CustomScan plan is deep-copied) then takes its own
             * deep copies of String nodes via pstrdup in
             * _copyString. */
            cpriv = lappend(cpriv, makeString(pstrdup(oid_cstr)));
            pfree(oid_cstr);
            cpriv = lappend(cpriv, makeInteger(pkflag));
            if (pkflag && (pk_where_sql == NULL || *pk_where_sql == '\0'))
            {
                pkflag = 0;
                has_pk_pred = false;
                if (pk_serialized_key) { pfree(pk_serialized_key); pk_serialized_key = NULL; }
                /* Replace the Integer cell we just appended. */
                list_nth_cell(cpriv, 1)->ptr_value = (void *)(intptr_t) makeInteger(0);
            }
            cpriv = lappend(cpriv,
                            makeString(pstrdup(
                                (has_pk_pred && pk_where_sql) ? pk_where_sql : "")));
            cpriv = lappend(cpriv,
                            makeString(pstrdup(
                                (has_pk_pred && pk_serialized_key) ? pk_serialized_key : "")));
            elog(DEBUG2, "P0 DEBUG: pkflag=%d where=%s key_prefix=%.*s; gwflag=%d general=%s",
                 pkflag,
                 (has_pk_pred && pk_where_sql) ? pk_where_sql : "(empty)",
                 (has_pk_pred && pk_serialized_key) ? 16 : 0,
                 (has_pk_pred && pk_serialized_key) ? pk_serialized_key : "(null)",
                 gwflag,
                 (has_general_where && general_where_sql) ? general_where_sql : "(empty)");
            cpriv = lappend(cpriv, makeInteger(gwflag));
            cpriv = lappend(cpriv,
                            makeString(pstrdup(
                                (has_general_where && general_where_sql) ? general_where_sql : "")));
            /* [6] Integer → plan-time branch id (0 = MAIN).  Consumed by the
             * A1/A6 Stage 3 assertion in ob_branchscan_begin. */
            cpriv = lappend(cpriv, makeInteger((int) plan_bid));
            /* [7] Integer → dml_target (0/1, ACC-1).  Older plans carry 7
             * elements; the executor treats a missing element as 0. */
            cpriv = lappend(cpriv, makeInteger(dml_target));
            cpath->custom_private = cpriv;
        }
        if (pk_where_sql)
            pfree(pk_where_sql);
        if (pk_serialized_key)
            pfree(pk_serialized_key);
        if (general_where_sql)
            pfree(general_where_sql);

        cpath->methods         = &ob_branchscan_path_methods;
        /* =================================================================
         * CRITICAL FIX (A8 regression): do NOT use add_path() here!
         * ---------------------------------------------------------------
         * Standard add_path() performs dominance/cheaper checks: if our
         * CustomPath's total_cost is HIGHER than an existing SeqScan/
         * IndexScan (which becomes true the moment we report a realistic
         * non-zero cost model instead of the old 0.00 1e-5 artificial
         * floor), add_path() SILENTLY REJECTS our cpath and returns NULL
         * without appending it to rel->pathlist.
         *
         * Then the MVP pathlist-prune block below finds newlist=NIL
         * (no CustomPath exists) and falls back to keeping the
         * original SeqScan/IndexScan-only pathlist → the entire overlay
         * delta merge is BYPASSED (silent correctness bug: UPDATEs/
         * DELETEs/INSERTs of the branch silently don't show up in reads).
         *
         * MVP contract = in active branch, BranchScan is the ONLY
         * correct scan (MAIN heap's bare rows are just the baseline).
         * We must therefore FORCE our cpath into the pathlist regardless
         * of Planner cost comparisons, then delete all native competitors.
         * Direct lappend bypasses add_path() entirely; the prune step
         * ensures only ours survives.
         * ================================================================= */
        rel->pathlist = lappend(rel->pathlist, cpath);

        /* MVP override: in an active branch, our overlay merge result
         * MUST be used — the MAIN heap rows are only the baseline and
         * do NOT reflect the branch's writes (INSERT/UPDATE/DELETE
         * redirected to the delta table).  If the Planner is allowed
         * to pick an IndexScan or SeqScan on the bare heap (because
         * we only undercut their cost by a 1e-5 multiplicative
         * factor but index pathkeys match ORDER BY etc.), it will
         * silently ignore the delta table and return stale results.
         * Simplest safe fix for MVP: delete every non-CustomPath path
         * from rel->pathlist so our CustomPath is the ONLY candidate
         * (retaining any parallel-aware variants of CustomScan if
         * any — MVP has none).  Later optimizations can whitelist
         * specific MAIN-heap scans for inner uses (e.g. PK lookups
         * from write-redirection helper SELECTs already bypass here
         * via the B1.25 guard above). */
        {
            List     *newlist = NIL;
            ListCell *lc2;
            foreach(lc2, rel->pathlist)
            {
                Path *p = (Path *) lfirst(lc2);
                if (IsA(p, CustomPath))
                    newlist = lappend(newlist, p);
            }
            /* If our CustomPath is the only candidate (normal active-branch
             * case, at least one SeqScan/IndexScan existed to seed pathlist),
             * strip everything else.  If newlist is EMPTY (e.g. PG Planner
             * already reduced baserel to a single dummy Result path with
             * rows=0, as happens for `WHERE id = NULL` or
             * `WHERE id IS NULL` on a NOT-NULL PK), KEEP the original
             * pathlist.  A zero-row dummy plan is correct (no rows can
             * possibly match, so no need for BranchScan injection), and
             * otherwise `pathlist = NIL` leads to a fatal
             * "could not devise a query plan for the given query" error. */
            if (newlist != NIL)
                rel->pathlist = newlist;
        }

        /* =================================================================
         * MF-2/MF-3: parallel paths MUST be pruned too.
         * ---------------------------------------------------------------
         * The prune above only rewrites rel->pathlist.  Core PG then calls
         * generate_useful_gather_paths() *after* this hook returns, which
         * turns whatever is left in rel->partial_pathlist into a Gather
         * path appended to rel->pathlist.  A partial path is always a bare
         * MAIN heap scan (seqscan/index scan) — CustomScan has no parallel
         * variant — so letting it through means the planner can pick
         * "Gather → Parallel Seq Scan on <MAIN>" and the whole delta merge
         * is silently skipped (reads return pre-branch data).
         *
         * Clearing partial_pathlist is enough to stop that: the core's
         * generate_gather_paths() bails out immediately on an empty list.
         * consider_parallel=false additionally keeps join-level partial
         * paths from being built (joinrel->consider_parallel is AND-ed
         * from its inputs), so no parallel plan of any shape can survive.
         *
         * Side benefit: no parallel worker is ever launched for a branch
         * read, so no worker hits RestoreGUCState() on
         * overlay_branch.current — which used to fail with "cannot execute
         * SQL without an outer snapshot or portal" (MF-3).
         * ================================================================= */
        rel->partial_pathlist = NIL;
        rel->consider_parallel = false;

        table_close(reln, NoLock);
    }
}

static Plan *
ob_branchscan_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
                               CustomPath *cpath, List *tlist,
                               List *clauses, List *custom_plans)
{
    CustomScan *cscan = makeNode(CustomScan);
    /* P0: custom_private 4-element protocol:
     *   [0] String  → OID decimal string (parsed with strtoul, avoiding large-OID int32 truncation)
     *   [1] Integer → has_pk_pred_flag (0/1)
     *   [2] String  → pk_where_sql_cstr (may be empty)
     *   [3] String  → serialized_pk_key (may be empty)
     * pass through directly to cscan, no structural conversion. */
    const char *relid_cstr = strVal(linitial(cpath->custom_private));
    Oid         relid = (Oid) strtoul(relid_cstr, NULL, 10);
    List       *custom_exprs_list;

    (void) root;
    (void) relid;    /* relid OID is already passed in custom_private[0]; here it is only unpacked for a check */
    (void) clauses;  /* qual is NOT pushed down in MVP: we materialize the
                      * full overlay result set in ExecCustomScan and let PG
                      * wrap our CustomScan node in an upper Filter node to
                      * evaluate WHERE quals.  For PK-predicate fast-path:
                      * the Planner has already recognized PK=Const and passed pk_where_sql;
                      * non-PK conditions still run in scan.plan.qual.
                      * Note: clauses may contain RestrictInfos already absorbed by the PK condition;
                      * they still need to be unwrapped and attached to scan.plan.qual —
                      * ExecQual's evaluation of AND-conditions is idempotent, so re-evaluating
                      * a PK=Const once more does not affect correctness (just one extra comparison).*/

    cscan->scan.scanrelid  = rel->relid;
    cscan->flags           = cpath->flags;
    cscan->custom_plans    = custom_plans;

    /* Strip RestrictInfo wrappers: scan.plan.qual expects plain Exprs,
     * not RestrictInfo nodes (T_RestrictInfo=315 would be reported as
     * "unrecognized node type" by ExecQual). */
    custom_exprs_list = NIL;
    {
        ListCell *lc;
        foreach(lc, clauses)
        {
            RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
            if (IsA(rinfo, RestrictInfo))
                custom_exprs_list = lappend(custom_exprs_list, copyObject(rinfo->clause));
        }
    }
    cscan->custom_exprs    = NIL;
    cscan->custom_private  = copyObject(cpath->custom_private);
    cscan->custom_scan_tlist = NIL;
    cscan->custom_relids   = bms_make_singleton(rel->relid);
    cscan->methods         = &ob_branchscan_scan_methods;
    cscan->scan.plan.startup_cost = cpath->path.startup_cost;
    cscan->scan.plan.total_cost   = cpath->path.total_cost;
    cscan->scan.plan.plan_rows    = cpath->path.rows;
    cscan->scan.plan.plan_width   = rel->reltarget->width;
    cscan->scan.plan.targetlist   = copyObject(tlist);
    cscan->scan.plan.qual         = custom_exprs_list;
    cscan->scan.plan.lefttree     = NULL;
    cscan->scan.plan.righttree    = NULL;
    cscan->scan.plan.extParam     = NULL;
    cscan->scan.plan.allParam     = NULL;
    return (Plan *) cscan;
}

/* ================================================================
 * Executor integration
 * ================================================================ */
static Node *
ob_branchscan_create_custom_scan_state(CustomScan *cscan)
{
    ExtendedCustomScanState *ebs;
    List       *cpriv = cscan->custom_private;
    int         dml_target = 0;

    /* ACC-1: custom_private[7] (Integer) marks an UPDATE/DELETE DML-target
     * BranchScan.  Plans built before this element existed (7 entries) fall
     * through to 0, i.e. the plain virtual-slot behaviour. */
    if (list_length(cpriv) >= 8)
    {
        Node *nde = list_nth(cpriv, 7);

        if (nde != NULL && IsA(nde, Integer))
            dml_target = intVal(nde);
    }

    ebs = (ExtendedCustomScanState *) palloc0(sizeof(ExtendedCustomScanState));
    /* Tag the embedded CustomScanState as T_CustomScanState.  We
     * can't use makeNode() because it allocates exactly
     * sizeof(CustomScanState). */
    NodeSetTag(&ebs->css, T_CustomScanState);
    ebs->css.methods = &ob_branchscan_exec_methods;
    /* slotOps NULL lets ExecInitCustomScan default to TTSOpsVirtual
     * (nodeCustom.c L69-70).  For a DML target we need a heap-backed slot
     * instead: the row-identity ctid is a system column and only
     * heap_getsysattr() can deliver it (tts_virtual_getsysattr raises
     * "cannot retrieve a system column in this context").  See
     * doc/internals.md §2.5. */
    if (dml_target)
        ebs->css.slotOps = &TTSOpsHeapTuple;
    else
        ebs->css.slotOps = NULL;
    /* custom_ps starts NIL; no child PlanState subtrees.  Begin fills
     * ExtendedCustomScanState private fields. */
    ebs->css.custom_ps = NIL;
    ebs->dml_target = (dml_target != 0);
    return (Node *) ebs;
}

/* ================================================================
 * SRF-facing streaming wrapper.
 *
 * overlay_main_plus_delta() has no executor qual to push down, so it drives
 * the same iterator with qual = NULL / has_pk_pred = false.  The relation is
 * opened here and must stay open for the whole scan (the iterator borrows it),
 * so the handle is allocated in result_cxt together with a reset callback that
 * closes the relation and releases the delta payload even when the caller stops
 * early (LIMIT above the SRF) and never reaches the drained state.
 * ================================================================ */
typedef struct ObSrfScanHandle
{
    Relation                rel;      /* opened here, closed by release() */
    OverlayScanState       *st;       /* borrowed from result_cxt */
    MemoryContextCallback   cb;       /* in result_cxt; fires on reset/delete */
    bool                    closed;
} ObSrfScanHandle;

static void
ob_srf_scan_release(ObSrfScanHandle *h)
{
    if (h == NULL || h->closed)
        return;
    h->closed = true;
    if (h->st != NULL)
    {
        overlay_scan_close(h->st);
        h->st = NULL;
    }
    if (h->rel != NULL)
    {
        table_close(h->rel, AccessShareLock);
        h->rel = NULL;
    }
}

static void
ob_srf_scan_reset_cb(void *arg)
{
    ob_srf_scan_release((ObSrfScanHandle *) arg);
}

void *
ob_overlay_srf_open(Oid relid, int32 branch_id, MemoryContext result_cxt)
{
    MemoryContext    oldcxt;
    ObSrfScanHandle *h;

    oldcxt = MemoryContextSwitchTo(result_cxt != NULL ? result_cxt
                                                      : CurrentMemoryContext);

    h = (ObSrfScanHandle *) palloc0(sizeof(ObSrfScanHandle));
    h->rel = table_open(relid, AccessShareLock);
    h->st  = overlay_scan_open(h->rel, branch_id,
                               false, NULL, NULL, false, NULL,
                               NULL, NULL, false, result_cxt);

    h->cb.func = ob_srf_scan_reset_cb;
    h->cb.arg  = h;
    MemoryContextRegisterResetCallback(CurrentMemoryContext, &h->cb);

    MemoryContextSwitchTo(oldcxt);
    return h;
}

TupleTableSlot *
ob_overlay_srf_next(void *handle)
{
    ObSrfScanHandle *h = (ObSrfScanHandle *) handle;

    if (h == NULL)
        return NULL;
    {
        TupleTableSlot *slot = overlay_scan_next(h->st);

        if (slot == NULL)
            ob_srf_scan_release(h);   /* drained; release is idempotent */
        return slot;
    }
}

void
ob_overlay_srf_close(void *handle)
{
    ob_srf_scan_release((ObSrfScanHandle *) handle);
}

static void
ob_branchscan_begin(CustomScanState *node, EState *estate, int eflags)
{
    CustomScan      *cscan = castNode(CustomScan, node->ss.ps.plan);
    ExtendedCustomScanState *ebs = CSS2BS(node);
    Oid              relid;
    int32            branch_id;
    Relation         rel;
    List            *cpriv;
    int              flag;

    (void) eflags;

    /* P0: custom_private 4-element protocol unpacking.
     * OID is no longer parsed from custom_private[0] (strings are error-prone),
     * instead taken directly from PG structures inherited from estate: RT index → RTE → relid.*/
    cpriv = cscan->custom_private;
    {
        Index           rti = cscan->scan.scanrelid;
        RangeTblEntry  *rte;
        /* scanrelid is the baserel's RT index (numbered from 1).
         * For CustomScan we already set cscan->scan.scanrelid during the Plan phase
         * = rel->relid (RT index), so it is always available here.*/
        rte = rt_fetch(rti, estate->es_range_table);
        relid = rte->relid;
    }
    /* P0 PK=Const fast-path unpacking (see doc/p0_pk_oidx_deparse_strategy.md §5).
     * Protocol: cpriv[1]=Integer pkflag; cpriv[2]=String pk_where_sql;
     *       cpriv[3]=String pk_serialized_key.
     * Any malformed / empty string → has_pk_pred=false, safely fall back to seqscan
     * (P2 general_where can still be enabled independently; the two paths are decoupled).
     * Note: **you MUST use intVal() to read ival from the Integer node, not
     * lsecond_int / list_nth_int** — the latter directly truncates the
     * Integer* pointer stored in the ListCell into int (high bits lost on 64-bit), crashing
     * the whole process with exit code 2. */
    flag = 0;
    if (list_length(cpriv) >= 2)
    {
        Node *nde = lsecond(cpriv);
        if (nde != NULL && IsA(nde, Integer))
            flag = intVal(nde);
    }
    ebs->has_pk_pred = (flag != 0);
    ebs->pk_where_sql = NULL;
    ebs->pk_serialized_key = NULL;
    if (ebs->has_pk_pred && list_length(cpriv) >= 4)
    {
        Node       *n_w = lthird(cpriv);
        Node       *n_k = lfourth(cpriv);
        const char *wstr = (n_w != NULL && IsA(n_w, String))
                               ? strVal(n_w) : NULL;
        const char *kstr = (n_k != NULL && IsA(n_k, String))
                               ? strVal(n_k) : NULL;

        /* pk_where_sql ALONE decides the pushdown: IN / range / OR carry no
         * single key, so an empty pk_serialized_key must NOT disable it — it
         * only sends the delta side to the full (branch_id, relid) list (see
         * overlay_scan_open).  An empty pk_where_sql is still a hard
         * fallback (never run MAIN with a blank WHERE). */
        if (wstr != NULL && *wstr != '\0')
        {
            ebs->pk_where_sql = pstrdup(wstr);
            if (kstr != NULL && *kstr != '\0')
                ebs->pk_serialized_key = pstrdup(kstr);
        }
        else
        {
            ebs->pk_where_sql = NULL;
            ebs->pk_serialized_key = NULL;
            ebs->has_pk_pred = false;
        }
    }

    /* P2 general (non-PK) qual pushdown.  Any malformed element = fallback.
     * custom_private[5] is a String node; "" or NULL → skip pushdown. */
    ebs->has_general_where = false;
    ebs->general_where_sql = NULL;
    if (list_length(cpriv) >= 6)
    {
        int             gwflag;
        Node           *nde;
        const char     *gwstr;

        nde = list_nth(cpriv, 4);
        if (nde != NULL && IsA(nde, Integer))
            gwflag = intVal(nde);
        else
            gwflag = 0;
        ebs->has_general_where = (gwflag != 0);
        nde = list_nth(cpriv, 5);
        gwstr = (nde != NULL && IsA(nde, String)) ? strVal(nde) : NULL;
        if (ebs->has_general_where && gwstr != NULL && *gwstr != '\0')
            ebs->general_where_sql = pstrdup(gwstr);
        else
        {
            ebs->general_where_sql = NULL;
            ebs->has_general_where = false;
        }
        elog(DEBUG2, "P2 EXEC UNPACK: gwflag=%d general_where=%s",
             gwflag,
             ebs->general_where_sql ? ebs->general_where_sql : "(null)");
    }

    /* A1/A6: branch identity comes from the statement-level bind performed
     * in overlay_ExecutorStart, not from the live global. */
    {
        BranchExecContext *ectx = ob_get_exec_context();

        branch_id = (ectx != NULL) ? ectx->bid : overlay_branch_get_current_id();
    }

    /* A1/A6 Stage 3 — cached-plan branch identity contract.
     *
     * The plan records the branch id it was built for (custom_private[6]).
     * R07 already invalidates cached plans on every branch enter/exit, so a
     * plan is normally re-planned (and re-stamped) before it can be reused
     * under a different branch.  This assertion is the backstop for any path
     * that misses that invalidation: rather than silently returning another
     * branch's overlay (or MAIN's rows), refuse loudly.  A plan carrying no
     * stamp (older/lower element count) is skipped for compatibility. */
    if (list_length(cpriv) >= 7)
    {
        Node *nde = list_nth(cpriv, 6);

        if (nde != NULL && IsA(nde, Integer))
        {
            int         plan_bid = intVal(nde);

            if (plan_bid != (int) branch_id)
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("overlay_branch: cached plan branch identity mismatch "
                                "(plan built for branch %d, statement bound to branch %d)",
                                plan_bid, branch_id),
                         errhint("Re-plan the statement after switching branches; "
                                 "a cached plan must not be reused across a branch change.")));
        }
    }

    /* ----- LAZY OPEN -----
     * PG 17's ExecInitCustomScan calls BeginCustomScan unconditionally
     * even for plain EXPLAIN (without ANALYZE) because ExplainState
     * needs every plan node's state structure fully initialized.
     * If we called overlay_scan_open here, its SPI Pass1 SELECT
     * would re-enter the Planner hook and (despite the
     * B1.5 guard) could still cause stack overflow in some edge
     * cases.  The safe fix is *never* to do heavy work in
     * BeginCustomScan: just open the relation and hold the lock so
     * RelationGetDescr's TupleDesc remains valid (owned by relcache,
     * not the current Portal resource owner — avoids the "tupdesc
     * reference ... is not owned by resource owner Portal" error),
     * and defer the actual 2-pass overlay merge until the FIRST call
     * to ExecCustomScan (which EXPLAIN without ANALYZE never
     * executes). */
    rel = table_open(relid, AccessShareLock);

    /* NOTE: PG ExecInitCustomScan (nodeCustom.c L88) already called
     * ExecInitScanTupleSlot on node->ss using scan_rel's
     * RelationGetDescr, so node->ss.ss_ScanTupleSlot is already set
     * to a fully-initialized TTSOpsVirtual slot with the correct
     * tupdesc.  We MUST NOT overwrite it with ExecAllocTableSlot
     * here — double-init leaks resources and breaks ownership. */
    ebs->scan         = NULL;
    ebs->scan_cxt     = AllocSetContextCreate(estate->es_query_cxt,
                                              "overlay_branch BranchScan",
                                              ALLOCSET_DEFAULT_SIZES);
    ebs->rel          = rel;
    ebs->relid        = relid;
    ebs->branch_id    = branch_id;

    /* custom_ps (PG-owned field) = NIL: no child PlanState subtrees.
     * Never set custom_ps = private pointer; walker in
     * ExecShutdownNode/ExecEndNode treats it as List* and SIGSEGVs. */
    node->custom_ps = NIL;
}

static TupleTableSlot *
ob_branchscan_exec(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);
    TupleTableSlot     *dst = node->ss.ss_ScanTupleSlot;
    TupleTableSlot     *src;
    int                 natts;
    ExprContext        *econtext;

    econtext = node->ss.ps.ps_ExprContext;

    /* ----- First-tuple lazy open -----
     * Open the streaming iterator here, not in BeginCustomScan.
     * This guarantees that plain EXPLAIN (without ANALYZE) — which
     * calls BeginCustomScan but NEVER ExecCustomScan — cannot
     * trigger any SPI or Planner recursion. */
    if (ebs->scan == NULL)
        ebs->scan = overlay_scan_open(ebs->rel,
                                      ebs->branch_id,
                                      ebs->has_pk_pred,
                                      ebs->pk_where_sql,
                                      ebs->pk_serialized_key,
                                      ebs->has_general_where,
                                      ebs->general_where_sql,
                                      node->ss.ps.qual,
                                      node->ss.ps.ps_ExprContext,
                                      ebs->dml_target,
                                      ebs->scan_cxt);

next_tuple:
    /* Streaming: pull exactly ONE merged row per Exec call, so an upper
     * Limit / cursor can stop early without paying for the whole overlay. */
    src = overlay_scan_next(ebs->scan);
    if (src == NULL)
        return NULL;

    ExecClearTuple(dst);
    natts = dst->tts_tupleDescriptor->natts;

    if (ebs->dml_target)
    {
        /* ACC-1: heap-backed scan slot.  The DML subplan carries the row
         * identity as a ctid system-column Var; only heap-backed slots can
         * deliver system columns (tts_virtual_getsysattr ERRORS), so we form
         * a HeapTuple from src's fully-deformed attributes and carry the MAIN
         * anchor ctid (src->tts_tid) in t_self.  Handed over with
         * shouldFree=true, so at most one is live and the next
         * ExecClearTuple(dst) releases it.
         *
         * Allocated in es_query_cxt, NOT ebs->scan_cxt: this slot is
         * registered in estate->es_tupleTable and ExecResetTupleTable()
         * (from ExecEndPlan) clears it AFTER overlay_branchscan_end has
         * already MemoryContextDelete'd scan_cxt.  Allocating in scan_cxt
         * would leave the tuple dangling and turn that clear into a
         * double-free (BogusFree).  es_query_cxt outlives both. */
        MemoryContext oldcxt;
        HeapTuple     htup;

        slot_getallattrs(src);
        oldcxt = MemoryContextSwitchTo(node->ss.ps.state->es_query_cxt);
        htup = heap_form_tuple(dst->tts_tupleDescriptor,
                               src->tts_values, src->tts_isnull);
        MemoryContextSwitchTo(oldcxt);
        if (ItemPointerIsValid(&src->tts_tid))
            htup->t_self = src->tts_tid;
        ExecStoreHeapTuple(htup, dst, true);
        /* De-form the heap tuple into tts_values/tts_isnull.  The scan-level
         * qual below (and any projection column Var) was compiled against a
         * VIRTUAL scan slot, so ExecComputeSlotInfo() elided the
         * EEOP_SCAN_FETCHSOME step; without this the qual would read stale
         * tts_values and silently filter overlay rows out (ACC-1 G2). */
        slot_getallattrs(dst);
    }
    else
    {
        /* Copy src -> ss.ss_ScanTupleSlot.  dst is already initialized by
         * PG ExecInitCustomScan with the scan relation's physical tupdesc
         * (all columns).  We always copy ALL columns from src into dst
         * regardless of the query's requested projection because the
         * scan-level projection (ProjInfo) is applied below via
         * ExecProject() to produce the plan node's final output slot. */
        for (int a = 0; a < natts; a++)
        {
            bool                sisnull;
            Datum               sd;
            Form_pg_attribute   dstatt;

            if (a >= src->tts_tupleDescriptor->natts)
            {
                dst->tts_isnull[a] = true;
                continue;
            }
            sd = slot_getattr(src, (AttrNumber)(a + 1), &sisnull);
            dst->tts_isnull[a] = sisnull;
            if (!sisnull)
            {
                /* RD-3: use the attribute's own attlen/attbyval (kept in sync
                 * with pg_type) instead of a per-column get_typlenbyval()
                 * syscache round trip on every emitted row. */
                dstatt = TupleDescAttr(dst->tts_tupleDescriptor, a);
                sd = datumCopy(sd, dstatt->attbyval, dstatt->attlen);
                dst->tts_values[a] = sd;
            }
            else
                dst->tts_values[a] = (Datum) 0;
        }
        ExecStoreVirtualTuple(dst);
        dst->tts_nvalid = natts;
    }

    /* Reset per-tuple expr context so quals / projection have a clean
     * slate for this tuple. */
    ResetExprContext(econtext);
    econtext->ecxt_scantuple = dst;

    /* Run any scan-level qual (MVP: scan.plan.qual=NIL so quals live
     * in an upper Filter node, but check anyway for future proofing). */
    if (node->ss.ps.qual != NULL &&
        !ExecQual(node->ss.ps.qual, econtext))
        goto next_tuple;

    /* Apply projection (scan.plan.targetlist -> ps_ResultTupleSlot).
     * THIS STEP IS NON-OPTIONAL: the PlanState contract says
     * ExecProcNode() must return the *projected* result slot, not
     * the internal scan slot.  For SELECT * the projection collapses
     * to a trivial no-op (identity mapping), but for partial column
     * SELECT (id, color) / CASE WHEN expressions the projection
     * physically reshuffles columns / evaluates expressions into
     * the correct result tupdesc.  Skipping this step produces
     * "unsupported format code: 32638" or "attribute N has wrong
     * type" errors because printtup / upper nodes see the raw
     * 3-column scan slot where they expect the 2-column projected
     * output. */
    if (node->ss.ps.ps_ProjInfo != NULL)
        return ExecProject(node->ss.ps.ps_ProjInfo);
    return dst;
}

static void
ob_branchscan_end(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);

    /* Release the iterator's transient resources (heap scan descriptor, probe
     * slot, delta payload in TopMemoryContext) BEFORE dropping scan_cxt, where
     * the iterator state itself lives.  Idempotent for the plain-EXPLAIN case
     * where Begin ran but ExecCustomScan never opened anything. */
    if (ebs->scan != NULL)
    {
        overlay_scan_close(ebs->scan);
        ebs->scan = NULL;
    }

    /* The iterator state, its shared tupdesc, the pre-materialised P0 rows and
     * the per-row scratch context all live under scan_cxt (see
     * ob_branchscan_begin).  Dropping the context reclaims them in one shot —
     * no per-slot ExecDrop needed, and nothing under it may be touched
     * afterwards. */
    if (ebs->scan_cxt != NULL)
    {
        MemoryContextDelete(ebs->scan_cxt);
        ebs->scan_cxt = NULL;
    }

    /* P0 PK-IndexScan MVP: free the strings pstrdup'd in the Begin phase */
    if (ebs->pk_where_sql != NULL)
    {
        pfree(ebs->pk_where_sql);
        ebs->pk_where_sql = NULL;
    }
    if (ebs->pk_serialized_key != NULL)
    {
        pfree(ebs->pk_serialized_key);
        ebs->pk_serialized_key = NULL;
    }
    /* P2 general (non-PK) qual pushdown cleanup */
    if (ebs->general_where_sql != NULL)
    {
        pfree(ebs->general_where_sql);
        ebs->general_where_sql = NULL;
    }

    /* The relation was opened in Begin and is borrowed by the iterator; the
     * iterator has already released everything that referenced it. */
    if (ebs->rel != NULL)
        table_close(ebs->rel, AccessShareLock);
    /* ebs itself lives inside ExtendedCustomScanState allocated by
     * CreateCustomScanState (as part of the CustomScanState); PG's
     * ExecEndNode frees the PlanState subtree, so we must NOT
     * pfree(node) or pfree(ebs) ourselves — that would double-free.
     * Just NULL our per-scan owned pointers above; custom_ps remains
     * NIL as set in Begin. */
    node->custom_ps = NIL;
}

static void
ob_branchscan_rescan(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);

    (void) ob_invalidate_check_throttled(/*for_dml=*/ false);

    /* Rewind = discard the exhausted iterator and rebuild it on the next Exec:
     * overlay_scan_open() re-reads the delta list and restarts the MAIN heap
     * scan, so a fresh materialization happens lazily (FR6).  Releasing the
     * iterator first frees the heap scan descriptor, the probe slot and the
     * delta payload (TopMemoryContext); the iterator state, the P0 rows and the
     * per-row scratch die with the scan_cxt reset.  Do NOT ExecDrop anything the
     * iterator handed out — its memory is gone after the reset. */
    if (ebs->scan != NULL)
    {
        overlay_scan_close(ebs->scan);
        ebs->scan = NULL;
    }
    MemoryContextReset(ebs->scan_cxt);
}

/* ================================================================
 * Module entrypoint (called from overlay_branch.c::_PG_init())
 * ================================================================ */
void
branch_scan_init(void)
{
    prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
    set_rel_pathlist_hook     = ob_branchscan_planner_hook;
    RegisterCustomScanMethods(&ob_branchscan_scan_methods);
}
