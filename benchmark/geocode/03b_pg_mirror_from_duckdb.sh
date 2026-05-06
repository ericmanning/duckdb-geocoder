#!/usr/bin/env bash
# Mirror TIGER data from DuckDB → PG by streaming CSV. Replaces PG's native
# loader (which would re-download every shapefile from Census, ~5–15h) for
# the geocoding-quality benchmark. We accept that this loses PG-side LOAD
# timing — that's recovered separately by 03c_pg_load_2states.sh on a
# representative sample (CA + KY).
#
# Approach per table:
#   DuckDB:  COPY (SELECT cols, ST_AsText(geom)) TO STDOUT (CSV, HEADER)
#   pipe →
#   PG:      \COPY into a TEMP staging table (text geom),
#            then INSERT INTO tiger.<table> with ST_Multi(ST_GeomFromText(...))
#            so the column-level SRID/multi-type constraints land cleanly.
#
# Pre-mirror cleanup: drop any leftover per-state inheritance children in
# tiger_data.* (from earlier partial PG loads), then TRUNCATE tiger.*
# parents. After mirroring, the parents alone hold all rows — PG's
# inheritance scan transparently finds them via SELECT FROM tiger.<table>.
#
# Prereq: DuckDB load already complete ($DUCKDB_DB exists with tiger.*
# populated); PG container running with postgis_tiger_geocoder installed
# (00_setup_pg.sh).

set -euo pipefail
source "$(dirname "$0")/../config.sh"

LOG="$RESULTS_DIR/mirror_pg_from_duckdb.log"
mkdir -p "$RESULTS_DIR"

echo "=== Mirroring DuckDB tiger.* → PG tiger.* ==="
echo "Log: $LOG"
date | tee "$LOG"

# ─── Step 1: clear PG side ──────────────────────────────────────
echo "" | tee -a "$LOG"
echo "[1/14] Clearing PG tiger.* (drop child tables, truncate parents)..." | tee -a "$LOG"

docker exec -i -u postgres "$PG_CONTAINER" psql -d "$PG_DB" -v ON_ERROR_STOP=1 2>&1 | tee -a "$LOG" <<'SQL'
DO $$
DECLARE r record;
BEGIN
    FOR r IN
        SELECT n.nspname AS sch, c.relname AS tbl
        FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname = 'tiger_data'
          AND c.relkind = 'r'
          AND c.relname ~ '^[a-z]{2}_(addr|cousub|edges|faces|featnames|place|tabblock20|tract|zcta5|zip_lookup_base|zip_state|zip_state_loc)$'
    LOOP
        EXECUTE format('DROP TABLE IF EXISTS %I.%I CASCADE', r.sch, r.tbl);
    END LOOP;
END $$;

TRUNCATE TABLE tiger.state, tiger.county, tiger.place, tiger.cousub, tiger.zcta5,
               tiger.zip_state, tiger.zip_state_loc, tiger.zip_lookup_base,
               tiger.edges, tiger.faces, tiger.featnames, tiger.addr
               RESTART IDENTITY;
SQL

# ─── Step 2: per-table mirror ──────────────────────────────────
# Each call: `mirror DUCKDB_SELECT PG_STAGING_DDL PG_INSERT label`
# DUCKDB_SELECT must produce CSV-friendly columns with geometry as
# WKT text via ST_AsText. PG_STAGING_DDL declares a TEMP table with
# matching column order; PG_INSERT copies from stg to the tiger.* parent.
mirror() {
    local LABEL="$1"
    local DK_SQL="$2"
    local PG_DDL="$3"
    local PG_INSERT="$4"

    local t0
    t0=$(date +%s)
    echo "" | tee -a "$LOG"
    echo "[$LABEL] starting..." | tee -a "$LOG"

    # DuckDB → stdout (CSV) → docker exec psql \COPY into staging → INSERT
    "$DUCKDB_BIN" "$DUCKDB_DB" -csv -noheader -c "$DK_SQL" \
      | docker exec -i -u postgres "$PG_CONTAINER" psql -d "$PG_DB" \
            -v ON_ERROR_STOP=1 -q -c "BEGIN" \
            -c "$PG_DDL" \
            -c "\\COPY stg FROM STDIN WITH (FORMAT csv, NULL '')" \
            -c "$PG_INSERT" \
            -c "COMMIT" \
        2>&1 | tee -a "$LOG"

    local elapsed=$(( $(date +%s) - t0 ))
    echo "[$LABEL] done in ${elapsed}s" | tee -a "$LOG"
}

