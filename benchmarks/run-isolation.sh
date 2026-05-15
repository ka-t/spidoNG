#!/usr/bin/env bash
# Pressure-isolation benchmark — SpidoNG vs Actix.
# Two phases per stack:
#   A: baseline — every endpoint gets a polite 32-conn wrk run (parallel)
#   B: attack   — /analytics hammered with 256 conn while /payments and
#                 /products keep their 32-conn baseline (all parallel)
# Captures per-endpoint p50/p99/rps for both phases. Critical insight:
# in phase B the *interesting* number is /payments p99 — does it survive
# the noisy neighbour?
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS=$ROOT/benchmarks/results
mkdir -p "$RESULTS"
TS=$(date +%Y%m%d_%H%M%S)
TSV=$RESULTS/iso_$TS.tsv
echo -e "stack\tphase\tendpoint\trps\tp50_us\tp99_us" > "$TSV"

DUR_A=${BASELINE_DURATION:-10}
DUR_B=${ATTACK_DURATION:-15}

to_us() {
    local v=$1
    [[ -z "$v" ]] && { echo 0; return; }
    if   [[ $v == *us ]]; then echo "${v%us}"
    elif [[ $v == *ms ]]; then awk "BEGIN{printf \"%.2f\", ${v%ms}*1000}"
    elif [[ $v == *s ]];  then awk "BEGIN{printf \"%.2f\", ${v%s}*1000000}"
    else echo "$v"; fi
}

# Parse one wrk output file → "rps<TAB>p50_us<TAB>p99_us"
parse_wrk() {
    local f=$1 RPS P50 P99
    RPS=$(awk '/Requests\/sec:/ {print $2}' "$f")
    P50=$(awk '/^[[:space:]]+50%/ {print $2}' "$f")
    P99=$(awk '/^[[:space:]]+99%/ {print $2}' "$f")
    printf "%s\t%s\t%s\n" "$RPS" "$(to_us "$P50")" "$(to_us "$P99")"
}

run_stack() {
    local STACK=$1 BASE=$2
    shift 2
    echo
    echo "=== $STACK @ $BASE ==="
    fuser -k -n tcp "${BASE##*:}" 2>/dev/null || true; sleep 0.5
    "$@" > "$RESULTS/iso_$TS.${STACK}.log" 2>&1 &
    local PID=$!
    for _ in {1..50}; do
        curl -fs --max-time 1 "$BASE/payments" >/dev/null 2>&1 && break
        sleep 0.2
    done
    wrk -t2 -c4 -d3s "$BASE/payments" >/dev/null 2>&1 || true

    # PHASE A — baseline, three parallel runs.
    echo "  phase A (baseline, 32 conn each)..."
    local A=$RESULTS/iso_$TS.$STACK.A
    wrk -t4 -c32 -d${DUR_A}s --latency "$BASE/payments"  > "$A.payments.log"  2>&1 &
    wrk -t4 -c32 -d${DUR_A}s --latency "$BASE/products"  > "$A.products.log"  2>&1 &
    wrk -t4 -c32 -d${DUR_A}s --latency "$BASE/analytics" > "$A.analytics.log" 2>&1 &
    wait
    for ep in payments products analytics; do
        triple=$(parse_wrk "$A.$ep.log")
        printf "%s\tbaseline\t%s\t%s\n" "$STACK" "$ep" "$triple" >> "$TSV"
        echo "    $ep $triple"
    done

    # PHASE B — /analytics under attack, others stay at baseline.
    echo "  phase B (attack: /analytics @ 256 conn)..."
    local B=$RESULTS/iso_$TS.$STACK.B
    wrk -t4  -c32  -d${DUR_B}s --latency "$BASE/payments"  > "$B.payments.log"  2>&1 &
    wrk -t4  -c32  -d${DUR_B}s --latency "$BASE/products"  > "$B.products.log"  2>&1 &
    wrk -t8  -c256 -d${DUR_B}s --latency "$BASE/analytics" > "$B.analytics.log" 2>&1 &
    wait
    for ep in payments products analytics; do
        triple=$(parse_wrk "$B.$ep.log")
        printf "%s\tattack\t%s\t%s\n" "$STACK" "$ep" "$triple" >> "$TSV"
        echo "    $ep $triple"
    done

    kill "$PID" 2>/dev/null || true
    for _ in {1..20}; do
        fuser -n tcp "${BASE##*:}" 2>/dev/null | grep -q "." || break
        sleep 0.2
    done
    sleep 1
}

COMP=$ROOT/benchmarks/competitors
BUILD_OUT=$ROOT/benchmarks/build-out

run_stack spidong "http://127.0.0.1:9100" \
    "$BUILD_OUT/spidong_isolation/build/spidong_isolation"

run_stack actix "http://127.0.0.1:9101" \
    env PORT=9101 "$COMP/actix-multi/target/release/actix_multi"

echo
echo "=== SUMMARY ==="
column -t -s $'\t' "$TSV"
echo
echo "TSV: $TSV"
