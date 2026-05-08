# Geocoder parity-and-timing benchmark

End-to-end comparison of `us_geocoder` (DuckDB) vs `postgis_tiger_geocoder` (PostgreSQL) over a user-supplied nationwide address corpus. Outputs:

- **Per-row diff** of the picked address (`pprint_adr(adr)`), `rating`, and `(lng, lat)`.
- **Wall-clock timings** at multiple sizes and DuckDB thread counts.

## Layout

- [`pg/`](pg/) — Dockerfile + init.sql + `load_tiger_via_pg.sh`. Builds the PG 16 + PostGIS + `address_standardizer` + `postgis_tiger_geocoder` image used by both the benchmark and the regress harness. Clone the upstream `postgis_tiger_geocoder` mirror into `pg/tiger_geocoder/` (gitignored) before first build.
- [`geocode/`](geocode/) — full nationwide PG-vs-DuckDB benchmark (this README).
- [`regress/`](regress/) — fixed-test PG-parity harness: runs PG's `geocode_regress.sql` / `reverse_geocode_regress.sql` inputs through *our* geocoder and diffs against the PG-2025-PAGC oracle in [`test/parity/upstream/`](../test/parity/upstream/). See [docs/pg_parity.md § How to verify parity](../docs/pg_parity.md#how-to-verify-parity).

