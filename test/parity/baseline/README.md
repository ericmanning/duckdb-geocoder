# Parity baseline

Snapshot of [`scripts/parity/run_geocode_regress.sh`](../../../scripts/parity/run_geocode_regress.sh) run against a `pg_parity.duckdb` reference DB containing TIGER 2025 nation + MA + MN. Captured for comparison after future loader/geocoder changes.

## Reproduce

```sh
# One-time DB build (~25 min):
./build/release/duckdb ~/pg_parity.duckdb <<'EOF'
LOAD us_geocoder;
CALL load_tiger_nation();
CALL load_tiger_states(['MA','MN'], build_containment := false);
EOF

# Run harness:
./scripts/parity/run_geocode_regress.sh ~/pg_parity.duckdb > test/parity/baseline/geocode_regress.txt
```

## Reading the snapshot

Each PG test ID gets one of three verdicts:

- **`✓ match`** — our output bytewise-identical to PG's. Currently 0/58.
- **`! diverge`** — at least one row differs. Currently 23/58.
- **`✗ missing`** — test ID exists in PG's expected file but harness didn't run it (batched-VALUES queries the simple-shape regex skips). Currently 35/58.

## Divergence tiers (manual analysis, 2026-04-24 baseline)

Of the 23 diverging tests:

### Tier 1 — TIGER vintage drift (10 cases)

Same canonicalized address text, same integer rating, lat/lng off by ≤ 0.0001 degrees (~10 m). These reflect TIGER 2025 block-shift vs PG's ~2010-era reference data. Not a code bug.

`T1, T2, T4, T5, T7, T8, T11, T17, #1087c, T18b`

### Tier 2 — small rating arithmetic difference (4 cases)

Same address, rating off by 1-2. Likely ZIP-penalty / Levenshtein cumulative rounding differences vs PG's PL/pgSQL.

`T3, T15, #1087a` (and T18b spans Tier 1 and Tier 2)

### Tier 3 — relaxed Stage A/B short-circuit (D7) — we return more candidates (8 cases)

For partial / fuzzy queries we list additional cities PG doesn't because we relaxed PG's `if best rating < 30 → skip Stage B` short-circuit. Documented deviation in [docs/parity.md](../../../docs/parity.md) D7.

`T6, T9, T12, T13, T16, #1087b, #1073b`

### Tier 4 — semantic divergence worth investigating (1 case)

`T18a`: input `26 Court Street, 02109` — PG picks `26 Court St`, we pick `26 Court Sq`. Both are real Boston streets. With no state in the input, the city/state-from-ZIP fallback path differs.

## What "missing" means

35 tests use batched-VALUES form like `SELECT geocode(target,1) FROM (VALUES (...)) AS f(target)`. The harness regex extracts simple `FROM geocode('addr', N)` shapes only. These need a smarter extractor — patches welcome.
