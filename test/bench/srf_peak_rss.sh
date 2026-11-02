#!/usr/bin/env bash
#
# srf_peak_rss.sh -- L2 (non-gating) probe for MF-1 leak 9.
#
# Regression it guards
#   overlay_main_plus_delta(), the FROM-clause SRF that materialises MAIN
#   (plus) delta, used to allocate its per-row scratch -- the values[] and
#   isnulls[] arrays of every returned row -- in the SRF's multi-call memory
#   context, a context PostgreSQL deletes only when the SRF finishes.  A
#   full-table scan therefore grew the backend by one unremovable chunk per
#   returned row.  Unlike every other MF-1 leak it is invisible to the
#   TopMemoryContext before/after probes in test/regress, because those can
#   only observe allocations that outlive the statement; the scratch is gone
#   by the time the statement ends.  The process footprint during the scan is
#   the only observable, which is why this probe lives in test/bench and is
#   NOT part of `make check` (see README.md).
#
# Method (why two scans)
#   A single scan is not a usable signal.  Even with the leak fixed, the first
#   full-table scan of a fresh backend raises the task's high-water mark by
#   O(rows): the executor's per-tuple context, the SRF's tuplestore and the
#   underlying heap pages churn through the allocator, and glibc keeps the
#   freed pages in the process heap (VmRSS stays high long after the statement
#   returns) rather than handing them back to the OS.  That warms the heap
#   once; it is bounded, not a leak.
#
#   A leak has a different signature: every scan allocates scratch that is
#   *never* reclaimed, so the footprint grows by the same amount on *every*
#   scan.  So the probe runs the scan twice and fails only if the *second*
#   scan -- by which time the heap is warm and nothing should be added --
#   grows the high-water mark by more than <max_growth_kb>.  With the leak a
#   300k-row scan adds ~80 MB per repetition; fixed, the second scan adds
#   well under 1 MB.
#
# Method (mechanics)
#   1. build a fixture table of <rows> rows plus a branch over it;
#   2. hold one backend open over a FIFO, so /proc/<pid>/status can be read
#      while the session is still alive (VmHWM, the peak RSS, is monotonic,
#      so the timing of the read does not matter as long as the backend
#      lives);
#   3. run one full-table overlay_main_plus_delta() scan to warm the heap,
#      waiting for the backend to go idle again (polled through
#      pg_stat_activity, so no dependence on client-side output buffering);
#   4. run the scan a second time and compare the high-water mark;
#   5. fail (exit 1) if the second scan grew it by more than <max_growth_kb>.
#
# Usage
#   ./srf_peak_rss.sh [rows] [max_growth_kb]     (default 300000, 16384)
#
# Environment
#   Standard libpq variables (PGHOST/PGPORT/PGDATABASE/PGUSER) must point at
#   a server that has the overlay_branch extension installed in the target
#   database.  PSQL overrides the psql binary (default: psql from PATH).
#
set -euo pipefail

ROWS="${1:-300000}"
MAX_GROWTH_KB="${2:-16384}"

PSQL="${PSQL:-psql}"
# Unique names: a discarded branch keeps its registry row and its name can
# never be reused, so a fixed name breaks every re-run.  A crashed run may
# leave one inert registry row behind; that is harmless.
TBL="ob_srf_rss_t_$$"
BR="ob_srf_rss_b_$$"

export LC_ALL=C.utf8

TMPD="${TMPDIR:-/tmp}"
FIFO="${TMPD}/ob_srf_rss.$$.fifo"
LOG="${TMPD}/ob_srf_rss.$$.log"
FD_OPEN=0
BPID=""

cleanup() {
	if [ "${FD_OPEN}" = 1 ]; then exec 3>&- || true; fi
	wait 2>/dev/null || true
	rm -f "${FIFO}" "${LOG}"
}
trap cleanup EXIT

echo "srf_peak_rss: rows=${ROWS} max_growth_kb=${MAX_GROWTH_KB} (second scan)"

