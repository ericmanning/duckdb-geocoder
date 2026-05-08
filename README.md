# us_geocoder

A DuckDB community extension that geocodes US addresses against [Census TIGER/Line](https://www.census.gov/geographies/mapping-files/time-series/geo/tiger-line-file.html) data. A pure-DuckDB rewrite of PostGIS's [`tiger_geocoder`](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder).

Given a street address, it returns (a) a point in NAD83 coordinates interpolated along the street centerline with a 10m perpendicular side-of-street offset, (b) the 2025 census block / tract / block-group GEOIDs covering that point, and (c) a `rating` that lower-bounds match quality (0 = perfect).

**Status:** alpha. Core geocoder is functional end-to-end against full TIGER data; see [docs/parity.md](docs/parity.md) for where v0.1 diverges from PostGIS. Future versions are likely to further diverge from PostGIS as improvements are made against ground-truth parcel data.

## Install

```sql
INSTALL us_geocoder FROM community;
INSTALL spatial;
INSTALL us_address_standardizer FROM community;  -- optional (for raw-string inputs)

LOAD us_geocoder;
LOAD spatial;
LOAD us_address_standardizer;
```

## Load TIGER data

From the Census (default — needs core `httpfs` that should be bundled already, as with `spatial`):

```sql
CALL load_tiger_nation(year := 2025);          -- one-time: state, county, zcta5
CALL load_tiger_state('RI');                   -- one state
CALL load_tiger_states(['RI','MA','CT']);      -- several states in one call
CALL load_tiger_all_states();                  -- 50 states + DC
```

`load_tiger_state[s]` must run after `load_tiger_nation`. RI loads in ~45s from the Census CDN, ~30s from a local mirror.

Local sources need a **Census-nested** layout (`STATE/`, `COUNTY/`, `EDGES/`, `FACES/`, `FEATNAMES/`, … under a single root) — see [docs/api.md § Local source layout](docs/api.md#local-source-layout) for the full subdirectory map.

```sql
CALL load_tiger_nation('/data/tiger_2025');
CALL load_tiger_states(['RI','MA'], '/data/tiger_2025');
```

Pass `build_containment := false` to skip the per-state `edge_containment` precompute (saves ~1–2 min/state) if you don't need block / tract / block-group GEOIDs. Populate later with `CALL build_edge_containment(['RI','MA'])`.

### Resumable loads

Loads checkpoint to `tiger.loader_progress` at per-(state, county, table) granularity, so re-running a `load_tiger_*` call skips work that already completed. Cancel and re-run safely; a partial state (e.g. AK failed mid-load on county 016) resumes by redoing only the missing per-county-per-table pieces — counties that already loaded are skipped, counties whose `edges` made it but `faces`/`featnames`/`addr` didn't get patched up. HTTP fetches retry up to 3× with backoff (2s/5s/15s) and switch to a cache-busting query string on retry to bypass any stale Cloudflare edge response. To force a re-load of a state, `CALL unload_tiger_state('AK')` clears its data + progress entries.

## Reference databases (attached catalogs)

The 13 TIGER data tables can live in the current database, a read-write attached catalog, or a read-only attached catalog. Macros stay local; only data moves. See [docs/api.md § Reference databases](docs/api.md#reference-databases) for the full contract; quick examples:

```sql
-- Read-write: load into 'work' attached DB, repoint local tiger.* at it.
ATTACH 'work.duckdb' AS work;
CALL load_tiger_state('RI', target_db := 'work');
CALL set_tiger_reference('work');
SELECT * FROM tiger.geocode(...);

-- Portable: ship a prebuilt DB to an air-gapped env, attach READ_ONLY.
ATTACH 'tiger_us_2025.duckdb' AS ref (READ_ONLY);
CALL set_tiger_reference('ref');
SELECT * FROM tiger.geocode(...);
```

## Geocode

The quickest path — a free-form single-string address:

```sql
SELECT rating, (adr).address, (adr).street_name, ST_AsText(geom), block_geoid
FROM tiger.geocode('120 Benefit St, Providence RI 02903');
```

This routes through [`tiger.from_pagc()`](docs/api.md) (PAGC standardizer) and dispatches to the full geocoder with defaults (`max_results=10`, no spatial restriction, no containment filter). Requires `us_address_standardizer` to be installed — auto-loaded on first use.

For full control, pass a `geocode_input` struct and tune `max_results` / `restrict_geom` / `require_containment`:

```sql
SELECT rating, (adr).address, (adr).street_name, (adr).location,
       ST_AsText(geom), block_geoid, tract_geoid, containment_guaranteed
FROM tiger.geocode(
    CAST({
        address:      120,
        street_name:  'Benefit',
        street_type:  'St',
        pre_dir:      NULL,
        post_dir:     NULL,
        location:     'Providence',
        state_abbrev: 'RI',
        zip:          '02903'
    } AS tiger.geocode_input),
    3,          -- max_results
    NULL,       -- restrict_geom
    'none'      -- require_containment: 'none' | 'block' | 'tract' | 'blkgrp'
);
```

→
```
rating=0, address=120, street_name='Benefit', location='Providence',
geom=POINT(-71.40882 41.82993), block_geoid=440070031004024,
tract_geoid=44007003100, containment_guaranteed=true
```

See [docs/api.md](docs/api.md) for the full reference, including `geocode_intersection`, `reverse_geocode`, and the rating scale.

### Batch: a table of addresses

For nationwide-scale batches use `geocode_batch`, a C++ table function that buffers input rows, partitions by resolved state, and dispatches one per-state SQL per state — letting the planner prune the big TIGER tables to one state at a time:

```sql
SELECT id, rating, lng, lat, adr_text, block_geoid, containment_guaranteed
FROM geocode_batch((SELECT id, address AS addr_str FROM my_table));
```

Two input shapes are accepted: free-form `addr_str` (parsed via `tiger.from_pagc` internally) or pre-parsed columns matching `tiger.geocode_input` field names. Any non-recognized columns pass through to the output.

**Tunable: `us_geocoder_slice_cap`.** Per-state SQL dispatch is sliced when a single state's input bucket exceeds this cap (default 10000). Lowering helps on tight-RAM machines that hit `memory_limit` blockers; raising rarely helps because the wall-clock-vs-cap curve is U-shaped (memory pressure rises faster past the optimum than plan-overhead falls).

```sql
SET us_geocoder_slice_cap = 5000;     -- session
SET LOCAL us_geocoder_slice_cap = 5000;  -- single statement
```

**Tunable: `us_geocoder_disable_join_order`** (BOOLEAN, default `true`). Disables DuckDB's `join_order` optimizer inside `geocode_batch`'s transient per-flush Connection — workaround for a planner cardinality misestimate that otherwise pushes a spill-heavy plan (370 s + 17 GB tmp → 196 s + 0 GB tmp at 100K mixed). Set to `false` to let DuckDB's default behavior win, e.g. on a future DuckDB release that fixes the underlying misestimate.

See [docs/api.md](docs/api.md#geocode_batchinput-table--table) for the full reference.

Alternative for one-off / interactive queries: `tiger.geocode` via `CROSS JOIN LATERAL`. It's slower at scale (no per-state dispatch — DuckDB can't push a runtime statefp into the unified TIGER tables) but works fine for small inputs:

```sql
WITH inputs AS (
    SELECT id, address AS _raw FROM my_table   -- alias to avoid a name clash below
)
SELECT inputs.id, inputs._raw AS address,
       g.rating, ST_X(g.geom) AS lng, ST_Y(g.geom) AS lat,
       g.block_geoid, g.containment_guaranteed
FROM inputs
CROSS JOIN LATERAL tiger.geocode(_raw) AS g;
```

Two DuckDB binder quirks worth knowing for the LATERAL form:

- **Reference the column unqualified inside the LATERAL call** (`tiger.geocode(_raw)`, not `tiger.geocode(inputs._raw)`). When the outer source is a CTE, DuckDB mis-parses `alias.col` as struct-field access on a row.
- **Don't reuse the CSV column name as an output alias.** `SELECT ... address` while the input column is also `address` triggers "column cannot be referenced before it is defined." Rename one side (the CTE column, above) to avoid the clash.

Working end-to-end scripts in [scripts/demo/](scripts/demo/): `build_nj_db.sql` builds a portable NJ reference DB from the Census CDN, then `geocode_addresses.sql` (structured input) and `geocode_raw.sql` (free-form single-string input) each read a CSV and write a geocoded CSV.

## Runtime dependencies

| ext | source | required for |
|---|---|---|
| `spatial` | core | everything |
| `httpfs` | core | HTTP source mode only (`load_tiger_*` without a local source) |
| [`us_address_standardizer`](https://duckdb.org/community_extensions) | community | `from_pagc()` + `geocode(VARCHAR)` overload (not strictly required — struct-input always works) |

`soundex` is built in (vendored MIT from [splink_udfs](https://github.com/moj-analytical-services/splink_udfs); see [LICENSE-vendored](LICENSE-vendored)) — no community dep.

## Building

```sh
git submodule update --init --recursive
make release                   # ~10 min first time (builds duckdb from source)
make test                      # 218 assertions across 14 sqllogictest files
```

The build produces a loadable extension at `build/release/extension/us_geocoder/us_geocoder.duckdb_extension` and a DuckDB CLI at `build/release/duckdb` with the extension statically linked.

## Performance

Benchmarked on TIGER 2025 Rhode Island (136K edges, 126K featnames, 105K addr) on a 2024 M4 Max:

| operation | time |
|---|---|
| single geocode (warm) | 130–200 ms |
| batch of 100 addresses | 20 ms/addr |
| batch of 1,000 addresses | 17 ms/addr (~60/sec/thread) |
| full state load (local source, RI) | ~25 s |
| full state load (Census HTTP, RI) | ~45 s |
| full state load (Census HTTP, NJ) | ~5–8 min |

### Faster HTTP loads

The Census CDN path issues one HTTPS fetch per `(county, table-type)` via GDAL's `/vsicurl/`, which doesn't parallelize well for large states (NJ ≈ 8 min, CA ≈ 30 min). The fix is a parallel pre-download into a Census-nested local mirror + local ingest:

```sh
./scripts/parallel_download_state.sh NJ ./tiger_nj        # ~26 s for all 89 NJ zips
./build/release/duckdb tiger_nj.duckdb <<'EOF'
CALL load_tiger_nation('./tiger_nj');
CALL load_tiger_state('NJ', './tiger_nj');                -- ~3-4 min local ingest
EOF
```

The script uses `xargs -P 16 curl` and scrapes the Census directory index for county-level enumeration — no hardcoded per-state FIPS list. See [scripts/parallel_download_state.sh](scripts/parallel_download_state.sh) for knobs (parallelism, year, destination).

## License

GPLv2 — same license as the upstream [postgis_tiger_geocoder](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder). See [LICENSE](LICENSE).

### Vendored third-party code

`src/include/vendored_soundex.hpp` is a Soundex encoder copied verbatim from [splink_udfs](https://github.com/moj-analytical-services/splink_udfs) under MIT (Copyright (c) 2025 Ministry of Justice). Vendoring this single file lets us register `soundex` directly in the extension, dropping the runtime dependency on splink_udfs. Full attribution + permission notice in [LICENSE-vendored](LICENSE-vendored). Inline acknowledgement at the top of the vendored file preserves the upstream credit chain (the splink_udfs implementation itself credits Rob Tillaart's MIT [Arduino Soundex](https://github.com/RobTillaart/Soundex) library as algorithmic inspiration).
