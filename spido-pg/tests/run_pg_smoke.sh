#!/usr/bin/env bash
# Smoke-test harness that drives the PG-dependent test binaries against a
# real running PostgreSQL instance. Run AFTER installing PG:
#
#   sudo apt install -y postgresql && sudo systemctl start postgresql
#   sudo -u postgres createdb spido_pg_test
#   sudo -u postgres psql -c "CREATE USER spido_pg_test WITH PASSWORD 'test' SUPERUSER;"
#
# Then:
#   ./tests/run_pg_smoke.sh
#
# We point the binaries at the user-local Unix socket and report which
# subset of tests actually exercises real PG vs which still skip.

set -euo pipefail

SOCKET_DIR="/var/run/postgresql"
SOCKET_PORT="${SPIDO_PG_TEST_PORT:-5432}"
SOCKET_PATH="${SOCKET_DIR}/.s.PGSQL.${SOCKET_PORT}"

if [[ ! -S "$SOCKET_PATH" ]]; then
    echo "no PG socket at $SOCKET_PATH" >&2
    echo "is PG running? try: pg_isready" >&2
    exit 1
fi

export SPIDO_PG_TEST_SOCKET="$SOCKET_PATH"
export SPIDO_PG_TEST_USER="${SPIDO_PG_TEST_USER:-spido_pg_test}"
export SPIDO_PG_TEST_DBNAME="${SPIDO_PG_TEST_DBNAME:-spido_pg_test}"
export SPIDO_PG_TEST_PASSWORD="${SPIDO_PG_TEST_PASSWORD:-test}"

cd "$(dirname "$0")/../build"
echo "spido-pg PG smoke test → $SOCKET_PATH (user=$SPIDO_PG_TEST_USER db=$SPIDO_PG_TEST_DBNAME)"
echo

for t in test_connection test_pool test_batch_writer; do
    echo "=== $t ==="
    ./$t
    echo
done
