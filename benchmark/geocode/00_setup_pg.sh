#!/usr/bin/env bash
# Build (if needed) and start the pgparity Docker container with PG 16 +
# PostGIS + address_standardizer + postgis_tiger_geocoder. Reuses the existing
# image at scripts/parity/pg_compare/.
#
# Idempotent: re-running just verifies the container is up.
source "$(dirname "$0")/../config.sh"

echo "=== PostgreSQL (Docker) Setup ==="

DOCKERFILE_DIR="$REPO_DIR/scripts/parity/pg_compare"

# 1. Build the image if absent.
if ! docker image inspect "$PG_IMAGE" >/dev/null 2>&1; then
    echo "Building $PG_IMAGE from $DOCKERFILE_DIR ..."
    docker build -t "$PG_IMAGE" "$DOCKERFILE_DIR"
else
    echo "Image $PG_IMAGE already built."
fi

# 2. Start the container if not running.
if docker ps --filter "name=^${PG_CONTAINER}$" --format '{{.Names}}' | grep -q "$PG_CONTAINER"; then
    echo "Container $PG_CONTAINER already running."
else
    # Stop any prior stopped container with the same name.
    docker rm -f "$PG_CONTAINER" 2>/dev/null || true

    echo "Starting $PG_CONTAINER on host port $PG_HOST_PORT ..."
    docker run -d --rm --name "$PG_CONTAINER" \
        --shm-size=2g \
        -p "$PG_HOST_PORT:5432" \
        -e POSTGRES_PASSWORD=parity \
        "$PG_IMAGE" >/dev/null

    # Wait for PG to accept connections.
    echo -n "Waiting for PG to start "
    for i in {1..60}; do
        if docker exec "$PG_CONTAINER" pg_isready -U "$PG_USER" -d "$PG_DB" >/dev/null 2>&1; then
            echo "  ready (after ${i}s)"
            break
        fi
        echo -n "."
        sleep 1
    done
fi

# 3. Sanity-check the extensions are installed.
echo ""
echo "=== PG extensions ==="
pg_psql -c "SELECT extname, extversion FROM pg_extension WHERE extname IN ('postgis','address_standardizer','postgis_tiger_geocoder','fuzzystrmatch') ORDER BY extname;"

# 4. Enable PAGC parser at the DB level so geocode() and reverse_geocode()
#    use it without per-call setting calls. This is the "use PG-with-PAGC"
#    parity mode our DuckDB extension always operates in.
echo ""
echo "=== Enabling PAGC parser ==="
pg_psql -c "SELECT tiger.set_geocode_setting('use_pagc_address_parser','true');"
pg_psql -c "SELECT tiger.get_geocode_setting('use_pagc_address_parser') AS pagc_enabled;"

echo ""
echo "=== PG setup complete ==="
echo "Container:  $PG_CONTAINER  (host port $PG_HOST_PORT, db $PG_DB)"
echo "Connect:    PGPASSWORD=parity psql -h localhost -p $PG_HOST_PORT -U $PG_USER -d $PG_DB"
