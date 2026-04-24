#!/usr/bin/env bash
# Test C — concurrent ST_Reads: does GDAL parallelize across files, or does
# it hold a global driver lock?
#
# We run N different shapefile reads in parallel via two shapes:
#   (1) K separate `duckdb` OS processes (no shared GDAL state between them)
#   (2) K in-process UNION ALL branches inside one query, threads=K
#
# Hypothesis 5 says GDAL might serialize these behind a driver lock. If so:
#   - shape (1) should still parallelize (each process has its own libgdal)
#   - shape (2) would NOT parallelize (one libgdal, one lock)
# If both parallelize: no GDAL lock, something else is the bottleneck.
# If neither: parallel ST_Read infeasible at this layer.
#
# Signal of parallelism: wall-time ratio of K-concurrent vs K-serial. With
# perfect parallelism, K-concurrent ≈ serial/K. With a global lock,
# K-concurrent ≈ serial.

set -eu -o pipefail
cd "$(dirname "$0")/../.."
source scripts/benchmarks/helpers.sh

echo "=== Test C: ST_Read concurrency (GDAL global-lock probe) ==="
ensure_mirror

# Pick 4 reasonably-sized RI county zips that read different files — avoids
# OS-page-cache aliasing across repeated reads of the same file.
ZIPS=(
    "$BENCH_MIRROR/EDGES/tl_${BENCH_YEAR}_${BENCH_FIPS}001_edges.zip|tl_${BENCH_YEAR}_${BENCH_FIPS}001_edges.shp"
    "$BENCH_MIRROR/EDGES/tl_${BENCH_YEAR}_${BENCH_FIPS}003_edges.zip|tl_${BENCH_YEAR}_${BENCH_FIPS}003_edges.shp"
    "$BENCH_MIRROR/EDGES/tl_${BENCH_YEAR}_${BENCH_FIPS}005_edges.zip|tl_${BENCH_YEAR}_${BENCH_FIPS}005_edges.shp"
    "$BENCH_MIRROR/EDGES/tl_${BENCH_YEAR}_${BENCH_FIPS}007_edges.zip|tl_${BENCH_YEAR}_${BENCH_FIPS}007_edges.shp"
)
K=${#ZIPS[@]}

# --- Shape 0: K-serial inside one CLI ---------------------------------------
# Baseline: K ST_Reads in sequence.
serial_sql="LOAD spatial;"
for z in "${ZIPS[@]}"; do
    zip="${z%|*}"; inner="${z#*|}"
    serial_sql="${serial_sql} SELECT COUNT(*) FROM ST_Read('/vsizip/$zip/$inner');"
done
SERIAL_T=$(time_command_secs "$DUCKDB_BIN" -c "$serial_sql")

# --- Shape 1: K separate duckdb processes in parallel ------------------------
# Each OS process has its own libgdal; no shared lock possible.
procs_t_start=$(wall_seconds)
pids=()
for z in "${ZIPS[@]}"; do
    zip="${z%|*}"; inner="${z#*|}"
    ( "$DUCKDB_BIN" -c "LOAD spatial; SELECT COUNT(*) FROM ST_Read('/vsizip/$zip/$inner');" >/dev/null 2>&1 ) &
    pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid"; done
procs_t_end=$(wall_seconds)
PROCS_T=$(awk -v a="$procs_t_end" -v b="$procs_t_start" 'BEGIN {printf "%.2f", a-b}')

# --- Shape 2: one query, UNION ALL, threads=K -------------------------------
# All shares one libgdal; if GDAL has a global lock, this should not parallelize.
union_sql="LOAD spatial; SET threads=$K;"
first=1
for z in "${ZIPS[@]}"; do
    zip="${z%|*}"; inner="${z#*|}"
    if (( first )); then
        union_sql="$union_sql SELECT '$(basename "$zip")' AS src, COUNT(*) AS n FROM ST_Read('/vsizip/$zip/$inner')"
        first=0
    else
        union_sql="$union_sql UNION ALL SELECT '$(basename "$zip")', COUNT(*) FROM ST_Read('/vsizip/$zip/$inner')"
    fi
done
union_sql="$union_sql;"
UNION_T=$(time_command_secs "$DUCKDB_BIN" -c "$union_sql")

echo
printf "K = %d distinct shapefiles\n" "$K"
printf "  serial (1 CLI, %d sequential reads):   %ss\n" "$K" "$SERIAL_T"
printf "  parallel (%d CLI processes backgrounded): %ss\n" "$K" "$PROCS_T"
printf "  parallel (1 CLI, UNION ALL, threads=%d): %ss\n" "$K" "$UNION_T"
echo
awk -v s="$SERIAL_T" -v p="$PROCS_T" -v u="$UNION_T" -v k="$K" 'BEGIN {
    if (p > 0) printf "  procs speedup vs serial:   %.2fx  (ideal %dx)\n", s/p, k
    if (u > 0) printf "  union speedup vs serial:   %.2fx  (ideal %dx)\n", s/u, k
}'

echo
echo "interpretation:"
echo "  both ≈ K×   → no GDAL lock; bottleneck elsewhere (MVCC, disk, etc.)"
echo "  procs ≈ K×, union ≈ 1× → libgdal has a global lock in one process;"
echo "                            can only parallelize via processes"
echo "  both ≈ 1×   → something shared at OS level (file locks? disk?) —"
echo "                GDAL/ST_Read is not a viable parallelism point"