# ---- 1. fixture -----------------------------------------------------------
"${PSQL}" -X -q -A -t -v ON_ERROR_STOP=1 <<SQL
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS overlay_branch;
DROP TABLE IF EXISTS ${TBL};
CREATE TABLE ${TBL}(id int PRIMARY KEY, v text);
INSERT INTO ${TBL}
  SELECT g, repeat('x', 40) FROM generate_series(1, ${ROWS}) g;
ANALYZE ${TBL};
SELECT overlay_branch.create_branch('${BR}');
SQL

# ---- 2. long-lived backend driven through a FIFO ---------------------------
mkfifo "${FIFO}"
PGAPPNAME=ob_srf_rss stdbuf -oL -eL "${PSQL}" -X -q -A -t \
	< "${FIFO}" > "${LOG}" 2>&1 &
exec 3> "${FIFO}"
FD_OPEN=1

# Locate the probe backend (ignore the transient polling connections).
for _ in $(seq 1 100); do
	BPID="$("${PSQL}" -X -A -t -q -c \
		"SELECT pid FROM pg_stat_activity
		  WHERE application_name = 'ob_srf_rss'
		    AND pid <> pg_backend_pid()")"
	[ -n "${BPID}" ] && break
	sleep 0.1
done
if [ -z "${BPID}" ]; then
	echo "FAIL: could not identify the probe backend" >&2
	exit 1
fi
echo "probe backend pid=${BPID}"
echo "fixture: table=${TBL} branch=${BR}"

bg_state() {
	"${PSQL}" -X -A -t -q -c \
		"SELECT state FROM pg_stat_activity WHERE pid = ${BPID}"
}

# wait_idle <tenths>: block until the probe backend is idle again.
wait_idle() {
	local tries="$1" i=0
	while [ "${i}" -lt "${tries}" ]; do
		if [ "$(bg_state)" = "idle" ]; then return 0; fi
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

peak() { awk '/^VmHWM:/ { print $2 }' "/proc/${BPID}/status"; }

echo "SELECT overlay_branch.use_branch('${BR}');" >&3
wait_idle 100 || true

BEFORE="$(peak)"
echo "baseline VmHWM = ${BEFORE} kB"

SCAN="SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('${TBL}') AS r(id int, v text);"

# ---- 3a. warm-up scan (bounded; raises the high-water mark once) -----------
echo "${SCAN}" >&3
if ! wait_idle 6000; then
	echo "warn: warm-up scan did not finish within 600s" >&2
fi
WARM="$(peak)"
echo "after warm-up scan VmHWM = ${WARM} kB (grew $(( WARM - BEFORE )) kB)"

# ---- 3b. measured scan: must not grow the high-water mark ------------------
echo "${SCAN}" >&3
if ! wait_idle 6000; then
	echo "warn: measured scan did not finish within 600s" >&2
fi
AFTER="$(peak)"
echo "after measured scan VmHWM = ${AFTER} kB"
GROWTH=$(( AFTER - WARM ))
echo "second-scan growth = ${GROWTH} kB (budget ${MAX_GROWTH_KB} kB)"

echo "--- probe transcript ---"
cat "${LOG}" 2>/dev/null || true

if [ "$(grep -cx "${ROWS}" "${LOG}" 2>/dev/null || echo 0)" -lt 2 ]; then
	echo "FAIL: both scans did not return ${ROWS} rows (see transcript above)" >&2
	RESULT=1
elif [ "${GROWTH}" -le "${MAX_GROWTH_KB}" ]; then
	echo "SRF_PEAK_RSS_PASS"
	RESULT=0
else
	echo "SRF_PEAK_RSS_FAIL: second scan grew=${GROWTH} kB" >&2
	RESULT=1
fi

# ---- 4. cleanup ------------------------------------------------------------
echo "SELECT overlay_branch.use_branch(NULL);" >&3
echo "SELECT overlay_branch.discard_branch('${BR}');" >&3
echo "DROP TABLE IF EXISTS ${TBL};" >&3
wait_idle 100 || true

exit "${RESULT}"
