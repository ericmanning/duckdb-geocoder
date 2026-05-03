# Project orientation

DuckDB community extension `us_geocoder`: a pure-DuckDB port of PostGIS's `postgis_tiger_geocoder`. Geocodes US addresses against Census TIGER/Line data.

- **Spec:** [geocode_flow.md](geocode_flow.md) — 1,093-line semantic spec, locked design decisions D1–D14. This is the source of truth; consult it before making architectural changes.
- **Public docs:** [README.md](README.md), [docs/api.md](docs/api.md), [docs/parity.md](docs/parity.md).
- **License:** GPLv2 (matches upstream).
- **DuckDB pin:** 1.5.2 (submodule `duckdb/`).

## Layout

```
src/
  us_geocoder_extension.cpp    # ExtensionLoader entrypoint; registers macros + C++ table fns
  loader.cpp                    # TIGER loader + set_tiger_reference + install_tiger_schema
  sql/*.sql.in                  # embedded SQL (macros, lookup seeds, schema DDL, loader templates)
test/sql/*.test                 # sqllogictest — 216 assertions, all deterministic (hand-built fixtures)
docs/                           # api.md, parity.md
```

Embedded SQL is inlined at build time via the CMake pipeline in [CMakeLists.txt](CMakeLists.txt) — each `.sql.in` becomes `us_geocoder::<Name>Sql()`. Token `@TIGER@` (data location) and `@FUNC@` (local-macro location) are substituted at runtime.

## Build / test loop

```sh
make release                    # ~10 min cold (builds DuckDB); ~30s hot
./build/release/test/unittest "test/sql/*"       # full suite
./build/release/test/unittest "test/sql/X.test"  # single file
```

The built CLI `build/release/duckdb` statically links the extension, so no `INSTALL`/`LOAD` needed for manual smoke tests.

## Key design constraints

