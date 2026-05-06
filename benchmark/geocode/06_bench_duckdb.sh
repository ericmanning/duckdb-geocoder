#!/usr/bin/env bash
# Benchmark geocode_batch on DuckDB across SIZES × THREADS × RUNS. Logs
# wall-clock timings to $RESULTS_DIR/timings.tsv.
#
# Uses the C++ geocode_batch table function (table-valued input, partitions
# by resolved statefp internally, dispatches per-state SQL with literal
# WHERE statefp='<lit>' so the planner uses ART pushdown). The previous
# LATERAL macro form (`tiger.geocode(tiger.from_pagc(s))`) hung at
# nationwide scale due to the dynamic-statefp planning issue — see
# CLAUDE.md / commits on perf/geocode-batch-planning for the diagnosis.
#
# Methodology mirrors the standardizer benchmark:
#   - One CTAS into a TEMP TABLE per run, captured via .timer on.
source "$(dirname "$0")/../config.sh"

TIMINGS_FILE="$RESULTS_DIR/timings.tsv"

if [[ ! -f "$TIMINGS_FILE" ]]; then
    printf "engine\tsize\trun\tthreads\tseconds\n" > "$TIMINGS_FILE"
fi

echo "=== DuckDB Benchmark ==="
echo "Sizes: ${SIZES[*]}"
echo "Threads: ${DUCKDB_THREAD_COUNTS[*]}"
echo "Runs per config: $RUNS"
echo ""

# Same address-string shape as the PG side.
ADDR_SQL_DK="concat_ws(', ',
    NULLIF(addr1,''),
    NULLIF(addr2,''),
    concat_ws(' ', NULLIF(city,''),
                   concat_ws(' ', NULLIF(state,''), NULLIF(zip,''))))"

for size in "${SIZES[@]}"; do
    if [[ "$size" -eq 0 ]]; then
        LIMIT_SQL=""
        LABEL="all"
    else
        LIMIT_SQL="WHERE id IN (SELECT id FROM bench_input ORDER BY id LIMIT $size)"
        LABEL="$size"
    fi

    for threads in "${DUCKDB_THREAD_COUNTS[@]}"; do
        if [[ "$threads" -eq 0 ]]; then
            THREAD_CMD=""
            THREAD_LABEL="default"
        else
            THREAD_CMD="SET threads = $threads;"
            THREAD_LABEL="$threads"
        fi

        echo "--- Size: $LABEL, Threads: $THREAD_LABEL ---"

        for run in $(seq 1 "$RUNS"); do
            OUTPUT=$("$DUCKDB_BIN" "$DUCKDB_DB" 2>&1 <<SQL
LOAD us_geocoder; LOAD spatial; LOAD us_address_standardizer;
$THREAD_CMD
.timer on
CREATE TEMP TABLE _bench_geocode AS
SELECT id, rating, lng, lat, adr_text,
       block_geoid, tract_geoid, blkgrp_geoid, containment_guaranteed
FROM geocode_batch((
    SELECT a.id,
           $ADDR_SQL_DK AS addr_str
    FROM bench_input a
    $LIMIT_SQL
));
DROP TABLE _bench_geocode;
-- TEMP table doesn't itself need a checkpoint, but the LOAD us_geocoder
-- on first open creates session-private temp tiger refs. Force a clean
-- exit state here too so subsequent opens don't try to replay tiger.* DDL.
CHECKPOINT;
SQL
            )

            # DuckDB's .timer prints "Run Time (s): real X.XXX user X.XXX sys X.XXX"
            # First "Run Time" after .timer on is the CREATE TEMP TABLE.
            TIME_S=$(echo "$OUTPUT" | grep "Run Time" | head -1 | grep -oE 'real [0-9.]+' | grep -oE '[0-9.]+')

            if [[ -z "$TIME_S" ]]; then
                echo "  Run $run: ERROR — could not parse timing"
                echo "$OUTPUT" | tail -20
                continue
            fi

            printf "duckdb\t%s\t%d\t%s\t%s\n" "$LABEL" "$run" "$THREAD_LABEL" "$TIME_S" >> "$TIMINGS_FILE"
            echo "  Run $run: ${TIME_S} s"
        done
    done
    echo ""
done

echo "=== DuckDB benchmark complete. Results in $TIMINGS_FILE ==="
