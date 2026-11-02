# Copyright (c) 2024, PostgreSQL Global Development Group

overlay_branch_sources = files(
  'src/branch_lifecycle.c',
  'src/branch_scan.c',
  'src/delta_store.c',
  'src/overlay_branch.c',
  'src/spi_helpers.c',
  'src/write_redirect.c',
)

if host_system == 'windows'
  overlay_branch_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'overlay_branch',
    '--FILEDESC', 'overlay_branch - speculative database state using table overlay and delta store',])
endif

overlay_branch_inc = include_directories('include')

overlay_branch = shared_module('overlay_branch',
  overlay_branch_sources,
  include_directories: overlay_branch_inc,
  kwargs: contrib_mod_args,
)
contrib_targets += overlay_branch

install_data(
  'overlay_branch--0.1.sql',
  'overlay_branch.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'overlay_branch',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'overlay_branch_basic',
      'overlay_branch_user',
      'overlay_branch_scan_pk',
      'overlay_branch_scan_type',
      'overlay_branch_puredelta_mix',
      'overlay_branch_upsert',
      'overlay_branch_puredelta',
      'overlay_branch_mvcc_usage',
      'overlay_branch_mvcc_bounds',
      'overlay_branch_mvcc_review',
      'overlay_branch_rentry',
      'overlay_branch_view_unify',
      'overlay_branch_planner_cost',
      'overlay_branch_keyless',
      'overlay_branch_parallel',
      'overlay_branch_memgrow',
      'overlay_branch_pushdown',
      'overlay_branch_plancache',
      'overlay_branch_acc1',
      'overlay_branch_execcontext',
    ],
    'inputdir': meson.current_source_dir() / 'test' / 'regress',
    'expecteddir': meson.current_source_dir() / 'test' / 'regress',
    'regress_args': ['--temp-config', files('temp_instance_shared_libs.conf')],
    # Disabled for "runningcheck" because these tests require
    # "shared_preload_libraries=overlay_branch", which typical runningcheck
    # users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
  'isolation': {
    'specs': [
      'ob_apply_mutex',
      'ob_state_inval',
      'ob_applying_freeze',
      'ob_snapshot_mode',
      'ob_branchscan_rescan',
      'pure_delta_upsert',
      'pure_delta_update',
      'pure_delta_delete',
      'ob_view_unify_rescan',
      'keyless_basic',
      'keyless_pure_delta_repeat',
      'keyless_apply_conflict',
    ],
    'inputdir': meson.current_source_dir() / 'test' / 'isolation',
    'expecteddir': meson.current_source_dir() / 'test' / 'isolation',
    'regress_args': ['--temp-config', files('temp_instance_shared_libs.conf')],
    # See the regress entry above.
    'runningcheck': false,
  },
}
