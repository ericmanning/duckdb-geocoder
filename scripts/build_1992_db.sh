#!/usr/bin/env bash
# Build a self-contained DuckDB file holding all 1992 TIGER/Line states,
# for transfer into an offline environment. Resumable: re-run after an
# interruption and completed counties are skipped via the progress ledger.
#
#   ./scripts/build_1992_db.sh [OUT_DB] [DUCKDB_BIN]
#
# Needs network (Census CDN) and a duckdb that can load us_geocoder + spatial.
# Defaults to the statically-linked CLI from `make release`.
set -euo pipefail

OUT_DB="${1:-tiger1992.duckdb}"
DUCKDB="${2:-./build/release/duckdb}"

[ -x "$DUCKDB" ] || { echo "no duckdb at $DUCKDB — run 'make release' or pass a path" >&2; exit 1; }
[ -e "$OUT_DB" ] && echo "note: $OUT_DB exists — resuming, already-loaded counties will be skipped" >&2

echo "building $OUT_DB (50 states + DC; hours, and tens of GB — resumable)" >&2

"$DUCKDB" -init /dev/null "$OUT_DB" <<'SQL'
LOAD spatial;
CALL load_tiger_1992_all_states();
-- Mandatory: without it the writes stay in the WAL and the file will not
-- reopen (replay needs the tiger schema, which only exists once the
-- extension has loaded). See docs/tiger1992.md § Persistence.
CHECKPOINT;
SELECT 'edges' t, count(*) n FROM tiger.edges
UNION ALL SELECT 'addr', count(*) FROM tiger.addr
UNION ALL SELECT 'featnames', count(*) FROM tiger.featnames
UNION ALL SELECT 'states', count(*) FROM tiger.state
ORDER BY 1;
SQL

echo "done: $OUT_DB ($(du -h "$OUT_DB" | cut -f1))" >&2
