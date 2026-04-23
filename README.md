# us_geocoder

A DuckDB community extension that geocodes US addresses against [Census TIGER/Line](https://www.census.gov/geographies/mapping-files/time-series/geo/tiger-line-file.html) data. A pure-DuckDB rewrite of PostGIS's [`tiger_geocoder`](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder).

Given a street address, it returns (a) a point in NAD83 coordinates interpolated along the street centerline with a 10m perpendicular side-of-street offset, (b) the 2020 census block / tract / block-group GEOIDs covering that point, and (c) a `rating` that lower-bounds match quality (0 = perfect).

**Status:** alpha. Core geocoder is functional end-to-end against full TIGER data; see [docs/parity.md](docs/parity.md) for where v0.1 diverges from PostGIS.

## Install

Once published:

```sql
INSTALL us_geocoder FROM community;
INSTALL spatial;              -- core
INSTALL splink_udfs FROM community;
INSTALL us_address_standardizer FROM community;  -- optional (for raw-string inputs)

LOAD us_geocoder;
LOAD spatial;
LOAD splink_udfs;
LOAD us_address_standardizer;  -- if installed
```

(While the extension isn't in the community registry yet, build from source — see [Building](#building).)

## Load TIGER data

From the Census CDN (default — needs `httpfs`):

```sql
CALL load_tiger_nation(year := 2025);          -- one-time: state, county, zcta5
CALL load_tiger_state('RI', year := 2025);     -- per-state: place, cousub, edges, faces, featnames, addr + derived
```

Or from a local directory of zips (flat layout):

```sql
CALL load_tiger_nation('/data/tiger_2025', year := 2025);
CALL load_tiger_state('RI', '/data/tiger_2025', year := 2025);
```

`load_tiger_state` must run after `load_tiger_nation` — the state loader enumerates a state's counties from the `tiger.county` table populated by the nation load. Loading one state (all 5 counties of RI) from the Census CDN takes ~45s over residential broadband; a local source cuts that to ~25s.

## Geocode

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
make test                      # 183 assertions across 12 sqllogictest files
```

The build produces a loadable extension at `build/release/extension/us_geocoder/us_geocoder.duckdb_extension` and a DuckDB CLI at `build/release/duckdb` with the extension statically linked.

## Performance

Benchmarked on TIGER 2025 Rhode Island (136K edges, 126K featnames, 105K addr) on a 2024 M4 Max:

| operation | time |
|---|---|
| single geocode (warm) | 130–200 ms |
| batch of 100 addresses | 20 ms/addr |
| batch of 1,000 addresses | 17 ms/addr (~60/sec/thread) |
| full state load (local source) | ~25 s |
| full state load (Census HTTP) | ~45 s |

## License

GPLv2 — same license as the upstream [postgis_tiger_geocoder](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder). See [LICENSE](LICENSE).
