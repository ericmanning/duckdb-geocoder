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
SELECT g.rating,
       round(ST_X(g.geom)::DECIMAL(18,6), 6) AS lng,
       round(ST_Y(g.geom)::DECIMAL(18,6), 6) AS lat,
       tiger.pprint_adr(g.adr)               AS adr_text,
       g.block_geoid,
       g.containment_guaranteed
FROM tiger.geocode(tiger.from_pagc('$CANNED'), 1, NULL, 'none') AS g;
SQL

echo ""
echo "--- First bench_input row ---"

INPUT_SQL="concat_ws(', ',
    NULLIF(addr1,''),
    NULLIF(addr2,''),
    NULLIF(city,'') || COALESCE(', ' || NULLIF(state,''), '') || COALESCE(' ' || NULLIF(zip,''), ''))"

"$DUCKDB_BIN" "$DUCKDB_DB" <<SQL
LOAD us_geocoder; LOAD spatial; LOAD us_address_standardizer;
-- DuckDB CTE-LATERAL binder quirk: alias.col inside the LATERAL parses as
-- struct-field access. Reference addr_str unqualified — see README batch
-- section.
WITH input AS (
    SELECT id,
           $INPUT_SQL AS addr_str
    FROM bench_input ORDER BY id LIMIT 1
)
SELECT input.id,
       input.addr_str,
       g.rating,
       round(ST_X(g.geom)::DECIMAL(18,6), 6) AS lng,
       round(ST_Y(g.geom)::DECIMAL(18,6), 6) AS lat,
       tiger.pprint_adr(g.adr)               AS adr_text,
       g.block_geoid,
       g.containment_guaranteed
FROM input, LATERAL tiger.geocode(tiger.from_pagc(addr_str), 1, NULL, 'none') g;
SQL