- **Macros always live in local `tiger` schema.** They reference `tiger.<table>` unqualified. The 13 TIGER data tables (`state`, `county`, `place`, `cousub`, `zcta5`, `zip_state`, `zip_state_loc`, `zip_lookup_base`, `edges`, `faces`, `featnames`, `addr`, `edge_containment`) can be either base tables or views over an attached catalog — see [`set_tiger_reference`](docs/api.md#set_tiger_referencedatabase-varchar-schema-varchar-default-tiger--table).
- **Macro overloading syntax** is `CREATE OR REPLACE MACRO name (args1) AS body1, (args2) AS body2;` (single statement, comma-separated). Separate `CREATE MACRO` calls error on "already exists."
- **Struct fields in macros** must use bracket notation (`inp['zip']`), not dot. Dot gets parsed as `table.column`.
- **`soundex`** lives in the `splink_udfs` community extension (auto-loaded best-effort in `LoadInternal`).
- **`st_read` + `/vsizip//vsicurl/`** is the HTTP ingestion path. UNION-ALL batching across counties does **not** parallelize in practice (measured 8m batched vs ~5m serial for NJ on DuckDB 1.5 + spatial) — serial per-county INSERTs is the shipped loop.
- **Local source layout is Census-nested only.** `BuildVsiPath` uses `<source>/<SUBDIR>/<zip>.zip/<inner>` for both HTTP and local — no flat-layout fallback. Users point the loader at a mirror of `TIGER<year>/`.
- **Loader state-list API.** `load_tiger_state(VARCHAR)` and `load_tiger_states(VARCHAR[])` share one bind-data structure (`std::vector<StatePlan>`); `load_tiger_all_states()` resolves the 50+DC list at bind time from `state_lookup WHERE statefp::INT BETWEEN 1 AND 56`.
- **C++11** is the extension ABI baseline — no `inline constexpr std::string_view`, no structured bindings in public headers.

## Loader performance: what worked and what didn't

Benchmarked on NJ (21 counties, 86 zips, ~800K edges) in April 2026. Details in the memory roadmap; highlights:

**Worked:**
- Census-nested local mirror + parallel shell download ([`scripts/parallel_download_state.sh`](scripts/parallel_download_state.sh)): 26 s for all 89 NJ zips vs several minutes serial. Real win when users can run a shell script first.
- `unload_state` DELETE coverage for all 13 TIGER data tables (fixed the `edge_containment` doubling bug).

**Didn't work — measurements were the lesson:**
- **Parallel HTTP prefetch via `read_blob` inside the loader.** Sounded like an obvious win; measured ~15 s saved on a 7-min NJ load. Likely explanation: Census CDN supports HTTP Range (verified: `accept-ranges: bytes`, 206 Partial Content on a range probe), and GDAL's `/vsicurl/` is designed to exploit that, so serial `/vsicurl/` reads were already doing partial fetches. Our parallel full-zip downloads moved more total bytes and approximately canceled. (Not independently verified via `tcpdump`; if you want the true byte breakdown, measure first before rebuilding this.)
- **Parallel ingest via K worker Connections + concurrent INSERTs.** Sounded like the obvious next win; measured ~5% saved (556 s → 528 s) with user CPU going *up*. DuckDB's task scheduler was already running each "serial" INSERT at ~6 threads via internal parallelism — outer threading just oversubscribed cores.

**Methodology bite-marks:**
- Always benchmark before promising order-of-magnitude speedups. My estimates were off by 10× in both cases.
- Compare `user`/`real` CPU-time ratios to see whether DuckDB is already saturating cores — if user/real is already ~N, adding N more outer workers won't help.
- Benchmark cold vs warm caches separately. Network variance and CDN warming make consecutive runs non-comparable.

**April 2026 follow-up hypothesis tests** ([`scripts/benchmarks/`](scripts/benchmarks/)): wrote four targeted tests to isolate *why* parallel download and parallel ingest didn't pay off, expecting at least one hypothesis to be confirmed. **All four were falsified:**

- `/vsicurl/` is NOT doing partial Range fetches — we measured 128% of full-zip bytes on the wire during a serial load. Census does support Range; GDAL apparently isn't using it for shapefile reads. So commit 1's "parallel prefetch moves more bytes" story was wrong — both paths move roughly full-zip bytes.
- DuckDB is NOT internally parallelizing `INSERT ... FROM ST_Read(...)` — `threads=1` vs `threads=14` ran the same INSERT in 0.45s vs 0.41s. So commit 2's "outer parallelism oversubscribes cores" story was also wrong; DuckDB wasn't using those cores for the INSERT.
- 4 separate CLI **processes** reading 4 distinct local shapefiles only got 1.10× speedup, and in-process UNION-ALL only got 1.35×. GDAL doesn't have an obvious in-process driver lock (processes would have worked); but *something* OS-level is capping concurrency on small workloads (possibly disk I/O queue + CLI startup cost).
- Scratch-table CTAS was *slower* than shared-target UNION-ALL INSERT, not faster — so MVCC write contention isn't the bottleneck either.

**Follow-up at NJ scale** (`BENCH_STATE=NJ BENCH_FIPS=34`, 21 counties, ~835 MB zips): the RI-scale caveat was right. NJ reveals a real signal RI hid — parallel `SELECT … FROM ST_Read(…) UNION ALL …` with threads=4 gets **~2× speedup**. But the same UNION-ALL shape wrapped in `INSERT INTO … SELECT …` stays at 1.06× (no speedup).

**Parse parallelizes; write serializes.** That's the actual story. Row-group allocation / WAL / something in the INSERT-write path doesn't scale with concurrency, and it cancels the parse-phase gain.

**April 2026 commit-3 attempt** (parse-parallel → serialized-write architecture): **also failed.** I built K worker Connections each running `CREATE TABLE scratch.<tbl>_<cfp> AS SELECT ... FROM ST_Read(...)` into a distinct scratch schema, then a main-thread UNION-ALL merge into the real targets, then `DROP SCHEMA CASCADE`. On NJ: **parallel 604s vs legacy serial 405s — 1.5× SLOWER.** Likely causes: 84 total scratch tables add catalog-lock contention during the parallel CTAS phase, and the merge step is essentially a full second pass over the data (scan 21 scratch tables per target, INSERT into real target) that serial never has to do. Test C's 2× signal on raw `SELECT … FROM ST_Read(…) UNION ALL …` does NOT translate to CTAS-concurrent-across-Connections at 84-table scale.

**Three strategies attempted, all reverted:** parallel HTTP prefetch (commit-1 attempt), parallel INSERT-to-shared (commit-2 attempt), parallel CTAS-into-scratch + merge (commit-3 attempt). Pattern is clear: **no application-layer parallelism strategy at our code layer beats serial loading at NJ scale on this hardware.**

**May 2026 GDB/GPKG hybrid investigation** (`feature/gdb` branch, deleted): explored using TIGER GeoPackage / Geodatabase formats published by Census ([TGRGDB25](https://www2.census.gov/geo/tiger/TGRGDB25/) / [TGRGPKG25](https://www2.census.gov/geo/tiger/TGRGPKG25/)). Per-state GDB bundles edges + place + cousub + blocks in 1 zip vs the shapefile distribution's 4-zip-per-county fan-out. Pitched as ~50% fewer HTTP requests for state loads.

  Findings (all empirical):

  - **Per-state edges All_Lines layer has the parsed name fields and TNIDF/TNIDT** that we need (verified via `ogrinfo` + sample reads). PREDIR/SUFTYP/etc. are stored as numeric MAF/TIGER codes, not abbrevs — the PDF doc says "Expanded text" but that's wrong; they're codes. Need a code→abbrev lookup table to use them.
  - **GDB has no `faces` layer** at all (any flavour: per-state, nationgeo, substategeo). The TFID join key our `edge_containment` SQL uses is gone. Tried replacing with `ST_Within(midpoint_offset, Block20.geom)` spatial join — works on RI (0.4 s) but **takes >17 minutes on NJ alone** before being killed. DuckDB's planner produces `BLOCKWISE_NL_JOIN` for `ST_Intersects` in joins regardless of RTree presence (only filter-pushdown uses RTree). NJ-scale spatial join is not viable.
  - **GDB has no `featnames` table** with multiple alt-name rows per TLID (shapefile featnames is N:1). Lost data.
  - **`/vsicurl/` does HTTP Range requests on FileGDB** (verified — small layer reads are <0.5s vs 2.2s full curl), unlike its shapefile behaviour. So multi-layer reads from one zip don't pay full-zip-per-layer cost.
  - **Hybrid benchmarked** (GDB for big files + shapefile for `faces`+`featnames`+`addr` parity, with national `addr.gdb` for per-segment addr ranges): NJ + nation **1.43→1.87 GB DB**, **305→348 s wall** (14% slower), **824→1,336 MB downloaded** (62% more). Per-state ingest essentially tied (300 vs 293 s). All overhead is in the 38 M-row `addr.gdb` staging step that only amortizes at >10-state loads.
  - **Even with patches, parity isn't achieved.** GDB `All_Lines` returns `MULTILINESTRING` while shapefile EDGES returns `LINESTRING`; `All_Lines` includes hydro/rails/legal alongside roads (52K extra NJ rows we'd need to filter out via `ROADFLG/MTFCC`); `edges.countyfp` derived via spatial test differs on 6,279 boundary-spanning edges.

  Net: **slower, bigger, and not byte-identical**. Branch deleted. Don't repeat unless the use case shifts to multi-state-wide loads where the addr.gdb amortisation plus 50% HTTP-request reduction outweighs the perf and parity costs.

**The only remaining path to >3× loader speedup** is option 2: pre-built Parquet distribution. Bypasses shapefile-parse *and* INSERT-write *and* all the parallelism dead-ends.

**Where the real wins are:** removing work, not parallelizing it. `edge_containment` precompute is ~1–2 min per state and many users don't need GEOIDs — making it opt-out is the one remaining in-process lever that actually moves the needle.

## Common traps

- **`CREATE TYPE`** isn't idempotent across DB reopens; use `CREATE TYPE IF NOT EXISTS`.
- **`lpad` / `regexp_*`** come from `core_functions`; `LoadInternal` explicitly `LOAD`s it.
- **`information_schema.tables` returns rows from all attached catalogs.** Filter by `table_catalog = current_catalog()` when writing cross-DB tests.
- **LATERAL + LEFT JOIN + correlated CTE** is not supported by DuckDB's planner in some shapes — restructure as CTEs-before-join.
- **Stage A 5B-row cartesian** has a known shape: `OR`-based face-side join. Split L/R into `UNION ALL` branches (see `geocode_address.sql.in`).
- **Test fixtures must populate precomputed columns** (`name_lower`, `fullname_norm`, `name_soundex` on `featnames`) or name-match branches silently miss.

## Workflow preferences

- Prefer editing `.sql.in` over regenerating macros from scratch.
- Run the full sqllogic suite after any SQL change; individual file runs miss regression interactions.
- Don't change rating weights or the "location ratings ≥ 100" invariant without an explicit spec amendment — downstream consumers depend on the total order.
- Small focused commits per phase/feature. Commit messages reference phase numbers from [geocode_flow.md](geocode_flow.md) where applicable.

## Roadmap / deferred

See end of [geocode_flow.md § 14.9](geocode_flow.md). Active deferred work:
- Phase 12: PG-regress parity ports + 10k-row random-sample corpus.
- Phase 14: cross-platform CI via [.github/workflows/MainDistributionPipeline.yml](.github/workflows/MainDistributionPipeline.yml) + community-extensions submission.
- Real parallel HTTPS loads (`read_blob` prefetch, or upstream `UNION ALL` + `ST_Read` parallelization fix in DuckDB/spatial).
- Parquet distribution (prebuilt per-state parquet for faster first-run UX).
