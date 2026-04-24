#!/usr/bin/env bash
# Test D — does UNION-ALL batching help vs serial-INSERT when writing K
# shapefiles into a shared target table?
#
# Hypothesis 6: commit-2 saw ~5% outer-parallelism benefit because K workers'
# shared writes serialized behind DuckDB's write lock. If we move the work
# inside one query (UNION ALL of K ST_Read branches feeding one INSERT),
# DuckDB's own scheduler can parallelize the parse phase and do a single
# batched write — potentially much faster.
#
# We compare three in-process shapes (all local files, no /vsicurl/):
#   baseline:  K serial INSERTs into tiger.edges (one per county)
#   union:     one INSERT ... UNION ALL of K ST_Read branches
#   scratch:   K CTAS into distinct scratch tables, then one merge INSERT
#              (explicitly avoids shared-target write contention)
#
# Prior anecdote: in April 2026 we observed UNION ALL INSERT over /vsicurl/
# being *slower* than serial — but that was network-dominated. Local files
# remove the network confound; this test shows whether DuckDB-internal
# parallelism wins when the bytes are already on disk.

set -eu -o pipefail
cd "$(dirname "$0")/../.."
source scripts/benchmarks/helpers.sh

echo "=== Test D: batched INSERT shapes against local shapefiles ==="
ensure_mirror

COUNTIES=(001 003 005 007 009)
K=${#COUNTIES[@]}
CORES="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)"

# Shared prelude used by every shape.
prelude() {
    cat <<EOF
LOAD us_geocoder;
LOAD spatial;
SET threads = $CORES;
CREATE TABLE bench_edges AS SELECT * FROM tiger.edges WHERE false;
CREATE SCHEMA IF NOT EXISTS scratch;
EOF
}

# One SELECT branch for county \$1 reading the local shapefile.
branch_select() {
    local cfp="$1"
    local zip_base="tl_${BENCH_YEAR}_${BENCH_FIPS}${cfp}_edges"
    local path="/vsizip/$BENCH_MIRROR/EDGES/${zip_base}.zip/${zip_base}.shp"
    cat <<EOF
SELECT '$BENCH_FIPS' AS statefp, '$cfp' AS countyfp, TLID AS tlid, TFIDL AS tfidl, TFIDR AS tfidr,
       TNIDF AS tnidf, TNIDT AS tnidt, MTFCC AS mtfcc, FULLNAME AS fullname, geom AS the_geom,
       NULLIF(ZIPL, '') AS zipl, NULLIF(ZIPR, '') AS zipr
FROM ST_Read('$path')
EOF
}

# --- Shape A: serial INSERTs --------------------------------------------
DB="$(make_scratch_db)"
sql_a="$(prelude)"
for c in "${COUNTIES[@]}"; do
    sql_a="$sql_a INSERT INTO bench_edges $(branch_select "$c");"
done
A_T="$(time_command_secs "$DUCKDB_BIN" "$DB" -c "$sql_a")"
rm -f "$DB" "$DB.wal"

# --- Shape B: single INSERT ... UNION ALL (K branches) -------------------
DB="$(make_scratch_db)"
sql_b="$(prelude) INSERT INTO bench_edges"
first=1
for c in "${COUNTIES[@]}"; do
    if (( first )); then
        sql_b="$sql_b $(branch_select "$c")"
        first=0
    else
        sql_b="$sql_b UNION ALL $(branch_select "$c")"
    fi
done
sql_b="$sql_b;"
B_T="$(time_command_secs "$DUCKDB_BIN" "$DB" -c "$sql_b")"
rm -f "$DB" "$DB.wal"

# --- Shape C: K CTAS into distinct scratch tables + one merge INSERT -----
# Within one duckdb process, successive CREATE TABLE AS statements may
# still serialize at the executor level; but the scratch targets differ
# so write-lock contention (if any) can't apply to this shape.
DB="$(make_scratch_db)"
sql_c="$(prelude)"
for c in "${COUNTIES[@]}"; do
    sql_c="$sql_c CREATE OR REPLACE TABLE scratch.edges_${c} AS $(branch_select "$c");"
done
sql_c="$sql_c INSERT INTO bench_edges"
first=1
for c in "${COUNTIES[@]}"; do
    if (( first )); then
        sql_c="$sql_c SELECT * FROM scratch.edges_${c}"
        first=0
    else
        sql_c="$sql_c UNION ALL SELECT * FROM scratch.edges_${c}"
    fi
done
sql_c="$sql_c;"
C_T="$(time_command_secs "$DUCKDB_BIN" "$DB" -c "$sql_c")"
rm -f "$DB" "$DB.wal"

echo
printf "K = %d counties (%s), threads=%s, local files:\n" "$K" "${COUNTIES[*]}" "$CORES"
printf "  A) %d serial INSERTs:                  %ss\n" "$K" "$A_T"
printf "  B) 1 INSERT, UNION ALL of %d branches:  %ss\n" "$K" "$B_T"
printf "  C) %d CTAS + 1 merge INSERT:            %ss\n" "$K" "$C_T"
echo
awk -v a="$A_T" -v b="$B_T" -v c="$C_T" -v k="$K" 'BEGIN {
    if (b > 0) printf "  UNION-ALL speedup vs serial:  %.2fx\n", a/b
    if (c > 0) printf "  CTAS-merge speedup vs serial: %.2fx\n", a/c
    printf "  (ideal parallel speedup on K branches would be %dx)\n", k
}'

echo
echo "interpretation:"
echo "  UNION ≈ K× and ≈ CTAS → DuckDB internally parallelizes UNION-ALL ST_Reads;"
echo "                         batching at this layer would beat commit-2's"
echo "                         multi-Connection approach."
echo "  CTAS ≫ UNION         → shared-target MVCC/lock WAS serializing; the"
echo "                         scratch-table pattern from the roadmap is the"
echo "                         right shape for a future parallel-ingest commit."
echo "  both ≈ serial         → no in-process parallelism to harvest; work"
echo "                         is GDAL-parse-bound per branch, schedule"
echo "                         serialized somewhere upstream."
