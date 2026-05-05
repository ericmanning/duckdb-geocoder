# us_geocoder

A DuckDB community extension that geocodes US addresses against [Census TIGER/Line](https://www.census.gov/geographies/mapping-files/time-series/geo/tiger-line-file.html) data. A pure-DuckDB rewrite of PostGIS's [`tiger_geocoder`](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder).

Given a street address, it returns (a) a point in NAD83 coordinates interpolated along the street centerline with a 10m perpendicular side-of-street offset, (b) the 2020 census block / tract / block-group GEOIDs covering that point, and (c) a `rating` that lower-bounds match quality (0 = perfect).

**Status:** alpha. Core geocoder is functional end-to-end against full TIGER data; see [docs/parity.md](docs/parity.md) for where v0.1 diverges from PostGIS.

## Install

```sql
INSTALL us_geocoder FROM community;
INSTALL spatial;
INSTALL splink_udfs            FROM community;
INSTALL us_address_standardizer FROM community;  -- optional (for raw-string inputs)

LOAD us_geocoder;
LOAD spatial;
LOAD splink_udfs;
LOAD us_address_standardizer;
```

If the extension isn't yet in the community registry, build from source — see [Building](#building).

## Load TIGER data

From the Census CDN (default — needs `httpfs`):

```sql
CALL load_tiger_nation(year := 2025);          -- one-time: state, county, zcta5
CALL load_tiger_state('RI');                   -- one state
CALL load_tiger_states(['RI','MA','CT']);      -- several states in one call
CALL load_tiger_all_states();                  -- 50 states + DC
```

`load_tiger_state[s]` must run after `load_tiger_nation`. RI loads in ~45s from the Census CDN, ~25s from a local mirror.

Local sources need a **Census-nested** layout (`STATE/`, `COUNTY/`, `EDGES/`, `FACES/`, `FEATNAMES/`, … under a single root) — see [docs/api.md § Local source layout](docs/api.md#local-source-layout) for the full subdirectory map.

```sql
CALL load_tiger_nation('/data/tiger_2025');
CALL load_tiger_states(['RI','MA'], '/data/tiger_2025');
```

Pass `build_containment := false` to skip the per-state `edge_containment` precompute (saves ~1–2 min/state) if you don't need block / tract / block-group GEOIDs. Populate later with `CALL build_edge_containment(['RI','MA'])`.

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
SELECT rating, (addy).address, (addy).street_name, ST_AsText(geom), block_geoid
FROM tiger.geocode('120 Benefit St, Providence RI 02903');
```

This routes through [`tiger.from_pagc()`](docs/api.md) (PAGC standardizer) and dispatches to the full geocoder with defaults (`max_results=10`, no spatial restriction, no containment filter). Requires `us_address_standardizer` to be installed — auto-loaded on first use.

For full control, pass a `geocode_input` struct and tune `max_results` / `restrict_geom` / `require_containment`:

```sql
SELECT rating, (addy).address, (addy).street_name, (addy).location,
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

Vectorize across a whole table via `CROSS JOIN LATERAL`:

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

Two DuckDB binder quirks worth knowing:

- **Reference the column unqualified inside the LATERAL call** (`tiger.geocode(_raw)`, not `tiger.geocode(inputs._raw)`). When the outer source is a CTE, DuckDB mis-parses `alias.col` as struct-field access on a row.
- **Don't reuse the CSV column name as an output alias.** `SELECT ... address` while the input column is also `address` triggers "column cannot be referenced before it is defined." Rename one side (the CTE column, above) to avoid the clash.

Working end-to-end scripts in [scripts/demo/](scripts/demo/): `build_nj_db.sql` builds a portable NJ reference DB from the Census CDN, then `geocode_addresses.sql` (structured input) and `geocode_raw.sql` (free-form single-string input) each read a CSV and write a geocoded CSV.

## Runtime dependencies

| ext | source | required for |
|---|---|---|
| `spatial` | core | everything |
| `httpfs` | core | HTTP source mode only (`load_tiger_*` without a local source) |
| [`splink_udfs`](https://duckdb.org/community_extensions/extensions/splink_udfs) | community | every geocoder call (`soundex`) |
| [`us_address_standardizer`](https://duckdb.org/community_extensions) | community | `from_pagc()` + `geocode(VARCHAR)` overload (not strictly required — struct-input always works) |

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

A genuinely-parallel HTTP mode inside the loader (`httpfs` + `read_blob()` prefetch, eliminating the need for a shell script) is a v0.2 target.

## License

GPLv2 — same license as the upstream [postgis_tiger_geocoder](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder). See [LICENSE](LICENSE).
