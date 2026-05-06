#!/usr/bin/env bash
# Build the us_geocoder release CLI. The CLI statically links us_geocoder +
# spatial + us_address_standardizer (per extension_config.cmake)
# so the benchmark can call tiger.geocode() / tiger.from_pagc() without an
# `INSTALL` round-trip.
source "$(dirname "$0")/../config.sh"

echo "=== DuckDB Extension Setup ==="
cd "$REPO_DIR"

# 1. Submodules pinned (required for first-time build).
git submodule update --init --recursive

# 2. Build release. ~10 min cold (rebuilds DuckDB + dep extensions),
#    ~30s hot.
echo ""
echo "Building release (this can take ~10 min cold)..."
make release

# 3. Verify the CLI exists and the extension loads.
if [[ ! -x "$DUCKDB_BIN" ]]; then
    echo "ERROR: $DUCKDB_BIN not found after build" >&2
    exit 1
fi

echo ""
echo "=== DuckDB CLI ==="
echo "Binary:  $DUCKDB_BIN"
"$DUCKDB_BIN" --version

echo ""
echo "Verifying us_geocoder + deps load:"
"$DUCKDB_BIN" -c "
LOAD us_geocoder;
LOAD spatial;
LOAD us_address_standardizer;
SELECT us_geocoder_version() AS us_geocoder_version;
"

echo ""
echo "=== DuckDB setup complete ==="
