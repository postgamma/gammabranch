#!/usr/bin/env bash
#
# run_perf_baseline.sh -- driver for test/bench/perf_baseline.sql.
#
# Produces a reproducible wall-clock baseline for the cost centers tracked in
# doc/internals.md §5.  Tune the two size knobs (N, M) to explore scaling:
#
#     PGOPTIONS= ./run_perf_baseline.sh 20000   2000    # quick smoke
#     PGOPTIONS= ./run_perf_baseline.sh 200000  20000   # default shape
#     PGOPTIONS= ./run_perf_baseline.sh 1000000 50000   # large
#
# Exit status is the psql exit status; the full transcript is tee'd to
# perf_baseline_<N>_<M>.log next to this script.
#
set -euo pipefail

N="${1:-200000}"
M="${2:-20000}"
PORT="${PGPORT:-5432}"
DB="${PGDATABASE:-postgres}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${HERE}/perf_baseline_${N}_${M}.log"

# A real installed UTF-8 locale is required by initdb-class tooling and keeps
# multibyte width measurements honest (see AGENTS.md section 3).
export LC_ALL=C.utf8

echo "perf baseline: N=${N} M=${M} port=${PORT} db=${DB}"
echo "log -> ${OUT}"

psql -p "${PORT}" -d "${DB}" \
     -v n="${N}" -v m="${M}" \
     -f "${HERE}/perf_baseline.sql" 2>&1 | tee "${OUT}"
