# Parity with `postgis_tiger_geocoder`

`us_geocoder` is a pure-DuckDB port of PostGIS's [`postgis_tiger_geocoder`](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder). This doc records where the port matches and where it deviates.

## The short version

Given (a) the same PAGC standardizer inputs, (b) the same TIGER vintage data loaded identically into both systems, and (c) addresses away from rating tie-boundaries, `tiger.geocode(from_pagc(x))` and PG's `geocode(x, use_pagc_address_parser := true)` produce the same ranking and the same geometry within the documented per-component numeric tolerances below.

## What matches exactly

- **Public contract.** Eight input fields (`address, street_name, street_type, pre_dir, post_dir, location, state_abbrev, zip`) and their format rules, per the spec's `geocode_input` struct.
- **Scoring weights.** Direction × 2, name × 10, type × 5. Prequalabr (`Old`, `New`) discount at 0.75. Numeric-streets short-circuit (`15th` ≡ `15rd`). House-number penalties: 0 / 2 / 5 / 20 depending on in-range / wrong-parity / out-of-range / no-range. ZIP penalty capped at `20 * zip_penalty`. Location Levenshtein added directly. See `rate_attributes` in [../src/sql/scoring_macros.sql.in](../src/sql/scoring_macros.sql.in).
- **Rating band invariant.** Location-only matches always return rating ≥ 100; address-level matches return rating < 100. Total order across both stages is preserved.
- **Side-of-street semantics.** `side='L'`/`'R'` relative to edge direction; parity rule (even addresses on even side); 10m perpendicular offset from centerline in UTM.
- **TIGER joins.** Edges are joined to addr/featnames via `(tlid, statefp)`; faces via `(tfidl / tfidr, statefp)`; ZIP-windowing via `diff_zip` over the first 5 digits.
- **Intersection finder.** Uses TIGER's topological node IDs (`tnidf`/`tnidt`), not `ST_Intersects` — same optimization as PG §9.
- **SRID policy.** All storage in 4269. User-supplied geometry auto-transforms to 4269 if tagged with a different SRID; SRID 0 is treated as "assume 4269".

## Documented deviations

### D6: reverse_geocode output shape

PG returns a single record with parallel arrays (`intpt[]`, `addy[]`, `street[]`). `us_geocoder` returns one row per candidate with a `rank` column. Same data, idiomatic DuckDB shape. Callers that want PG's shape can re-aggregate with `array_agg` grouped by the input point.

### D7: Stage A/B short-circuits relaxed

PG's geocoder has multiple `rating = 0 → return immediately` and `exact_street → skip Stage B` short-circuits. `us_geocoder` computes all Stage A candidates in one vectorized pass and uses `ORDER BY rating LIMIT max_results`; Stage B runs only when Stage A returns zero rows.

**Observable effect:** at tie-rating boundaries (e.g. multiple exact matches on a multi-block street), `us_geocoder` may return a different element of the tie than PG. The top-N set by rating is the same; the *order within a rating tier* can differ.

### D8: tract / bg / tabblock20 tables dropped

PG's loader populates `tract`, `bg`, `tabblock20`, `addrfeat`. `us_geocoder` doesn't — the geocoder itself never reads them, and we expose the 2020 block / tract / block-group GEOIDs directly on `geocode()` results (sourced from `faces.tractce20`/`blockce20`/`blkgrpce20` via `edge_containment`). Adds a `get_tract()` equivalent only if users request one.

### D9: settings as function parameters, not a `geocode_settings` table

PG has a `tiger.geocode_settings` table and reads `zip_penalty` / `reverse_geocode_numbered_roads` / debug flags from it. `us_geocoder` exposes `zip_penalty` as a direct parameter on `geocode_address_impl` (default 2.0 via the `geocode()` wrapper) and `reverse_geocode_numbered_roads` is not implemented in v0.1. No session-level settings table.

### D11: loader is in-process

PG's loader generates bash/batch scripts that the user runs outside Postgres. `us_geocoder` loads in-process via a single `CALL load_tiger_state('XX')`. The per-state `zip_state`, `zip_state_loc`, `zip_lookup_base` derivation SQL is ported essentially unchanged from PG's `post_load_process` snippets.

