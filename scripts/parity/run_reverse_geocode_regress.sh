#!/usr/bin/env bash
# Parity harness: run PG's reverse_geocode_regress.sql test inputs through our
# tiger.reverse_geocode() and compare output against PG's expected file.
#
# Companion to run_geocode_regress.sh — same shape, different test set.
# Source inputs: scripts/parity/pg_compare/tiger_geocoder/src/regress/reverse_geocode_regress.sql
#   (8 ST_Point() calls; 5 unnamed → T1–T5, 3 ticketed → #1913 #2927 #3806).
#
# Usage:
#   ./scripts/parity/run_reverse_geocode_regress.sh <reference_db.duckdb>
#
# Prerequisites:
#   - Built extension: ./build/release/duckdb exists
#   - Reference DB has TIGER 2025 loaded for at least MA + MN (the states all
#     8 PG test points fall in). Build via:
#       ATTACH 'pg_parity.duckdb' AS tgt;
#       CALL load_tiger_nation(target_db := 'tgt');
#       CALL load_tiger_states(['MA','MN'], target_db := 'tgt');
#
# Output:
#   - /tmp/parity_reverse_actual.txt — our outputs canonicalized to
#     <test_id>|<pprint_adr(rank=1)>
#   - stdout: per-test match/diverge summary + final tally. Exit 0 if all
#     match, 1 otherwise.

set -eu -o pipefail
cd "$(dirname "$0")/../.."

REPO_ROOT="$(pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-$REPO_ROOT/build/release/duckdb}"
REF_DB="${1:-}"

EXPECTED="$REPO_ROOT/test/parity/upstream/reverse_geocode_regress_pg2025"
INPUTS="$REPO_ROOT/test/parity/inputs/reverse_geocode_regress.csv"
ACTUAL=/tmp/parity_reverse_actual.txt

if [[ -z "$REF_DB" ]]; then
    echo "usage: $0 <reference_db.duckdb>" >&2
    echo "  reference_db must have TIGER loaded for MA + MN" >&2
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
echo "Loaded $n_inputs test points from $INPUTS"

# For each (test_id, lng, lat) row, take rank=1 from tiger.reverse_geocode and
# format with tiger.pprint_adr(adr). One DuckDB invocation per row keeps the
# macro CTE chain self-contained — wrapping multiple calls in a single LATERAL
# trips DuckDB's scalar-subquery cardinality check on the macro's internal
# (SELECT ... LIMIT 1) lookups.
echo "Running our reverse_geocoder against $REF_DB ..."
: > "$ACTUAL"
while IFS=, read -r test_id lng lat _note; do
    [[ "$test_id" == "test_id" ]] && continue
    "$DUCKDB_BIN" "$REF_DB" 2>/dev/null <<EOF >> "$ACTUAL"
LOAD us_geocoder; LOAD spatial;
.mode list
.separator '|'
.headers off
SELECT '${test_id}|' || tiger.pprint_adr(adr)
FROM tiger.reverse_geocode(ST_Point(${lng}, ${lat}), 5)
WHERE rank = 1;
EOF
done < "$INPUTS"

echo "  wrote $(wc -l < "$ACTUAL" | tr -d ' ') output rows to $ACTUAL"

echo
echo "Comparing to PG expected ($EXPECTED):"
echo

ACTUAL_BY_ID=/tmp/parity_reverse_actual_by_id.txt
EXPECTED_BY_ID=/tmp/parity_reverse_expected_by_id.txt
grep -E '^[T#][A-Za-z0-9]' "$ACTUAL"   | sort > "$ACTUAL_BY_ID"
grep -E '^[T#][A-Za-z0-9]' "$EXPECTED" | sort > "$EXPECTED_BY_ID"

ids_expected="$(awk -F'|' '{print $1}' "$EXPECTED_BY_ID" | sort -u)"

n_match=0
n_diverge=0
n_missing=0

for id in $ids_expected; do
    exp_rows=$(grep "^${id}|" "$EXPECTED_BY_ID" | sort || true)
    act_rows=$(grep "^${id}|" "$ACTUAL_BY_ID"   | sort || true)
    if [[ -z "$act_rows" ]]; then
        printf "  ✗ %-15s MISSING\n" "$id"
        n_missing=$((n_missing + 1))
    elif [[ "$exp_rows" == "$act_rows" ]]; then
        printf "  ✓ %-15s match\n" "$id"
        n_match=$((n_match + 1))
    else
        printf "  ! %-15s diverge\n" "$id"
        diff <(echo "$exp_rows") <(echo "$act_rows") 2>/dev/null | sed 's/^/      /' || true
        n_diverge=$((n_diverge + 1))
    fi
done

echo
echo "Summary: $n_match match | $n_diverge diverge | $n_missing missing"
echo "Total expected test IDs: $(echo "$ids_expected" | wc -l | tr -d ' ')"

if (( n_diverge == 0 && n_missing == 0 )); then
    exit 0
fi
exit 1
