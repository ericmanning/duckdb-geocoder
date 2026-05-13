PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=us_geocoder
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Note on test extension deps (spatial, us_address_standardizer):
# Both are runtime deps but neither is built into our CI binaries —
# spatial needs a vcpkg-merge dance that's flaky on arm64, and
# us_address_standardizer is a C-API community extension. Tests that
# need either are gated on `require-env TIGER_TEST_EXTENSIONS`, which
# skips when the env var is unset and runs otherwise. CI doesn't set
# it → those tests skip cleanly. Local dev sets it to run the full
# suite (developer is responsible for having both extensions installed
# in ~/.duckdb/extensions/):
#
#   TIGER_TEST_EXTENSIONS=1 ./build/release/test/unittest "test/sql/*"
#
# Without the env var locally, you get the same coverage CI runs —
# 6 tests, 93 assertions — useful for quickly mirroring what CI sees.

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile