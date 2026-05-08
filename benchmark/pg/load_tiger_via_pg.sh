#!/usr/bin/env bash
# Run PG's own tiger loader inside the running pgparity container, exactly
# the way the postgis_tiger_geocoder README prescribes. End-to-end test of
# PG's loader path against TIGER 2025 from Census, populating MA + MN.
#
# Usage:
#   ./load_tiger_via_pg.sh [STATES_CSV]    # default: 'MA,MN'
#
# Prereqs:
#   - container `pgparity` running (built from this dir's Dockerfile)
#   - Census CDN reachable from inside the container

set -eu -o pipefail

STATES="${1:-MA,MN}"

CONTAINER="${CONTAINER:-pgparity}"
docker exec "$CONTAINER" true 2>/dev/null \
    || { echo "ERROR: container '$CONTAINER' not running" >&2; exit 2; }

echo "Loading TIGER 2025 (states: $STATES) via PG's tiger loader inside $CONTAINER ..."
echo "This downloads ~$(echo "$STATES" | tr ',' '\n' | wc -l) state-worth of"
echo "TIGER data from Census via wget (no parallelism); expect 1-3 hours."
echo

# Run the loader inside the container as the postgres OS user (so psql can
# connect via local socket without password). The script:
#   1. Sets staging dir env vars the generator expects.
#   2. Generates the nation-level script (state, county, zcta5).
#   3. Executes it.
#   4. Generates the state-level + county-level scripts for $STATES.
#   5. Executes them.
docker exec -i -u postgres "$CONTAINER" bash -s <<EOF
set -eu -o pipefail

# Env vars the generated scripts reference. PG bundles loader_platform
# rows for 'sh' and 'windows'. The 'sh' row uses PSQL/SHP2PGSQL/UNZIPTOOL
# placeholders we expand here.
export PGUSER=postgres
export PGDATABASE=parity
export PSQL=/usr/lib/postgresql/16/bin/psql
export SHP2PGSQL=/usr/bin/shp2pgsql
export UNZIPTOOL=unzip
export WGETTOOL=wget
export TMPDIR=/gisdata/temp
mkdir -p "\$TMPDIR"
cd /gisdata

# Census's CDN (Cloudflare-fronted) sporadically returns 403 for sequential
# downloads from the same IP — even when the file is reachable. PG's loader
# has no retry logic and \`set -e\` kills the multi-hour script on the first
# 403. wget by default treats 403 as fatal (no retry), so we add
# retry_on_http_error and a non-default UA (Cloudflare WAF flags Wget/* UAs).
cat >"\$HOME/.wgetrc" <<'WGETRC'
tries = 10
waitretry = 60
retry_on_http_error = 403,429,500,502,503,504
random_wait = on
wait = 1
user_agent = Mozilla/5.0 (X11; Linux x86_64) Gecko/20100101 Firefox/120.0
WGETRC

# Patch the loader_platform 'sh' template so the generated script uses the
# right PG version, db, and password for our container. PG ships
# placeholder values (PG18, yourpasswordhere, geocoder) the user is meant
# to edit before first use.
"\$PSQL" -d parity <<'PATCH'
SET search_path TO tiger, public;
UPDATE loader_platform SET declare_sect = E'TMPDIR="/gisdata/temp/"\nUNZIPTOOL=unzip\nWGETTOOL="/usr/bin/wget"\nexport PGBIN=/usr/lib/postgresql/16/bin\nexport PGPORT=5432\nexport PGHOST=/var/run/postgresql\nexport PGUSER=postgres\nexport PGDATABASE=parity\nPSQL=\${PGBIN}/psql\nSHP2PGSQL=shp2pgsql\ncd /gisdata\n'
WHERE os = 'sh';
PATCH

echo "--- nation-level script ---"
# tiger schema must be in search_path so the loader function can see its
# loader_lookuptables / loader_platform tables (it queries unqualified).
"\$PSQL" -At -q -c "SELECT tiger.loader_generate_nation_script('sh')" \\
    > /tmp/nation_load.sh
# Per README: prepend `set -e -u` so the script fails fast on any error
# instead of cascading downstream (the loader is a 50+ line shell pipeline).
sed -i '1i set -e -u' /tmp/nation_load.sh
echo "Generated /tmp/nation_load.sh (\$(wc -l < /tmp/nation_load.sh) lines)"
echo "Executing ..."
bash /tmp/nation_load.sh

echo
echo "--- state-level script for: $STATES ---"
"\$PSQL" -At -q -c "SELECT tiger.loader_generate_script(ARRAY[$(echo "$STATES" | sed "s/[^,]*/'&'/g")], 'sh')" \\
    > /tmp/state_load.sh
sed -i '1i set -e -u' /tmp/state_load.sh
echo "Generated /tmp/state_load.sh (\$(wc -l < /tmp/state_load.sh) lines)"
echo "Executing ..."
bash /tmp/state_load.sh

echo
echo "--- post-load: install_missing_indexes ---"
"\$PSQL" -c "SELECT tiger.install_missing_indexes()"

echo
echo "--- row counts (parent tables; inheritance aggregates per-state children) ---"
"\$PSQL" -c "
SELECT 'state'     AS tbl, COUNT(*) FROM tiger.state     UNION ALL
SELECT 'county',           COUNT(*) FROM tiger.county    UNION ALL
SELECT 'place',            COUNT(*) FROM tiger.place     UNION ALL
SELECT 'cousub',           COUNT(*) FROM tiger.cousub    UNION ALL
SELECT 'zcta5',            COUNT(*) FROM tiger.zcta5     UNION ALL
SELECT 'edges',            COUNT(*) FROM tiger.edges     UNION ALL
SELECT 'faces',            COUNT(*) FROM tiger.faces     UNION ALL
SELECT 'featnames',        COUNT(*) FROM tiger.featnames UNION ALL
SELECT 'addr',             COUNT(*) FROM tiger.addr
ORDER BY tbl;"
EOF

echo
echo "TIGER load complete."
