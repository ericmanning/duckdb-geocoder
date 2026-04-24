#!/usr/bin/env bash
# Shared helpers for the loader-perf benchmarks. Source from test scripts.

set -eu -o pipefail

# Repo root (scripts/benchmarks/helpers.sh → ../..). ${BASH_SOURCE} may be
# unset when bash -n just syntax-checks; fall back to $0.
_src="${BASH_SOURCE[0]:-$0}"
REPO_ROOT="$(cd "$(dirname "$_src")/../.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-$REPO_ROOT/build/release/duckdb}"

# Small state used by default. RI has 5 counties, ~136 K edges, ~130 MB of
# zips — small enough to iterate quickly, big enough to show signal.
BENCH_STATE="${BENCH_STATE:-RI}"
BENCH_FIPS="${BENCH_FIPS:-44}"
BENCH_YEAR="${BENCH_YEAR:-2025}"

# Local mirror dir (shared across tests; populate once, reuse).
BENCH_MIRROR="${BENCH_MIRROR:-/tmp/us_geocoder_bench_$BENCH_STATE}"

# Set BENCH_IFACE=<name> to measure a single interface. Default: aggregate
# across every non-loopback interface (catches the case where a VPN default
# route points at utun* but HTTPS actually exits via en0 / Wi-Fi).
BENCH_IFACE="${BENCH_IFACE:-ALL}"

# Read cumulative input bytes — either for a specific interface or summed
# across all non-loopback interfaces if BENCH_IFACE is "ALL". Output: integer.
read_iface_bytes_in() {
    case "$(uname)" in
        Darwin)
            # netstat -i -b output columns: Name Mtu Network Address Ipkts Ierrs Ibytes Opkts Oerrs Obytes Coll
            # Only the first row per interface has a Network/Address; later
            # rows duplicate the interface name with different link-layer info.
            # Use `netstat -I <iface>` when asked for a single one.
            if [[ "$BENCH_IFACE" == "ALL" ]]; then
                netstat -i -b 2>/dev/null \
                    | awk 'NR>1 && $1 !~ /^lo/ && !seen[$1]++ { sum += $7 } END {print sum+0}'
            else
                netstat -I "$BENCH_IFACE" -b 2>/dev/null \
                    | awk -v i="$BENCH_IFACE" '$1 == i {print $7; exit}' || echo 0
            fi
            ;;
        Linux)
            if [[ "$BENCH_IFACE" == "ALL" ]]; then
                awk -F '[: ]+' '/^[[:space:]]*[^l][^o]/ { sum += $3 } END {print sum+0}' /proc/net/dev
            else
                awk -v i="$BENCH_IFACE:" '$1 == i {print $2; exit}' /proc/net/dev 2>/dev/null || echo 0
            fi
            ;;
        *) echo 0 ;;
    esac
}

# Human-readable bytes.
human_bytes() {
    local b="$1"
    awk -v b="$b" 'BEGIN {
        split("B KB MB GB TB", u)
        i = 1
        while (b >= 1024 && i < 5) { b /= 1024; i++ }
        printf "%.1f %s", b, u[i]
    }'
}

# Ensure the local Census-nested mirror for $BENCH_STATE exists. Uses the
# existing scripts/parallel_download_state.sh. Idempotent — skips files on disk.
ensure_mirror() {
    if [[ -s "$BENCH_MIRROR/STATE/tl_${BENCH_YEAR}_us_state.zip" ]]; then
        return
    fi
    echo "  fetching local mirror into $BENCH_MIRROR (one-time) …"
    "$REPO_ROOT/scripts/parallel_download_state.sh" \
        "$BENCH_STATE" "$BENCH_MIRROR" "$BENCH_YEAR" 16 >/dev/null
}

# Create a scratch .duckdb path. mktemp gives us an empty file; delete it
# so duckdb creates a fresh DB there. Caller is responsible for cleanup.
# Register a cleanup trap in the caller if the test may exit non-zero.
make_scratch_db() {
    local p
    p="$(mktemp -t us_geocoder_bench_XXXXXX).duckdb"
    rm -f "$p" "$p.wal"
    echo "$p"
}

# Print a one-liner timing for a command, returning the `real` seconds as float.
time_command_secs() {
    local tf; tf="$(mktemp)"
    /usr/bin/time -p "$@" >/dev/null 2>"$tf" || true
    awk '/^real/ {print $2}' "$tf"
    rm -f "$tf"
}

# Sub-second wall-clock reading (float seconds since epoch) — for timing
# multi-process parallel blocks where `date +%s` truncates too aggressively.
# Uses python3 if available; falls back to perl; falls back to `date +%s`.
wall_seconds() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time; print(time.time())'
    elif command -v perl >/dev/null 2>&1; then
        perl -MTime::HiRes=time -e 'print time'
    else
        date +%s
    fi
}
