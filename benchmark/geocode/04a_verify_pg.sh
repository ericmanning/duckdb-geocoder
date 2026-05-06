#!/usr/bin/env bash
# Eyeball check on the PG side: a canned address + the first row from the
# loaded `bench_input` table, both routed through PostGIS's geocode().
source "$(dirname "$0")/../config.sh"

echo "=== PG verification ==="

CANNED="120 Benefit St, Providence, RI 02903"

echo ""
echo "--- Canned probe: '$CANNED' ---"
pg_psql <<SQL
SELECT g.rating,
       round(ST_X(g.geomout)::numeric, 6) AS lng,
       round(ST_Y(g.geomout)::numeric, 6) AS lat,
       pprint_addy(g.addy)               AS adr_text
FROM geocode('$CANNED', 1) AS g;
SQL

echo ""
echo "--- First bench_input row ---"

INPUT_SQL="concat_ws(', ',
    NULLIF(addr1,''),
    NULLIF(addr2,''),
    NULLIF(city,'') || COALESCE(', ' || NULLIF(state,''), '') || COALESCE(' ' || NULLIF(zip,''), ''))"

pg_psql <<SQL
WITH input AS (
    SELECT id,
           $INPUT_SQL AS addr_str
    FROM bench_input ORDER BY id LIMIT 1
)
SELECT i.id,
       i.addr_str,
       g.rating,
       round(ST_X(g.geomout)::numeric, 6) AS lng,
       round(ST_Y(g.geomout)::numeric, 6) AS lat,
       pprint_addy(g.addy)               AS adr_text
FROM input i, LATERAL geocode(i.addr_str, 1) g;
SQL
