#!/usr/bin/env bash
# Quick load-test harness for spido. Picks wrk if installed, falls back to hey.
# Usage: ./bench/bench.sh [host] [port] [path] [duration] [conns] [threads]
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"
PATH_="${3:-/health}"
DUR="${4:-30s}"
CONNS="${5:-512}"
THREADS="${6:-$(nproc)}"
URL="http://${HOST}:${PORT}${PATH_}"

echo "spido bench → ${URL}  duration=${DUR}  conns=${CONNS}  threads=${THREADS}"

if command -v wrk >/dev/null 2>&1; then
    exec wrk -d"${DUR}" -c"${CONNS}" -t"${THREADS}" --latency "${URL}"
fi

if command -v hey >/dev/null 2>&1; then
    # hey doesn't take duration the same way; convert to a request count.
    SECS="${DUR%s}"
    N=$(( CONNS * 10000 ))
    exec hey -z "${DUR}" -c "${CONNS}" "${URL}"
fi

echo "neither wrk nor hey found; install one:" >&2
echo "  sudo apt install wrk     # or" >&2
echo "  go install github.com/rakyll/hey@latest" >&2
exit 1
