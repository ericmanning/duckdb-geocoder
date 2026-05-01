# Parity baseline

Snapshots of the parity harnesses run against a `pg_parity.duckdb` reference DB
containing TIGER 2025 nation + MA + MN. Captured for comparison after future
loader/geocoder changes.

## Reproduce

```sh
# One-time DB build (~25 min):
./build/release/duckdb ~/pg_parity.duckdb <<'EOF'
LOAD us_geocoder;
CALL load_tiger_nation();
CALL load_tiger_states(['MA','MN'], build_containment := false);
EOF

# Run harnesses:
./scripts/parity/run_geocode_regress.sh         ~/pg_parity.duckdb > test/parity/baseline/geocode_regress.txt
./scripts/parity/run_reverse_geocode_regress.sh ~/pg_parity.duckdb > test/parity/baseline/reverse_geocode_regress.txt
```

## geocode_regress (forward geocode)

Each PG test ID gets one of three verdicts:

- **`✓ match`** — our output bytewise-identical to PG's. Currently 0/58.
- **`! diverge`** — at least one row differs. Currently 23/58.
- **`✗ missing`** — test ID exists in PG's expected file but harness didn't run it (batched-VALUES queries the simple-shape regex skips). Currently 35/58.

### Divergence tiers (manual analysis, 2026-04-24 baseline)

Of the 23 diverging tests:

#### Tier 1 — TIGER vintage drift (10 cases)

Same canonicalized address text, same integer rating, lat/lng off by ≤ 0.0001 degrees (~10 m). These reflect TIGER 2025 block-shift vs PG's ~2010-era reference data. Not a code bug.

`T1, T2, T4, T5, T7, T8, T11, T17, #1087c, T18b`

#### Tier 2 — small rating arithmetic difference (4 cases)

Same address, rating off by 1-2. Likely ZIP-penalty / Levenshtein cumulative rounding differences vs PG's PL/pgSQL.

`T3, T15, #1087a` (and T18b spans Tier 1 and Tier 2)

#### Tier 3 — relaxed Stage A/B short-circuit (D7) — we return more candidates (8 cases)

For partial / fuzzy queries we list additional cities PG doesn't because we relaxed PG's `if best rating < 30 → skip Stage B` short-circuit. Documented deviation in [docs/parity.md](../../../docs/parity.md) D7.

`T6, T9, T12, T13, T16, #1087b, #1073b`

#### Tier 4 — semantic divergence worth investigating (1 case)

`T18a`: input `26 Court Street, 02109` — PG picks `26 Court St`, we pick `26 Court Sq`. Both are real Boston streets. With no state in the input, the city/state-from-ZIP fallback path differs.

### What "missing" means

35 tests use batched-VALUES form like `SELECT geocode(target,1) FROM (VALUES (...)) AS f(target)`. The harness regex extracts simple `FROM geocode('addr', N)` shapes only. These need a smarter extractor — patches welcome.

## reverse_geocode_regress (reverse geocode)

8 PG test points (5 anonymous → T1–T5; 3 ticketed → #1913 #2927 #3806). The
parity oracle is `test/parity/upstream/reverse_geocode_regress_pg2025` — PG's
*current* output against TIGER 2025, captured byte-for-byte from the running
container. (The historical regress file `scripts/parity/pg_compare/.../reverse_geocode_regress`
was captured against ~2010-era TIGER and drifts under TIGER 2025 even when
running PG itself; the current snapshot is the right comparison target.)

Current state (2026-05-01, after structural port: face-side narrowing,
multi-key sort, primary-line filter, parity-corrected interpolation,
`ST_SnapToGrid(0.00005)` on input):

```
✓ #2927   match  — 71 N Washington St, Boston, MA 02114
✓ #3806   match  — 220 3rd Ave N, Minneapolis, MN 55401
✓ T2      match  — 98 Merriam St, Auburn, MA 01501
✓ T3      match  — 190 Washington St, Boston, MA 02108
✓ T4      match  — 28 Capen St, Medford, MA 02155
✓ T5      match  — 106 Massachusetts Ave, Cambridge, MA 02139
! #1913   us-better — we resolve ZIP 02494 via ZCTA fallback; PG returns no ZIP
! T1      us-better — we resolve ZIP 02493 via ZCTA fallback; PG returns no ZIP
```

6/8 byte-identical to PG; remaining 2 are us-better. Both PG and us pull ZIP
from `addr.zip` when present and fall back to the ZCTA spatial lookup when
not. For interstate points (no `addr.zip`), the fallback is what fills the
ZIP — PG's fallback fails for these two points (likely a missing-state-clip
in their ZCTA load); ours succeeds because we don't filter ZCTA by `statefp`
(our loader leaves `tiger.zcta5.statefp = NULL` until per-state clip is
implemented; see `loader_templates.sql.in :nation_zcta5:` + geocode_flow.md
§14.9).

### Note on the historical expected file

The original PG regress file shows different addresses for 5 cases (T2
`I- 90`, T3 `158`, T4 `32`, T5 `58`, #2927 `77`, #3806 `212`) — those are
the answers PG returned in the early 2010s. Running current PG against
TIGER 2025 reproduces our answers exactly. The drift comes from TIGER edge
geometry / house-number range updates over 15 years, not from a code bug
on either side.
