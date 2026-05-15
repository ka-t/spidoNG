#!/usr/bin/env bash
# Sequentially benchmark every stack against the same PG (ecom DB) with
# the same wrk workload. Each stack is started, given a 3-second warmup,
# hit with wrk for $DURATION seconds, then killed before the next one
# launches. Results are appended to benchmarks/results/<timestamp>.txt
# plus a single TSV summary the chart script consumes.
#
# Usage:
#   ./run-all.sh           # default workload (8 threads, 64 conns, 10s)
#   DURATION=20 ./run-all.sh
set -euo pipefail

THREADS=${THREADS:-8}
CONNS=${CONNS:-64}
DURATION=${DURATION:-10}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
COMP=$ROOT/benchmarks/competitors
BUILD_OUT=$ROOT/benchmarks/build-out
RESULTS=$ROOT/benchmarks/results
mkdir -p "$RESULTS"
TS=$(date +%Y%m%d_%H%M%S)
LOG=$RESULTS/$TS.txt
TSV=$RESULTS/$TS.tsv
echo -e "stack\trps\tp50_us\tp90_us\tp99_us" > "$TSV"

bench_one() {
  local NAME=$1 PORT=$2 PATHQ=$3
  shift 3
  echo "" | tee -a "$LOG"
  echo "================================================================" | tee -a "$LOG"
  echo "== $NAME → http://127.0.0.1:$PORT$PATHQ" | tee -a "$LOG"
  echo "================================================================" | tee -a "$LOG"

  # Make sure no leftover process holds the port.
  fuser -k -n tcp "$PORT" 2>/dev/null || true
  sleep 0.5

  # Start the stack in background.
  "$@" > "$RESULTS/$TS.$NAME.log" 2>&1 &
  local PID=$!

  # Wait for socket to accept connections (max 10s).
  for i in {1..50}; do
    if curl -fs --max-time 1 "http://127.0.0.1:$PORT$PATHQ" >/dev/null 2>&1; then break; fi
    sleep 0.2
  done

  # 3-second warmup at low concurrency (lets connection pools fill).
  wrk -t2 -c4 -d3s "http://127.0.0.1:$PORT$PATHQ" > /dev/null 2>&1 || true

  # Actual measurement.
  local OUT
  OUT=$(wrk -t"$THREADS" -c"$CONNS" -d"${DURATION}s" --latency \
        "http://127.0.0.1:$PORT$PATHQ" 2>&1)
  echo "$OUT" | tee -a "$LOG"

  # Parse Req/s + latency percentiles. wrk's --latency block looks like:
  #   Latency Distribution
  #      50%   123.00us
  #      90%     1.23ms
  #      99%     5.67ms
  local RPS P50 P90 P99
  RPS=$(echo "$OUT" | awk '/Requests\/sec:/ {print $2}')
  P50=$(echo "$OUT" | awk '/^[[:space:]]+50%/ {print $2}')
  P90=$(echo "$OUT" | awk '/^[[:space:]]+90%/ {print $2}')
  P99=$(echo "$OUT" | awk '/^[[:space:]]+99%/ {print $2}')

  # Normalize all to microseconds (wrk emits us/ms/s).
  to_us() {
    local v=$1
    [[ -z "$v" ]] && { echo 0; return; }
    if [[ $v == *us ]]; then echo "${v%us}"
    elif [[ $v == *ms ]]; then awk "BEGIN{printf \"%.2f\", ${v%ms}*1000}"
    elif [[ $v == *s ]];  then awk "BEGIN{printf \"%.2f\", ${v%s}*1000000}"
    else echo "$v"
    fi
  }
  echo -e "${NAME}\t${RPS}\t$(to_us "$P50")\t$(to_us "$P90")\t$(to_us "$P99")" >> "$TSV"

  # Clean up.
  kill "$PID" 2>/dev/null || true
  # Wait for the listening socket to actually be released so the next
  # stack on the same port doesn't EADDRINUSE.
  for i in {1..20}; do
    if ! fuser -n tcp "$PORT" 2>/dev/null | grep -q "."; then break; fi
    sleep 0.2
  done
  sleep 1
}

# ----- SpidoNG variants ----------------------------------------------------
bench_one spidong_cached 9000 "/products?page_size=20" \
  "$BUILD_OUT/spidong_bench/build/spidong_bench"

bench_one spidong_nocache 9000 "/products?page_size=20" \
  "$BUILD_OUT/spidong_nocache_bench/build/spidong_nocache_bench"

# ----- competitors ---------------------------------------------------------
if [[ -x /home/kaan/.local/bin/postgrest ]]; then
  bench_one postgrest 9001 "/products?limit=20" \
    /home/kaan/.local/bin/postgrest "$COMP/postgrest/postgrest.conf"
fi

bench_one fastapi 9002 "/products?page_size=20" \
  python3 -m uvicorn app:app --host 127.0.0.1 --port 9002 \
    --workers 1 --log-level warning --app-dir "$COMP/fastapi"

# Run Express from its own dir so node finds node_modules/.
bench_one express 9003 "/products?page_size=20" \
  env PORT=9003 node "$COMP/express/index.js"

if [[ -x "$COMP/go-chi/bench" ]]; then
  bench_one go-chi 9004 "/products?page_size=20" \
    env PORT=9004 "$COMP/go-chi/bench"
fi

if [[ -x "$COMP/actix/target/release/actix_bench" ]]; then
  bench_one actix 9005 "/products?page_size=20" \
    env PORT=9005 "$COMP/actix/target/release/actix_bench"
fi

echo "" | tee -a "$LOG"
echo "================================================================" | tee -a "$LOG"
echo "== SUMMARY" | tee -a "$LOG"
echo "================================================================" | tee -a "$LOG"
column -t -s $'\t' "$TSV" | tee -a "$LOG"
echo "" | tee -a "$LOG"
echo "Full log:    $LOG"
echo "TSV summary: $TSV"
