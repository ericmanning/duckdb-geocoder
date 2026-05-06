#!/usr/bin/env bash
# Re-run differing rows through PG via a single (non-parallel) connection,
# then compare against DuckDB. Diffs that vanish in the recheck are PAGC's
# in-process state leaking between rows when PG runs them in parallel —
# not real engine differences.
#
# Requires: 07_export_and_compare.sh has produced results_pg.csv and
#           results_duckdb.csv.
source "$(dirname "$0")/../config.sh"

PG_CSV="$RESULTS_DIR/results_pg.csv"
DK_CSV="$RESULTS_DIR/results_duckdb.csv"
DIFF_IDS="$RESULTS_DIR/diff_ids.csv"
PG_RECHECK_CSV="$RESULTS_DIR/results_pg_recheck.csv"
RECHECK_SUMMARY="$RESULTS_DIR/recheck_summary.txt"

for f in "$PG_CSV" "$DK_CSV"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: $f not found — run 07_export_and_compare.sh first" >&2
        exit 1
    fi
done

# ─── Step 1: Extract differing IDs ───────────────────────────────
echo "=== Step 1: Extracting differing row IDs ==="

"$DUCKDB_BIN" <<SQL >/dev/null
CREATE TABLE pg AS SELECT * FROM read_csv('$PG_CSV', all_varchar=true, header=true, null_padding=true);
CREATE TABLE dk AS SELECT * FROM read_csv('$DK_CSV', all_varchar=true, header=true, null_padding=true);

COPY (
    SELECT p.id
    FROM pg p JOIN dk d ON p.id = d.id
    WHERE p.rating    IS DISTINCT FROM d.rating
       OR p.adr_text IS DISTINCT FROM d.adr_text
       OR p.lng       IS DISTINCT FROM d.lng
       OR p.lat       IS DISTINCT FROM d.lat
    ORDER BY p.id
) TO '$DIFF_IDS' (HEADER);
SQL

DIFF_COUNT=$(($(wc -l < "$DIFF_IDS") - 1))
echo "  $DIFF_COUNT differing rows"

if [[ "$DIFF_COUNT" -eq 0 ]]; then
    echo "No diffs — nothing to recheck."
    exit 0
fi

# ─── Step 2: Re-run those rows through a single PG connection ────
echo ""
echo "=== Step 2: Re-running $DIFF_COUNT rows through single PG conn ==="

ADDR_SQL_PG="concat_ws(', ',
    NULLIF(a.addr1,''),
    NULLIF(a.addr2,''),
    concat_ws(' ', NULLIF(a.city,''),
                   concat_ws(' ', NULLIF(a.state,''), NULLIF(a.zip,''))))"

# Stream the diff IDs into a TEMP TABLE inside PG, then join+geocode.
docker exec -i -u postgres "$PG_CONTAINER" \
    psql -d "$PG_DB" -v ON_ERROR_STOP=1 \
    -c "CREATE TEMP TABLE diff_ids (id text);" \
    -c "\COPY diff_ids FROM STDIN WITH (FORMAT csv, HEADER true)" \
    -c "\COPY (SELECT a.id, a.state AS input_state, g.rating, ST_X(g.geomout) AS lng, ST_Y(g.geomout) AS lat, pprint_addy(g.addy) AS adr_text FROM public.bench_input a JOIN diff_ids d ON a.id = d.id LEFT JOIN LATERAL geocode($ADDR_SQL_PG, 1) g ON TRUE ORDER BY a.id) TO STDOUT WITH (FORMAT csv, HEADER, NULL '')" \
    < "$DIFF_IDS" > "$PG_RECHECK_CSV"

RECHECK_ROWS=$(($(wc -l < "$PG_RECHECK_CSV") - 1))
echo "  Rechecked $RECHECK_ROWS rows"

# ─── Step 3: Compare recheck vs DuckDB ───────────────────────────
echo ""
echo "=== Step 3: Comparing PG recheck vs DuckDB ==="

"$DUCKDB_BIN" <<SQL | tee "$RECHECK_SUMMARY"
CREATE TABLE pg_re AS SELECT * FROM read_csv('$PG_RECHECK_CSV', all_varchar=true, header=true, null_padding=true);
CREATE TABLE dk    AS SELECT * FROM read_csv('$DK_CSV',         all_varchar=true, header=true, null_padding=true);

WITH joined AS (
    SELECT p.id,
           p.rating::INTEGER IS NOT DISTINCT FROM d.rating::INTEGER AS rating_match,
           p.adr_text       IS NOT DISTINCT FROM d.adr_text       AS adr_match,
           p.lng::DOUBLE     IS NOT DISTINCT FROM d.lng::DOUBLE     AS lng_match,
           p.lat::DOUBLE     IS NOT DISTINCT FROM d.lat::DOUBLE     AS lat_match
    FROM pg_re p JOIN dk d ON p.id = d.id
)
SELECT
    count(*)                                                            AS rechecked,
    count(*) FILTER (
        WHERE rating_match AND adr_match AND lng_match AND lat_match
    )                                                                   AS now_strict_match,
    count(*) FILTER (
        WHERE rating_match AND adr_match
    )                                                                   AS now_adr_match,
    count(*) FILTER (
        WHERE NOT (rating_match AND adr_match)
    )                                                                   AS still_adr_diff
FROM joined;

-- Per-field breakdown of remaining (post-recheck) diffs.
SELECT
    count(*) FILTER (WHERE p.rating::INTEGER IS DISTINCT FROM d.rating::INTEGER) AS rating_diff,
    count(*) FILTER (WHERE p.adr_text       IS DISTINCT FROM d.adr_text)       AS adr_diff,
    count(*) FILTER (WHERE p.lng::DOUBLE     IS DISTINCT FROM d.lng::DOUBLE)     AS lng_diff,
    count(*) FILTER (WHERE p.lat::DOUBLE     IS DISTINCT FROM d.lat::DOUBLE)     AS lat_diff
FROM pg_re p JOIN dk d ON p.id = d.id;
SQL

echo ""
echo "=== Recheck complete ==="
echo "Files:"
echo "  $DIFF_IDS"
echo "  $PG_RECHECK_CSV"
echo "  $RECHECK_SUMMARY"
echo ""
echo "Reading the result:"
echo "  rechecked - now_adr_match  =>  PAGC parallel state-leak (cosmetic)"
echo "  still_adr_diff             =>  real engine differences worth investigating"
