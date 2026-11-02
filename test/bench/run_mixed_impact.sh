#!/usr/bin/env bash
#
# run_mixed_impact.sh -- G05/S28 mixed foreground-impact baseline (L2, manual).
#
# Measures how much a co-located Agent branch session degrades the MAIN
# foreground OLTP workload on the same instance and the same configuration:
#
#   Phase A : N MAIN OLTP clients, no Agent           -> tps / p50 / p95 / p99
#   Phase B : N MAIN OLTP clients + M Agent clients   -> tps / p50 / p95 / p99
#
# The cases are interleaved R times and reported as the median of each column,
# because a single A/B pair is dominated by run-to-run variance on a shared
# box.  The case order is rotated as a Latin square over the rounds, so each
# case occupies each position (first/second/...) equally often -- otherwise a
# "the phase that runs later is faster" bias is indistinguishable from a real
# Agent impact.
#
# Set PGEXTRASQL to also run a third case C interleaved with A and B.  The
# recommended control (mixed_agent_nobranch.sql) keeps the Agent's exact query
# load but does NOT enter the branch, which separates "Agent overlay path"
# from "merely one more concurrent client".
#
# This is a measurement, NOT a gate: the G05 thresholds are still OPEN and
# must be frozen before use.
#
# usage: run_mixed_impact.sh [MAIN_CLIENTS] [DURATION_S] [AGENT_CLIENTS]
#        defaults: 100 30 1
# env:   PGREPEAT  (default 4)   interleaved repetitions (an EVEN value
#                                balances each case across positions)
#        PGWARM    (default 5)   warm-up seconds before the first phase
#        PGLEADIN  (default 2)   idle settle before every phase
#        PGAGENTSQL             Agent script (default mixed_agent.sql)
#        PGEXTRASQL             optional control script -> adds case C
#        PGTAG                  suffix for the output log name
#        PGBRANCH / PGPORT / PGDATABASE / PGUSER / PGJOBS
#
# Prerequisites:
#   * PostgreSQL reachable on $PGPORT (default 5432) with the overlay_branch
#     extension installed and its .so loaded (shared_preload_libraries).
#   * A clean measurement DB (default ob_bench) that permits create_branch
#     (no views / FDW / partitioned rels in public -- G01 scope preflight).
#   * max_connections >= MAIN_CLIENTS + AGENT_CLIENTS + 10.
#   * test/bench/mixed_impact_fixture.sql already applied.
#
set -euo pipefail

N="${1:-100}"     # MAIN OLTP clients
T="${2:-30}"      # measured seconds per phase
M="${3:-1}"       # Agent branch clients
R="${PGREPEAT:-4}"  # interleaved repetitions (even => balanced phase order)
W="${PGWARM:-5}"    # warm-up seconds before the first phase

PORT="${PGPORT:-5432}"
DB="${PGDATABASE:-ob_bench}"
USER="${PGUSER:-$(id -un)}"
JOBS="${PGJOBS:-4}"
# create_branch() rejects any pre-existing row with the same name (regardless of
# state), so the Agent branch must be unique per run.
BR="${PGBRANCH:-mi_impact_$$}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${HERE}/mixed_impact_${N}_${T}_${M}${PGTAG:+_${PGTAG}}.log"

# Agent workload script.  Override with PGAGENTSQL to run a CONTROL phase that
# keeps the same query load but does NOT enter the branch (see
# mixed_agent_nobranch.sql); this separates "Agent overlay path" from "merely
# one more concurrent client".
AGENT_SQL="${PGAGENTSQL:-${HERE}/mixed_agent.sql}"

# Optional third case, run interleaved as Phase C.  With PGEXTRASQL set (e.g.
# to mixed_agent_nobranch.sql) the driver rotates the phase order as a Latin
# square over A/B/C, so each case occupies each position equally often and the
# cross-case comparison is immune to both position and within-run drift.
EXTRA_SQL="${PGEXTRASQL:-}"

# Idle settle before every phase, with nothing running yet (see run_phase).
LEADIN="${PGLEADIN:-2}"

# pgbench picks its target up from the libpq environment, while psql is given
# explicit -d/-p/-U flags below.  Export them so both tools always agree.
export PGPORT="${PORT}" PGDATABASE="${DB}" PGUSER="${USER}"

