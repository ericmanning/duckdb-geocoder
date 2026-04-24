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
CALL load_tiger_nation(year := 2025);                     -- one-time: state, county, zcta5
CALL load_tiger_state('RI');                              -- one state
CALL load_tiger_states(['RI','MA','CT']);                 -- several states in one call
CALL load_tiger_all_states();                             -- all 50 states + DC
```

Or from a local **Census-nested** mirror of `https://www2.census.gov/geo/tiger/TIGER<year>/`:

```
/data/tiger_2025/
  STATE/tl_2025_us_state.zip
  COUNTY/tl_2025_us_county.zip
  ZCTA520/tl_2025_us_zcta520.zip
  PLACE/tl_2025_44_place.zip
  COUSUB/tl_2025_44_cousub.zip
  EDGES/tl_2025_44007_edges.zip
  FACES/tl_2025_44007_faces.zip
  FEATNAMES/tl_2025_44007_featnames.zip
  ADDR/tl_2025_44007_addr.zip
  …
```

```sql
CALL load_tiger_nation('/data/tiger_2025');
CALL load_tiger_states(['RI','MA'], '/data/tiger_2025');
```

This layout matches what `wget --recursive` / `curl --remote-name` against the Census FTP produces, so it's also what the parallel-fetch recipe below targets. `load_tiger_state[s]` must run after `load_tiger_nation` — the state loader enumerates counties from the `tiger.county` table populated by the nation load. Loading one state (all 5 counties of RI) from the Census CDN takes ~45s over residential broadband; a local source cuts that to ~25s.

## Reference databases (attached catalogs)

The 13 TIGER data tables can live in the current database, in a separate read-write attached catalog, or in a shared read-only attached catalog. The macros always stay local — only the data moves.

**Current DB (default):**
```sql
CALL load_tiger_state('RI');
SELECT * FROM tiger.geocode(...);
```

**Attached read-write:**
```sql
ATTACH 'work.duckdb' AS work;
CALL load_tiger_state('RI', target_db := 'work');
CALL set_tiger_reference('work');                -- repoint local tiger.* at work.tiger.*
SELECT * FROM tiger.geocode(...);
```

**Attached read-only (portable-DB workflow for secure / air-gapped envs):**
```sql
-- Staging env, with internet:
ATTACH 'tiger_us_2025.duckdb' AS tgt;
CALL load_tiger_nation(target_db := 'tgt');
CALL load_tiger_state('RI', target_db := 'tgt');
DETACH tgt;
-- Ship tiger_us_2025.duckdb to the secure env.

-- Secure env, no internet:
ATTACH 'tiger_us_2025.duckdb' AS ref (READ_ONLY);
CALL set_tiger_reference('ref');
SELECT * FROM tiger.geocode(...);
```

`set_tiger_reference()` with no arguments resets the local `tiger.*` data tables to empty base tables. See [docs/api.md](docs/api.md) for the full contract.

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
make test                      # 216 assertions across 14 sqllogictest files
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

The Census CDN path issues one HTTPS fetch per `(county, table-type)` via GDAL's `/vsicurl/`, which doesn't parallelize well for large states. For NJ / NY / CA etc., pre-download into a Census-nested local mirror via `wget --mirror` (or parallel `curl`) and point the loader at the local copy:

```sh
# Mirror the Census tree for NJ (statefp 34) + nation-level zips.
# Keeps the STATE/, EDGES/, FACES/, … subdirectory structure the loader expects.
DEST=/tmp/tiger_nj
ROOT=https://www2.census.gov/geo/tiger/TIGER2025
mkdir -p $DEST
wget -q -nH --cut-dirs=3 -x -P $DEST \
     -A 'tl_2025_us_*.zip,tl_2025_34_*.zip,tl_2025_34???_*.zip' \
     -r -np -l 2 $ROOT/ 2>/dev/null
```

Or, for only the tables the geocoder uses, hand-roll a parallel curl loop:

```sh
DEST=/tmp/tiger_nj; ROOT=https://www2.census.gov/geo/tiger/TIGER2025
mkdir -p $DEST/{STATE,COUNTY,ZCTA520,PLACE,COUSUB,EDGES,FACES,FEATNAMES,ADDR}

# nation-level
for f in STATE/tl_2025_us_state.zip COUNTY/tl_2025_us_county.zip ZCTA520/tl_2025_us_zcta520.zip; do
  curl -sS -o $DEST/$f $ROOT/$f &
done
# state-level
for f in PLACE/tl_2025_34_place.zip COUSUB/tl_2025_34_cousub.zip; do
  curl -sS -o $DEST/$f $ROOT/$f &
done
# county-level (NJ FIPS 34; counties run 001..041 odd)
for c in $(seq -f "%03g" 1 2 41); do
  for sub in EDGES FACES FEATNAMES ADDR; do
    curl -sS -o $DEST/$sub/tl_2025_34${c}_$(echo $sub | tr A-Z a-z).zip \
         $ROOT/$sub/tl_2025_34${c}_$(echo $sub | tr A-Z a-z).zip &
    (( $(jobs -r | wc -l) >= 8 )) && wait -n
  done
done
wait
```

Then:
```sql
CALL load_tiger_nation('/tmp/tiger_nj');
CALL load_tiger_state('NJ', '/tmp/tiger_nj');  -- ~2 min total
```

A genuinely-parallel HTTP mode inside the loader is a v0.2 target. Attempts with `UNION ALL`-of-`ST_Read` batching didn't parallelize in practice (DuckDB 1.5 + duckdb-spatial), so v0.1 ships the serial fetch plus this workaround.

## License

GPLv2 — same license as the upstream [postgis_tiger_geocoder](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder). See [LICENSE](LICENSE).
