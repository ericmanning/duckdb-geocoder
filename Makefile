PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=us_geocoder
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Build spatial as a test dependency. Every sqllogic test in test/sql/ does
# `LOAD spatial;` after the `require us_geocoder` line — without spatial in
# the local extension repo the LOAD fails and the whole test bails. Setting
# DEFAULT_TEST_EXTENSION_DEPS=spatial in the included extension-ci-tools
# Makefile appends spatial to CORE_EXTENSIONS so it builds into the local
# repo alongside us_geocoder, and the test runner's autoinstall path picks
# it up.
#
# Gated on VCPKG_TOOLCHAIN_PATH being set: building duckdb-spatial from
# source needs vcpkg for ZLIB and friends. CI always sets this. Local dev
# may not — without vcpkg, `make release` would fail on the spatial build,
# so we skip the test dep and rely on the developer's pre-installed
# spatial in ~/.duckdb/extensions/ (REPOSITORY mode in duckdb_extensions()).
ifneq (${VCPKG_TOOLCHAIN_PATH},)
    DEFAULT_TEST_EXTENSION_DEPS=spatial
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile