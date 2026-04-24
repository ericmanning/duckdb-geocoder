#!/usr/bin/env bash
# Test B — INSERT time with threads=1 vs default threads.
# Hypothesis 4: DuckDB's task scheduler already saturates cores for a single
# `INSERT ... SELECT ... FROM ST_Read(...)` query, via internal vectorized
# parallelism. If true, wrapping N such INSERTs in an outer K-way thread
# pool can't yield proportional speedup — we're only oversubscribing cores.
#
# How to interpret:
#   t1 = wall time with threads=1
#   tN = wall time with threads=default (hardware concurrency)
#   ratio = t1 / tN
#
#   ratio ≥ 3× (on an 8+ core box)  →  hypothesis 4 confirmed. DuckDB already
#                                      multi-threaded the INSERT; outer
#                                      threading won't help.
#   ratio ≈ 1× (t1 ≈ tN)            →  DuckDB wasn't parallelizing this INSERT;
#                                      outer K-way threading should in principle work.
#   ratio ∈ [1×, 3×]                →  partial parallelism; some win possible
#                                      from outer threading but not K×.

set -eu -o pipefail
cd "$(dirname "$0")/../.."
source scripts/benchmarks/helpers.sh

echo "=== Test B: INSERT time vs DuckDB 'threads' setting ==="
ensure_mirror

# We measure one isolated county-edges INSERT. County 007 (Providence) is RI's
# biggest — ~64 K edges in a ~7 MB zip. A few seconds serial, gives a clean
# signal without the noise of other tables / edge_containment.
COUNTY="007"
ZIP_BASE="tl_${BENCH_YEAR}_${BENCH_FIPS}${COUNTY}_edges"
VSIPATH="/vsizip/$BENCH_MIRROR/EDGES/${ZIP_BASE}.zip/${ZIP_BASE}.shp"

# Fresh scratch DB per trial so caches don't skew.
# run_trial runs ONE INSERT at a specified threads setting. Writes the measured
# real-seconds to stdout; everything else goes to stderr so $() only captures
# the number.
run_trial() {
    local threads="$1"
    local db; db="$(make_scratch_db)"
    local sql="LOAD us_geocoder; LOAD spatial; SET threads = $threads;
CREATE TABLE bench_edges AS SELECT * FROM tiger.edges WHERE false;
INSERT INTO bench_edges
SELECT '$BENCH_FIPS', '$COUNTY', TLID, TFIDL, TFIDR, TNIDF, TNIDT, MTFCC, FULLNAME, geom,
       NULLIF(ZIPL, ''), NULLIF(ZIPR, '')
FROM ST_Read('$VSIPATH');"
    local t
    t="$(time_command_secs "$DUCKDB_BIN" "$db" -c "$sql")"
    rm -f "$db" "$db.wal"
    echo "${t:-0}"
}

CORES="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)"
echo "single INSERT of county $COUNTY edges (~64k rows), fresh DB each trial:"
T1="$(run_trial 1)"
printf "  threads=1      real=%ss\n" "$T1"
TN_AUTO="$(run_trial "$CORES")"
printf "  threads=%-6s real=%ss\n" "$CORES" "$TN_AUTO"

echo
RATIO="$(awk -v a="$T1" -v b="$TN_AUTO" 'BEGIN {if (b>0) printf "%.2f", a/b; else printf "inf"}')"
printf "ratio threads=1 / threads=auto: %s×\n" "$RATIO"

echo
echo "interpretation:"
echo "  ≥ 3×    → DuckDB was already multi-threaded internally (hypothesis 4 confirmed)"
echo "  ≈ 1×    → DuckDB wasn't parallelizing; outer threading SHOULD have worked"
echo "  1×–3×   → partial internal parallelism; outer threading can help modestly"
