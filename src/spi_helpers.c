/*-------------------------------------------------------------------------
 *
 * spi_helpers.c
 *	  Internal SPI execution helpers: one-shot execution plus a
 *	  parameterized prepared-plan cache (WR-1a).
 *
 *	ob_spi_one_shot() connects to SPI, runs the given statement text once,
 *	and leaves SPI connected for the caller.
 *
 *	The write path runs the same handful of SPI statements once per touched
 *	row.  ob_spi_one_shot() re-parses and re-plans every one of them, and the
 *	parse/plan cost dominates the per-row work.  ob_spi_exec_params() prepares
 *	each distinct statement text exactly once -- with CURSOR_OPT_GENERIC_PLAN
 *	so the plan is not re-derived per parameter set, and SPI_keepplan() so it
 *	lives for the backend lifetime -- keyed by the statement text.  Row data
 *	is always bound as $n parameters, never interpolated into the text, so a
 *	cached plan is completely independent of the row being processed.
 *
 *	Saved plans are revalidated/replanned automatically by the regular
 *	plancache invalidation machinery (relcache/syscache events, including the
 *	CacheInvalidateRelcacheAll() that use_branch()/apply/discard issue), so no
 *	extra invalidation hook is needed here.
 *
 *	Contract: both helpers SPI_connect() and leave SPI connected; the caller
 *	reads SPI_tuptable / SPI_processed and must SPI_finish().
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/spi.h"
#include "nodes/parsenodes.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "overlay_branch.h"

/*
 * Statement texts here are short (delta/MAIN point lookups with fully
 * qualified names), so a fixed-size key is simpler than a separately
 * allocated one.  HASH_STRINGS copies the key with strlcpy() (truncating at
 * keysize-1), and ob_plancache_get() rejects anything that would truncate.
 */
#define OB_PLANCACHE_KEYLEN		1024

typedef struct ObPlanCacheEntry
{
	char		sql[OB_PLANCACHE_KEYLEN];	/* hash key: statement text */
	SPIPlanPtr	plan;						/* saved (SPI_keepplan) plan */
} ObPlanCacheEntry;

static MemoryContext ob_plancache_cxt = NULL;
static HTAB *ob_plancache_hash = NULL;

/*
 * ob_plancache_init
 *
 *	Lazily create the plan-cache hash in a dedicated child of
 *	TopMemoryContext.  Never in _PG_init: that also runs in the postmaster
 *	when the library is preloaded, and a postmaster-owned hash inherited by
 *	every backend would be shared state (same rationale as the PK cache).
 */
static void
ob_plancache_init(void)
{
	HASHCTL		ctl;

	if (ob_plancache_hash != NULL)
		return;

	ob_plancache_cxt = AllocSetContextCreate(TopMemoryContext,
											 "overlay_branch plan cache",
											 ALLOCSET_SMALL_SIZES);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = OB_PLANCACHE_KEYLEN;
	ctl.entrysize = sizeof(ObPlanCacheEntry);
	ctl.hcxt = ob_plancache_cxt;
	ob_plancache_hash = hash_create("overlay_branch prepared plans",
									16, &ctl,
									HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
}

/*
 * ob_plancache_get
 *
 *	Return the saved plan for `sql`, preparing and saving it on first use.
 *	Requires SPI to be connected (SPI_prepare_cursor needs a proc context).
 */
static SPIPlanPtr
ob_plancache_get(const char *sql, int nargs, const Oid *argtypes)
{
	ObPlanCacheEntry *entry;
	bool		found;

	ob_plancache_init();

	if (strlen(sql) >= OB_PLANCACHE_KEYLEN)
		elog(ERROR, "overlay_branch: cached statement text too long (%zu bytes)",
			 strlen(sql));

	entry = (ObPlanCacheEntry *) hash_search(ob_plancache_hash, sql,
											 HASH_ENTER, &found);
	if (!found)
		entry->plan = NULL;

	if (entry->plan == NULL)
	{
		entry->plan = SPI_prepare_cursor(sql, nargs, (Oid *) argtypes,
										 CURSOR_OPT_GENERIC_PLAN);
		if (entry->plan == NULL)
			elog(ERROR,
				 "overlay_branch: SPI_prepare_cursor failed (SPI_result=%d): %s",
				 SPI_result, sql);
		if (SPI_keepplan(entry->plan) != 0)
			elog(ERROR, "overlay_branch: SPI_keepplan failed: %s", sql);
	}

	return entry->plan;
}

/*
 * ob_spi_exec_params
 *
 *	Parameterized equivalent of ob_spi_one_shot(): SPI_connect(), execute the
 *	cached plan for `sql` with the given $n values, and return the SPI result
 *	code.  SPI is left connected for the caller (reads SPI_tuptable /
 *	SPI_processed, then SPI_finish()).
 *
 *	Value/Nulls follow SPI_execute_plan() conventions: Nulls may be NULL when
 *	all parameters are non-NULL, otherwise one char per value (' ' / 'n').
 */
int
ob_spi_exec_params(const char *sql, bool read_only, uint64 tcount,
				   int nargs, const Oid *argtypes,
				   Datum *values, const char *nulls)
{
	SPIPlanPtr	plan;
	int			cret;

	cret = SPI_connect();
	if (cret != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect failed: %d", cret);

	plan = ob_plancache_get(sql, nargs, argtypes);

	return SPI_execute_plan(plan, values, nulls, read_only, (long) tcount);
}

/*
 * ob_spi_one_shot
 *
 *	SPI_connect(), execute `sql` once, and return the SPI result code.  SPI is
 *	left connected for the caller, exactly like ob_spi_exec_params().
 */
int
ob_spi_one_shot(const char *sql, bool read_only, uint64 tcount)
{
	int			cret;
	int			eret;

	cret = SPI_connect();
	if (cret != SPI_OK_CONNECT)
		elog(ERROR, "overlay_delta: SPI_connect failed: %d", cret);

	eret = SPI_execute(sql, read_only, tcount);
	return eret;
}
