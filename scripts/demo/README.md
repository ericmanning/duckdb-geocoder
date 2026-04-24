# NJ geocoding demo

Two-step flow: build a portable NJ reference DB directly from the Census CDN, then geocode a CSV.

## Prerequisites

- Built extension: `make release` from the repo root (produces `./build/release/duckdb` with `us_geocoder` statically linked).
- ~300 MB free disk for the resulting `.duckdb` reference DB.
- Internet access for the first-run data pull.

## 1. Build the reference DB

**Recommended — parallel download, then local ingest (~4 min total):**

```sh
cd /Users/ericmm/Documents/GitHub/duckdb-geocoder    # repo root

# Parallel-download NJ's 89 TIGER zips (~26 s, vs ~5-8 min serial /vsicurl/)
./scripts/parallel_download_state.sh NJ ./tiger_nj

# Ingest the local mirror (~3-4 min: shapefile parse + edge_containment)
./build/release/duckdb tiger_nj_2025.duckdb <<'EOF'
CALL load_tiger_nation('./tiger_nj');
CALL load_tiger_state('NJ', './tiger_nj');
EOF
```

[`parallel_download_state.sh`](../parallel_download_state.sh) scrapes the Census CDN's directory index to enumerate county files, then `xargs -P 16` downloads everything concurrently. Falls back to whatever the network allows; macOS default bash is supported.

**Alternative — single-step via the loader (simpler, slower — ~8-10 min):**

```sh
./build/release/duckdb tiger_nj_2025.duckdb -f scripts/demo/build_nj_db.sql
```

Pulls directly from Census via DuckDB's `httpfs` + GDAL `/vsicurl/`. Each county zip is fetched serially inside GDAL, which is the dominant cost. Good enough for a small state; painful for CA/NY/TX. See the loader-perf roadmap for the in-loader parallel-fetch work that'll eventually fold both paths into one.

At the end the script prints row counts — NJ has ~640K edges, ~180K featnames, ~130K addr ranges.

**Cache it.** `tiger_nj_2025.duckdb` is a single-file portable reference DB. Keep it around; subsequent demos just ATTACH it.

## 2. Geocode a CSV

```sh
./build/release/duckdb -f scripts/demo/geocode_addresses.sql
```

Reads [`sample_addresses.csv`](sample_addresses.csv) (8 Princeton/Newark/Trenton addresses), writes `geocoded.csv` with `lat`, `lng`, `rating`, and the 2020 census block / tract / block-group GEOIDs.

The SQL ATTACHes `tiger_nj_2025.duckdb` read-only and calls `set_tiger_reference('ref')` to point the macros at it — no data re-load needed.

## Typical output

```
$ head geocoded.csv
id,input_address,rating,lat,lng,block_geoid,tract_geoid,blkgrp_geoid,containment_guaranteed
1,1 Nassau Hall, Princeton, NJ 08544,0,-74.65907,40.34887,340210037001008,34021003700,340210037001,true
2,65 Witherspoon St, Princeton, NJ 08542,0,-74.65968,40.35194,340210036001015,34021003600,340210036001,true
…
```

## Geocoding your own addresses

Edit [`geocode_addresses.sql`](geocode_addresses.sql): change the hardcoded CSV path (`scripts/demo/sample_addresses.csv`) and adapt the `CAST({...} AS tiger.geocode_input)` struct builder to your column names. Expected fields:

| column | type | notes |
|---|---|---|
| `address_num` | INTEGER | numeric house number; `1731` not `"1731A"` |
| `street_name` | VARCHAR | just the name — `"New Hampshire"`, not `"New Hampshire Ave NW"` |
| `street_type` | VARCHAR | `St`, `Ave`, `Blvd`, `Hwy`… (TIGER title-case; soft-penalty-only) |
| `location` | VARCHAR | city / place name (free-form) |
| `state` | VARCHAR | uppercase 2-letter (`NJ`, `NY`) — **strict** |
| `zip` | VARCHAR | 5-char string with leading zeros preserved (`'07102'` not `7102`) — **strict** |

See [docs/api.md](../../docs/api.md) for the full `geocode_input` contract, the rating scale, and the `containment_guaranteed` semantics.

## Other states

- Change `'NJ'` in [`build_nj_db.sql`](build_nj_db.sql) to any other abbrev.
- Multiple states in one pass:
  ```sql
  CALL load_tiger_states(['NJ', 'NY', 'PA']);
  ```
- All 50 states + DC in one call (takes ~3–4 hours serially):
  ```sql
  CALL load_tiger_all_states();
  ```

## Free-form addresses

If you have single-string addresses (`"1 Nassau Hall, Princeton NJ 08544"`) rather than split columns, use the 1-arg `tiger.geocode(VARCHAR)` shortcut:

```sh
./build/release/duckdb -f scripts/demo/geocode_raw.sql
```

That script reads [`sample_addresses_raw.csv`](sample_addresses_raw.csv) (same 8 addresses, one free-form string per row) and writes `geocoded_raw.csv`. It uses `CROSS JOIN LATERAL tiger.geocode(i.address)` — the 1-arg overload internally calls `tiger.from_pagc()` which standardizes via PAGC.

Requires the `us_address_standardizer` community extension — auto-installed on first `LOAD us_geocoder` (via `INSTALL us_address_standardizer FROM community`).
