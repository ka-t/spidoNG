#!/usr/bin/env bash
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"
DUR="${3:-30s}"
CONNS="${4:-512}"
THREADS="${5:-$(nproc)}"

DB_NAME="${DB_NAME:-mydb}"
DB_USER="${DB_USER:-admin}"
DB_HOST="${DB_HOST:-/var/run/postgresql}"
TABLE="${TABLE:-firmware}"

URL="http://${HOST}:${PORT}/firmware"
LUA_SCRIPT="./bench/firmware.lua"

echo "spido DB write bench"
echo "URL       : ${URL}"
echo "duration  : ${DUR}"
echo "conns     : ${CONNS}"
echo "threads   : ${THREADS}"
echo "db        : ${DB_NAME}"
echo "table     : ${TABLE}"
echo

if ! command -v wrk >/dev/null 2>&1; then
    echo "wrk bulunamadı. Kur:"
    echo "sudo apt install wrk"
    exit 1
fi

echo "DB count before..."
BEFORE=$(psql -h "${DB_HOST}" -U "${DB_USER}" -d "${DB_NAME}" -tAc "SELECT COUNT(*) FROM ${TABLE};")
echo "before: ${BEFORE}"

echo
echo "Running wrk..."
wrk -d"${DUR}" \
    -c"${CONNS}" \
    -t"${THREADS}" \
    -s "${LUA_SCRIPT}" \
    --latency \
    "${URL}"

echo
echo "Waiting batch flush..."
sleep 3

echo "DB count after..."
AFTER=$(psql -h "${DB_HOST}" -U "${DB_USER}" -d "${DB_NAME}" -tAc "SELECT COUNT(*) FROM ${TABLE};")
echo "after: ${AFTER}"

WRITES=$((AFTER - BEFORE))

SECS="${DUR%s}"
WRITE_PER_SEC=$(awk "BEGIN { printf \"%.2f\", ${WRITES} / ${SECS} }")

echo
echo "======================================"
echo "DB WRITES       : ${WRITES}"
echo "DB WRITES / SEC : ${WRITE_PER_SEC}"
echo "======================================"
