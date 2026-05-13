# Project orientation

DuckDB community extension `us_geocoder`: a pure-DuckDB port of PostGIS's `postgis_tiger_geocoder`. Geocodes US addresses against Census TIGER/Line data.

- **PG comparison + design decisions D1–D14:** [docs/pg_parity.md](docs/pg_parity.md). The locked design ledger + per-test divergence audit + condensed PG cascade reference all live here.
- **Public docs:** [README.md](README.md) (overview + quickstart pointer), [docs/quickstart.md](docs/quickstart.md), [docs/api.md](docs/api.md) (function reference), [docs/pg_parity.md](docs/pg_parity.md).
- **License:** GPLv2 (matches upstream).
- **DuckDB pin:** 1.5.2 (submodule `duckdb/`).

## Layout

```
src/
  us_geocoder_extension.cpp    # ExtensionLoader entrypoint; registers macros + C++ table fns
  loader.cpp                    # TIGER loader + set_tiger_reference + install_tiger_schema
  sql/*.sql.in                  # embedded SQL (macros, lookup seeds, schema DDL, loader templates)
test/sql/*.test                 # sqllogictest — 331 assertions, all deterministic (hand-built fixtures)
docs/                           # quickstart.md, api.md, pg_parity.md, UPDATING.md
```

Embedded SQL is inlined at build time via the CMake pipeline in [CMakeLists.txt](CMakeLists.txt) — each `.sql.in` becomes `us_geocoder::<Name>Sql()`. Token `@TIGER@` (data location) and `@FUNC@` (local-macro location) are substituted at runtime.

## Build / test loop

```sh
make release                    # ~10 min cold (builds DuckDB); ~30s hot
TIGER_TEST_EXTENSIONS=1 ./build/release/test/unittest "test/sql/*"  # full suite (331 assertions, 21 cases)
./build/release/test/unittest "test/sql/*"                          # CI-equivalent subset (93 assertions, 6 cases)
./build/release/test/unittest "test/sql/X.test"                     # single file
```

The `TIGER_TEST_EXTENSIONS=1` sentinel gates the 15 tests that `LOAD spatial` or exercise `tiger.from_pagc()` (which calls into `us_address_standardizer`). CI doesn't set it — building those extensions in the matrix is fraught (spatial's vcpkg deps differ per platform; us_address_standardizer is a C-API community extension). Locally, dev machines typically have both pre-installed in `~/.duckdb/extensions/`, so set the env var to run the full suite. Without it, you see the same 6-test subset CI runs.

The built CLI `build/release/duckdb` statically links the extension, so no `INSTALL`/`LOAD` needed for manual smoke tests.

## Key design constraints

