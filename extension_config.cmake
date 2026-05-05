# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(us_geocoder
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Test-time dep: splink_udfs is built from source and statically linked into
# the test runner so `LOAD splink_udfs` resolves without an autoinstall
# round-trip (the autoinstall path hangs inside the linux_amd64 ci-tools
# Docker test image). Pinned to the same SHA the community-extensions
# registry ships (moj-analytical-services/splink_udfs v0.0.11). The repo
# vendors rapidfuzz-cpp as a git submodule under third_party/rapidfuzz —
# SUBMODULES makes FetchContent recurse into it at clone time.
#
# us_address_standardizer is a C-API extension and can't be embedded the
# same way; the one test that exercises it (pg_intersection_regress_pagc)
# is gated on `require us_address_standardizer` and skipped in CI.
duckdb_extension_load(splink_udfs
    GIT_URL https://github.com/moj-analytical-services/splink_udfs
    GIT_TAG cf00056f887486d0aee0a853a764f9775aa40438
    SUBMODULES "third_party/rapidfuzz"
)