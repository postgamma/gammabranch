#!/usr/bin/env bash
#
# run_agent_scale.sh -- multi-agent concurrency-scaling probe (L2, manual).
#
# Answers "how does the MAIN foreground OLTP degrade as the number of
# CONCURRENT independent Agents grows, and where is the knee?" by sweeping the
# Agent client count M over a list (default 1 2 4 8) while holding the MAIN
# load FIXED.
#
# Unlike run_mixed_impact.sh -- which places every Agent client in ONE shared
# branch -- each Agent client here has its OWN branch, so M Agents exercise
# CROSS-branch interference: pg_branch registry rows, per-branch advisory
# locks, the GUC mode cache and state-change NOTIFY.  (mixed_agent_own.sql.)
#
# ALL cases -- the A baseline plus B<M> for every M in the list -- run
# INTERLEAVED in one session and the case order is rotated as a Latin square
# over the rounds.  Sweeping M in ONE interleaved run (rather than one run per
# M) is what makes the M-to-M comparison immune to run-to-run drift: on a small
# or shared box a per-M sequential sweep would attribute machine drift to the M
# axis and manufacture a knee that is not there.
#
# This is a measurement, NOT a gate.  The G05 thresholds are still OPEN and
# must be frozen before use.  The
# absolute numbers are only meaningful relative to each other and to the box
# they were taken on (nproc/mem are logged); the probe looks for the SHAPE of
# the curve -- a superlinear latency knee or a CPU-saturation plateau -- not a
# capacity figure.  On a small/noisy box treat it as a "gross pathology" filter.
#
# usage: run_agent_scale.sh [MAIN_CLIENTS] [DURATION_S] "M_LIST"
#        defaults: 16 15 "1 2 4 8"
# env:   PGSCALE_REPEAT  (default 4)   interleaved repetitions (even => balanced)
#        PGSCALE_WARM    (default 5)   warm-up seconds before the first phase
#        PGSCALE_LEADIN  (default 2)   idle settle before every phase
#        PGSCALE_CONTROL (default 0)   1 => also sweep a no-branch control case
#                                      C<M> (same load, no use_branch) to
#                                      separate the overlay path from "just
#                                      more concurrent clients"
#        PGSCALE_CONTROL_SQL  control script (default mixed_agent_nobranch.sql)
#        PGTAG / PGPORT / PGDATABASE / PGUSER / PGJOBS
#
# Prerequisites: identical to run_mixed_impact.sh --
#   * PostgreSQL reachable on $PGPORT with the overlay_branch extension
#     installed and its .so in shared_preload_libraries.
#   * a clean measurement DB (default ob_bench) that permits create_branch.
#   * test/bench/mixed_impact_fixture.sql already applied.
#   * max_connections >= MAIN_CLIENTS + max(M) + 10.
#
set -euo pipefail

N="${1:-16}"                 # MAIN OLTP clients
T="${2:-15}"                 # measured seconds per phase
MS="${3:-1 2 4 8}"           # Agent client counts to sweep
R="${PGSCALE_REPEAT:-4}"     # interleaved repetitions (even => balanced order)
W="${PGSCALE_WARM:-5}"       # warm-up seconds before the first phase
LEADIN="${PGSCALE_LEADIN:-2}"
PORT="${PGPORT:-5432}"
DB="${PGDATABASE:-ob_bench}"
USER="${PGUSER:-$(id -un)}"
JOBS="${PGJOBS:-4}"
CONTROL="${PGSCALE_CONTROL:-0}"
# Unique per invocation.  A discarded branch name can never be reused, so the
# per-phase branch prefixes below must differ across runs; a bare $$ (pid) can
# be recycled, hence the timestamp suffix.
RUNID="$$$(date +%s)"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AGENT_SQL="${HERE}/mixed_agent_own.sql"
CONTROL_SQL="${PGSCALE_CONTROL_SQL:-${HERE}/mixed_agent_nobranch.sql}"