# A real installed UTF-8 locale is required by initdb-class tooling
# (see AGENTS.md section 3).
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

col() {  # col <A|B|C> <field>  (fields: 3=tps 4=p50 5=p95 6=p99 7=cpu 8=agent_p99)
    awk -v t="$1" -v c="$2" '$1 == t {print $c}' "${RESULTS}"
}

case_script() {  # case_script <A|B|C> -> agent sql script ("" = MAIN only)
    case "$1" in
        A) printf '' ;;
        B) printf '%s' "${AGENT_SQL}" ;;
        C) printf '%s' "${EXTRA_SQL}" ;;
    esac
}

# --- preflight -----------------------------------------------------------

"${PSQL[@]}" -c "SELECT 1" >/dev/null
MAXCONN="$("${PSQL[@]}" -c "SHOW max_connections")"
NEED=$(( N + M + 10 ))
if [ "${MAXCONN}" -lt "${NEED}" ]; then
    echo "FATAL: max_connections=${MAXCONN} < needed ${NEED} (N+M+10)." >&2
    echo "       Raise it and restart, e.g.:" >&2
    echo "         psql -c \"ALTER SYSTEM SET max_connections=${NEED};\" && pg_ctl restart" >&2
    exit 1
fi

run_phase() {  # run_phase <A|B|C>
    local tag="$1"
    local nc="${N}" scr na
    scr="$(case_script "${tag}")"
    if [ -n "${scr}" ]; then na="${M}"; else na=0; fi
    local secs="${T}"
    local mlog alog apid="" d_total d_idle cpu_busy
    local tps p50 p95 p99 ap99="NA"

    mlog="$(mktemp)"; alog="$(mktemp)"

    # Identical idle settle for EVERY phase, with nothing running yet.  The
    # settle must not overlap the Agent: letting the Agent run during the
    # lead-in (as an earlier revision did) pre-warms CPU/pages for that phase
    # only and biases the comparison.
    sleep "${LEADIN}"
    local c0; c0="$(read_cpu)"

    if [ -n "${scr}" ] && [ "${na}" -gt 0 ]; then
        pgbench -n -c "${na}" -j 1 -T "${secs}" -M prepared \
                --log --log-prefix="${alog}" -D brname="${BR}" \
                -f "${scr}" >/dev/null 2>&1 &
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
    if [ "${na}" -gt 0 ]; then ap99="$(pct "${alat_f}" 99)"; fi

    echo "---- Phase ${tag} #${RUN_I} (MAIN=${nc} AGENT=${na} ${secs}s) ----"
    echo "${main_out}" | grep -E 'tps =|latency average' | sed 's/^/  pgbench: /'
    echo "  MAIN latency ms : p50=${p50}  p95=${p95}  p99=${p99}"
    echo "  CPU busy %      : ${cpu_busy}"
    if [ "${na}" -gt 0 ]; then echo "  AGENT p99 ms    : ${ap99}"; fi
    echo

    printf '%s %d %s %s %s %s %s %s\n' \
        "${tag}" "${nc}" "${tps}" "${p50}" "${p95}" "${p99}" "${cpu_busy}" "${ap99}" \
        >> "${RESULTS}"

    rm -f "${mlat_f}" "${alat_f}"
}

# --- run -----------------------------------------------------------------

