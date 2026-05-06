#!/usr/bin/env bash
# Parity harness: run PG's geocode_regress.sql test inputs through our
# tiger.geocode() and compare output against PG's expected file.
#
# This is NOT a CI test — it requires a reference DB that has TIGER data
# loaded for at least Massachusetts and Minnesota (the states PG's tests
# touch). It's a diagnostic tool: when someone claims "us_geocoder
# disagrees with PG on address X," run this against their TIGER data and
# see whether the disagreement is a real divergence or a TIGER-vintage
# drift (block reshuffles, new addresses, etc.).
#
# Usage:
#   ./scripts/parity/run_geocode_regress.sh <reference_db.duckdb>
#
# Prerequisites:
#   - Built extension: ./build/release/duckdb exists
#   - Reference DB has TIGER 2025 (or other) loaded for MA + MN (or any
#     states the tests touch). Build via:
#       ATTACH 'pg_parity.duckdb' AS tgt;
#       CALL load_tiger_nation(target_db := 'tgt');
#       CALL load_tiger_states(['MA','MN'], target_db := 'tgt');
#   - us_address_standardizer community extension installed (auto-loaded
#     by us_geocoder; we use tiger.from_pagc() which depends on it).
#
# Output:
#   - /tmp/parity_geocode_actual.txt — our outputs in PG format
#   - stdout: a per-test summary (match / divergence / missing) and a
#     final tally. Exit code 0 if all tests match, 1 otherwise.

set -eu -o pipefail
cd "$(dirname "$0")/../.."

REPO_ROOT="$(pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-$REPO_ROOT/build/release/duckdb}"
REF_DB=""
BASELINE="${BASELINE:-pg2025}"

# Two positional/flag forms supported:
#   $0 <reference_db.duckdb>                       # default baseline = pg2025
#   $0 <reference_db.duckdb> --baseline=vendored   # legacy comparison
for arg in "$@"; do
    case "$arg" in
        --baseline=pg2025|--baseline=vendored) BASELINE="${arg#*=}" ;;
        *) [[ -z "$REF_DB" ]] && REF_DB="$arg" ;;
    esac
done

case "$BASELINE" in
    pg2025)   EXPECTED="$REPO_ROOT/test/parity/upstream/geocode_regress_2025_pagc" ;;
    vendored) EXPECTED="$REPO_ROOT/test/parity/upstream/geocode_regress" ;;
esac

INPUTS="$REPO_ROOT/test/parity/inputs/geocode_regress.csv"
ACTUAL=/tmp/parity_geocode_actual.txt

if [[ -z "$REF_DB" ]]; then
    echo "usage: $0 <reference_db.duckdb> [--baseline=pg2025|vendored]" >&2
    echo "  reference_db must have TIGER loaded for MA + MN (and ideally other test states)" >&2
    echo "  default baseline = pg2025 (PG-with-PAGC against TIGER 2025; the parity oracle)" >&2
    echo "  --baseline=vendored compares against PG's original expected file (built-in parser, ~2010-era TIGER)" >&2
    exit 2
fi
if [[ ! -x "$DUCKDB_BIN" ]]; then
    echo "ERROR: $DUCKDB_BIN not found. Run 'make release' first." >&2
    exit 2
fi
if [[ ! -f "$REF_DB" ]]; then
    echo "ERROR: reference DB not found: $REF_DB" >&2
    exit 2
fi
if [[ ! -f "$EXPECTED" ]] || [[ ! -f "$INPUTS" ]]; then
    echo "ERROR: parity files missing — expected $EXPECTED and $INPUTS" >&2
    exit 2
fi

n_inputs=$(($(wc -l < "$INPUTS") - 1))
echo "Loaded $n_inputs test cases from $INPUTS"

# Run our geocoder against each test case, output in PG's pipe-delimited format.
# PG's pprint_addy(addy) format: "<num> <pre_dir> <street_name> <street_type>, <city>, <state> <zip>"
# (whitespace squeezed, NULL fields elided). We replicate this with a SQL macro.
echo "Running our geocoder against $REF_DB ..."
"$DUCKDB_BIN" "$REF_DB" 2>/dev/null <<EOF > "$ACTUAL"
LOAD us_geocoder; LOAD spatial;
LOAD us_address_standardizer;
.mode list
.separator '|'
.headers off

-- Use the extension's tiger.pprint_adr (handles is_hw type-prepending).