OUT="${HERE}/agent_scale_${N}_${T}_${MS// /-}${PGTAG:+_${PGTAG}}.log"

export PGPORT="${PORT}" PGDATABASE="${DB}" PGUSER="${USER}"
export LC_ALL=C.utf8

PSQL=(psql -p "${PORT}" -U "${USER}" -d "${DB}" -v ON_ERROR_STOP=1 -qAt)
RESULTS="$(mktemp)"

# --- helpers -------------------------------------------------------------

read_cpu() {  # -> "<total_jiffies> <idle_plus_iowait>"
    awk '/^cpu /{t=0; for(i=2;i<=NF;i++) t+=$i; print t, $5+$6}' /proc/stat
}

median() {  # read numbers on stdin -> median (2 dp)
    sort -n | awk '{a[NR]=$1}
        END {
            if (NR == 0) { print "NA"; exit }
            m = int((NR + 1) / 2)
            if (NR % 2 == 1) printf "%.2f", a[m]
            else             printf "%.2f", (a[m] + a[m+1]) / 2
        }'
}

pct() {  # pct <latency_file_us> <percentile 0..100> -> ms
    sort -n "$1" | awk -v p="$2" '
        {a[NR]=$1}
        END {
            if (NR == 0) { print "NA"; exit }
            i = int(p / 100.0 * NR); if (i < 1) i = 1; if (i > NR) i = NR
            printf "%.2f", a[i] / 1000.0
        }'
}

lat_file() {  # lat_file <logPrefix> -> path to a temp file of latencies (us)
    local f; f="$(mktemp)"
    cat "$1"* 2>/dev/null | awk 'NF >= 3 {print $3}' > "$f"
    echo "$f"
}

col() {  # col <tag> <field>  (fields: 2=M 3=tps 4=p50 5=p95 6=p99 7=cpu 8=ap99 9=atps)
    awk -v t="$1" -v c="$2" '$1 == t {print $c}' "${RESULTS}"
}

case_M() {  # case_M <tag> -> Agent client count (A => 0)
    local tag="$1"
    if [ "${tag}" = "A" ]; then printf '0'; else printf '%s' "${tag#?}"; fi
}

case_script() {  # case_script <tag> -> agent sql script ("" for the baseline)
    case "${1:0:1}" in
        A) printf '' ;;
        B) printf '%s' "${AGENT_SQL}" ;;
        C) printf '%s' "${CONTROL_SQL}" ;;
    esac
}

# --- preflight -----------------------------------------------------------

"${PSQL[@]}" -c "SELECT 1" >/dev/null
MAXCONN="$("${PSQL[@]}" -c "SHOW max_connections")"
maxM=0
for m in ${MS}; do if [ "${m}" -gt "${maxM}" ]; then maxM="${m}"; fi; done
NEED=$(( N + maxM + 10 ))
if [ "${MAXCONN}" -lt "${NEED}" ]; then
    echo "FATAL: max_connections=${MAXCONN} < needed ${NEED} (N+max(M)+10)." >&2
    echo "       psql -c \"ALTER SYSTEM SET max_connections=${NEED};\" && pg_ctl restart" >&2
    exit 1
fi

