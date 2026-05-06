# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(us_geocoder
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# us_address_standardizer is a C-API extension and can't be embedded into
# the test runner the way splink_udfs used to be; the one test that
# exercises it (pg_intersection_regress_pagc) is gated on
# `require us_address_standardizer` and skipped in CI.
#
# splink_udfs was previously embedded for its `soundex` function. We now
# vendor that single function (MIT, see src/include/vendored_soundex.hpp +
# LICENSE-vendored) and register it directly in LoadInternal, eliminating
# the dependency.
