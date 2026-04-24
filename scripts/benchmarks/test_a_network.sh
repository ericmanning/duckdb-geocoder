#!/usr/bin/env bash
# Test A — bytes-on-wire during a serial /vsicurl/ state load.
# Hypothesis 1: GDAL's /vsicurl/ uses HTTP Range requests to fetch only the
# portions of each shapefile it actually needs, so serial downloads pull far
# fewer bytes than the full-zip total. If true, adding parallel full-zip
# downloads can't help much — we'd be moving *more* bytes in parallel to
# approximately tie the serial-partial-fetch path.
#
# How to interpret:
#   total zip size (full download): ~130 MB for RI
#   if measured bytes-in during load ≪ total zip size  →  partial fetches confirmed
#   if measured bytes-in ≈ total zip size              →  hypothesis falsified,
#                                                        something else is
#                                                        holding back parallelism
#
# Noise: counts *all* interface traffic during the window, not just our
# downloads. Run with other network-heavy apps quiesced. Repeat 3× and
# take the minimum.

set -eu -o pipefail
cd "$(dirname "$0")/../.."
source scripts/benchmarks/helpers.sh

echo "=== Test A: bytes-on-wire during serial /vsicurl/ state load ==="
echo "state=$BENCH_STATE iface=$BENCH_IFACE"
echo

if [[ -z "$BENCH_IFACE" ]]; then
    echo "ERROR: couldn't detect a primary interface; set BENCH_IFACE=<iface> and re-run." >&2
    exit 2
fi

# Expected full-zip total for comparison. We can compute this by statting
# a pre-built mirror (one-time download) or from the Census HEAD responses.
# Simpler: ensure mirror exists first, then du it.
ensure_mirror
ZIPS_ON_DISK_BYTES="$(du -sk "$BENCH_MIRROR" | awk '{print $1 * 1024}')"
echo "full-zip mirror size: $(human_bytes "$ZIPS_ON_DISK_BYTES")"

# Run a fresh nation + state load against the CDN (not the mirror) while
# bracketing with interface byte counters. Use a fresh scratch DB so we're
# not fighting WAL catchup from prior runs.
SCRATCH="$(make_scratch_db)"
trap 'rm -f "$SCRATCH" "$SCRATCH.wal"' EXIT

echo "warm-up: downloading nation-level to seed county table (not measured) …"
"$DUCKDB_BIN" "$SCRATCH" -c "LOAD us_geocoder; CALL load_tiger_nation();" > /dev/null 2>&1

# Now measure just the state load. Default loader uses /vsicurl/ (HTTP source).
BYTES_BEFORE="$(read_iface_bytes_in)"
START_NS=$(date +%s)
"$DUCKDB_BIN" "$SCRATCH" -c "LOAD us_geocoder; CALL load_tiger_state('$BENCH_STATE', build_containment := false);" > /dev/null 2>&1
END_NS=$(date +%s)
BYTES_AFTER="$(read_iface_bytes_in)"

# Compute just the state-level mirror size (exclude nation zips which we already fetched).
STATE_ZIPS_BYTES="$(find "$BENCH_MIRROR" \( -path "*/PLACE/*${BENCH_FIPS}_*" -o -path "*/COUSUB/*${BENCH_FIPS}_*" -o -path "*/EDGES/*${BENCH_FIPS}*" -o -path "*/FACES/*${BENCH_FIPS}*" -o -path "*/FEATNAMES/*${BENCH_FIPS}*" -o -path "*/ADDR/*${BENCH_FIPS}*" \) -type f -print0 | xargs -0 stat -f %z 2>/dev/null | awk '{s+=$1} END {print s+0}')"

DUR=$((END_NS - START_NS))
DELTA=$((BYTES_AFTER - BYTES_BEFORE))

echo
printf "state load duration:     %d s\n" "$DUR"
printf "bytes-in on %-10s %s\n" "$BENCH_IFACE:" "$(human_bytes "$DELTA")"
printf "full state-level zips:   %s\n" "$(human_bytes "$STATE_ZIPS_BYTES")"
if (( STATE_ZIPS_BYTES > 0 )); then
    RATIO="$(awk -v d="$DELTA" -v z="$STATE_ZIPS_BYTES" 'BEGIN {printf "%.1f", 100*d/z}')"
    printf "ratio (observed / full): %s%%\n" "$RATIO"
fi

echo
echo "interpretation:"
echo "  < 60%  → /vsicurl/ IS doing partial fetches (hypothesis 1 confirmed)"
echo "  ≈ 100% → /vsicurl/ fetched full zips (hypothesis 1 falsified)"
echo "  > 100% → HTTP overhead / retries / other-traffic noise; re-run quietly"
