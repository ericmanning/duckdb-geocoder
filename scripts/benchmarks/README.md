# Loader-perf hypothesis tests

Four tests that isolate why application-layer parallelism (parallel downloads, parallel ingest) hasn't helped the TIGER loader. Each tests a single hypothesis — HTTP Range partial fetches, DuckDB internal INSERT parallelism, GDAL global driver lock, or MVCC write contention — independent of the loader internals.

Defaults use Rhode Island (5 counties, ~130 MB of zips) for fast iteration. Override via env vars: `BENCH_STATE=NJ BENCH_FIPS=34 ./run_all.sh`.

## Prerequisites

- Built extension: `make release` from the repo root.
- Internet access for the first run (tests populate a local Census-nested mirror under `/tmp/us_geocoder_bench_<STATE>/` on first invocation; subsequent runs reuse it).
- Quiet network for Test A (it measures interface byte counters; other network-heavy apps will inflate the number).

## Running

```sh
cd <repo-root>
./scripts/benchmarks/run_all.sh                     # everything, one after another
./scripts/benchmarks/test_b_threads.sh              # just one test
./scripts/benchmarks/run_all.sh 2>&1 | tee /tmp/loader_perf_tests.log
```

## What each test answers

| # | file | hypothesis (from memory roadmap) | how to interpret |
|---|---|---|---|
| A | [`test_a_network.sh`](test_a_network.sh) | H1: `/vsicurl/` uses Range requests for partial fetches | `bytes observed / full zip size`: <60% → partial fetches confirmed; ≈100% → hypothesis falsified |
| B | [`test_b_threads.sh`](test_b_threads.sh) | H4: DuckDB's task scheduler already saturates cores per INSERT | `t(threads=1) / t(threads=N)`: ≥3× → internal parallelism real; ≈1× → outer threading should have worked |
| C | [`test_c_gdal.sh`](test_c_gdal.sh) | H5: GDAL holds a global driver lock across ST_Read calls | `procs speedup` ≈ K× + `union speedup` ≈ 1× → GDAL lock (parallelize only via processes); both ≈ K× → no GDAL lock |
| D | [`test_d_mvcc.sh`](test_d_mvcc.sh) | H6: shared-target INSERT serializes on DuckDB's write lock | `scratch-table speedup` ≫ `shared-target speedup` → MVCC contention was the issue; future parallel-ingest should use scratch tables + merge |

## Caveats

- **Wall-clock, single-run timings.** Results are noisy. Repeat tests 3× and eyeball the pattern; don't over-interpret single-digit % differences.
- **Cold vs warm caches.** Tests that hit the network (A) are sensitive to CDN warmth. Local-mirror tests (B, C, D) are sensitive to OS page cache — repeat each test a few times to see the warm steady-state.
- **macOS vs Linux.** Interface detection uses `route`/`ip route` and byte counters from `netstat`/`/proc/net/dev`. Tested on macOS Darwin 25. Linux path is provided but not exercised here.
- **Don't touch user data.** Every test uses `mktemp` scratch .duckdb files and cleans up. The local Census mirror lives under `/tmp/us_geocoder_bench_<STATE>/` — safe to delete if you want to re-test cold.

## After running

If a test result changes our model (e.g. scratch-table speedup is huge while shared-target isn't), update the loader-perf roadmap and [`CLAUDE.md`](../../CLAUDE.md) with the new evidence so future attempts don't repeat the same mistakes.