{
echo "==== mixed foreground-impact baseline (G05/S28) ===="
echo "date        : $(date -Is)"
echo "db          : ${DB}   port=${PORT}   host=$(hostname)"
echo "cpucores    : $(nproc)   MAIN_CLIENTS=${N}  AGENT_CLIENTS=${M}  DURATION=${T}s  REPEAT=${R}  WARMUP=${W}s"
echo "max_conn    : ${MAXCONN}"
echo

"${PSQL[@]}" -c "SELECT overlay_branch.create_branch('${BR}');" >/dev/null
echo "agent branch : ${BR} (created fresh)"
echo "agent sql    : ${AGENT_SQL}"
if [ -n "${EXTRA_SQL}" ]; then echo "control sql  : ${EXTRA_SQL}"; fi
echo

echo "warm-up: MAIN ${N} x ${W}s ..."; echo
pgbench -n -c "${N}" -j "${JOBS}" -T "${W}" -M prepared \
        -f "${HERE}/mixed_main_oltp.sql" >/dev/null 2>&1

# Rotate the phase order as a Latin square over the cases, so that each case
# occupies each position equally often.  This makes the cross-case comparison
# immune to both the "second phase is faster" position effect and within-run
# drift; without it a mere ordering bias is indistinguishable from a real
# Agent impact.
CASES=(A B)
if [ -n "${EXTRA_SQL}" ]; then CASES=(A B C); fi
NCASES=${#CASES[@]}
for RUN_I in $(seq 1 "${R}"); do
    OFF=$(( (RUN_I - 1) % NCASES ))
    for i in $(seq 0 $(( NCASES - 1 ))); do
        run_phase "${CASES[$(( (i + OFF) % NCASES ))]}"
    done
done

"${PSQL[@]}" -c "SELECT overlay_branch.discard_branch('${BR}');" >/dev/null 2>&1 || true

# --- summary (medians over the interleaved repetitions) ------------------

awk -v at="$(col A 3 | median)"   -v bt="$(col B 3 | median)"  -v ct="$(col C 3 | median)" \
    -v ap50="$(col A 4 | median)" -v bp50="$(col B 4 | median)" -v cp50="$(col C 4 | median)" \
    -v ap95="$(col A 5 | median)" -v bp95="$(col B 5 | median)" -v cp95="$(col C 5 | median)" \
    -v ap99="$(col A 6 | median)" -v bp99="$(col B 6 | median)" -v cp99="$(col C 6 | median)" \
    -v acpu="$(col A 7 | median)" -v bcpu="$(col B 7 | median)" -v ccpu="$(col C 7 | median)" \
    -v agp99="$(col B 8 | median)" -v cgp99="$(col C 8 | median)" \
    -v reps="${R}" -v nc="${NCASES}" '
BEGIN {
    printf "==== SUMMARY (medians over %s interleaved rounds) ====\n", reps
    printf "  A = MAIN only        B = MAIN + branch Agent        C = MAIN + control (no branch)\n"
    if (nc < 3) {
        printf "  MAIN tps    : A=%s  B=%s   delta=%+.2f%%\n", at, bt, (at>0 ? 100*(bt-at)/at : 0)
        printf "  MAIN p50 ms : A=%s  B=%s\n", ap50, bp50
        printf "  MAIN p95 ms : A=%s  B=%s\n", ap95, bp95
        printf "  MAIN p99 ms : A=%s  B=%s   delta=%+.2f%%\n", ap99, bp99, (ap99>0 ? 100*(bp99-ap99)/ap99 : 0)
        printf "  CPU busy %%  : A=%s  B=%s\n", acpu, bcpu
        printf "  AGENT p99 ms: (branch Agent) %s\n", agp99
    } else {
        printf "  MAIN tps    : A=%s  B=%s  C=%s   (B-A=%+.2f%%  C-A=%+.2f%%  B-C=%+.2f%%)\n", \
               at, bt, ct, (at>0 ? 100*(bt-at)/at : 0), (at>0 ? 100*(ct-at)/at : 0), (ct>0 ? 100*(bt-ct)/ct : 0)
        printf "  MAIN p50 ms : A=%s  B=%s  C=%s\n", ap50, bp50, cp50
        printf "  MAIN p95 ms : A=%s  B=%s  C=%s\n", ap95, bp95, cp95
        printf "  MAIN p99 ms : A=%s  B=%s  C=%s   (B-A=%+.2f%%  C-A=%+.2f%%)\n", \
               ap99, bp99, cp99, (ap99>0 ? 100*(bp99-ap99)/ap99 : 0), (ap99>0 ? 100*(cp99-ap99)/ap99 : 0)
        printf "  CPU busy %%  : A=%s  B=%s  C=%s\n", acpu, bcpu, ccpu
        printf "  AGENT p99 ms: (branch) %s   (control) %s\n", agp99, cgp99
    }
    printf "\n  NOTE: no pass/fail here -- G05 thresholds are still OPEN\n"
    printf "        (must be frozen before use).\n"
}'
} 2>&1 | tee "${OUT}"

echo
echo "log -> ${OUT}"
rm -f "${RESULTS}"