-- DuckDB rejects correlated columns inside the geocode() macro's
-- internal LIMIT. Workaround: ask for up to 50 candidates (largest
-- max_n in PG's test set) and trim per-test via ROW_NUMBER below.
-- Output shape mirrors PG's expected file:
--   simple tests:   <id>|<address>|<point>|<rating>      (4 fields)
--   batched tests:  <id>|<address>|<target>|<point>|<rating>  (5 fields)
WITH inputs AS (
    SELECT * FROM read_csv('$INPUTS', header=true, auto_detect=true)
),
geocoded AS (
    SELECT inputs.test_id, inputs.raw AS target, inputs.max_n,
           inputs.is_batched, g.adr, g.rating,
           -- Snap to PG's 5-decimal grid so the tiebreak ORDER BY is stable.
           tiger.st_snaptogrid(g.geom, 0.00001) AS snapped,
           ROW_NUMBER() OVER (
               PARTITION BY inputs.test_id, inputs.raw
               ORDER BY g.rating,
                        ST_X(tiger.st_snaptogrid(g.geom, 0.00001)),
                        ST_Y(tiger.st_snaptogrid(g.geom, 0.00001))
           ) AS rn
    FROM inputs
    CROSS JOIN LATERAL tiger.geocode(tiger.from_pagc(raw), 50, NULL, 'none') AS g
)
SELECT
    test_id || '|' || tiger.pprint_adr(adr)
        || CASE WHEN is_batched = 1 THEN '|' || target ELSE '' END
        -- PG renders ST_AsText(ST_SnapToGrid(geom,0.00001)) with trailing
        -- zeros ("42.35900"); duckdb-spatial's ST_AsText drops them. Use
        -- printf to force the .5f formatting so byte-comparison succeeds.
        || '|POINT(' || printf('%.5f', ST_X(snapped)) || ' '
                     || printf('%.5f', ST_Y(snapped)) || ')|'
        || rating::VARCHAR
FROM geocoded
WHERE rn <= max_n
ORDER BY test_id, target, rating, ST_X(snapped), ST_Y(snapped);
EOF

echo "  wrote $(wc -l < "$ACTUAL" | tr -d ' ') output rows to $ACTUAL"

# Diff actual vs expected. The diff is informational — many divergences are
# expected (TIGER vintage drift). Tally exact-match / partial-match / divergent.
echo
echo "Comparing to PG expected ($EXPECTED):"
echo

ACTUAL_BY_ID=/tmp/parity_actual_by_id.txt
EXPECTED_BY_ID=/tmp/parity_expected_by_id.txt
# Filter to test-id-prefixed rows (drops `# ...` provenance comments in
# the pg2025 baseline; harmless on the vendored file which has none).
grep -E '^[T#][A-Za-z0-9]' "$ACTUAL"   | sort > "$ACTUAL_BY_ID"
grep -E '^[T#][A-Za-z0-9]' "$EXPECTED" | sort > "$EXPECTED_BY_ID"

ids_actual="$(awk -F'|' '{print $1}' "$ACTUAL_BY_ID"   | sort -u)"
ids_expected="$(awk -F'|' '{print $1}' "$EXPECTED_BY_ID" | sort -u)"

n_match=0
n_diverge=0
n_missing=0

for id in $ids_expected; do
    exp_rows=$(grep "^${id}|" "$EXPECTED_BY_ID" | sort || true)
    act_rows=$(grep "^${id}|" "$ACTUAL_BY_ID"   | sort || true)
    if [[ -z "$act_rows" ]]; then
        printf "  ✗ %-15s MISSING (test was not run by harness)\n" "$id"
        n_missing=$((n_missing + 1))
    elif [[ "$exp_rows" == "$act_rows" ]]; then
        printf "  ✓ %-15s match\n" "$id"
        n_match=$((n_match + 1))
    else
        # Normalize POINT(X Y) coordinates to N decimal places before
        # comparing — PG's ST_AsText(ST_SnapToGrid(...)) and our
        # printf('%.5f', ...) can differ at the 5th decimal due to UTM
        # round-trip precision. We don't care about sub-meter geom drift
        # for parity purposes; only addr text and rating.
        normalize_geom() {
            # Round X,Y to 4 decimals (~10m precision; well above any
            # legitimate float drift from interpolation arithmetic).
            sed -E 's|POINT\(([+-]?[0-9]+\.[0-9]+) ([+-]?[0-9]+\.[0-9]+)\)|POINT('"$(printf '%%.4f %%.4f')"')|g'
        }
        exp_norm=$(echo "$exp_rows" | awk -F'|' 'BEGIN{OFS="|"} {
            for (i=1; i<=NF; i++) {
                if (match($i, /^POINT\(/)) {
                    coords = substr($i, 7, length($i)-7)
                    split(coords, parts, " ")
                    $i = sprintf("POINT(%.4f %.4f)", parts[1], parts[2])
                }
            }
            print
        }')
        act_norm=$(echo "$act_rows" | awk -F'|' 'BEGIN{OFS="|"} {
            for (i=1; i<=NF; i++) {
                if (match($i, /^POINT\(/)) {
                    coords = substr($i, 7, length($i)-7)
                    split(coords, parts, " ")
                    $i = sprintf("POINT(%.4f %.4f)", parts[1], parts[2])
                }
            }
            print
        }')
        if [[ "$exp_norm" == "$act_norm" ]]; then
            printf "  ✓ %-15s match (geom rounded to 4 decimals)\n" "$id"
            n_match=$((n_match + 1))
        else
            printf "  ! %-15s diverge\n" "$id"
            diff <(echo "$exp_norm") <(echo "$act_norm") 2>/dev/null | sed 's/^/      /' || true
            n_diverge=$((n_diverge + 1))
        fi
    fi
done

echo
echo "Summary: $n_match match | $n_diverge diverge | $n_missing missing"
echo "Total expected test IDs: $(echo "$ids_expected" | wc -l | tr -d ' ')"

if (( n_diverge == 0 && n_missing == 0 )); then
    exit 0
fi
exit 1
