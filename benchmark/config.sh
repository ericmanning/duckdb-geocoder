#!/usr/bin/env bash
# Shared configuration for the geocoder parity-and-timing benchmark suite.
# Set INPUT_FILE before running any 0X_*.sh script.
#
# Mirrors the layout of the duckdb-address-standardizer benchmark/ tree, but
# adapted to the geocoder's reality:
#   - PG side runs inside the existing scripts/parity/pg_compare/ Docker image
#     (PG 16 + PostGIS + tiger_geocoder + address_standardizer); building
#     PostGIS from source via brew is a 1-3hr ordeal we avoid.
#   - TIGER data must be loaded into both engines before any geocode call.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- User must set this ---
# Path to the input table. Either CSV or Parquet — we auto-detect by extension.
# Required columns: addr1, addr2, city, state, zip. An `id` column is optional;
# we generate row_number()-based IDs if it's missing.
INPUT_FILE="${INPUT_FILE:?Set INPUT_FILE env var to your .csv or .parquet path}"

case "$INPUT_FILE" in
    *.parquet) INPUT_READER="read_parquet('$INPUT_FILE')" ;;
    *.csv)     INPUT_READER="read_csv('$INPUT_FILE', header=true, all_varchar=true)" ;;
    *)         echo "ERROR: INPUT_FILE must be .csv or .parquet (got: $INPUT_FILE)" >&2; exit 1 ;;
esac

# --- PostgreSQL (Docker) ---
PG_CONTAINER="${PG_CONTAINER:-pgparity}"
PG_IMAGE="${PG_IMAGE:-duckdb-geocoder-pgparity}"
PG_DB="${PG_DB:-parity}"
PG_USER="${PG_USER:-postgres}"
PG_HOST_PORT="${PG_HOST_PORT:-55432}"

# Helper that runs a psql command inside the container as the postgres OS user
# (so it picks up the local socket — no password prompt). Preserves arg
# arrays via "$@" so quoted SQL doesn't get re-tokenized by the shell.
# Use as:
#   pg_psql -c 'SELECT 1;'
#   pg_psql <<SQL ... SQL
pg_psql() {
    docker exec -i -u postgres "$PG_CONTAINER" \
        psql -d "$PG_DB" -v ON_ERROR_STOP=1 "$@"
}

# --- DuckDB ---
DUCKDB_BIN="${DUCKDB_BIN:-$REPO_DIR/build/release/duckdb}"
DUCKDB_DB="${DUCKDB_DB:-/tmp/geocoder_bench.duckdb}"
# IMPORTANT: $DUCKDB_DB is *persistent* across benchmark runs. It holds the
# nationwide TIGER tables loaded in 03_load_tiger.sh (~50 GB), which we want
# to reuse downstream (e.g. for the prebuilt-parquet distribution). The
# load scripts use CREATE OR REPLACE on the user-data tables but never
# drop the DB or the tiger.* schema. To start fresh: `rm -f $DUCKDB_DB`.
#
# The CLI built by `make release` has us_geocoder + spatial +
# us_address_standardizer all statically linked. Plain `LOAD us_geocoder;`
# works without -unsigned because we built them in-process.

# --- Benchmark parameters ---
RESULTS_DIR="${RESULTS_DIR:-/tmp/geocoder_bench_results}"
SIZES=("${SIZES_OVERRIDE:-1000 10000 0}")  # 0 = all rows; default array via override
RUNS="${RUNS:-3}"
DUCKDB_THREAD_COUNTS=("${DUCKDB_THREAD_COUNTS_OVERRIDE:-1 4 8 0}")  # 0 = all cores
PG_JOBS="${PG_JOBS:-8}"  # Parallel psql connections for the all-rows export

# Force-rebuild SIZES / DUCKDB_THREAD_COUNTS as proper bash arrays if the
# user didn't override them. The single-string form above is a workaround
# for `set -u` complaining when the override env var is unset.
if [[ -z "${SIZES_OVERRIDE:-}" ]]; then SIZES=(1000 10000 0); fi
if [[ -z "${DUCKDB_THREAD_COUNTS_OVERRIDE:-}" ]]; then DUCKDB_THREAD_COUNTS=(1 4 8 0); fi

mkdir -p "$RESULTS_DIR"
