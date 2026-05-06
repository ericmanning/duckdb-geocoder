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

PG's geocoder has multiple `rating = 0 → return immediately` and `exact_street → skip Stage B` short-circuits, plus a `LOOP` over `zip_info` shapes that can short-circuit after iter 1 returns enough rows. `us_geocoder` is a SQL macro with no equivalent control flow — it computes all candidates in one vectorized pass with `ORDER BY rating LIMIT max_results`.

Stage A is a two-pass structure that mirrors PG's primary + fallback distinction *as predicates*, without the iteration:

- **Pass A**: primary-shaped filter (length-gated soundex/prefix); always runs.
- **Pass B**: fallback-shaped filter (ungated soundex / fullname-LIKE / name-prefix); fires when Pass A is weak (`n_rows = 0 OR min_r ≥ 30`), mirroring PG's `IF var_bestrating < 30 THEN RETURN`.

A per-row `via_primary` discriminator (verbatim port of PG primary's name filter, ANDed with `place IS NOT NULL`) drives:
- No-input-ZIP rating constant (`+1` PG primary vs `+3` PG fallback).
- Address rendering (PG primary clamps out-of-range house numbers; PG fallback NULLs them).
- ZIP penalty formula (`LEAST(diff_zip, 20) * penalty` primary vs `LEAST(diff_zip * penalty, lev_zip * penalty)` fallback).
- City penalty formula (place-only primary vs `LEAST` over place/cousub/county/zip_lookup_base.city fallback).

**Observable effects** that remain:
- At rating ties within a `DISTINCT ON` partition, our pick may differ from PG's because PG's tiebreak after `ORDER BY 1,2,3,4,5,6,7,9` in `geocode_address.sql` ultimately depends on PostgreSQL's physical heap row order (disk insertion order). DuckDB columnar storage has different natural ordering. Documented as #1113d-class.
- PG's iter-2-of-fallback inner has `ORDER BY rate_attributes+house_penalty LIMIT 200`, then `DISTINCT ON ... ORDER BY (predirabrv, fename, ...) LIMIT 10` *before* final rating sort. This alphabetical pre-cut hides candidates from PG's output that we (with no equivalent cut) score and surface. For #1073a we find Minneapolis 3rd Ave N r=4; PG returns Hanover 3rd St NE r=38 because the alphabetical LIMIT 10 dropped Minneapolis from PG's final-sort input.

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
   SELECT rating, ST_AsText(geom), (adr).* FROM tiger.geocode(
       tiger.from_pagc('120 Benefit St, Providence RI 02903'), 3, NULL, 'none')
   ORDER BY rating;
   ```

Expect identical `rating` values, identical `(adr)` contents, and geometry within ~1 m. Wider divergence typically means a difference in PAGC rules version, TIGER vintage, or a rating tie boundary (D7).

## Parity test layout

Three layers, separating CI checks from harnesses that need a real TIGER reference DB.

### Layer 1 — unit tests (CI)

Hand-curated [test/sql/](../test/sql/) sqllogic tests covering the geocoder primitives — happy path, perpendicular offset math, side-of-street semantics, location fallback, intersection finder, reverse-geocode rank, containment GEOID derivation, every scoring primitive. No TIGER data; synthetic fixtures only.

### Layer 2 — stress-class regression (CI)

[test/sql/parity_stress.test](../test/sql/parity_stress.test) systematically exercises every stress class D13 calls out (numeric-street equivalence, prequalabr discount, short-name LIKE-prefix bypass, highway spacing, ZIP window tolerance, house-number penalties, soft type/direction penalties, Stage B fallback). Synthetic multi-street RI fixture. **Not** a PG parity test — asserts our own per-branch behavior.

### Layer 3 — PG-mirrored parity harnesses (local-only)

[scripts/parity/run_geocode_regress.sh](../scripts/parity/run_geocode_regress.sh) and [scripts/parity/run_reverse_geocode_regress.sh](../scripts/parity/run_reverse_geocode_regress.sh) run PG's regression inputs through *our* geocoder and diff against PG-2025-PAGC oracle output captured in [test/parity/upstream/](../test/parity/upstream/) (NOTICE + GPLv2 attribution there).

**Not run in CI** — needs a reference DB with TIGER 2025 loaded for at least MA + MN (multi-GB). Build once:

```sql
ATTACH 'pg_parity.duckdb' AS tgt;
CALL load_tiger_nation(target_db := 'tgt');
CALL load_tiger_states(['MA','MN'], target_db := 'tgt');
DETACH tgt;
```

Then:

```sh
./scripts/parity/run_geocode_regress.sh         pg_parity.duckdb
./scripts/parity/run_reverse_geocode_regress.sh pg_parity.duckdb
```

Output is a per-test `match` / `diverge` / `missing` tally with diffs inlined. **Match** means identical `pprint_adr(adr)` + 4-decimal-truncated `POINT(lng lat)` + integer rating.

The Docker image at [scripts/parity/pg_compare/](../scripts/parity/pg_compare/) builds the PG side (PG 16 + PostGIS + upstream `address_standardizer` + `postgis_tiger_geocoder`) so the oracle in [test/parity/upstream/](../test/parity/upstream/) can be regenerated. See the script headers for full reproduce instructions.

### Documented improvements over PG (deliberate divergences)

Three mechanisms produce *better* results than PG-with-PAGC on certain inputs. All intentional; **don't remove in any "match PG bit-for-bit" cleanup**. See [parity-divergences.md](parity-divergences.md) for per-test detail.

- **A. `from_pagc` post-PAGC validation** — when PAGC misparses a street-type word as the city (e.g. `"26 Court Street, 02109"` → `city='STREET'`), our adapter infers `street_type` from that word and nulls location. PG's `pagc_normalize_address` accepts the misparse verbatim. Resolves T18a-class.
- **B. Unconditional `numeric_streets_equal` in candidate-finding** — PAGC strips ordinal suffixes (`27th` → `27`); PG's primary stage_a only exact-matches short names, so it misses TIGER's `name='27th'`. Our `name_match_tlids_a` always runs the `numeric_streets_equal` branch. Resolves #1145a/b/e-class.
- **C. PAGC numeric-suffix recombination** — for inputs like `"35W"` PAGC over-splits to `name='35', sufdir='W'`, and `soundex('35')` collides with every digit-stem street; we detect and recombine. Net positive vs PG's plpgsql LOOP short-circuit.

### Status

Strict-match parity against PG-2025-PAGC: **34/51** (`pprint_adr(adr)` + 4-decimal-truncated `POINT(lng lat)` + integer rating identical up to per-test `max_n`).

All 17 remaining divergences fall into three classes — none are bugs in our code. Per-test breakdown in [parity-divergences.md](parity-divergences.md). One-line summary:

- **11 us-better-than-PG** (Mechanisms A/B/C)
- **5 PG heap row-order tiebreak** at sub_rating ties — not closeable in SQL (would require replicating PostgreSQL's physical heap layout)
- **1 rating path-divergence** (#1076e — same edge, different formula path)

### Roadmap

- 10k-row random-sample corpus from a loaded TIGER state — catches scoring/parser regressions outside the curated test set.
- Port `pagc_normalize_address_regress` as a parser-level harness for `from_pagc` vs PG's `normalize_address(use_pagc=true)`.
- Extend `run_geocode_regress.sh` to cover the batched-VALUES forms (`#TB1`, `#1073*`, `#1076*`).
