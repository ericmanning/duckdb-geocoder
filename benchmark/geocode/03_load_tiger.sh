#!/usr/bin/env bash
# Load TIGER 2025 (nation + 50 states + DC) into DuckDB. PG side is no
# longer loaded by this script — see 03b_pg_mirror_from_duckdb.sh, which
# mirrors DuckDB → PG via streaming COPY in ~minutes instead of ~hours
# (PG's native loader re-downloads every Census shapefile, which adds
# nothing the DuckDB load doesn't already have for a geocoding-quality
# comparison).
#
# To recover the PG-side LOAD timing for the bench writeup, run
# 03c_pg_load_2states_for_timing.sh — it times PG's native loader on
# CA + KY (representative state-size extremes) into an isolated DB and
# pairs the result with the matching DuckDB times from load_tiger_duckdb.log.
#
# Wall-clock expectation:
#   DuckDB: 2-4 hours nationwide via /vsicurl/ (with retry+cache-bust on
#           transient errors and resumable per-(state,county,table) progress).
#
# Idempotent: DuckDB's load_tiger_state[s] uses tiger.loader_progress to
# skip work already done — re-running picks up where a partial load left off.
source "$(dirname "$0")/../config.sh"

DK_LOG="$RESULTS_DIR/load_tiger_duckdb.log"

echo "=== Loading TIGER 2025 (nation + 50 states + DC) into DuckDB ==="
echo "DuckDB log: $DK_LOG"
echo ""
echo "WARNING: this will run for ~2-4 hours pulling from the Census CDN"
echo "         (~50 GB total). Resumable — interrupt and re-run safely."
echo ""

# ─── DuckDB load (in-process loader) ─────────────────────────────
echo "--- DuckDB TIGER loader ---"
echo "Streaming progress to $DK_LOG (also mirrored to stdout)."
echo

# Note: NO `source` argument — Census CDN path on both sides.
# DuckDB DB persists across runs; loader's DELETE-first idempotency handles
# partial prior loads. `tee` lets the user follow progress directly.
DK_START=$(date +%s)
set +e
"$DUCKDB_BIN" "$DUCKDB_DB" <<SQL 2>&1 | tee "$DK_LOG"
LOAD us_geocoder;
LOAD spatial;

-- Nation tables: state, county, zcta5
CALL load_tiger_nation();

-- 50 states + DC. Per-state, per-county progress goes to stderr live;
-- the CALL itself returns a summary table at the end.
CALL load_tiger_all_states();

-- Checkpoint so a subsequent open doesn't try to replay tiger.* DDL before
-- the extension has been loaded (LoadInternal creates the tiger schema,
-- but WAL replay runs first — leaving uncheckpointed tiger.* writes makes
-- the DB unreadable until us_geocoder loads).
CHECKPOINT;
SQL
DK_RC=${PIPESTATUS[0]}
set -e
DK_END=$(date +%s)
DK_ELAPSED=$((DK_END - DK_START))

if [[ "$DK_RC" -ne 0 ]]; then
    echo ""
    echo "DuckDB loader FAILED (exit $DK_RC after ${DK_ELAPSED}s)."
    echo "See $DK_LOG for details. Re-run this script — load resumes from where it stopped."
    exit 1
fi

echo ""
echo "DuckDB load complete in ${DK_ELAPSED}s ($((DK_ELAPSED/60)) min)."

# ─── Sanity: row counts ────────────────────────────────────────
echo "=== Row counts (DuckDB side) ==="
"$DUCKDB_BIN" "$DUCKDB_DB" <<'SQL'
LOAD us_geocoder; LOAD spatial;
SELECT 'state'        AS tbl, count(*) FROM tiger.state
UNION ALL SELECT 'county',     count(*) FROM tiger.county
UNION ALL SELECT 'place',      count(*) FROM tiger.place
UNION ALL SELECT 'cousub',     count(*) FROM tiger.cousub
UNION ALL SELECT 'zcta5',      count(*) FROM tiger.zcta5
UNION ALL SELECT 'edges',      count(*) FROM tiger.edges
UNION ALL SELECT 'faces',      count(*) FROM tiger.faces
UNION ALL SELECT 'featnames',  count(*) FROM tiger.featnames
UNION ALL SELECT 'addr',       count(*) FROM tiger.addr
UNION ALL SELECT 'edge_containment', count(*) FROM tiger.edge_containment;
SQL

echo ""
echo "Next:  ./03b_pg_mirror_from_duckdb.sh   (mirror tiger.* → PG, ~minutes)"
echo "Then:  ./04_verify.sh, 05/06_bench_*.sh, 07_export_and_compare.sh"
