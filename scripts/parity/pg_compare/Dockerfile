# PG-parity comparison environment.
#
# Installs PostgreSQL 16 + PostGIS + address_standardizer (built from upstream
# master) + postgis_tiger_geocoder (built from the local mirror at the
# repo path passed in via build context). Used to run PG's geocode_regress.sql
# against the actual PG tiger_geocoder code, populated with TIGER data from
# our pg_parity.duckdb, so we can compare PG's per-row output to ours and
# pinpoint where the rating-arithmetic / parser divergences come from.
#
# Build context expects:
#   ./tiger_geocoder/  — full local clone of postgis_tiger_geocoder
#   ./init.sql         — DB bootstrap (CREATE EXTENSION etc.)
#
# Build:
#   docker build -t duckdb-geocoder-pgparity .
#
# Run:
#   docker run -d --rm --name pgparity \
#       -p 55432:5432 -e POSTGRES_PASSWORD=parity \
#       duckdb-geocoder-pgparity
#
# Connect:
#   PGPASSWORD=parity psql -h localhost -p 55432 -U postgres -d parity

FROM postgres:16

ENV DEBIAN_FRONTEND=noninteractive

# Build deps for address_standardizer + tiger_geocoder; runtime deps for
# postgis itself (provided by postgresql-16-postgis-3).
RUN apt-get update && apt-get install -y --no-install-recommends \
        git build-essential autoconf automake libtool \
        libpcre2-dev pkg-config ca-certificates perl \
        postgresql-server-dev-16 \
        postgresql-16-postgis-3 \
        postgresql-16-postgis-3-scripts \
        postgis \
        wget unzip \
    && rm -rf /var/lib/apt/lists/*

# PG's tiger loader writes downloaded TIGER zips here. Created per spec
# in the loader_variables defaults (`staging_fold = '/gisdata'`).
RUN mkdir -p /gisdata/temp && chown -R postgres:postgres /gisdata

# address_standardizer from upstream master. PGXS-based Makefile —
# perl scripts in pl/ generate C headers from data/usps-st-city-name.txt
# during the build, then we just `make && make install`.
RUN git clone --depth=1 https://github.com/postgis/address_standardizer.git /tmp/as && \
    cd /tmp/as && \
    make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config && \
    make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config install && \
    cd / && rm -rf /tmp/as

# tiger_geocoder from the local mirror copied into the build context.
# Builds the .sql files under data/ (Makefile concatenates src/*.sql) and
# installs the extension via PostgreSQL's standard PGXS pipeline.
COPY tiger_geocoder /tmp/tiger_geocoder
RUN cd /tmp/tiger_geocoder && \
    make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config && \
    make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config install && \
    cd / && rm -rf /tmp/tiger_geocoder

# Bootstrap script: CREATE EXTENSION + populate PAGC rules.
COPY init.sql /docker-entrypoint-initdb.d/01_init.sql

ENV POSTGRES_DB=parity
ENV POSTGRES_USER=postgres
