#!/usr/bin/env bash
# Benchmark tiger.geocode() on PostgreSQL across SIZES × RUNS. Logs wall-clock
# timings to $RESULTS_DIR/timings.tsv (one row per (engine, size, run, …, secs)).
#
# Methodology mirrors the standardizer benchmark:
#   - One CTAS into a TEMP TABLE per run, capturing the third \timing line
#     (which is the CTAS itself; lines 1-2 are the housekeeping DROP/CREATE).
#   - PAGC parser is enabled at the DB level by 00_setup_pg.sh.
source "$(dirname "$0")/../config.sh"

TIMINGS_FILE="$RESULTS_DIR/timings.tsv"

if [[ ! -f "$TIMINGS_FILE" ]]; then
    printf "engine\tsize\trun\tthreads\tseconds\n" > "$TIMINGS_FILE"
fi

echo "=== PostgreSQL Benchmark ==="
echo "Sizes: ${SIZES[*]}"
echo "Runs per size: $RUNS"
echo ""

# Address string PG's geocode() expects: "<street>, <city>, <state> <zip>".
# concat_ws skips NULLs but not empty strings, so wrap each piece in NULLIF
# to drop empty fields cleanly.
ADDR_SQL_PG="concat_ws(', ',
    NULLIF(addr1,''),
    NULLIF(addr2,''),
    concat_ws(' ', NULLIF(city,''),
                   concat_ws(' ', NULLIF(state,''), NULLIF(zip,''))))"

for size in "${SIZES[@]}"; do
    if [[ "$size" -eq 0 ]]; then
        # Full-dataset bench — handled by 07_export_and_compare.sh which uses
        # parallel connections to keep wall-clock manageable.
        echo "--- Skipping full dataset (use 07_export_and_compare.sh) ---"
        echo ""
        continue
    fi

    LIMIT_SQL="WHERE id IN (SELECT id FROM public.bench_input ORDER BY id LIMIT $size)"
    echo "--- Size: $size ---"

    for run in $(seq 1 "$RUNS"); do
        # \timing prints "Time: NNN.NNN ms" after each statement. We run a
        # housekeeping DROP/CREATE before the CTAS so the third "Time:" line
        # is the geocode CTAS we care about.
        OUTPUT=$(pg_psql 2>&1 <<SQL
\\timing on
DROP TABLE IF EXISTS _bench_geocode;
CREATE TEMP TABLE _bench_geocode AS
SELECT a.id,
       g.rating,
       ST_X(g.geomout) AS lng,
       ST_Y(g.geomout) AS lat,
       pprint_addy(g.addy) AS adr_text
FROM public.bench_input a
LEFT JOIN LATERAL geocode($ADDR_SQL_PG, 1) g ON TRUE
$LIMIT_SQL;
DROP TABLE _bench_geocode;
SQL
        )

        TIME_MS=$(echo "$OUTPUT" | grep "^Time:" | sed -n '2p' | grep -oE '[0-9]+\.[0-9]+' | head -1)

        if [[ -z "$TIME_MS" ]]; then
            echo "  Run $run: ERROR — could not parse timing"
            echo "$OUTPUT" | tail -20
            continue
        fi

        TIME_S=$(python3 -c "print(f'{$TIME_MS / 1000:.3f}')")
        printf "pg\t%s\t%d\t1\t%s\n" "$size" "$run" "$TIME_S" >> "$TIMINGS_FILE"
        echo "  Run $run: ${TIME_MS} ms (${TIME_S} s)"
    done
    echo ""
done

echo "=== PG benchmark complete. Results in $TIMINGS_FILE ==="