# State (56 rows). geometry is MultiPolygon on PG; our LineString/Polygon
# may be Polygon — wrap in ST_Multi unconditionally.
mirror "2/14 tiger.state" \
"SELECT statefp, stusps, name, ST_AsText(the_geom)
   FROM tiger.state ORDER BY statefp" \
"CREATE TEMP TABLE stg(statefp TEXT, stusps TEXT, name TEXT, geom_wkt TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.state(statefp, stusps, name, the_geom)
 SELECT statefp, stusps, name, ST_Multi(ST_GeomFromText(geom_wkt, 4269)) FROM stg"

# County (~3.2K rows).
mirror "3/14 tiger.county" \
"SELECT statefp, countyfp, cntyidfp, name, ST_AsText(the_geom)
   FROM tiger.county ORDER BY statefp, countyfp" \
"CREATE TEMP TABLE stg(statefp TEXT, countyfp TEXT, cntyidfp TEXT, name TEXT, geom_wkt TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.county(statefp, countyfp, cntyidfp, name, the_geom)
 SELECT statefp, countyfp, cntyidfp, name, ST_Multi(ST_GeomFromText(geom_wkt, 4269)) FROM stg"

# Place.
mirror "4/14 tiger.place" \
"SELECT statefp, placefp, plcidfp, name, ST_AsText(the_geom)
   FROM tiger.place ORDER BY statefp, placefp" \
"CREATE TEMP TABLE stg(statefp TEXT, placefp TEXT, plcidfp TEXT, name TEXT, geom_wkt TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.place(statefp, placefp, plcidfp, name, the_geom)
 SELECT statefp, placefp, plcidfp, name, ST_Multi(ST_GeomFromText(geom_wkt, 4269)) FROM stg"

# Cousub.
mirror "5/14 tiger.cousub" \
"SELECT statefp, countyfp, cousubfp, cosbidfp, name, ST_AsText(the_geom)
   FROM tiger.cousub ORDER BY statefp, countyfp, cousubfp" \
"CREATE TEMP TABLE stg(statefp TEXT, countyfp TEXT, cousubfp TEXT, cosbidfp TEXT, name TEXT, geom_wkt TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.cousub(statefp, countyfp, cousubfp, cosbidfp, name, the_geom)
 SELECT statefp, countyfp, cousubfp, cosbidfp, name, ST_Multi(ST_GeomFromText(geom_wkt, 4269)) FROM stg"

# ZCTA5. statefp is NULL for nation-level ZCTA rows; PG's column is NOT NULL,
# so substitute '' (matches PG loader's behavior pre-clip).
mirror "6/14 tiger.zcta5" \
"SELECT COALESCE(statefp,'') AS statefp, zcta5ce, ST_AsText(the_geom)
   FROM tiger.zcta5 ORDER BY statefp NULLS FIRST, zcta5ce" \
"CREATE TEMP TABLE stg(statefp TEXT, zcta5ce TEXT, geom_wkt TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.zcta5(statefp, zcta5ce, the_geom)
 SELECT statefp, zcta5ce, ST_Multi(ST_GeomFromText(geom_wkt, 4269)) FROM stg"

# zip_state (no geometry).
mirror "7/14 tiger.zip_state" \
"SELECT zip, stusps, statefp FROM tiger.zip_state" \
"CREATE TEMP TABLE stg(zip TEXT, stusps TEXT, statefp TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.zip_state(zip, stusps, statefp) SELECT * FROM stg"

# zip_state_loc.
mirror "8/14 tiger.zip_state_loc" \
"SELECT zip, stusps, statefp, place FROM tiger.zip_state_loc" \
"CREATE TEMP TABLE stg(zip TEXT, stusps TEXT, statefp TEXT, place TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.zip_state_loc(zip, stusps, statefp, place) SELECT * FROM stg"

# zip_lookup_base.
mirror "9/14 tiger.zip_lookup_base" \
"SELECT zip, state, county, city, statefp FROM tiger.zip_lookup_base" \
"CREATE TEMP TABLE stg(zip TEXT, state TEXT, county TEXT, city TEXT, statefp TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.zip_lookup_base(zip, state, county, city, statefp) SELECT * FROM stg"

# Edges. PG geom is MultiLineString; ours is LineString → ST_Multi.
# This is the big one: ~50–60M rows nationwide. Streaming COPY handles it
# without buffering the whole thing in either process.
mirror "10/14 tiger.edges" \
"SELECT statefp, countyfp, tlid, tfidl, tfidr, tnidf, tnidt, mtfcc, fullname,
        zipl, zipr, ST_AsText(the_geom)
   FROM tiger.edges" \
"CREATE TEMP TABLE stg(
    statefp TEXT, countyfp TEXT, tlid BIGINT, tfidl NUMERIC(10,0), tfidr NUMERIC(10,0),
    tnidf NUMERIC(10,0), tnidt NUMERIC(10,0), mtfcc TEXT, fullname TEXT,
    zipl TEXT, zipr TEXT, geom_wkt TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.edges(statefp, countyfp, tlid, tfidl, tfidr, tnidf, tnidt, mtfcc, fullname, zipl, zipr, the_geom)
 SELECT statefp, countyfp, tlid, tfidl, tfidr, tnidf, tnidt, mtfcc, fullname, zipl, zipr,
        ST_Multi(ST_GeomFromText(geom_wkt, 4269)) FROM stg"

# Faces. We have countyfp/tractce20/blockce20/blkgrpce20; PG has both
# unsuffixed (tractce/blockce/blkgrpce) and 2010-suffixed columns. Map our
# 2020-vintage data to the unsuffixed columns — that's where PG's geocode()
# expects current data.
mirror "11/14 tiger.faces" \
"SELECT statefp, countyfp, tfid, placefp, cousubfp, tractce20, blockce20, blkgrpce20,
        ST_AsText(the_geom)
   FROM tiger.faces" \
"CREATE TEMP TABLE stg(
    statefp TEXT, countyfp TEXT, tfid NUMERIC(10,0), placefp TEXT, cousubfp TEXT,
    tractce TEXT, blockce TEXT, blkgrpce TEXT, geom_wkt TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.faces(statefp, countyfp, tfid, placefp, cousubfp, tractce, blockce, blkgrpce, the_geom)
 SELECT statefp, countyfp, tfid, placefp, cousubfp, tractce, blockce, blkgrpce,
        ST_Multi(ST_GeomFromText(geom_wkt, 4269)) FROM stg"

# Featnames (no geometry — DBF only).
mirror "12/14 tiger.featnames" \
"SELECT statefp, tlid, fullname, name, predirabrv, pretypabrv, prequalabr,
        suftypabrv, sufdirabrv, mtfcc
   FROM tiger.featnames" \
"CREATE TEMP TABLE stg(
    statefp TEXT, tlid BIGINT, fullname TEXT, name TEXT,
    predirabrv TEXT, pretypabrv TEXT, prequalabr TEXT,
    suftypabrv TEXT, sufdirabrv TEXT, mtfcc TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.featnames(statefp, tlid, fullname, name, predirabrv, pretypabrv, prequalabr, suftypabrv, sufdirabrv, mtfcc)
 SELECT * FROM stg"

# Addr (no geometry).
mirror "13/14 tiger.addr" \
"SELECT statefp, tlid, fromhn, tohn, side, zip, plus4 FROM tiger.addr" \
"CREATE TEMP TABLE stg(statefp TEXT, tlid BIGINT, fromhn TEXT, tohn TEXT, side TEXT, zip TEXT, plus4 TEXT) ON COMMIT DROP" \
"INSERT INTO tiger.addr(statefp, tlid, fromhn, tohn, side, zip, plus4) SELECT * FROM stg"

# ─── Step 3: rebuild indexes that the geocoder needs ──────────
echo "" | tee -a "$LOG"
echo "[14/14] Rebuilding tiger indexes..." | tee -a "$LOG"
docker exec -i -u postgres "$PG_CONTAINER" psql -d "$PG_DB" -v ON_ERROR_STOP=1 -c \
    "SELECT install_missing_indexes();" 2>&1 | tee -a "$LOG"

# ─── Sanity: row counts side-by-side ──────────────────────────
echo "" | tee -a "$LOG"
echo "=== Row counts side-by-side ===" | tee -a "$LOG"

PG_COUNTS=$(docker exec -i -u postgres "$PG_CONTAINER" psql -d "$PG_DB" -t -A -F$'\t' <<'SQL'
SELECT 'state'    , count(*) FROM tiger.state UNION ALL
SELECT 'county'   , count(*) FROM tiger.county UNION ALL
SELECT 'place'    , count(*) FROM tiger.place UNION ALL
SELECT 'cousub'   , count(*) FROM tiger.cousub UNION ALL
SELECT 'zcta5'    , count(*) FROM tiger.zcta5 UNION ALL
SELECT 'zip_state', count(*) FROM tiger.zip_state UNION ALL
SELECT 'zip_state_loc', count(*) FROM tiger.zip_state_loc UNION ALL
SELECT 'zip_lookup_base', count(*) FROM tiger.zip_lookup_base UNION ALL
SELECT 'edges'    , count(*) FROM tiger.edges UNION ALL
SELECT 'faces'    , count(*) FROM tiger.faces UNION ALL
SELECT 'featnames', count(*) FROM tiger.featnames UNION ALL
SELECT 'addr'     , count(*) FROM tiger.addr;
SQL
)

DK_COUNTS=$("$DUCKDB_BIN" "$DUCKDB_DB" -csv -noheader -c "
LOAD us_geocoder; LOAD spatial;
SELECT 'state'    , count(*) FROM tiger.state UNION ALL
SELECT 'county'   , count(*) FROM tiger.county UNION ALL
SELECT 'place'    , count(*) FROM tiger.place UNION ALL
SELECT 'cousub'   , count(*) FROM tiger.cousub UNION ALL
SELECT 'zcta5'    , count(*) FROM tiger.zcta5 UNION ALL
SELECT 'zip_state', count(*) FROM tiger.zip_state UNION ALL
SELECT 'zip_state_loc', count(*) FROM tiger.zip_state_loc UNION ALL
SELECT 'zip_lookup_base', count(*) FROM tiger.zip_lookup_base UNION ALL
SELECT 'edges'    , count(*) FROM tiger.edges UNION ALL
SELECT 'faces'    , count(*) FROM tiger.faces UNION ALL
SELECT 'featnames', count(*) FROM tiger.featnames UNION ALL
SELECT 'addr'     , count(*) FROM tiger.addr;
")

printf '%-20s  %14s  %14s\n' table pg duckdb | tee -a "$LOG"
printf '%-20s  %14s  %14s\n' --- --- --- | tee -a "$LOG"
join -t$'\t' \
    <(echo "$PG_COUNTS" | sort) \
    <(echo "$DK_COUNTS" | tr ',' $'\t' | sort) \
  | awk -F$'\t' '{ printf "%-20s  %14s  %14s\n", $1, $2, $3 }' | tee -a "$LOG"

echo "" | tee -a "$LOG"
echo "=== Mirror complete ===" | tee -a "$LOG"
date | tee -a "$LOG"