- **Macros always live in local `tiger` schema.** They reference `tiger.<table>` unqualified. The 13 TIGER data tables (`state`, `county`, `place`, `cousub`, `zcta5`, `zip_state`, `zip_state_loc`, `zip_lookup_base`, `edges`, `faces`, `featnames`, `addr`, `edge_containment`) can be either base tables or views over an attached catalog — see [`set_tiger_reference`](docs/api.md#set_tiger_referencedatabase-varchar-schema-varchar-default-tiger--table).
- **Macro overloading syntax** is `CREATE OR REPLACE MACRO name (args1) AS body1, (args2) AS body2;` (single statement, comma-separated). Separate `CREATE MACRO` calls error on "already exists."
- **Struct fields in macros** must use bracket notation (`inp['zip']`), not dot. Dot gets parsed as `table.column`.
- **`soundex`** is **vendored** at `src/include/vendored_soundex.hpp` (MIT, originally from splink_udfs). Registered unconditionally by `LoadInternal`. No community-extension dependency for it; the macros and the loader's `name_soundex` precompute both call the locally-registered scalar.
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

**April 2026 follow-up hypothesis tests** (lived under `scripts/benchmarks/` — removed in tree-tidy commit; see git history if you want to rerun): wrote four targeted tests to isolate *why* parallel download and parallel ingest didn't pay off, expecting at least one hypothesis to be confirmed. **All four were falsified:**

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

## Geocoder performance: findings

Wins shipped during the May 2026 perf push (`perf/geocode-batch-planning` + follow-on commits). 100K mixed input on 32 GB-RAM hardware went from ~630 s + 10 GB peak tmp to **~205 s + 0 GB tmp** (≈3× wall-clock, zero spill). What landed and why:

**1. Per-state dispatch with literal statefp + ART pushdown** — non-negotiable architectural floor. The C++ `geocode_batch` table function partitions input rows by resolved statefp and dispatches one SQL per state with the literal `statefp_lit` baked in. Lets DuckDB constant-fold to ART pruning on `(statefp, tlid)` instead of building 65M-row hash tables on the unified TIGER tables. **Removing this layer re-introduces unbounded spill**: a parallel "unified" path test killed by watchdog at 21.8 GB peak tmp at 73s on 100K mixed.

**2. Slice cap (`kPerStateSliceCap`, default 10000)** — limits input rows fed to each per-state SQL dispatch. Original purpose was capping intermediate-fanout spill on big states (CA/TX/FL/NY); after the join_order workaround below, spill is gone and the cap is mostly cosmetic — sweep showed flat wall-clock from cap=10K to cap=50K, mild ~3% regression at cap=1M. Exposed as `us_geocoder_slice_cap` setting; users on tighter RAM can lower if needed (counter-intuitive: cap=10000 was *more* robust than cap=5000 at `memory_limit='8GB'` — cap=5000 OOM'd on Texas dispatch because more-concurrent-smaller dispatches contend harder on the buffer pool than fewer-larger ones).

**3. Precomputed equi-key columns on `tiger.featnames`** — `numeric_stem`, `name_first_5`, `fullname_first_5` populated at load. Replaces three `BLOCKWISE_NL_JOIN`s in the macro:
- `numeric_streets_equal(name, street_name)` regex+trim+regex+trim+compare — was the **227 s heaviest operator** in the CA-1K profile. Equi-join on `f.numeric_stem = p.street_name_numeric_stem` is ~6700× faster on that predicate.
- `name_lower LIKE input || '%'` — equi-join on `name_first_5 = substr(input, 1, 5)` plus a post-filter LIKE. Pass B keeps a short-input fallback (length<5) since the equi-key can't replicate prefix semantics for inputs shorter than the key.
- `fullname_norm LIKE prefix || '%'` — same shape as above.

**4. ANALYZE at end of every loader call** — refreshes planner stats after `load_tiger_*`. Was ~37% wall-clock improvement when first applied; statistics-driven join orders no longer pessimal. Loader does it once per call regardless of state count to avoid 50× redundant ANALYZEs.

**5. `list_contains` for `a.zip IN (SELECT UNNEST(window_zips))`** — 5 occurrences across the macros. The `IN (SELECT UNNEST(...))` shape couldn't be decorrelated cleanly; DuckDB wrapped it in tautological `struct_arg IS NOT DISTINCT FROM struct_arg` MARK joins eating 20 s of operator-time at CA-1K. `list_contains` is a scalar function — no subquery, no decorrelation, no MARK joins.

**6. CTE hoists for repeated subexpressions** (`fae_faces_a_scored`, `fae_faces_b_scored`, `NOT MATERIALIZED`):
- `via_primary` was inlined 4× into the rating CASE in `candidates_a` (an explicit comment in the source called it a workaround for the same-SELECT alias-reference limitation). Hoisted to one column.
- `hn_min` / `hn_max` (= `tiger.least_hn`/`greatest_hn`, each: 2 regex + 2 trim + 2 cast on house numbers) were called **7+ times per row** in NULL-guarded CASEs. Per [splink#2929](https://github.com/moj-analytical-services/splink/issues/2929), DuckDB's CSE optimization skips repeated subexpressions inside a CASE whose first branch is a NULL guard — exactly our shape. Hoisted to two columns.
- Both CTEs are flagged `NOT MATERIALIZED` so DuckDB's v1.4+ default doesn't hold the wider intermediate in RAM.

**7. `disabled_optimizers='join_order'` inside `geocode_batch`'s per-flush Connection** — biggest single win in the perf push: **47% wall-clock + spill 17 GB → 0 GB at 100K mixed**. Workaround for a planner cardinality misestimate (179M est vs 1.2M actual on a `IS NOT DISTINCT FROM` join from LATERAL+macro+correlated-subquery decorrelation). Acting on the bad estimate, `join_order` picks a strategy that builds an oversized hash table and spills. With `join_order` off, DuckDB falls back to SQL-clause-order joining, which on our query shape happens to be near-optimal (build hash on small input, probe through ART-pruned per-state TIGER slices). `disabled_optimizers` is a "DEBUG SETTING" per DuckDB docs — we apply it only to `geocode_batch`'s transient local Connection (not the user's main session) and expose `us_geocoder_disable_join_order` as the escape hatch (default true). Re-evaluate when DuckDB upstream fixes the underlying misestimate.

**8. Multi-state-ZIP + state-abbrev/ZIP union resolution** — `RunResolution` unions the state_lookup result with the zip_lookup_base result (DISTINCT) and dispatches to all candidates. Two layered improvements over PG:
- ZIP-only inputs in multi-state ZIPs (~7% of US ZIPs cross state lines) — PG uses `LIMIT 1`, arbitrary state wins, real match silently lost when ordered second. We dispatch to all candidate states.
- `state_abbrev` typo'd to a valid-but-wrong code while ZIP correctly resolves — PG's `COALESCE(state_lookup, zip_lookup_base)` lets state_abbrev win authoritatively. We dispatch to both. Mitigated for placeholder ZIPs (12345/99999/etc.): when ZIP-derived list size > 3 AND state_abbrev resolves, drop the ZIP set as unreliable. See [docs/pg_parity.md](docs/pg_parity.md) divergences D and E.

**Architectural lessons:**
- **Cardinality misestimates on synthesized columns (decorrelation residue) aren't fixable by `ANALYZE`.** The planner's estimate on `#1 IS NOT DISTINCT FROM #17` joins is on anonymous correlation columns, not real table stats. The misestimate has to be neutered at the optimizer-rule level (or via SQL rewrite that avoids the decorrelation).
- **`NOT MATERIALIZED` matters since DuckDB v1.4** — the materialize-by-default change in PR #17459 means hoist-as-CTE patterns now silently materialize wider intermediates unless explicitly told not to.
- **More-concurrent-smaller dispatches contend harder on the buffer pool than fewer-larger ones.** Counter-intuitive: cap=5000 OOM'd at `memory_limit='8GB'` while cap=10000 ran fine in the same constrained env. The intuition "smaller per-dispatch peak = safer" is wrong; what actually fits is "fewer simultaneously-live operator pipelines."
- **Falsified hypothesis: per-state physical sharding.** Tested empirically (perf/per-state-tables, deleted): physical `pst.featnames_<sfp>` tables vs unified+ART-pushdown gives 12% wall-clock gain but **2× peak RSS** because inter-state pipeline parallelism multiplies hash builds. ART pushdown ≈ physical sharding for the hot path. See `project_perstate_sharding_falsified.md`.

## Common traps

- **`CREATE TYPE`** isn't idempotent across DB reopens; use `CREATE TYPE IF NOT EXISTS`.
- **`lpad` / `regexp_*`** come from `core_functions`; `LoadInternal` explicitly `LOAD`s it.
- **`information_schema.tables` returns rows from all attached catalogs.** Filter by `table_catalog = current_catalog()` when writing cross-DB tests.
- **LATERAL + LEFT JOIN + correlated CTE** is not supported by DuckDB's planner in some shapes — restructure as CTEs-before-join.
- **Stage A 5B-row cartesian** has a known shape: `OR`-based face-side join. Split L/R into `UNION ALL` branches (see `geocode_address.sql.in`).
- **Test fixtures must populate precomputed columns** on `featnames` — six in total: `name_lower`, `fullname_norm`, `name_soundex` (used by all branches) plus `numeric_stem`, `name_first_5`, `fullname_first_5` (used by the equi-join replacements for `numeric_streets_equal` regex and prefix-LIKE `BLOCKWISE_NL_JOIN`s). Fixture INSERTs in `test/sql/*.test` either populate them inline or backfill via an `UPDATE` block — see e.g. `geocode_batch_multistate.test`.

## Workflow preferences

- Prefer editing `.sql.in` over regenerating macros from scratch.
- Run the full sqllogic suite after any SQL change; individual file runs miss regression interactions.
- Don't change rating weights or the "location ratings ≥ 100" invariant without an explicit spec amendment — downstream consumers depend on the total order.
- Small focused commits per phase/feature. Commit messages reference design decisions D1–D14 from [docs/pg_parity.md](docs/pg_parity.md) where applicable.

## Roadmap / deferred

Active deferred work:
- 10k-row random-sample parity corpus from a loaded TIGER state — catches scoring/parser regressions outside the curated 51-case stress set.
- Submit `description.yml` to `duckdb/community-extensions` so users can `INSTALL us_geocoder FROM community`. CI matrix already builds the 5 platforms the registry expects (linux_amd64/arm64, osx_arm64, windows_amd64, windows_amd64_mingw).
- File DuckDB upstream issue for the 179 M `IS NOT DISTINCT FROM` cardinality misestimate that the join_order workaround currently sidesteps. Same shape as splink#3023 / splink#2929.
- Real parallel HTTPS loads at the loader layer — only viable path left is upstream `UNION ALL` + `ST_Read` parallelization in DuckDB/spatial. `read_blob` prefetch / parallel INSERT / parallel CTAS were all tried and reverted (see Loader performance notes above).
- Parquet distribution (prebuilt per-state parquet for faster first-run UX) — bypasses shapefile-parse + INSERT-write entirely. Only remaining path to >3× loader speedup.
- All-states pre-download → local-ingest → cleanup wrapper. Loop [`scripts/parallel_download_state.sh`](scripts/parallel_download_state.sh) over 50+DC, ingesting each state then `rm -rf` of its zips before the next, so disk stays bounded by `max(state_size)` (~10 GB worst case for TX).

Falsified hypotheses (don't re-attempt without new information):
- **Per-state TIGER tables (storage sharding).** Tested May 2026 on `perf/per-state-tables` (deleted): 12% wall-clock gain over unified+ART-pushdown but **2× peak RSS** because inter-state pipeline parallelism multiplies hash builds. ART pushdown gives equivalent scan shape without the schema refactor. See project memory `project_perstate_sharding_falsified.md`.
- **Removing per-state dispatch entirely.** With the join_order workaround on, runtime-statefp queries spilled 21.8 GB at 73 s on 100K mixed before being killed by watchdog. Per-state dispatch + literal statefp + ART pushdown is the architectural floor.
- **Smaller slice cap = safer on smaller-RAM machines.** Inverse turned out to be true: at `memory_limit='8GB'`, cap=5000 OOM'd on Texas dispatch while cap=10000 ran fine. Fewer-larger dispatches stream more cleanly than many-smaller through a constrained buffer pool. Default ships at 10000.
