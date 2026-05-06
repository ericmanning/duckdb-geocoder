#!/usr/bin/env bash
# Load the user's address corpus into both engines as table `addr`. Reuses
# the same CSV sidecar file for the PG side so the read shape is identical.
# Auto-detects parquet vs csv from $INPUT_FILE extension.
#
# Required INPUT_FILE columns: addr1, addr2, city, state, zip.
# Optional: id (we synthesize one via row_number() if absent).
#
# Idempotent: CREATE OR REPLACE on `addr`. The TIGER tables in `tiger.*`
# are not touched (they live in DuckDB across runs and in PG via the
# Docker volume).
source "$(dirname "$0")/../config.sh"

echo "=== Loading addresses ==="
echo "Input:    $INPUT_FILE"

# Convert to a CSV sidecar via DuckDB (handles both parquet and csv input
# uniformly, and adds a synthetic `id` if missing). PG reads from this CSV.
CSV_FILE="$RESULTS_DIR/addresses.csv"
echo "Writing  $CSV_FILE ..."

"$DUCKDB_BIN" <<SQL
COPY (
    SELECT
        COALESCE(
            CAST(TRY_CAST(id AS VARCHAR) AS VARCHAR),
            CAST(row_number() OVER () AS VARCHAR)
        ) AS id,
        CAST(addr1 AS VARCHAR) AS addr1,
        CAST(addr2 AS VARCHAR) AS addr2,
        CAST(city  AS VARCHAR) AS city,
        CAST(state AS VARCHAR) AS state,
        CAST(zip   AS VARCHAR) AS zip
    FROM $INPUT_READER
) TO '$CSV_FILE' (HEADER, DELIMITER ',', NULL '');
SQL

ROW_COUNT=$(($(wc -l < "$CSV_FILE") - 1))
echo "  $ROW_COUNT data rows"

# ─── PostgreSQL ───────────────────────────────────────────────
echo ""
echo "--- PostgreSQL ---"

# Stream the CSV into the container and load via \COPY FROM STDIN. Avoids
# needing a shared volume; works for any-size CSV.
# NOTE: tiger.addr is a parent table created by postgis_tiger_geocoder, so we
# use bench_input (in the public schema) for our user-data here.
pg_psql <<'SQL'
DROP TABLE IF EXISTS public.bench_input;
CREATE TABLE public.bench_input (
    id    TEXT PRIMARY KEY,
    addr1 TEXT,
    addr2 TEXT,
    city  TEXT,
    state TEXT,
    zip   TEXT
);
SQL

echo "Streaming CSV into PG ..."
docker exec -i -u postgres "$PG_CONTAINER" \
    psql -d "$PG_DB" -v ON_ERROR_STOP=1 \
    -c "\COPY public.bench_input FROM STDIN WITH (FORMAT csv, HEADER true, NULL '')" \
    < "$CSV_FILE"

pg_psql -c "ANALYZE public.bench_input;"
PG_COUNT=$(pg_psql -tAc "SELECT count(*) FROM public.bench_input;" | tr -d '[:space:]')
echo "  PG: $PG_COUNT rows"

# ─── DuckDB ───────────────────────────────────────────────────
echo ""
echo "--- DuckDB ---"
echo "Loading into $DUCKDB_DB (preserves any existing tiger.* tables) ..."

"$DUCKDB_BIN" "$DUCKDB_DB" <<SQL
LOAD us_geocoder;
LOAD spatial;

-- bench_input mirrors the PG-side table name. The TIGER tables in tiger.*
-- are not touched.
CREATE OR REPLACE TABLE bench_input AS
SELECT
    COALESCE(
        CAST(TRY_CAST(id AS VARCHAR) AS VARCHAR),
        CAST(row_number() OVER () AS VARCHAR)
    ) AS id,
    CAST(addr1 AS VARCHAR) AS addr1,
    CAST(addr2 AS VARCHAR) AS addr2,
    CAST(city  AS VARCHAR) AS city,
    CAST(state AS VARCHAR) AS state,
    CAST(zip   AS VARCHAR) AS zip
FROM $INPUT_READER;

SELECT count(*) AS duckdb_row_count FROM bench_input;

-- Force WAL → main DB so a subsequent open doesn't try to replay
-- tiger.* DDL before the extension has been loaded (the extension's
-- LoadInternal creates the tiger schema, but WAL replay runs first).
CHECKPOINT;
SQL

echo ""
echo "=== Address load complete ==="
