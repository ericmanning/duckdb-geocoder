#!/usr/bin/env bash
# Export full geocode results from PostgreSQL across $PG_JOBS parallel \COPY
# connections (id-hash partitioning). Independent of the DuckDB export — run
# 07a and 07b in parallel; both write to $RESULTS_DIR.
#
# Output:
#   - results_pg.csv              (id, input_state, rating, lng, lat, adr_text)
#   - timings.tsv                 appends one row: pg | all | 1 | <PG_JOBS>-conn | secs
#
# No truncation on lat/lng — sub-meter floating-point drift surfaces as
# divergences in 07c. Filter post-hoc if you want a tolerance.
source "$(dirname "$0")/../config.sh"

PG_CSV="$RESULTS_DIR/results_pg.csv"

ADDR_SQL_PG="concat_ws(', ',
    NULLIF(addr1,''),
    NULLIF(addr2,''),
    concat_ws(' ', NULLIF(city,''),
                   concat_ws(' ', NULLIF(state,''), NULLIF(zip,''))))"

# tiger.geocode reads from a configuration table and the PAGC parser has
# in-process state — both unsafe under parallel execution. Mark UNSAFE
# unconditionally so per-worker query plans don't get parallel paths.
echo "Ensuring tiger.geocode is PARALLEL UNSAFE ..."
pg_psql <<'SQL' >/dev/null
DO $$
DECLARE r record;
BEGIN
    FOR r IN
        SELECT n.nspname, p.proname, pg_catalog.pg_get_function_identity_arguments(p.oid) AS args
        FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
        WHERE n.nspname IN ('tiger','public') AND p.proname IN ('geocode','reverse_geocode')
    LOOP
        EXECUTE format('ALTER FUNCTION %I.%I(%s) PARALLEL UNSAFE', r.nspname, r.proname, r.args);
    END LOOP;
END $$;
SQL

echo ""
echo "Exporting PG results across $PG_JOBS parallel connections ..."

PG_START=$(python3 -c "import time; print(time.time())")
PG_PIDS=()

for i in $(seq 0 $((PG_JOBS - 1))); do
    PG_PART="$RESULTS_DIR/results_pg_part${i}.csv"
    # Each worker handles rows where abs(hashtext(id)) % $PG_JOBS = $i.
    # \COPY needs the whole SELECT on one line for psql -c; readability
    # sacrificed for shell escaping sanity.
    docker exec -i -u postgres "$PG_CONTAINER" \
        psql -d "$PG_DB" -v ON_ERROR_STOP=1 \
        -c "\COPY (SELECT a.id, a.state AS input_state, g.rating, ST_X(g.geomout) AS lng, ST_Y(g.geomout) AS lat, pprint_addy(g.addy) AS adr_text FROM public.bench_input a LEFT JOIN LATERAL geocode($ADDR_SQL_PG, 1) g ON TRUE WHERE abs(hashtext(a.id)) % $PG_JOBS = $i) TO STDOUT WITH (FORMAT csv, NULL '')" \
        > "$PG_PART" &
    PG_PIDS+=($!)
done

PG_FAIL=0
for pid in "${PG_PIDS[@]}"; do
    wait "$pid" || PG_FAIL=1
done
PG_END=$(python3 -c "import time; print(time.time())")

if [[ "$PG_FAIL" -eq 1 ]]; then
    echo "ERROR: one or more PG export workers failed"
    exit 1
fi

# Write merged CSV with header.
echo "id,input_state,rating,lng,lat,adr_text" > "$PG_CSV"
cat "$RESULTS_DIR"/results_pg_part*.csv >> "$PG_CSV"
rm -f "$RESULTS_DIR"/results_pg_part*.csv

PG_TIME_S=$(python3 -c "print(f'{$PG_END - $PG_START:.3f}')")
printf "pg\tall\t1\t%s-conn\t%s\n" "$PG_JOBS" "$PG_TIME_S" >> "$RESULTS_DIR/timings.tsv"
PG_ROWS=$(($(wc -l < "$PG_CSV") - 1))
echo "  PG: $PG_ROWS rows in ${PG_TIME_S} s ($PG_JOBS parallel conns)"
echo "  → $PG_CSV"
