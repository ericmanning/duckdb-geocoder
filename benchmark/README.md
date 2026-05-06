# Geocoder parity-and-timing benchmark

End-to-end comparison of `us_geocoder` (DuckDB) vs `postgis_tiger_geocoder` (PostgreSQL) over a user-supplied nationwide address corpus. Outputs:

- **Per-row diff** of the picked address (`pprint_adr(adr)`), `rating`, and `(lng, lat)`.
- **Wall-clock timings** at multiple sizes and DuckDB thread counts.

Mirrors the structure of the [duckdb-address-standardizer benchmark/postgis/](https://github.com/ericmanning/duckdb-address-standardizer/tree/main/benchmark/postgis) suite, adapted to the geocoder's reality:

- **PG runs inside Docker.** Building PostGIS from source via Homebrew is a 1–3hr ordeal we avoid by reusing the existing [scripts/parity/pg_compare/Dockerfile](../scripts/parity/pg_compare/Dockerfile) (PG 16 + PostGIS + `address_standardizer` + `postgis_tiger_geocoder`).
- **TIGER is loaded independently into both engines, both pulling from the Census CDN.** No shared local mirror — we want to test-drive the full Census-fetch path on each side. Disk footprint: ~50 GB per engine, so plan for ~100 GB free during the load.
- **The DuckDB benchmark database persists across runs.** Once `03_load_tiger.sh` populates it (~50 GB of nationwide TIGER tables), subsequent benchmark steps reuse those tables without reloading. The load scripts use `CREATE OR REPLACE` on the address table only; the `tiger.*` schema is left intact so the same DB can later be repurposed (e.g. as the source for a prebuilt-parquet distribution).

## Layout

```
benchmark/
├── README.md          # This file
├── config.sh          # Shared paths, sizes, thread counts
└── geocode/
    ├── 00_setup_pg.sh           # Build + start the pgparity Docker container
    ├── 01_setup_duckdb.sh       # Build the DuckDB extension (release)
    ├── 02_load_addresses.sh     # Load INPUT_FILE into both engines as `addr`
    ├── 03_load_tiger.sh         # Load TIGER 2025 (50 states + DC) into both
    ├── 04_verify.sh             # Run one address through both, eyeball
    ├── 05_bench_pg.sh           # PG timings at SIZES × RUNS
    ├── 06_bench_duckdb.sh       # DuckDB timings at SIZES × THREADS × RUNS
    ├── 07_export_and_compare.sh # Full diff (per-row, per-state, per-field)
    └── 08_recheck_diffs.sh      # Re-run diff rows through PG single-conn
                                 # to isolate PAGC state-leakage from real bugs
```

## Quick start

```bash
# Path to your input table — CSV or Parquet, columns:
#   id (optional), addr1, addr2, city, state, zip
export INPUT_FILE=/path/to/addresses.parquet

cd benchmark/geocode
./00_setup_pg.sh           # ~5 min first time (image build)
./01_setup_duckdb.sh       # ~10 min first time (DuckDB build)
./02_load_addresses.sh     # seconds
./03_load_tiger.sh         # MANY HOURS — see warning below
./04_verify.sh             # quick sanity check
./05_bench_pg.sh           # ~10–30 min depending on SIZES
./06_bench_duckdb.sh       # ~5–15 min depending on SIZES × threads
./07_export_and_compare.sh # ~10–60 min depending on dataset size
./08_recheck_diffs.sh      # only if 07 found diffs
```

## Configuration (`config.sh`)

```bash
INPUT_FILE                # Required — path to input addresses table
PG_CONTAINER=pgparity     # Docker container name
PG_HOST_PORT=55432        # Host port mapped to container's 5432
DUCKDB_BIN=…/build/release/duckdb
DUCKDB_DB=/tmp/geocoder_bench.duckdb
RESULTS_DIR=/tmp/geocoder_bench_results
SIZES=(1000 10000 0)      # 0 = all rows
RUNS=3                    # Repetitions per (size, threads)
DUCKDB_THREAD_COUNTS=(1 4 8 0)  # 0 = default (all cores)
PG_JOBS=8                 # Parallel psql connections for the all-rows export
```

Override any of these by exporting `SIZES_OVERRIDE`, `RUNS`, etc. before running.

## Why TIGER load takes hours

PG's tiger loader is a per-state shell-script generator that downloads each county-level zip serially via `wget`, populates per-state partitions, and runs `CREATE INDEX` over edges/faces/etc. Wall-clock time is dominated by sequential HTTP fetches (~40s/state × 51 states ≈ 35 min just for downloads, plus the COPY + index time).

DuckDB's loader is in-process — calls `ST_Read('/vsicurl/https://www2.census.gov/geo/tiger/TIGER2025/...')` directly via httpfs. Per [CLAUDE.md § Loader performance](../CLAUDE.md), nationwide via Census CDN is ~3–4 hours.

Both can be run in parallel (different containers, different network sockets), so the real-world wall-clock for `03_load_tiger.sh` is `max(pg_load_time, duckdb_load_time) ≈ 4–6 hours`.

## Output

All results land in `$RESULTS_DIR` (default `/tmp/geocoder_bench_results/`):

| File | Source | Contents |
|---|---|---|
| `timings.tsv` | 05/06 | One row per `(engine, size, run, threads, seconds)` |
| `results_pg.csv` | 07 | Per-row PG output: `id, pprint_adr, rating, lng, lat` |
| `results_duckdb.csv` | 07 | Same, DuckDB |
| `diff_summary.txt` | 07 | Match counts (overall, per-field, per-state) |
| `diff_ids.csv` | 08 | IDs that differed in step 07 |
| `results_pg_recheck.csv` | 08 | PG single-conn re-run of just the diff rows |
| `recheck_summary.txt` | 08 | Resolved-by-recheck vs still-differing tally |