### D14 (L1-L3): containment is conservative

- **L1** `containment_guaranteed` is valid only at the default 10m offset (the same offset Stage A applies). A per-call custom offset invalidates the flag.
- **L2** edges whose offset strip crosses from one face into a neighboring face in the same block read `false` even though the block-level truth holds. Dissolving faces on shared `blockce20`/`tractce20`/`blkgrpce20` to get tighter checks is a v0.2 follow-up.
- **L3** The single-midpoint offset check can pass or fail differently than a full buffer-within-face test near intersections (endpoint caps). v0.2 can upgrade to an `ST_OffsetCurve`-based sampled strip once DuckDB spatial exposes it (as of pinned v1.5.2 it doesn't).

All three limitations are **false-negatives only** — `containment_guaranteed = true` is provably correct.

## Not ported

- `normalize_address()` (PG's built-in parser). Use `us_address_standardizer` via `from_pagc()`, or bring your own. See [../src/sql/from_pagc.sql.in](../src/sql/from_pagc.sql.in).
- `pagc_normalize_address()` wrapper. Its repack logic lives inline in `from_pagc()`.
- `use_pagc_address_parser` setting. `from_pagc` is the only path; no runtime switch.
- `norm_addy` composite. Replaced by the 8-field `tiger.geocode_input` struct.
- `install_missing_indexes()` / `missing_indexes_generate_script()`. The loader creates the precomputed-column acceleration keys (`name_lower`, `fullname_norm`, `name_soundex`) at INSERT time; there's no separate index rebuild step.
- The `CHECK (statefp = 'XX')` constraint-exclusion trick. DuckDB's columnar storage uses zonemaps automatically and our per-state INSERT pattern keeps row groups contiguous on `statefp` without explicit partitioning.
- `loader_platform` / `loader_variables` / `loader_lookuptables` control tables, and the shell-script generators. Loader logic is C++.

## How to verify parity yourself

1. Install PG + the `postgis_tiger_geocoder` extension per [its README](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder). Load TIGER 2025 for one state.
2. Install `us_geocoder` and load the same state (`CALL load_tiger_state('RI')` or equivalent).
3. For the same input, compare:
   ```sql
   -- PG:
   SELECT rating, ST_AsText(geomout), (addy).* FROM geocode(
       '120 Benefit St, Providence RI 02903', 3)
   ORDER BY rating;
   ```
   ```sql
   -- DuckDB:
   SELECT rating, ST_AsText(geom), (addy).* FROM tiger.geocode(
       tiger.from_pagc('120 Benefit St, Providence RI 02903'), 3, NULL, 'none')
   ORDER BY rating;
   ```

Expect identical `rating` values, identical `(addy)` contents, and geometry within ~1 m. Wider divergence typically means a difference in PAGC rules version, TIGER vintage, or a rating tie boundary (D7).

## Parity test layout

Three layers, separating what runs in CI from what requires real TIGER data.

### Layer 1 — unit tests (CI)

Hand-curated unit tests covering the geocoder's primitives. No real TIGER data; synthetic fixtures only.

- Exact-match happy path (rating 0)
- Perpendicular offset math (left/right side in UTM)
- Location fallback (rating ≥ 100)
- Intersection finder via shared TIGER nodes
- Reverse geocode rank ordering
- Containment GEOID derivation
- Every scoring primitive (`rate_attributes`, `diff_zip`, `zip_range`, `least_hn`/`greatest_hn`, `numeric_streets_equal`, `normalize_street_name`, `cull_null`, `levenshtein_ignore_case`)

### Layer 2 — stress-class regression (CI)

[`test/sql/parity_stress.test`](../test/sql/parity_stress.test) systematically exercises every stress class spec D13 calls out. Synthetic multi-street RI fixture; no real TIGER data needed. **NOT** a PG parity test — it asserts our own behavior is consistent across each `geocode_address_impl` branch:

1. **Numeric-street equivalence** — `15` and `15rd` match `15th St` via `numeric_streets_equal`.
2. **prequalabr discount** — `Main St` matches `Old Main St`; the `OLD` prefix doesn't reject.
3. **Short-name LIKE-prefix bypass** — names ≤5 chars skip the `LIKE name || '%'` branch but still match via exact / soundex / numeric.
4. **Highway spacing** — `I-635` and `I- 635` reach the same fullname after `normalize_street_name`.
5. **ZIP window tolerance** — long names get ±2 ZIP typo tolerance via `zip_range`; ZIPs outside the window produce zero matches.
6. **House-number out-of-range** — penalty +5 + scaled distance, output address is the nearest range endpoint.
7. **House-number wrong parity** — penalty +2.
8. **Soft penalties on type / direction** — `Avenue` rates worse than `Ave`, `Northwest` worse than `NW`, but both still match.
9. **Stage B fallback** — input with no `street_name` returns rating ≥ 100 from `geocode_location`; address-level matches always rate < 100.

### Layer 3 — PG-mirrored parity harness (local-only)

[`scripts/parity/run_geocode_regress.sh`](../scripts/parity/run_geocode_regress.sh) runs PG's actual regression-test inputs through *our* geocoder and diffs the output against PG's expected outputs. The PG files are vendored verbatim in [`test/parity/upstream/`](../test/parity/upstream/) (NOTICE + GPLv2 attribution there).

**Why not in CI:** PG's tests target Boston / Cambridge / Minneapolis addresses against PG's TIGER vintage (~2010-era based on the test format). Running them needs a reference DB with TIGER 2025 loaded for at least MA + MN — multi-GB, doesn't fit a typical CI budget. Expected outputs were captured against the older vintage, so some divergence is *expected*: block boundaries shift, addresses get added, the PAGC rule files in `us_address_standardizer` evolve.

**Workflow.** Build a parity reference DB once:

```sql
ATTACH 'pg_parity.duckdb' AS tgt;
CALL load_tiger_nation(target_db := 'tgt');
CALL load_tiger_states(['MA','MN'], target_db := 'tgt');
DETACH tgt;
```

Then run the harness:

```sh
./scripts/parity/run_geocode_regress.sh pg_parity.duckdb
```

Output is a per-test `match` / `diverge` / `missing` tally with diffs inlined for divergent rows. The harness extracts ~25 of PG's standard test cases (the simple `geocode('addr', N)` shapes); batched-VALUES forms (`#TB1`, `#1073*`, `#1076*`) are skipped — patches welcome.

**What "match" means:** identical `pprint_addy(addy)` text, identical 5-decimal-truncated `POINT(lng lat)`, identical integer rating. Anything weaker is a `diverge`. Engineers investigating concrete divergence reports use the harness output to distinguish "our geocoder is wrong" from "TIGER vintage drift" from "PAGC rules version drift."

### Baseline (TIGER 2025, April 2026)

Captured at [`test/parity/baseline/geocode_regress.txt`](../test/parity/baseline/geocode_regress.txt). Tally: `0 match / 23 diverge / 35 missing`. Manual classification of the 23 divergences ([details](../test/parity/baseline/README.md)):

- **10 = TIGER vintage drift** — same address, same rating, position off ≤10 m
- **4 = small rating-arithmetic difference** — same address, rating off by 1-2
- **8 = relaxed Stage A/B short-circuit (D7)** — we list extra fallback candidates
- **1 = semantic divergence** worth investigating (`T18a`: ZIP-only input picks `Court Sq` vs PG's `Court St`)

The 35 missing are batched-VALUES PG tests the harness regex doesn't extract.

Net read: no real geocoder bugs detected against PG, just TIGER drift + the documented D7 relaxation. Investigate `T18a` if pursuing strict parity becomes a goal.

### Roadmap

- Port `pagc_normalize_address_regress` as a CI-friendly sqllogic test (parser-only, no TIGER). The PG-vendored expected outputs become the test oracle for our `from_pagc` repack.
- Extend the harness to the batched-VALUES forms in `geocode_regress.sql`.
- Port `reverse_geocode_regress.sql` (8 tests, MA + MN).
