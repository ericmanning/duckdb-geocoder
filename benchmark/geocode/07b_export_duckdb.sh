#!/usr/bin/env bash
# Export full geocode results from DuckDB via a single COPY using all cores
# (default thread setting). Independent of the PG export — run 07a and 07b
# in parallel; both write to $RESULTS_DIR.
#
# Output:
#   - results_duckdb.csv          (id, input_state, rating, lng, lat, adr_text)
#   - timings.tsv                 appends one row: duckdb | all | 1 | default | secs
source "$(dirname "$0")/../config.sh"

DK_CSV="$RESULTS_DIR/results_duckdb.csv"

ADDR_SQL_DK="concat_ws(', ',
    NULLIF(addr1,''),
    NULLIF(addr2,''),
    concat_ws(' ', NULLIF(city,''),
                   concat_ws(' ', NULLIF(state,''), NULLIF(zip,''))))"

echo "Exporting DuckDB results ..."

DK_START=$(python3 -c "import time; print(time.time())")
"$DUCKDB_BIN" "$DUCKDB_DB" <<SQL >/dev/null
LOAD us_geocoder; LOAD spatial; LOAD us_address_standardizer;
-- Uses geocode_batch (C++ table function w/ per-state literal-statefp
-- dispatch). The input subquery's `state` column passes through to the
-- output as `input_state`; geocode_batch detects `addr_str` and uses
-- Form 1 (freeform → from_pagc internally). Other named columns
-- (addr1/addr2/city/zip) would also be consumed if present, so we wrap
-- them under aliases that don't collide with the recognized field names.
COPY (
    SELECT id, input_state, rating, lng, lat, adr_text,
           block_geoid, tract_geoid, blkgrp_geoid, containment_guaranteed
    FROM geocode_batch((
        SELECT a.id,
               a.state AS input_state,
               $ADDR_SQL_DK AS addr_str
        FROM bench_input a
    ))
) TO '$DK_CSV' (HEADER, DELIMITER ',', NULL '');
CHECKPOINT;
SQL
DK_END=$(python3 -c "import time; print(time.time())")

DK_TIME_S=$(python3 -c "print(f'{$DK_END - $DK_START:.3f}')")
printf "duckdb\tall\t1\tdefault\t%s\n" "$DK_TIME_S" >> "$RESULTS_DIR/timings.tsv"
DK_ROWS=$(($(wc -l < "$DK_CSV") - 1))
echo "  DuckDB: $DK_ROWS rows in ${DK_TIME_S} s"
echo "  → $DK_CSV"