CASES=(A)
for m in ${MS}; do CASES+=("B${m}"); done
if [ "${CONTROL}" = 1 ]; then for m in ${MS}; do CASES+=("C${m}"); done; fi
NCASES=${#CASES[@]}

run_phase() {  # run_phase <tag>
    local tag="$1"
    local m; m="$(case_M "${tag}")"
    local scr; scr="$(case_script "${tag}")"
    local uses_br=0
    case "${tag:0:1}" in B) uses_br=1 ;; esac
    local nc="${N}"
    local secs="${T}"
    # Unique per (run, round, case): a discarded branch name can never be
    # reused, so each Agent phase must create fresh, empty per-client branches.
    local prefix="oas_${RUNID}_${RUN_I}_${tag}_"
    local mlog alog agout apid="" d_total d_idle cpu_busy
    local tps p50 p95 p99 ap99="NA" atps="NA"

    mlog="$(mktemp)"; alog="$(mktemp)"; agout="$(mktemp)"

    # Fresh empty delta per Agent-BRANCH phase; created BEFORE the measured
    # window.  The control case C uses the same query load but never calls
    # use_branch, so it needs no branches.
    if [ "${uses_br}" = 1 ] && [ "${m}" -gt 0 ]; then
        "${PSQL[@]}" -c "SELECT overlay_branch.create_branch('${prefix}' || g) \
                         FROM generate_series(0, $(( m - 1 ))) AS g;" >/dev/null
    fi

    # Identical idle settle for EVERY phase, with nothing running yet.
    sleep "${LEADIN}"
    local c0; c0="$(read_cpu)"

    if [ -n "${scr}" ] && [ "${m}" -gt 0 ]; then
        pgbench -n -c "${m}" -j 1 -T "${secs}" -M prepared \
                --log --log-prefix="${alog}" -D brpfx="${prefix}" \
                -f "${scr}" >"${agout}" 2>&1 &
        apid=$!
    fi

    local main_out
    main_out="$(pgbench -n -c "${nc}" -j "${JOBS}" -T "${secs}" -M prepared \
                --log --log-prefix="${mlog}" \
                -f "${HERE}/mixed_main_oltp.sql" 2>&1)"
    if [ -n "${apid}" ]; then wait "${apid}" || true; fi

    local c1; c1="$(read_cpu)"
    d_total=$(( $(echo "$c1" | cut -d' ' -f1) - $(echo "$c0" | cut -d' ' -f1) ))
    d_idle=$((  $(echo "$c1" | cut -d' ' -f2) - $(echo "$c0" | cut -d' ' -f2) ))
    cpu_busy="$(awk -v t="${d_total}" -v i="${d_idle}" 'BEGIN{ if(t>0) printf "%.1f", 100*(t-i)/t; else print "NA" }')"

    local mlat_f alat_f
    mlat_f="$(lat_file "${mlog}")"
    alat_f="$(lat_file "${alog}")"

    tps="$(echo "${main_out}" | awk -F'= ' '/^tps =/{gsub(/[^0-9.]/,"",$2); print $2; exit}')"
    p50="$(pct "${mlat_f}" 50)"
    p95="$(pct "${mlat_f}" 95)"
    p99="$(pct "${mlat_f}" 99)"
    if [ "${m}" -gt 0 ]; then
        ap99="$(pct "${alat_f}" 99)"
        atps="$(echo "$(cat "${agout}")" | awk -F'= ' '/^tps =/{gsub(/[^0-9.]/,"",$2); print $2; exit}')"
        [ -n "${atps}" ] || atps="NA"
    fi

    echo "---- Phase ${tag} #${RUN_I} (MAIN=${nc} AGENT=${m} ${secs}s) ----"
    echo "${main_out}" | grep -E 'tps =|latency average' | sed 's/^/  pgbench MAIN : /'
    echo "  MAIN latency ms : p50=${p50}  p95=${p95}  p99=${p99}"
    echo "  CPU busy %      : ${cpu_busy}"
    if [ "${m}" -gt 0 ]; then
        echo "  AGENT           : p99=${ap99} ms   tps=${atps}"
    fi
    echo

    printf '%s %s %s %s %s %s %s %s %s\n' \
        "${tag}" "${m}" "${tps}" "${p50}" "${p95}" "${p99}" "${cpu_busy}" "${ap99}" "${atps}" \
        >> "${RESULTS}"

    rm -f "${mlat_f}" "${alat_f}"
}

# --- run -----------------------------------------------------------------

