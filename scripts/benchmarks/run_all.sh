#!/usr/bin/env bash
# Run all loader-perf hypothesis tests back-to-back and print a summary.
set -eu -o pipefail
cd "$(dirname "$0")/../.."
source scripts/benchmarks/helpers.sh

echo "========================================================================"
echo "Loader-perf hypothesis tests"
echo "  repo:   $REPO_ROOT"
echo "  duckdb: $DUCKDB_BIN"
echo "  state:  $BENCH_STATE (FIPS $BENCH_FIPS, year $BENCH_YEAR)"
echo "  iface:  $BENCH_IFACE"
echo "  mirror: $BENCH_MIRROR"
echo "========================================================================"

if [[ ! -x "$DUCKDB_BIN" ]]; then
    echo "ERROR: $DUCKDB_BIN not found or not executable. Run 'make release' first." >&2
    exit 2
fi

for t in test_a_network.sh test_b_threads.sh test_c_gdal.sh test_d_mvcc.sh; do
    echo
    echo "------------------------------------------------------------------------"
    scripts/benchmarks/"$t" || echo "  (test $t exited non-zero; see output above)"
done

echo
echo "========================================================================"
echo "Results recap per hypothesis:"
echo "  H1 (Range partial fetches): see Test A 'ratio (observed / full)'"
echo "  H4 (DuckDB internal parallel): see Test B 'ratio threads=1 / auto'"
echo "  H5 (GDAL global lock): see Test C 'procs speedup' vs 'union speedup'"
echo "  H6 (MVCC write contention): see Test D 'scratch-table speedup'"
echo
echo "To capture results for the loader-perf roadmap memory, run with:"
echo "  ./scripts/benchmarks/run_all.sh 2>&1 | tee /tmp/loader_perf_tests.log"
echo "========================================================================"
