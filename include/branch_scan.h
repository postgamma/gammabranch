#ifndef OVERLAY_BRANCH_BRANCH_SCAN_H
#define OVERLAY_BRANCH_BRANCH_SCAN_H

#include "postgres.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"

/* Entry point: called by _PG_init() to register planner hooks + CustomScanMethods */
extern void branch_scan_init(void);

/*
 * Streaming 2-pass overlay helper shared by
 * SRF overlay_main_plus_delta() and BranchScan ExecCustomScan().
 * The handle is opaque; ob_overlay_srf_next() returns ONE merged row per call
 * (TTSOpsVirtual, owned by result_cxt) and NULL once the scan is drained.
 *
 * result_cxt owns every allocation made for the returned slots (slots,
 * tupdescs, datum copies).  The CALLER picks a context whose lifetime
 * matches the result set:
 *   • BranchScan   → per-scan context (child of es_query_cxt)
 *   • SRF          → funcctx->multi_call_memory_ctx
 * NULL → CurrentMemoryContext.
 *
 * ob_overlay_srf_open() opens (and owns) the relation; the handle registers a
 * context reset callback, so ob_overlay_srf_close() is optional — releasing on
 * drain OR on context delete both work, and both are idempotent.  Callers must
 * NOT ExecDrop the emitted slots once result_cxt is destroyed. */
extern void *ob_overlay_srf_open(Oid relid, int32 branch_id,
								 MemoryContext result_cxt);
extern TupleTableSlot *ob_overlay_srf_next(void *handle);
extern void ob_overlay_srf_close(void *handle);

#endif
