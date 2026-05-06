#!/usr/bin/env bash
# Eyeball check on the DuckDB side: a canned address + the first row from
# the loaded `bench_input` table, both routed through tiger.geocode().
source "$(dirname "$0")/../config.sh"

echo "=== DuckDB verification ==="

CANNED="120 Benefit St, Providence, RI 02903"

echo ""
echo "--- Canned probe: '$CANNED' ---"
"$DUCKDB_BIN" "$DUCKDB_DB" <<SQL
LOAD us_geocoder; LOAD spatial; LOAD us_address_standardizer;
SELECT rating,
       round(lng::DECIMAL(18,6), 6) AS lng,
       round(lat::DECIMAL(18,6), 6) AS lat,
       adr_text,
       block_geoid,
       containment_guaranteed
FROM geocode_batch((SELECT '$CANNED' AS addr_str));
SQL

echo ""
echo "--- First bench_input row ---"

INPUT_SQL="concat_ws(', ',
    NULLIF(addr1,''),
    NULLIF(addr2,''),
    NULLIF(city,'') || COALESCE(', ' || NULLIF(state,''), '') || COALESCE(' ' || NULLIF(zip,''), ''))"

"$DUCKDB_BIN" "$DUCKDB_DB" <<SQL
LOAD us_geocoder; LOAD spatial; LOAD us_address_standardizer;
-- Uses geocode_batch — the C++ table function with per-state literal-statefp
-- dispatch. Avoids the LATERAL macro form which doesn't scale to nationwide
-- TIGER (see commits on perf/geocode-batch-planning).
SELECT id,
       rating,
       round(lng::DECIMAL(18,6), 6) AS lng,
       round(lat::DECIMAL(18,6), 6) AS lat,
       adr_text,
       block_geoid,
       containment_guaranteed
FROM geocode_batch((
    SELECT id,
           $INPUT_SQL AS addr_str
    FROM bench_input ORDER BY id LIMIT 1
));
SQL
