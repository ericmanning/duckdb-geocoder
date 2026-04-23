# us_geocoder

DuckDB community extension that geocodes US addresses against Census TIGER/Line data. A pure-DuckDB rewrite of [postgis_tiger_geocoder](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder).

**Status:** pre-alpha. Phase 0 scaffolding only — no geocoding functions implemented yet. See [geocode_flow.md](./geocode_flow.md) for the spec.

## Building

```sh
git submodule update --init --recursive
make release
```

Produces `build/release/extension/us_geocoder/us_geocoder.duckdb_extension`.

## Testing

```sh
make test
```

## Runtime dependencies

- `spatial` (core) — geometry reads, spatial predicates, GDAL
- `httpfs` (core) — TIGER shapefile downloads in HTTP source mode
- [`splink_udfs`](https://duckdb.org/community_extensions/extensions/splink_udfs) (community) — `soundex`
- [`us_address_standardizer`](https://duckdb.org/community_extensions) (community) — PAGC address parsing for the `VARCHAR` overload of `geocode()`

## License

GPLv2. See [LICENSE](./LICENSE).
