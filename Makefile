
MODULE_big = overlay_branch
OBJS = \
	$(WIN32RES) \
	src/overlay_branch.o \
	src/branch_scan.o \
	src/delta_store.o \
	src/write_redirect.o \
	src/branch_lifecycle.o \
	src/spi_helpers.o

EXTENSION = overlay_branch
DATA = overlay_branch--0.1.sql
PGFILEDESC = "overlay_branch - speculative database state using table overlay and delta store"

# Header files live in include/
override CPPFLAGS += -I$(srcdir)/include

# Directory layout for test/ (clean separation of 3 test tiers):
#   test/regress/{sql,expected}   L3 pg_regress (make installcheck gate)
#   test/isolation/specs          L1 pg_isolation_regress (T8 concurrency)
#   test/isolation/expected       L1 baseline .out files
#   test/bench/                   L2 pgbench manual race-detection fixture
# Temp instance shared-libs conf lives at root level (shared by both harnesses).
REGRESS = \
  overlay_branch_basic \
  overlay_branch_user \
  overlay_branch_scan_pk \
  overlay_branch_scan_type \
  overlay_branch_puredelta_mix \
  overlay_branch_upsert \
  overlay_branch_puredelta \
  overlay_branch_mvcc_usage \
  overlay_branch_mvcc_bounds \
  overlay_branch_mvcc_review \
  overlay_branch_rentry \
  overlay_branch_view_unify \
  overlay_branch_planner_cost \
  overlay_branch_keyless \
  overlay_branch_parallel \
  overlay_branch_memgrow \
  overlay_branch_pushdown \
  overlay_branch_plancache \
  overlay_branch_acc1 \
  overlay_branch_execcontext \
  overlay_branch_deltastats
REGRESS_OPTS = \
	--inputdir=$(srcdir)/test/regress \
	--outputdir=./test_output \
	--temp-config=$(srcdir)/temp_instance_shared_libs.conf
ISOLATION_OPTS = \
	--inputdir=$(srcdir)/test/isolation \
	--temp-config=$(srcdir)/temp_instance_shared_libs.conf

# L1 pg_isolation_regress concurrency tests (T9 total acceptance).
ISOLATION = ob_apply_mutex ob_state_inval ob_applying_freeze ob_snapshot_mode ob_branchscan_rescan pure_delta_upsert pure_delta_update pure_delta_delete ob_view_unify_rescan keyless_basic keyless_pure_delta_repeat keyless_apply_conflict keyless_version_chain keyless_upsert keyless_delete_invalidation
EXTRA_CLEAN += test_output test_output_iso output_iso output_l3 tmp_check tmp_check_iso

# NOTE on test entry points:
#   Standalone (PGXS) build: `make check` is NOT supported by PGXS; run
#   `make install` first, then `make installcheck` against a running server
#   that has shared_preload_libraries='overlay_branch' (see README.md).

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
