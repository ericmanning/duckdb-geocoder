PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=us_geocoder
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Note on test extension deps (spatial, us_address_standardizer):
# Both are runtime deps but neither is built into our CI binaries —
# spatial needs a vcpkg-merge dance that's flaky on arm64, and
# us_address_standardizer is a C-API community extension. The
# affected tests gate themselves on `require spatial` and
# `require us_address_standardizer` so they skip cleanly in CI and
# run locally where developers have both extensions pre-installed
# in ~/.duckdb/extensions/.

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile