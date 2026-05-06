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

# Optional input filter for per-state-subset timings. Set STATE_FILTER to
# any SQL boolean expression over bench_input columns (e.g. STATE_FILTER=
# "state='AR'" or STATE_FILTER="state IN ('AR','CA','TX')") to constrain
# the input. With it empty (default), all rows of bench_input are eligible.
# This is the knob the parity scale-curve uses — single-state inputs let
# geocode_batch dispatch a single per-state SQL (best amortization),
# multi-state inputs pay per-state plan overhead × N distinct states.
STATE_FILTER="${STATE_FILTER:-}"
if [[ -n "$STATE_FILTER" ]]; then
    INPUT_PRED="WHERE $STATE_FILTER"
    LABEL_SUFFIX="-$(echo "$STATE_FILTER" | tr -dc 'A-Za-z0-9')"
else
    INPUT_PRED=""
    LABEL_SUFFIX=""
fi
echo "Input filter: ${STATE_FILTER:-<none>}"
echo ""

# Word-split SIZES / DUCKDB_THREAD_COUNTS into proper arrays. Existing
# config.sh defines them quoted, which yields a 1-element array containing
# a space-separated string ("1000 10000 0") instead of three numbers.
read -ra SIZES_ARR <<< "${SIZES[*]}"
read -ra THREADS_ARR <<< "${DUCKDB_THREAD_COUNTS[*]}"

for size in "${SIZES_ARR[@]}"; do
    if [[ "$size" -eq 0 ]]; then
        LIMIT_SQL="$INPUT_PRED"
        LABEL="all${LABEL_SUFFIX}"
    else
        # When STATE_FILTER is set, the LIMIT applies AFTER the filter so
        # we get N rows from the filtered population (not N from anywhere
        # then filtered, which would silently shrink under tight filters).
        if [[ -n "$INPUT_PRED" ]]; then
            LIMIT_SQL="WHERE id IN (SELECT id FROM bench_input $INPUT_PRED ORDER BY id LIMIT $size)"
        else
            LIMIT_SQL="WHERE id IN (SELECT id FROM bench_input ORDER BY id LIMIT $size)"
        fi
        LABEL="${size}${LABEL_SUFFIX}"
    fi

    for threads in "${THREADS_ARR[@]}"; do
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