Mirrors the structure of the [duckdb-address-standardizer benchmark/postgis/](https://github.com/ericmanning/duckdb-address-standardizer/tree/main/benchmark/postgis) suite, adapted to the geocoder's reality:

- **PG runs inside Docker.** Building PostGIS from source via Homebrew is a 1–3hr ordeal we avoid by reusing the [pg/Dockerfile](pg/Dockerfile) image (PG 16 + PostGIS + `address_standardizer` + `postgis_tiger_geocoder`).
- **TIGER is loaded into DuckDB via the Census CDN, then mirrored into PG.** The geocoding-quality comparison only needs PG to *have* the same TIGER data — not to re-load it from scratch (which would be 5–15 hours of redundant Census fetches). PG-side LOAD timing for the writeup is recovered separately on a 2-state subset (`03c`).
- **The DuckDB benchmark database persists across runs.** Once `03_load_tiger.sh` populates it (~50 GB of nationwide TIGER tables), subsequent steps reuse those tables. The load scripts use `CREATE OR REPLACE` on the address table only; the `tiger.*` schema is left intact so the same DB can later be repurposed (e.g. as the source for a prebuilt-parquet distribution).

## Prereqs

- macOS with Docker and ~150 GB free disk (~50 GB per engine for TIGER).
- An input table (`.csv` or `.parquet`) with columns `addr1, addr2, city, state, zip` (and optional `id`).

```bash
export INPUT_FILE=/path/to/addresses.csv
```

## Running

```bash
cd benchmark/geocode
./00_setup_pg.sh                   # ~5 min cold (Docker image build); idempotent
./01_setup_duckdb.sh               # ~10 min cold (DuckDB rebuild); idempotent
./02_load_addresses.sh             # seconds — CREATE OR REPLACE on `bench_input` in both engines
./03_load_tiger.sh                 # ~2-4h — DuckDB pulls TIGER from Census CDN. Resumable.
./03b_pg_mirror_from_duckdb.sh     # ~minutes — stream-mirror DuckDB tiger.* → PG
./03c_pg_load_2states_for_timing.sh   # OPTIONAL — time PG's native loader on CA + KY only
./04a_verify_pg.sh                 # ~5 s — eyeball one address through PG
./04b_verify_duckdb.sh             # ~5 s — eyeball one address through DuckDB
./05_bench_pg.sh                   # PG timings at SIZES × RUNS
./06_bench_duckdb.sh               # DuckDB timings at SIZES × THREADS × RUNS
./07a_export_pg.sh                 # PG full-dataset export (parallel \COPY)
./07b_export_duckdb.sh             # DuckDB full-dataset export (run in parallel with 07a)
./07c_compare.sh                   # diff results_pg.csv vs results_duckdb.csv
./08_recheck_diffs.sh              # only if 07c found diffs
```

`07a` and `07b` are independent — run them concurrently to halve wall-clock on the full-dataset export. `07c` is pure local CSV processing once both finish.

`03b` is the key shortcut: mirroring DuckDB's loaded TIGER into PG saves ~5–15 hours of PG re-downloading every Census shapefile. PG-side LOAD timing for the writeup is recovered via `03c` (CA + KY only — representative state-size extremes — into an isolated DB so the benchmark's mirrored PG isn't disturbed).

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

## Output

All results land in `$RESULTS_DIR` (default `/tmp/geocoder_bench_results/`):

| File | Step | Contents |
|---|---|---|
| `addresses.csv` | 02 | Normalized form of `$INPUT_FILE` (with synthesized `id` if needed) |
| `load_tiger_duckdb.log` | 03 | DuckDB loader log |
| `mirror_pg_from_duckdb.log` | 03b | DuckDB→PG mirror log |
| `pg_load_timing_2states.log` | 03c | PG-native loader timing on CA + KY |
| `timings.tsv` | 05/06 | One row per `(engine, size, run, threads, seconds)` |
| `results_pg.csv` | 07a | `id, input_state, rating, lng, lat, addy_text` |
| `results_duckdb.csv` | 07b | Same shape, DuckDB |
| `diff_summary.txt` | 07c | Field-level diff counts + per-state strict-match table |
| `sample_diffs.tsv` | 07c | First 50 mismatched rows for eyeballing |
| `diff_ids.csv` | 08 | IDs that differed in step 07c |
| `results_pg_recheck.csv` | 08 | PG single-conn re-run of just the diff rows |
| `recheck_summary.txt` | 08 | "Now matching" vs "still differing" tally |

## Diff classifier

A row that diverges at step 07c falls into one of three buckets after step 08:

1. **PAGC parallel state-leak (cosmetic).** Resolves on single-conn recheck. PG's PAGC has in-process state; with `PARALLEL UNSAFE` set we mostly avoid this, but per-worker caching at the connection level still leaks across rows.
2. **Real engine difference.** Doesn't resolve. These are the rows worth investigating. See [docs/pg_parity.md](../docs/pg_parity.md) for the existing taxonomy (Mechanism A/B/C, PG heap row-order tiebreak, rating path-divergence).
3. **TIGER vintage drift.** Both engines were loaded from Census 2025; if a row resolved differently, the data isn't the cause. Listed for completeness; in practice this bucket is empty since both pulls happen within hours of each other.

## Comparison criteria

- `rating` (integer): exact match.
- `addy_text` (`pprint_adr(adr)`): exact text match.
- `lng`, `lat` (double): exact match — **no truncation**. Sub-meter floating-point drift will surface as divergences. Filter post-hoc on `abs(pg_lat - dk_lat) < 1e-6` if you want a tolerance.

## Why TIGER load takes hours

PG's tiger loader is a per-state shell-script generator that downloads each county-level zip serially via `wget`, populates per-state partitions, and runs `CREATE INDEX` over edges/faces/etc. Wall-clock time is dominated by sequential HTTP fetches (~40s/state × 51 states ≈ 35 min just for downloads, plus the COPY + index time).

DuckDB's loader is in-process — calls `ST_Read('/vsicurl/https://www2.census.gov/geo/tiger/TIGER2025/...')` directly via httpfs. Per [CLAUDE.md § Loader performance](../CLAUDE.md), nationwide via Census CDN is ~3–4 hours. The benchmark only fully runs the DuckDB side (`03`) and mirrors into PG (`03b`); `03c` measures PG's native loader on a 2-state subset for the writeup.

## Preserving the loaded TIGER

The DuckDB benchmark database (`$DUCKDB_DB`, default `/tmp/geocoder_bench.duckdb`) holds the nationwide TIGER tables (~50 GB) after `03_load_tiger.sh`. The benchmark scripts never delete it — they `CREATE OR REPLACE` the `addr` table only. Reuse the DB downstream:

- Source for a prebuilt-parquet distribution (deferred follow-up).
- Cached reference for ad-hoc geocoding.
- Persistent oracle that doesn't require re-fetching from Census.

To start fresh, `rm -f $DUCKDB_DB`.
