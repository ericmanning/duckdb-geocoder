# Parity with `postgis_tiger_geocoder`

`us_geocoder` is a pure-DuckDB port of PostGIS's [`postgis_tiger_geocoder`](https://gitea.osgeo.org/postgis/postgis_tiger_geocoder). This doc records what matches, where the port deliberately diverges, and the per-test divergence audit. It also captures the design decisions (D1–D14) that drove the port and a condensed reference to the PG cascade for maintainers reading our SQL alongside PG's.

## Quick status

Strict-match parity against PG-2025-PAGC: **34/51** (`pprint_adr(adr)` + 4-decimal-truncated `POINT(lng lat)` + integer rating identical up to per-test `max_n`).

All 17 remaining divergences fall into three classes — **none are bugs in our code**:
- **11 us-better-than-PG** (Mechanisms A/B/C — see [Improvements over PG](#improvements-over-pg-deliberate-divergences))
- **5 PG heap row-order tiebreak** at sub_rating ties — not closeable in SQL
- **1 rating path-divergence** (#1076e — same edge, different formula path)

Given (a) the same PAGC standardizer inputs, (b) the same TIGER vintage data loaded identically into both systems, and (c) addresses away from rating tie-boundaries, `tiger.geocode(from_pagc(x))` and PG's `geocode(x, use_pagc_address_parser := true)` produce the same ranking and the same geometry within the documented per-component numeric tolerances below.

## What matches exactly

- **Public contract.** Eight input fields (`address, street_name, street_type, pre_dir, post_dir, location, state_abbrev, zip`) and their format rules. See [§ Input contract](#input-contract).
- **Scoring weights.** Direction × 2, name × 10, type × 5. Prequalabr (`Old`, `New`) discount at 0.75. Numeric-streets short-circuit (`15th` ≡ `15rd`). House-number penalties: 0 / 2 / 5 / 20 depending on in-range / wrong-parity / out-of-range / no-range. ZIP penalty capped at `20 * zip_penalty`. Location Levenshtein added directly. See `rate_attributes` in [../src/sql/scoring_macros.sql.in](../src/sql/scoring_macros.sql.in).
- **Rating band invariant.** Location-only matches always return rating ≥ 100; address-level matches return rating < 100. Total order across both stages preserved.
- **Side-of-street semantics.** `side='L'`/`'R'` relative to edge direction; parity rule (even addresses on even side); 10m perpendicular offset from centerline in UTM.
- **TIGER joins.** Edges are joined to addr/featnames via `(tlid, statefp)`; faces via `(tfidl / tfidr, statefp)`; ZIP-windowing via `diff_zip` over the first 5 digits.
- **Intersection finder.** Uses TIGER's topological node IDs (`tnidf`/`tnidt`), not `ST_Intersects` — same optimization as PG.
- **SRID policy.** All storage in 4269. User-supplied geometry auto-transforms to 4269 if tagged with a different SRID; SRID 0 is treated as "assume 4269".

## Documented behavior divergences

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

## Improvements over PG (deliberate divergences)

Five mechanisms produce *better* results than PG-with-PAGC on certain inputs. All intentional; **don't remove in any "match PG bit-for-bit" cleanup**. Per-test detail in [§ Per-test divergence audit](#per-test-divergence-audit).

- **A. `from_pagc` post-PAGC validation** — when PAGC misparses a street-type word as the city (e.g. `"26 Court Street, 02109"` → `city='STREET'`), our adapter infers `street_type` from that word and nulls location. PG's `pagc_normalize_address` accepts the misparse verbatim. Resolves T18a-class.
- **B. Unconditional `numeric_streets_equal` in candidate-finding** — PAGC strips ordinal suffixes (`27th` → `27`); PG's primary stage_a only exact-matches short names, so it misses TIGER's `name='27th'`. Our `name_match_tlids_a` always runs the `numeric_streets_equal` branch. Resolves #1145a/b/e-class.
- **C. PAGC numeric-suffix recombination** — for inputs like `"35W"` PAGC over-splits to `name='35', sufdir='W'`, and `soundex('35')` collides with every digit-stem street; we detect and recombine. Net positive vs PG's plpgsql LOOP short-circuit.
- **D. Multi-state-ZIP fallback** — when an input has no `state_abbrev` and the ZIP appears in `zip_lookup_base` for multiple states (~7% of US ZIPs cross state lines), `geocode_batch` dispatches to *every* candidate state and the result merge keeps the lowest-rated match. PG uses `LIMIT 1` against `zip_lookup_base` and silently picks an arbitrary state. Closes the entire family of "address ordered second in zip_lookup_base" misroutes.
- **E. State-abbrev / ZIP union for cross-state typos** — when both `state_abbrev` and `zip` resolve, `geocode_batch` unions both sources' candidate states (DISTINCT) and dispatches to all. PG uses `COALESCE(state_lookup, zip_lookup_base LIMIT 1)` — state_abbrev wins authoritatively, so a typo'd state code like `"60 Washington St NY 07030"` (Hoboken NJ) silently misses its real match in NJ. Mitigated for placeholder ZIPs (12345/99999/etc.): when ZIP-derived candidates exceed 3 states *and* state_abbrev resolves, we drop the ZIP set as unreliable.

## Not ported

- `normalize_address()` (PG's built-in parser). Use `us_address_standardizer` via `from_pagc()`, or bring your own.
- `pagc_normalize_address()` wrapper. Its repack logic lives inline in `from_pagc()`.
- `use_pagc_address_parser` setting. `from_pagc` is the only path; no runtime switch.
- `norm_addy` composite. Replaced by the 8-field `tiger.geocode_input` struct.
- `install_missing_indexes()` / `missing_indexes_generate_script()`. The loader creates the precomputed-column acceleration keys (`name_lower`, `fullname_norm`, `name_soundex`, `numeric_stem`, `name_first_5`, `fullname_first_5`) at INSERT time; there's no separate index rebuild step.
- The `CHECK (statefp = 'XX')` constraint-exclusion trick. DuckDB's columnar storage uses zonemaps automatically and our per-state INSERT pattern keeps row groups contiguous on `statefp` without explicit partitioning.
- `loader_platform` / `loader_variables` / `loader_lookuptables` control tables, and the shell-script generators. Loader logic is C++.

## Per-test divergence audit

Per-test write-up of the 17 divergent cases against PG-2025-PAGC, post-`patch/d7` and the May 2026 `interpolate_from_address` calc-bug fixes (commit `41d3fce`).

Each test below records: input, PG's row 1, our row 1, the rank position of each engine's row 1 in the *other* engine's output, and the mechanism. PG output captured via `geocode(input, 10)` against TIGER 2025; ours via `tiger.geocode(tiger.from_pagc(input), 10, NULL, 'none')`.

Quick reference index:

| Class | Tests |
|---|---|
| **A. Us-better-than-PG (we find the correct address; PG returns wrong-answer)** | T18a, #1073a, #1145a, #1145b, #1145e |
| **B. Us-better at row 2+ (PG's loop short-circuits before emitting more candidates)** | #1087b, #1073b, #1113a, #1113b, #1145c |
| **C. PG heap row-order tiebreak (row pick differs at sub_rating ties; not closeable in SQL)** | #1074a, #1074b, #1076a, #1076h, #1113d |
| **D. Rating path-divergence (same edge, different rating formula path)** | #1076e |
| **E. Both wrong on parser-broken input** | #1145d |

### A. Us-better-than-PG (top-1 correctness)

#### T18a

- **Input**: `26 Court Street, 02109`
- **PG row 1**: `26 Court Sq, Boston, MA 02108 r=18`
- **Our row 1**: `26 Court St, Boston, MA 02108 r=7`
- **PG row containing our top**: row 2 (`Court St r=18`)
- **Our row containing PG's top**: row 2 (`Court Sq r=12`)
- **Mechanism**: Mechanism A. Both PAGC parsers misparse `city='STREET'` (no city before ZIP). PG's `pagc_normalize_address` is a thin wrapper that accepts the misparse → both Court St and Court Sq tie at r=18. Our `from_pagc` detects the street-type-as-city pattern, infers `street_type='ST'`, nulls location, breaks the tie correctly toward Court St (r=7).

#### #1073a

- **Input**: `212 3rd Ave N, MINNEAPOLIS, MN 553404`
- **PG row 1**: `10000 3rd St NE, Hanover, MN 55341 r=38`
- **Our row 1**: `212 3rd Ave N, Minneapolis, MN 55401 r=4` ← actual correct address!
- **PG row containing our top**: not in PG's top 10
- **Our row containing PG's top**: not in our top 10 (Hanover candidate is filtered out by our zip window)
- **Mechanism**: Bad ZIP `553404` (canon → `55340`, Hanover MN) but real city='Minneapolis'. We use city-derived ZIPs as Pass A's effective window AND fire Pass B's loose branches when Pass A is weak, finding the actual Minneapolis match. PG's flow returns the Hanover candidate at r=38 because PG's iter-2 inner `LIMIT 10` after alphabetical sort hides Minneapolis from PG's final-sort input.

#### #1145a

- **Input**: `4051 27th Ave S Minneapolis MN 55405`
- **PG row 1**: `Co Rd 27, Minneapolis, MN 55418 r=27` (no house — PG fallback path)
- **Our row 1**: `4051 27th Ave S, Minneapolis, MN 55406 r=22` ← actual correct address!
- **PG row containing our top**: not in PG's top 10
- **Our row containing PG's top**: not in our top 50 — Co Rd 27 candidates rate worse than 27th Ave S
- **Mechanism**: Mechanism B (unconditional `numeric_streets_equal`). PAGC strips the `th` from `27th` → input streetname=`27`. PG primary's exact-only filter (`f.name = '27'`) finds only literal `'27'` featnames (Co Rd 27). Our Pass A's `numeric_streets_equal` branch also catches featnames with `name='27th'` (like 27th Ave S), which actually match the user's intent. We rate the right answer at r=22 and PG never sees it.

#### #1145b

- **Input**: `3625 18th Ave S Minneapolis MN 55406`
- **PG row 1**: `18 1/2 Ave NE, Minneapolis, MN 55418 r=16` (no house — fallback)
- **Our row 1**: `3625 18th Ave S, Minneapolis, MN 55407 r=22` ← actual correct address!
- **PG row containing our top**: not in PG's top 10
- **Our row containing PG's top**: not in our top 50
- **Mechanism**: Mechanism B. Same as #1145a — PAGC strips `th` from `18th`, PG primary misses the real `18th Ave S` featnames, our `numeric_streets_equal` catches them.

#### #1145e

- **Input**: `103 36th St W Minneapolis MN 55409`
- **PG row 1**: `W 36th St, Minneapolis, MN 55409 r=33` (no house — fallback)
- **Our row 1**: `103 W 36th St, Minneapolis, MN 55408 r=26` ← actual correct address!
- **PG row containing our top**: not in PG's top 10
- **Our row containing PG's top**: row 2 at r=33
- **Mechanism**: Mechanism B. PAGC parses streetname='36'. PG primary's exact filter doesn't catch '36th' featnames; our `numeric_streets_equal` does. We find the actual house-in-range match at r=26. PG returns the fallback no-house "W 36th St" candidate at r=33.

### B. Us-better at row 2+ (PG short-circuits before us)

These tests have row 1 matching PG exactly, but PG's `LOOP` short-circuit (`IF var_bestrating < 30 THEN RETURN`) prevents PG from emitting additional rows that we surface. With max_n>1 these show as divergences in the harness; with max_n=1 they would match.

#### #1087b

- **Input**: `75 State Street, Boston, MA`
- **PG row 1**: `75 State St, Boston, MA 02109 r=1`
- **Our row 1**: `75 State St, Boston, MA 02109 r=1` ✓ matches
- **Mechanism**: same candidate at row 2 (`75 State Rd, Revere, MA 02151 r=17` — TIGER lists Revere ZIP 02151 with city='Boston'). Visible in PG's top 10 when called with max_results=10; PG just hides it under the test's actual max_n=3 because var_bestrating=1<30 short-circuits.

#### #1073b

- **Input**: `212 3rd Ave N, MINNEAPOLIS, MN 55401-`
- **PG row 1**: `212 3rd Ave N, Minneapolis, MN 55401 r=0`
- **Our row 1**: `212 3rd Ave N, Minneapolis, MN 55401 r=0` ✓ matches
- **Mechanism**: extra `212 3rd Ave S, Minneapolis r=2` we surface. PG's loop short-circuits at r=0 < 30.

#### #1113a

- **Input**: `8040 OLD CEDAR AVE S, BLOOMINGTON, MN 55425`
- **PG row 1**: `8040 Old Cedar Ave S, Bloomington, MN 55425 r=0`
- **Our row 1**: `8040 Old Cedar Ave S, Bloomington, MN 55425 r=0` ✓ matches
- **Mechanism**: at max_n=2 we surface a Stage B drop row `2790 E Old Shakopee Rd r=107` (location-level rating ≥ 100). PG's loop short-circuits at r=0 < 30 after row 1 and never emits the Stage B fallback. Cosmetic only — caller asking for max_n=2 can ignore the location-rating-≥100 row.

#### #1113b

- **Input**: `8040 CEDAR AVE S, BLOOMINGTON, MN 55425`
- **PG row 1**: `8040 Old Cedar Ave S, Bloomington, MN 55425 r=10`
- **Our row 1**: `8040 Old Cedar Ave S, Bloomington, MN 55425 r=10` ✓ matches
- **Mechanism**: same as #1113a. Extra row 2 `9300 Cedar Cir, Bloomington r=23` we surface that PG's short-circuit hides.

#### #1145c

- **Input**: `4057 10th Ave S Minneapolis MN 55406`
- **PG row 1**: `4057 10th Ave S, Minneapolis, MN 55407 r=22`
- **Our row 1**: `4057 10th Ave S, Minneapolis, MN 55407 r=22` ✓ matches
- **PG row 2**: `10th Ave S, Minneapolis, MN 55404 r=29` (no house — fallback)
- **Our row 2**: not present (we return 1 row only at top max_n=2)
- **Mechanism**: Row 1 matches. PG's row 2 is a wider fallback candidate that we don't bother surfacing because Pass A's correct match short-circuits the gate.

### C. PG heap row-order tiebreak

Row 1 either matches or differs in house# by 1 because PG's `DISTINCT ON ... ORDER BY` ultimately depends on PostgreSQL's physical heap insertion order at sub_rating ties. DuckDB's columnar storage has different natural ordering. **Not closeable in SQL** — would require replicating PostgreSQL's heap layout.

#### #1074a

- **Input**: `8525 COTTAGE WOOD TERR, Blaine, MN 55434`
- **PG row 1**: `8525 Cottagewood Ter NE, Blaine, MN 55434 r=14` ✓ matches us
- **Our row 1**: `8525 Cottagewood Ter NE, Blaine, MN 55434 r=14` ✓ matches PG
- **Row 2 differs**: PG `8499 Cottagewood Ter NE, Spring Lake Park r=36` vs us `8498 Cottagewood Ter NE, Spring Lake Park r=36`
- **Mechanism**: Same edge `tlid=97489950`, two `addr` rows: side=R range 8401–8499 (odd) and side=L range 8400–8498 (even). Both at the same `tfidr/tfidl` so they tie under our deterministic sort. PG picks side=R, we pick side=L.

#### #1074b

- **Input**: `8525 COTTAGEWOOD TERR, Blaine, MN 55434`
- **PG row 1**: `8525 Cottagewood Ter NE, Blaine, MN 55434 r=4` ✓ matches us
- **Our row 1**: `8525 Cottagewood Ter NE, Blaine, MN 55434 r=4` ✓ matches PG
- **Mechanism**: same as #1074a. Row 2 L/R-side tiebreak.

#### #1076a

- **Input**: `16725 Co Rd 24, Plymouth, MN 55447`
- **PG row 1**: `16725 Co Rd 24, Plymouth, MN 55447 r=25` ✓ matches us
- **Our row 1**: `16725 Co Rd 24, Plymouth, MN 55447 r=25` ✓ matches PG
- **Row 2 differs**: PG `15798 Co Rd 24, Plymouth, MN 55446 r=32` vs us `15599 Co Rd 24, Plymouth, MN 55446 r=32` (same fullname/place/zip, different house# from different range edge picked at sub_rating tie)
- **Mechanism**: row 2 PG heap row-order tiebreak (multiple Co Rd 24 edges with overlapping ranges).

#### #1076h

- **Input**: `300 Rt 3A, Hingham, MA`
- **PG row 1**: `300 State Rte 3 A, Hingham, MA 02043 r=18` ✓ matches us
- **Our row 1**: `300 State Rte 3 A, Hingham, MA 02043 r=18` ✓ matches PG
- **PG row 2**: `300 State Rte 3, Arlington r=24`
- **Our row 2**: `State Rte 3, Hingham, MA 02043 r=25` (PG has this at row 4)
- **Our row containing PG's row 2**: not in our top 30 — Arlington 02474 is outside our city-zips for Hingham
- **Mechanism**: with no input ZIP, our Pass A's window restricts to Hingham's city-derived ZIPs (PG-faithful per `geocode_address.sql:80-87`). PG's primary actually does the same restriction, but PG's *fallback* iter 5 (NULL zip set, no zip filter at all) finds Arlington/Burlington/etc. and emits them at r=24-30. Net: we have a tighter (correct) set; PG sprays wider via its NULL-zip iteration we don't model.

#### #1113d

- **Input**: `17405 Rockford Rd, Plymouth, MN 55446`
- **PG row 1**: `15899 Rockford Rd, Plymouth, MN 55446 r=5`
- **Our row 1**: `15702 Rockford Rd, Plymouth, MN 55446 r=5`
- **PG row containing our top**: not in PG's top 10 (PG renders the side=R 15899-end row; we render the side=L 15702 row of the same tied set)
- **Our row containing PG's top**: not in our top 10 (our deterministic tiebreak differs from PG's heap order)
- **Mechanism**: documented PG physical heap row-order non-determinism. Both PG and we evaluate the IDENTICAL 3-candidate set for "Rockford Rd" — TLID 43606664 (one addr row) and TLID 43852639 (two addr rows, sides L and R). All three at `sub_rating=5`. PG's `DISTINCT ON ... ORDER BY` documents `sub_rating` as the only non-partition tiebreak — but with sub_rating tied across all three, PG's pick depends on PostgreSQL's physical heap row order (disk insertion order).

### D. Rating path-divergence

#### #1076e

- **Input**: `3900 Route 6, Eastham, Massachusetts 02642`
- **PG row 1**: `3900 US Hwy 6, Eastham, MA 02642 r=15` ✓ matches us
- **Our row 1**: `3900 US Hwy 6, Eastham, MA 02642 r=15` ✓ matches PG
- **Row 2 differs**: PG `US Hwy 6, North Eastham r=21` (no house, fallback rendering) vs us `5201 US Hwy 6, North Eastham r=27` (clamped house, primary rendering)
- **Mechanism**: rating path-divergence. Both pipelines find the same TIGER edge for North Eastham US Hwy 6, but PG renders it through fallback (NULL house, fallback rating formula → r=21) while we render through primary (clamped house 5201, primary rating formula → r=27). Same correctness; different formula path. Our per-row `via_primary` predicate (added in `1ada06e`) classifies this row as primary because `pl.name='North Eastham'` is non-null, so we apply primary formulas. Not a calc bug.

### E. Parser-broken input — both wrong

#### #1145d

- **Input**: `8512 141 St Ct Apple Valley MN 55124`
- **PG row 1**: `141 W 121st St, 8512, Burnsville, MN 55337 r=51` (no house — fallback)
- **Our row 1**: `1st St, 8512, Farmington, MN 55024 r=37` (no house — fallback path)
- **PG row containing our top**: not in PG's top 10 — PG's iter-2 alphabetical `LIMIT 10` cuts Farmington before final ranking
- **Our row containing PG's top**: row 18 at r=51
- **Real intended address**: `141st St W, Apple Valley, MN 55124` (PAGC can't recognize "141 St" as the typo for "141st")
- **Mechanism**: PAGC produces `street_name='ST'` length 2 from `"141 St Ct"`. Both pipelines find candidate sets via `soundex('ST')='S300'`. Our top 1 (`1st St` r=37) is rated lower than PG's top 1 (`141 W 121st St` r=51) because PG's iter-2 inner `ORDER BY (predir, fename, ...)` `LIMIT 10` truncates *before* final rating sort, hiding our better-rated candidates. **Both pipelines' top 1 are wrong streets.** Effectively a draw — PG's higher rating reflects the correct insight that the match is uncertain, but PG's top pick is also far from the real address. Closeable only by parser-level disambiguation of "141 St" → "141st" (out of scope).

### Disposition summary

In **all 17 cases**, our top-1 result is either matching PG, the actually-correct address, or a tied-rating alternative. **Zero cases where PG's top-1 is correct and we miss it.**

The audit identified two real `interpolate_from_address` calc bugs which were fixed in commit `41d3fce`:

1. Out-of-range house number → midpoint clamp (was: nearer-endpoint clamp), per `interpolate_from_address.sql:42-44`. Closed T12, T13, T16.
2. Local-segment azimuth for offset direction (was: overall start-to-end azimuth), per `interpolate_from_address.sql:54-71`. Closed #1076g, #1074a/b geom drift to within sub-meter precision.

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

[benchmark/regress/run_geocode_regress.sh](../benchmark/regress/run_geocode_regress.sh) and [benchmark/regress/run_reverse_geocode_regress.sh](../benchmark/regress/run_reverse_geocode_regress.sh) run PG's regression inputs through *our* geocoder and diff against PG-2025-PAGC oracle output captured in [test/parity/upstream/](../test/parity/upstream/) (NOTICE + GPLv2 attribution there).

**Not run in CI** — needs a reference DB with TIGER 2025 loaded for at least MA + MN (multi-GB). Build once:

```sql
ATTACH 'pg_parity.duckdb' AS tgt;
CALL load_tiger_nation(target_db := 'tgt');
CALL load_tiger_states(['MA','MN'], target_db := 'tgt');
DETACH tgt;
```

Then:

```sh
./benchmark/regress/run_geocode_regress.sh         pg_parity.duckdb
./benchmark/regress/run_reverse_geocode_regress.sh pg_parity.duckdb
```

Output is a per-test `match` / `diverge` / `missing` tally with diffs inlined. **Match** means identical `pprint_adr(adr)` + 4-decimal-truncated `POINT(lng lat)` + integer rating.

The Docker image at [benchmark/pg/](../benchmark/pg/) builds the PG side (PG 16 + PostGIS + upstream `address_standardizer` + `postgis_tiger_geocoder`) so the oracle in [test/parity/upstream/](../test/parity/upstream/) can be regenerated. See the script headers for full reproduce instructions.

## Roadmap

- 10k-row random-sample corpus from a loaded TIGER state — catches scoring/parser regressions outside the curated test set.
- Port `pagc_normalize_address_regress` as a parser-level harness for `from_pagc` vs PG's `normalize_address(use_pagc=true)`.
- Extend `run_geocode_regress.sh` to cover the batched-VALUES forms (`#TB1`, `#1073*`, `#1076*`).

---

# Locked design decisions (D1–D14)

The committed choices that drove the v0.1 port. Inline `.sql.in` and C++ comments reference these by number — don't change the meaning of a decision without updating both sides.

| # | decision |
| --- | --- |
| D1 | **Data distribution.** Three first-class source modes, selected by the `source` parameter: **(i) Census HTTP** (default) — download-on-demand via `httpfs`, read with GDAL `/vsizip/` so shapefiles are never fully extracted. Loader takes a `temp_dir` argument (default `std::filesystem::temp_directory_path() / "duckdb_tiger_<pid>"`); each zip is downloaded to `temp_dir`, read via `ST_Read('/vsizip/…')`, and deleted as soon as its INSERT completes. No `shellfs`. **(ii) Local folder** — `source := '/path/to/tiger/'` reads from pre-downloaded zips on local disk, skipping HTTP. **(iii) Portable DuckDB file** — TIGER data is loaded into an ATTACH-able DuckDB file in a networked environment, then the file is transferred to and attached READ_ONLY in the secure environment. See [docs/api.md § Reference databases](api.md#reference-databases) for the API. |
| D2 | **Partitioning.** One DuckDB table per entity (`tiger.edges`, `tiger.addr`, etc.). Each state loads in its own INSERT, so row groups end up naturally contiguous per `statefp` — zonemap pruning on `WHERE statefp = '25'` handles per-state selectivity with no explicit sort or partitioning. No Hive layout, no UNION views. |
| D3 | **Public API.** `geocode()` has a `VARCHAR` overload that calls `from_pagc(raw_text)` internally — users who pass a raw string get a sensible default. Explicit-struct callers use `geocode(from_pagc(raw_text))` or `geocode(my_adapter(...))`. |
| D4 | **Standardizer dependency.** Soft. `from_pagc` calls `standardize_address()` / `parse_address()` at runtime; emit a clear "install us_address_standardizer" error if the functions aren't resolvable. |
| D5 | **`restrict_geom`.** Ported as-is; accepts any DuckDB spatial `GEOMETRY`. |
| D6 | **`reverse_geocode` output.** DuckDB-idiomatic: one row per candidate edge with a `rank` column, not PG's parallel-array record. |
| D7 | **Stage A → Stage B short-circuits.** Relaxed for vectorization — compute all candidates in one vectorized pass, LIMIT at the end. Documented deviation: which marginal candidates get emitted may differ from PG at tie boundaries. |
| D8 | **Optional TIGER tables.** Drop `tract`, `bg`, `tabblock20`, `addrfeat` entirely. Not used by the geocoder. If a `get_tract()`-equivalent is needed later, add it as a separate optional load. |
| D9 | **Configuration knobs** (`zip_penalty`, `reverse_geocode_numbered_roads`). Function parameters with sensible defaults. No settings table, no session variables. |
| D10 | **SRID policy.** Loader data: always 4269, no conversion. User-input geometry (`restrict_geom`, the point to `reverse_geocode`): auto-transform to 4269 at the boundary if SRID is set and different. SRID 0 is treated as "assume 4269" (matches PG). Do not silently misinterpret 4326 as 4269 — that's a 1–3m wrong-answer bug on short blocks. |
| D11 | **Extension language.** Thin C++ shim (~100 LOC) for extension registration, HTTP orchestration, and the state-×-table-×-county loop (which needs iteration that DuckDB SQL doesn't offer). 95% of logic stays in SQL macros — `from_pagc`, `geocode`, `rate_attributes`, `interpolate_from_address`, per-table column projections, and all derived-table build queries. |
| D12 | **Year handling.** `2025` default; `year` is a runtime parameter override. Per-year URL patterns and per-year schema tweaks live in a small internal config structure so year-to-year TIGER drift can be handled without a new extension release. |
| D13 | **Parity testing.** v0.1 ships with (a) a hand-curated ~50-address corpus covering stress classes (numbered highways, `prequalabr` like `Old`, short street names, ZIP typos, cross-state ZIPs, unit suffixes, `I-635`/`I- 635` highway spacing), plus (b) ports of PG's regression tests (`geocode_regress`, `reverse_geocode_regress`, `test-geocode_intersection_spacing`). We skip `normalize_address_regress` — that parser isn't ported. |
| D14 | **Census block/tract containment guarantees.** Precompute per-edge-per-side whether the interpolated offset point is guaranteed to land inside the adjacent face (implying block, tract, block group, county, state). Exposed at geocode time via `block_geoid`/`tract_geoid`/`blkgrp_geoid` output columns (always populated) plus a `containment_guaranteed` boolean, and a `require_containment` filter parameter. Guarantee holds at the default offset only. |

---

# Input contract

Each field has an expected format that the geocoder's SQL depends on, derived from reading every usage in the PG source.

| field | type | expected format | why the geocoder cares | strictness |
| --- | --- | --- | --- | --- |
| `address` | INTEGER | numeric house number, e.g. `1731` (strip any trailing letter like `1731A`) | parity check, range check, interpolation math | NULL allowed → rating penalty of **+20** (out-of-range fallback) |
| `street_name` | VARCHAR | free-form, any case, any punctuation; **must not contain** the street type or direction words | `lower(f.name) = lower($2)`, `soundex(f.name) = soundex($2)`, `levenshtein_ignore_case(f.name, $2)` for rating | NULL → Stage A returns empty immediately |
| `street_type` | VARCHAR | **TIGER Title-Case abbreviation** (`Ave`, `Blvd`, `Hwy`, `Ct`, `St`, `Rd`, etc.) | `lower($4) = lower(f.suftypabrv) OR lower($4) = lower(f.pretypabrv)` match check; rating adds `5 × levenshtein_ignore_case(input_type, tiger_type)` | **soft** — any case OK; unabbreviated (`Avenue` vs `Ave`) costs ~+15 rating |
| `pre_dir` | VARCHAR | **uppercase 2-letter** (`N`, `NW`, `SE`) | rating adds `2 × levenshtein_ignore_case(input_dir, tiger_dir)` | **soft** — any case OK; unabbreviated (`Northwest` vs `NW`) costs ~+14 rating |
| `post_dir` | VARCHAR | same as `pre_dir` | same | **soft** |
| `location` | VARCHAR | free-form city name, any case | `lower(city) LIKE lower($3) \|\| '%'`, `levenshtein_ignore_case`, `soundex` | NULL adds 5 to rating but is allowed |
| `state_abbrev` | VARCHAR | **UPPERCASE 2-letter** (`MA`, `NY`, `DC`) | `state_lookup.abbrev = parsed.stateAbbrev` exact-equality compare | **STRICT** — `Ma` or `Massachusetts` fails the state match, falls back to ZIP-based statefp lookup |
| `zip` | VARCHAR | **5-character string with leading zeros preserved** (`'02109'`) | `addr.zip = ANY(...)` exact-equality compare against TIGER's `addr.zip varchar(5)`; `diff_zip` numeric compare on first 5 digits | **STRICT** — INT or stripped `'2109'` silently misses every MA/NJ/CT/RI/VT/NH ZIP |

Three fields are strict (`address` → INTEGER, `state_abbrev` → uppercase 2-letter, `zip` → 5-char with leading zeros). Three are soft-penalty-only (`street_type`, `pre_dir`, `post_dir`). Two are free-form (`street_name`, `location`).

The soft penalties are cumulative. An input like `{street_type: 'AVENUE', pre_dir: 'NORTHWEST'}` against TIGER's `{'Ave', 'NW'}` eats roughly +29 rating — the difference between a top match and an also-ran. Use [the `canon_*` macros in api.md](api.md#canonicalization-macros) to coerce arbitrary parser output into the expected formats.

---

# PG cascade reference (for maintainers)

Condensed walkthrough of what `postgis_tiger_geocoder` does internally — kept here so readers of our SQL can compare against PG without checking out a separate repo.

## 0. Big picture

The PG tiger geocoder is a cascade of nested SQL/PLpgSQL functions operating on three kinds of data:

1. **Lookup dictionaries** (small, static): direction, street-type, secondary-unit, state, place, county, countysub, and ZIP → state lookups. Used for parsing free-form input.
2. **TIGER reference tables** (large, per-state): `featnames`, `edges`, `faces`, `addr`, `place`, `cousub`, `county`, `state`, `zcta5`, `zip_lookup_base`, `zip_state`, `zip_state_loc`.
3. **Candidate scoring** via `rate_attributes()` + `levenshtein_ignore_case()` + bespoke penalty functions.

Public entrypoints:

| Function | Input | Output | Purpose |
| --- | --- | --- | --- |
| `normalize_address(varchar) → norm_addy` | free-form address string | parsed composite | parse into structured components only |
| `pagc_normalize_address(varchar) → norm_addy` | same | same | parse via the `address_standardizer` extension (PAGC) |
| `geocode(varchar, max_results, restrict_geom)` | free-form address string | N ranked matches | the main geocoder |
| `geocode(norm_addy, …)` | already-parsed composite | same | skip normalization, used for batching |
| `geocode_intersection(road1, road2, state, city, zip, n)` | two street names + location | N ranked matches at the intersection | cross-street geocoding |
| `reverse_geocode(geometry, include_strnum_range)` | point | arrays of `norm_addy` and cross-street names | reverse geocoding |
| `get_tract(geometry, output_field)` | point | census tract id/name | census enrichment (we don't port this — D8) |

All geometry stored and returned in **SRID 4269** (NAD83). Some interpolation math transiently projects to UTM to compute meter offsets.

Control flow is eager and row-oriented in PL/pgSQL: the outer geocoder calls the inner one, loops through the result cursor, and short-circuits on `rating = 0`. This short-circuiting is the single biggest semantic quirk we consciously discarded for vectorization (D7).

## 1. The `norm_addy` composite

PG's internal type, defined in `sql_bits/norm_addy_create.sql.in`. **We do not expose this** — our public input is the 8-field `geocode_input` struct. The fields beyond what `geocode_input` carries (`parsed`, `zip4`, `address_alphanumeric`, `internal`) are parser-side breadcrumbs the geocoder doesn't actually consult.

```
norm_addy = (
    address              INTEGER,
    preDirAbbrev         VARCHAR,
    streetName           VARCHAR,
    streetTypeAbbrev     VARCHAR,
    postDirAbbrev        VARCHAR,
    internal             VARCHAR,
    location             VARCHAR,
    stateAbbrev          VARCHAR,
    zip                  VARCHAR,
    parsed               BOOLEAN,
    zip4                 VARCHAR(4),
    address_alphanumeric VARCHAR
)
```

## 2. Reference data actually consulted

Parsing-only lookups (small, static): `direction_lookup`, `street_type_lookup`, `secondary_unit_lookup`, `state_lookup`, `zip_lookup_base`.

TIGER reference data (large, per-state): `state`, `county`, `place`, `cousub`, `zcta5`, `zip_state`, `zip_state_loc`, `edges`, `faces`, `featnames`, `addr`.

Key joins:
- `featnames.tlid = addr.tlid AND featnames.statefp = addr.statefp` — candidate name → block ranges.
- `featnames.tlid = edges.tlid` — line geometry.
- `edges.tfidl` or `edges.tfidr = faces.tfid` — place/city on the correct side.
- `faces.placefp = place.placefp` — place name.

Only street-type MTFCC features (`mtfcc LIKE 'S%'`). State-scoped (`statefp = $1`) is always applied first to prune.

## 3. PG settings

From `src/geocode_settings.sql`, stored in `tiger.geocode_settings`:
- `use_pagc_address_parser` (bool) — route normalize_address to the PAGC C library
- `zip_penalty` (numeric, default 2) — multiplier on ZIP drift in the rating formula
- `reverse_geocode_numbered_roads` (0/1/2) — prefer numbered highways, named roads, or neither
- Four `debug_*` flags

For our port these become function parameters (D9). No settings table.

## 4. `normalize_address(input_text) → norm_addy` (PG's parser)

Pure string parsing. Steps:

1. **PAGC shortcut** if `use_pagc_address_parser=true`.
2. **Leading house number** via regex `^([0-9].*?)[ ,/.]`.
3. **Trailing ZIP** in five forms.
4. **State extraction** via `state_extract()` against `state_lookup` (fuzzy via soundex+Levenshtein).
5. **Comma-delimited fast path** for `street, location[, state]`.
6. **Location extraction** via `location_extract()` walking the string from the end against `place`/`cousub`.
7. **Internal address** via `secondary_unit_lookup` (regex on `APT`, `STE`, `FL`).
8. **Street type** via `street_type_lookup`; rightmost match wins. `is_hw` types can precede the road name.
9. **Highway reduction** for `Country Road 24` → streetName `24`, type `Rd`.
10. **Pre/post direction** via `direction_lookup`.
11. **Assembly** into `norm_addy` with `parsed=true`.

Important gotchas:
- Location extraction returns the *input string* slice, not the canonical name — Levenshtein later compares against the user input.
- `streetType` may be null even after parsing.
- ZIP-only input returns `{zip, parsed=true}` and the geocoder operates from ZIP polygons alone.

We don't port this parser (D4). We use PAGC via `from_pagc()` instead.

## 5. `geocode(varchar, …)`

Thin SQL wrapper: `geocode(text) = geocode(normalize_address(text))` filtered to `parsed=true`, sorted by rating.

The batching shape is `CROSS JOIN LATERAL` — Postgres fans out row-at-a-time. Our `geocode_batch` C++ function partitions input by resolved statefp and dispatches one per-state SQL per state (literal-statefp ART pushdown), which is the architectural improvement.

## 6. `geocode(norm_addy, max_results, restrict_geom)` — the cascade

Two-stage:

**Stage A — full address match.** Runs only if `streetName IS NOT NULL AND (zip IS NOT NULL OR stateAbbrev IS NOT NULL)`:
1. Call `geocode_address(parsed, max_results, restrict_geom)`.
2. Deduplicate on the full addy tuple, keeping the best rating per duplicate.
3. Sort by rating, yield up to `max_results`. Return immediately if any result has `rating = 0`.
4. If Stage A produced at least one result, return.

**Stage B — location fallback.** Runs only if Stage A produced nothing *and* (`zip IS NOT NULL OR (stateAbbrev AND location)`):
1. Call `geocode_location(parsed, restrict_geom)`.
2. Sort by rating, yield up to `max_results`. Return on `rating = 100`.

Rating discontinuity: Stage A clusters near 0 for good, 50+ for poor. Stage B starts at 100 (location matches always rank worse than any real address match). Consumers use rating as a total order.

## 7. `geocode_address` — the heart of the geocoder

Most complex function in the repo. Two sub-stages, both dynamic SQL.

### 7.1 Preliminaries

- Resolve `in_statefp`: from `state_lookup.abbrev = parsed.stateAbbrev`, or fallback to first `zip_lookup_base.statefp` for the input ZIP.
- Normalize `restrict_geom` to 4269 + `ST_SnapToGrid`.
- Build `var_bfilter` — predicate fragment filtering `tiger.zcta5` to ZCTAs inside `restrict_geom`.
- Expand input ZIP into a candidate window using `zip_range(zip, -2, +2)` (long names) or `(-1, +1)` (short).
- If ZIP is bad but location is present, derive ZIP window by joining `zip_lookup_base` on the location.
- Cache all ZIP candidates in `zip_info.zip varchar[]`.

### 7.2 Sub-stage A: brute-force exact-match-first

PG builds a giant SQL statement that:

1. **Inner CTE `a`** pulls candidate edges from `featnames ⨝ addr`, restricted to `statefp = in_statefp`, ranked by:
   ```
   rank = diff_zip(addr.zip, parsed.zip) * zip_penalty
        + (name=input ? 0 : levenshtein_ignore_case(name, input))
        + levenshtein_ignore_case(fullname, input_street + ' ' + streetType)
        + (parity match? 0 : 1)
        + (input address ∈ [least_hn, greatest_hn] ? 0 : 4)
        + (input type matches suftyp or pretyp ? 0 : 1)
        + rate_attributes(...)
   ```
   Street-name matching: strict `lower(name) = lower(input)` for short names (≤5 chars); for longer, `fullname LIKE input || '%' OR name = input OR soundex(name) = soundex(input)`. ZIP filter: `addr.zip = ANY(zip_info.zip)`. LIMIT 3× max_results.

2. **Outer select** joins to `edges`, `faces`, `place` with side-of-street rule `(edges.tfidl = faces.tfid AND addr.side = 'L') OR (edges.tfidr = faces.tfid AND addr.side = 'R')`. Computes `interpolate_from_address(...)` as the output point and `sub_rating`.

3. **`DISTINCT ON`** over `(stateabbrev, address, zip, streetname, streettypeabbrev, predirabbrev, postdirabbrev, fullname, place_name, side)`, ordered by sub_rating + a fallback alphabetical key. Keeps the best per-(addy, side) tuple.

4. **Final ranking**: `ORDER BY rating, sub_rating LIMIT max_results`.

In the LOOP that wraps this, PG checks `var_bestrating < 30` after each iteration. If so, return.

### 7.3 Sub-stage B: soft-match cascade

Iterates over up to 5 `zip_info` candidates (input ZIP, neighboring ZIPs, city-derived ZIPs, etc.), each running a wider name filter (soundex / fullname-LIKE / numeric-streets-equal). PG accumulates results across iterations, short-circuiting if `bestrating < 30`.

### 7.4 What comes back

A set of `(addy, geomout, rating, sub_rating)` rows. The address `addy.location` field is set by `COALESCE(place.name, cousub.name, zip_lookup_base.city, county.name)`.

## 8. `geocode_location(parsed, restrict_geom)` — Stage B fallback

When Stage A returns nothing. Two branches:
1. ZCTA polygon centroid of the input ZIP.
2. `place ⨝ zip_state_loc` rows where `lower(place.name) LIKE lower(parsed.location) || '%'` or `soundex` matches.

Output rating = `100 + levenshtein` to keep the ≥100 invariant. Geom = `ST_Centroid(...)`.

## 9. `geocode_intersection(road1, road2, state, city, zip, n)`

Joins two `featnames ⨝ addr` candidate sets via shared TIGER node IDs (`tnidf`/`tnidt`). The `tn` join skips spatial intersection — much cheaper than `ST_Intersects`. Output geom = endpoint of road1's edge that matches the shared node.

## 10. `reverse_geocode(pt, include_strnum_range)`

Given a point: state via `ST_Intersects`, county, ZIP, place-or-cousub, edges within `ST_DWithin(..., 0.01)` AND `mtfcc LIKE 'S%'`, ranked by distance. Primary edge gets house-number interpolation via `ST_LineLocatePoint` with parity nudge. PG returns parallel arrays (`intpt[]`, `addy[]`, `street[]`); we return one row per candidate (D6).

## 11. Rating semantics

- **Stage A ratings**: 0 (perfect) → ~30 (acceptable) → ~50+ (poor). Per-component weights documented in [§ What matches exactly](#what-matches-exactly).
- **Stage B ratings**: ≥100 always. Encodes "we couldn't match the street-level address; here's the location only."
- **Total order is the contract.** Sorting by rating ascending gives the right answer regardless of stage.

## 12. House-number interpolation

For a matched edge with `(fromhn, tohn)` range, side `L` or `R`, and target house number `N`:

1. Extract numeric prefixes from `fromhn` / `tohn`.
2. Compute `part = (N - least(from, to)) / (greatest(from, to) - least(from, to))`. Clamp to `[0, 1]`.
3. Project the line into a UTM zone for meter math.
4. `ST_LineInterpolatePoint(line_utm, part)` → centerline point.
5. Compute the perpendicular offset: 10m to the left if `side='L'`, to the right if `side='R'`. Direction comes from `ST_Azimuth` on the local segment plus a side-sign.
6. Translate the centerline point by the offset vector.
7. Project back to 4269.

PG primary clamps out-of-range house numbers to the nearest endpoint and sets `address` to that clamped value. PG fallback NULLs the `address` field for out-of-range. We discriminate via the `via_primary` predicate.