{
echo "==== multi-agent concurrency-scaling probe (G05/S28) ===="
echo "date        : $(date -Is)"
echo "db          : ${DB}   port=${PORT}   host=$(hostname)"
echo "cpu         : $(nproc) vCPU   mem: $(awk '/MemTotal/{printf "%.1f GB", $2/1048576}' /proc/meminfo)"
echo "MAIN_CLIENTS=${N}   DURATION=${T}s   M_LIST=[${MS}]   REPEAT=${R}   WARMUP=${W}s   CONTROL=${CONTROL}"
echo "max_conn    : ${MAXCONN}"
echo "topology    : one INDEPENDENT branch per Agent client (cross-branch)"
echo "agent sql   : ${AGENT_SQL}"
if [ "${CONTROL}" = 1 ]; then echo "control sql : ${CONTROL_SQL}"; fi
echo

echo "warm-up: MAIN ${N} x ${W}s ..."; echo
pgbench -n -c "${N}" -j "${JOBS}" -T "${W}" -M prepared \
        -f "${HERE}/mixed_main_oltp.sql" >/dev/null 2>&1

# Latin-square phase rotation across ALL cases.
for RUN_I in $(seq 1 "${R}"); do
    OFF=$(( (RUN_I - 1) % NCASES ))
    for i in $(seq 0 $(( NCASES - 1 ))); do
        run_phase "${CASES[$(( (i + OFF) % NCASES ))]}"
    done
done

# Cleanup: discard every branch this run created (agent branches stay active).
"${PSQL[@]}" -c "SELECT overlay_branch.discard_branch(branch_name) \
                 FROM overlay_branch.pg_branch \
                 WHERE branch_name LIKE 'oas_${RUNID}\_%' AND state = 'active';" \
    >/dev/null 2>&1 || true

# --- summary (medians over the interleaved repetitions) ------------------

atps="$(col A 3 | median)"
ap50="$(col A 4 | median)"
ap95="$(col A 5 | median)"
ap99="$(col A 6 | median)"
acpu="$(col A 7 | median)"

echo "==== SUMMARY (medians over ${R} interleaved rounds) ===="
echo "  M = concurrent INDEPENDENT branch Agents;  A = M=0 baseline"
echo
printf '  %-6s %11s %9s %9s %9s %9s %12s %9s %10s\n' \
       "case" "MAIN tps" "dTps%" "p50 ms" "p95 ms" "p99 ms" "Agent p99 ms" "CPU%" "Agent tps"
printf '  %-6s %11s %9s %9s %9s %9s %12s %9s %10s\n' \
       "A" "${atps}" "-" "${ap50}" "${ap95}" "${ap99}" "-" "${acpu}" "-"

for tag in "${CASES[@]:1}"; do
    if [ "${tag}" = "A" ]; then continue; fi
    mtps="$(col "${tag}" 3 | median)"
    mp50="$(col "${tag}" 4 | median)"
    mp95="$(col "${tag}" 5 | median)"
    mp99="$(col "${tag}" 6 | median)"
    mcpu="$(col "${tag}" 7 | median)"
    map99="$(col "${tag}" 8 | median)"
    matps="$(col "${tag}" 9 | median)"
    dtps="$(awk -v a="${atps}" -v b="${mtps}" 'BEGIN{ if(a>0) printf "%+.2f", 100*(b-a)/a; else print "NA" }')"
    printf '  %-6s %11s %9s %9s %9s %9s %12s %9s %10s\n' \
           "${tag}" "${mtps}" "${dtps}" "${mp50}" "${mp95}" "${mp99}" "${map99}" "${mcpu}" "${matps}"
done

echo
echo "  NOTE: no pass/fail here -- G05 thresholds are still OPEN and must be"
echo "        frozen before use.  Read the SHAPE: watch for a"
echo "        superlinear p99 knee, a dTps% that turns sharply negative, or a"
echo "        CPU% plateau -- not the absolute numbers."
} 2>&1 | tee "${OUT}"

echo
echo "log -> ${OUT}"
rm -f "${RESULTS}"
