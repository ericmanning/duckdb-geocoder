#!/usr/bin/env bash
# Compare results_pg.csv vs results_duckdb.csv per-row. Pure local CSV
# processing via DuckDB-as-a-tool (the geocoder isn't called here).
#
# Prereq: 07a_export_pg.sh and 07b_export_duckdb.sh have produced the two
# results CSVs. Those two scripts are independent and can run in parallel.
#
# Outputs:
#   - diff_summary.txt   per-field diff counts + per-state breakdown
#   - sample_diffs.tsv   first 50 mismatched rows for eyeballing
#
# No truncation on lat/lng — sub-meter floating-point drift surfaces as
# divergences. Filter post-hoc on `abs(pg_lat - dk_lat) < 1e-6` if you want
# a tolerance.
source "$(dirname "$0")/../config.sh"

PG_CSV="$RESULTS_DIR/results_pg.csv"
DK_CSV="$RESULTS_DIR/results_duckdb.csv"
SUMMARY="$RESULTS_DIR/diff_summary.txt"
SAMPLES="$RESULTS_DIR/sample_diffs.tsv"

for f in "$PG_CSV" "$DK_CSV"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: $f not found — run 07a_export_pg.sh and 07b_export_duckdb.sh first" >&2
        exit 1
    fi
done

echo "=== Comparing results ==="

"$DUCKDB_BIN" <<SQL | tee "$SUMMARY"
CREATE TABLE pg_res AS SELECT * FROM read_csv('$PG_CSV', all_varchar=true, header=true, null_padding=true);
CREATE TABLE dk_res AS SELECT * FROM read_csv('$DK_CSV', all_varchar=true, header=true, null_padding=true);

SELECT 'pg_rows'     AS metric, count(*) FROM pg_res
UNION ALL SELECT 'duckdb_rows', count(*) FROM dk_res
UNION ALL SELECT 'pg_geocoded',     count(*) FILTER (WHERE rating IS NOT NULL) FROM pg_res
UNION ALL SELECT 'duckdb_geocoded', count(*) FILTER (WHERE rating IS NOT NULL) FROM dk_res;

-- Per-field diff counts (joined by id, casting numerics for comparison).
WITH joined AS (
    SELECT p.id,
           p.input_state,
           p.rating::INTEGER  AS pg_rating,
           d.rating::INTEGER  AS dk_rating,
           p.lng::DOUBLE      AS pg_lng,
           d.lng::DOUBLE      AS dk_lng,
           p.lat::DOUBLE      AS pg_lat,
           d.lat::DOUBLE      AS dk_lat,
           p.adr_text        AS pg_adr,
           d.adr_text        AS dk_adr
    FROM pg_res p
    JOIN dk_res d ON p.id = d.id
)
SELECT
    count(*)                                                                  AS total_joined,
    count(*) FILTER (WHERE pg_rating IS DISTINCT FROM dk_rating)              AS rating_diff,
    count(*) FILTER (WHERE pg_adr   IS DISTINCT FROM dk_adr)                AS adr_diff,
    count(*) FILTER (WHERE pg_lng    IS DISTINCT FROM dk_lng
                        OR pg_lat    IS DISTINCT FROM dk_lat)                 AS geom_diff,
    count(*) FILTER (
        WHERE pg_rating  IS NOT DISTINCT FROM dk_rating
          AND pg_adr    IS NOT DISTINCT FROM dk_adr
          AND pg_lng     IS NOT DISTINCT FROM dk_lng
          AND pg_lat     IS NOT DISTINCT FROM dk_lat
    )                                                                         AS strict_match,
    count(*) FILTER (
        WHERE pg_rating  IS NOT DISTINCT FROM dk_rating
          AND pg_adr    IS NOT DISTINCT FROM dk_adr
    )                                                                         AS rating_and_adr_match
FROM joined;

-- Per-state strict-match rate.
WITH joined AS (
    SELECT p.id,
           upper(coalesce(p.input_state,'??')) AS input_state,
           p.rating  IS NOT DISTINCT FROM d.rating  AS rating_match,
           p.adr_text IS NOT DISTINCT FROM d.adr_text AS adr_match,
           p.lng IS NOT DISTINCT FROM d.lng AND p.lat IS NOT DISTINCT FROM d.lat AS geom_match
    FROM pg_res p JOIN dk_res d ON p.id = d.id
)
SELECT input_state,
       count(*) AS rows,
       count(*) FILTER (WHERE rating_match AND adr_match AND geom_match) AS strict_match,
       count(*) FILTER (WHERE rating_match AND adr_match)               AS adr_match,
       round(100.0 * count(*) FILTER (WHERE rating_match AND adr_match) / nullif(count(*),0), 1) AS pct_adr_match
FROM joined
GROUP BY input_state
ORDER BY rows DESC, input_state;
SQL

# Sample 50 mismatches for manual inspection.
"$DUCKDB_BIN" <<SQL > "$SAMPLES"
COPY (
    WITH p AS (SELECT * FROM read_csv('$PG_CSV', all_varchar=true, header=true, null_padding=true)),
         d AS (SELECT * FROM read_csv('$DK_CSV', all_varchar=true, header=true, null_padding=true))
    SELECT p.id, p.input_state,
           p.rating AS pg_rating,    d.rating AS dk_rating,
           p.adr_text AS pg_adr,   d.adr_text AS dk_adr,
           p.lng    AS pg_lng,       d.lng    AS dk_lng,
           p.lat    AS pg_lat,       d.lat    AS dk_lat
    FROM p JOIN d ON p.id = d.id
    WHERE p.rating IS DISTINCT FROM d.rating
       OR p.adr_text IS DISTINCT FROM d.adr_text
    ORDER BY p.id
    LIMIT 50
) TO '$SAMPLES' (FORMAT csv, HEADER, DELIMITER E'\t');
SQL

echo ""
echo "=== Done ==="
echo "Files:"
echo "  $SUMMARY"
echo "  $SAMPLES   (first 50 mismatches)"
echo ""
echo "Run 08_recheck_diffs.sh to isolate PAGC state-leakage from real bugs."
