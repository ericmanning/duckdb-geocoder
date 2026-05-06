#!/usr/bin/env bash
# Time PG's native TIGER loader on a representative 2-state sample so we
# can publish a load-time comparison without paying the full nationwide
# cost. CA (58 counties, large per-county) + KY (120 counties, small
# per-county) cover the two extremes of PG's per-state cost curve.
#
# Pairs with the DuckDB load times we already have (CA = 480.8s, etc.) —
# logged inline in load_tiger_duckdb.log via the `[us_geocoder XX] done`
# lines.
#
# WARNING: this runs the official postgis_tiger_geocoder loader, which
# DROP+TRUNCATEs and re-creates inheritance child tables for each state
# in tiger_data. We isolate the work to its own DB ($PG_DB_TIMING) to
# keep the mirrored geocoding-benchmark DB ($PG_DB) untouched.

set -euo pipefail
source "$(dirname "$0")/../config.sh"

PG_DB_TIMING="${PG_DB_TIMING:-pg_timing}"
LOG="$RESULTS_DIR/pg_load_timing_2states.log"
mkdir -p "$RESULTS_DIR"

echo "=== PG TIGER load timing (CA + KY) ==="
echo "Target DB: $PG_DB_TIMING (isolated from $PG_DB)"
echo "Log: $LOG"
date | tee "$LOG"

# Create the timing DB if missing, install postgis_tiger_geocoder.
docker exec -i -u postgres "$PG_CONTAINER" psql -v ON_ERROR_STOP=1 <<SQL 2>&1 | tee -a "$LOG"
SELECT 'creating $PG_DB_TIMING' WHERE NOT EXISTS (
    SELECT 1 FROM pg_database WHERE datname = '$PG_DB_TIMING'
);
SQL
docker exec -i -u postgres "$PG_CONTAINER" psql -v ON_ERROR_STOP=1 \
    -c "CREATE DATABASE $PG_DB_TIMING" 2>/dev/null || true

docker exec -i -u postgres "$PG_CONTAINER" psql -d "$PG_DB_TIMING" -v ON_ERROR_STOP=1 <<'SQL' 2>&1 | tee -a "$LOG"
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS postgis_tiger_geocoder;
SELECT tiger.set_geocode_setting('use_pagc_address_parser','true');
SQL

# Run the same loader the real benchmark would have used, scoped to 2 states.
# load_tiger_via_pg.sh accepts a comma-separated state list; we pass CA,KY.
PG_DB="$PG_DB_TIMING" \
    "$REPO_DIR/scripts/parity/pg_compare/load_tiger_via_pg.sh" "CA,KY" 2>&1 | tee -a "$LOG"

echo "" | tee -a "$LOG"
echo "=== PG load-timing run complete ===" | tee -a "$LOG"
echo "Compare against DuckDB times in load_tiger_duckdb.log:" | tee -a "$LOG"
grep -E "\[us_geocoder (CA|KY)\] done" "$RESULTS_DIR/load_tiger_duckdb.log" | tee -a "$LOG" || true
date | tee -a "$LOG"
