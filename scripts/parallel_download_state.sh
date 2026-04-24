#!/usr/bin/env bash
# Parallel-download a single state's TIGER/Line shapefiles from the Census CDN
# into a Census-nested local mirror that `load_tiger_state('XX', './dir')` can
# ingest directly.
#
# Avoids the 5-8 min serial /vsicurl/ overhead in the loader by issuing many
# HTTPS fetches concurrently via xargs -P. Counties are enumerated by scraping
# the Census directory index (no hardcoded county-FIPS list needed).
#
# Usage:
#   ./parallel_download_state.sh NJ [DEST_DIR] [YEAR] [PARALLELISM]
#   ./parallel_download_state.sh NJ ./tiger_nj 2025 16
#
# Then from DuckDB:
#   CALL load_tiger_nation('./tiger_nj');
#   CALL load_tiger_state('NJ', './tiger_nj');
#
# Idempotent — re-running skips files already on disk.

set -euo pipefail

STATE_IN="${1:?state abbrev required (e.g. NJ)}"
DEST="${2:-./tiger_$(echo "$STATE_IN" | tr '[:upper:]' '[:lower:]')}"
YEAR="${3:-2025}"
PARALLELISM="${4:-16}"

STATE=$(echo "$STATE_IN" | tr '[:lower:]' '[:upper:]')

# 50 states + DC. Territories omitted; pass them explicitly if needed.
# Kept in sync with tiger.state_lookup (FIPS 01–56). Case statement used
# instead of an associative array for bash 3.2 compatibility (macOS default).
state_to_fips() {
    case "$1" in
        AL) echo 01 ;; AK) echo 02 ;; AZ) echo 04 ;; AR) echo 05 ;;
        CA) echo 06 ;; CO) echo 08 ;; CT) echo 09 ;; DE) echo 10 ;;
        DC) echo 11 ;; FL) echo 12 ;; GA) echo 13 ;; HI) echo 15 ;;
        ID) echo 16 ;; IL) echo 17 ;; IN) echo 18 ;; IA) echo 19 ;;
        KS) echo 20 ;; KY) echo 21 ;; LA) echo 22 ;; ME) echo 23 ;;
        MD) echo 24 ;; MA) echo 25 ;; MI) echo 26 ;; MN) echo 27 ;;
        MS) echo 28 ;; MO) echo 29 ;; MT) echo 30 ;; NE) echo 31 ;;
        NV) echo 32 ;; NH) echo 33 ;; NJ) echo 34 ;; NM) echo 35 ;;
        NY) echo 36 ;; NC) echo 37 ;; ND) echo 38 ;; OH) echo 39 ;;
        OK) echo 40 ;; OR) echo 41 ;; PA) echo 42 ;; RI) echo 44 ;;
        SC) echo 45 ;; SD) echo 46 ;; TN) echo 47 ;; TX) echo 48 ;;
        UT) echo 49 ;; VT) echo 50 ;; VA) echo 51 ;; WA) echo 53 ;;
        WV) echo 54 ;; WI) echo 55 ;; WY) echo 56 ;;
        *)  echo "" ;;
    esac
}

FP=$(state_to_fips "$STATE")
if [ -z "$FP" ]; then
    echo "error: unknown state abbrev '$STATE_IN' (expected a 50-state or DC abbrev)" >&2
    exit 1
fi

ROOT="https://www2.census.gov/geo/tiger/TIGER${YEAR}"

mkdir -p "$DEST"/{STATE,COUNTY,ZCTA520,PLACE,COUSUB,EDGES,FACES,FEATNAMES,ADDR}

echo "State: $STATE (FIPS $FP)"
echo "Dest:  $DEST"
echo "Year:  $YEAR"
echo "Parallelism: $PARALLELISM"
echo

# Nation + state-level zips (known filenames, no scraping needed).
declare -a targets=(
    "STATE/tl_${YEAR}_us_state.zip"
    "COUNTY/tl_${YEAR}_us_county.zip"
    "ZCTA520/tl_${YEAR}_us_zcta520.zip"
    "PLACE/tl_${YEAR}_${FP}_place.zip"
    "COUSUB/tl_${YEAR}_${FP}_cousub.zip"
)

# County-level: scrape each subdir's HTTP index for files matching the
# state-FIPS prefix. Parses the Apache directory listing HTML with a
# simple regex. Robust enough for Census's static FTP layout.
# Written for bash 3.2 (macOS default) — no mapfile, no associative arrays.
echo "Enumerating county-level files…"
for sub in EDGES FACES FEATNAMES ADDR; do
    count=0
    while IFS= read -r f; do
        targets+=("${sub}/${f}")
        count=$((count + 1))
    done < <(
        curl -sSfL "${ROOT}/${sub}/" \
            | grep -oE "tl_${YEAR}_${FP}[0-9]{3}_[a-z]+\.zip" \
            | sort -u
    )
    echo "  $sub: $count files"
done

total=${#targets[@]}
echo
echo "Downloading $total files with $PARALLELISM concurrent workers…"

export ROOT DEST

# xargs dispatches each relative path to a worker that downloads it unless
# it's already on disk. `sh -c` avoids bash-specific quoting gymnastics.
printf '%s\n' "${targets[@]}" \
    | xargs -n 1 -P "$PARALLELISM" -I {} sh -c '
        path="$1"
        src="${ROOT}/${path}"
        dst="${DEST}/${path}"
        if [ -s "$dst" ]; then
            exit 0
        fi
        if curl -sSfL --retry 3 --retry-delay 2 -o "$dst.tmp" "$src"; then
            mv "$dst.tmp" "$dst"
            printf "  ✓ %s\n" "$path"
        else
            rm -f "$dst.tmp"
            printf "  ✗ %s (HTTP error)\n" "$path" >&2
            exit 1
        fi
    ' _ {}

# Summary
downloaded=$(find "$DEST" -name "tl_${YEAR}_*.zip" | wc -l | tr -d ' ')
size=$(du -sh "$DEST" | cut -f1)
echo
echo "Done. $downloaded zips in $DEST ($size total)."
echo
STATE_LC=$(echo "$STATE" | tr '[:upper:]' '[:lower:]')
echo "Next:"
echo "  ./build/release/duckdb tiger_${STATE_LC}_${YEAR}.duckdb <<EOF"
echo "    CALL load_tiger_nation('$DEST');"
echo "    CALL load_tiger_state('$STATE', '$DEST');"
echo "  EOF"
