# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(us_geocoder
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Test-time community-extension dependencies. Built from source and statically
# linked into the test runner so `LOAD splink_udfs` / `LOAD us_address_standardizer`
# resolve without an autoinstall round-trip — the autoinstall path hangs inside
# the linux_amd64 ci-tools Docker test image.
#
# SHAs are pinned to the same refs the community-extensions registry ships:
#   - moj-analytical-services/splink_udfs        → cf00056 (registry v0.0.11)
#   - ericmanning/duckdb-address-standardizer    → 2d15228 (registry v0.1.0)
duckdb_extension_load(splink_udfs
    GIT_URL https://github.com/moj-analytical-services/splink_udfs
    GIT_TAG cf00056f887486d0aee0a853a764f9775aa40438
)

duckdb_extension_load(us_address_standardizer
    GIT_URL https://github.com/ericmanning/duckdb-address-standardizer
    GIT_TAG 2d1522898b2d9d20b34c90ee112900789cedf955
)