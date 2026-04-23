PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=us_geocoder
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Note: spatial is declared a test dep in CI (which runs with vcpkg)
# but not added here for local dev — building duckdb-spatial from source
# requires vcpkg for ZLIB. Local `make test` skips test/sql/spatial_scoring.test
# via `require spatial`. Use `VCPKG_TOOLCHAIN_PATH=... DEFAULT_TEST_EXTENSION_DEPS=spatial make`
# to exercise spatial locally.

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile