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

PG's geocoder has multiple `rating = 0 → return immediately` and `exact_street → skip Stage B` short-circuits. PG also has *two* Stage A SQL queries: a primary that filters by ZIP info (uses `+1` as the no-input-ZIP rating fallback) and a fallback that re-runs against location-derived ZIPs (uses `+3` to penalize the weaker confidence). `us_geocoder` computes all Stage A candidates in one vectorized pass and uses `ORDER BY rating LIMIT max_results`; Stage B runs only when Stage A returns zero rows.

**Observable effects:**
- At tie-rating boundaries (e.g. multiple exact matches on a multi-block street), `us_geocoder` may return a different element of the tie than PG. The top-N set by rating is the same; the *order within a rating tier* can differ.
- We use `+1` as the no-input-ZIP rating fallback uniformly; PG's primary path uses `+1` and its fallback path uses `+3`. Since we don't reproduce the two-pass structure, we don't have a natural site for the `+3` and it's omitted. In practice this matters only for inputs that *would* trip PG's primary into returning weak-rating candidates; current parity corpus shows no test hitting that boundary except `#1112a` (where we miss a Stage A candidate entirely — separate issue).

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

Captured at [`test/parity/baseline/geocode_regress.txt`](../test/parity/baseline/geocode_regress.txt). Tally vs the *vendored* expected file: `0 match / 23 diverge / 35 missing`. The 35 missing are batched-VALUES PG tests the harness regex doesn't extract.

Of the 23 divergences, the surface tally was misleading until validated against an actual PG instance running with PAGC enabled. See the next section.

### PG-on-PG validation (Docker side-by-side)

[`scripts/parity/pg_compare/`](../scripts/parity/pg_compare/) builds a Docker image with PG 16 + PostGIS + the upstream `address_standardizer` + `postgis_tiger_geocoder`, loads TIGER 2025 for MA + MN, and runs PG's `geocode_regress.sql` with `set_geocode_setting('use_pagc_address_parser','true')`. Three-way diff (PG-with-PAGC vs us-with-PAGC vs the vendored expected, which was generated with PG's *built-in* normalizer, not PAGC):

| Class | Tests | Reading |
|---|---|---|
| **A. Match PG-with-PAGC, both off vendored by +1** | T3, T6, T9, T18b | Pure parser-version artifact. Vendored expected was built with `normalize_address`, not PAGC. PAGC parses these inputs slightly differently from the built-in. **Not a bug.** |
| **B. We were 1 lower than PG-with-PAGC** | T12, T13, T14, T15, T16 | **Fixed.** Common factor: input has **no ZIP**. PG's [`geocode_address.sql:124-127`](../scripts/parity/pg_compare/tiger_geocoder/src/geocode/geocode_address.sql) uses literal `+1` as the ZIP-term fallback when input ZIP is missing; we used `+0`. The CASE in [`src/sql/geocode_address.sql.in:206`](../src/sql/geocode_address.sql.in) now splits the NULL branches (`p_zip IS NULL → 1`, `a_zip IS NULL → 0`, else compute). T12-T16 ratings (1, 1, 1, 11, 11) match PG-with-PAGC exactly post-fix. |
| **C. Same misparse, different downstream pick** | T18a | Both PAGC implementations misparse "26 Court Street, 02109" (city='STREET'). PG and we both end at rating 18. PG picks "26 Court **Sq**, Boston"; we pick "26 Court **St**, Boston". Tiebreak ordering differs. Worth a separate investigation, but not a rating-arithmetic issue. |

**Reproduce locally:**

```sh
docker build -t duckdb-geocoder-pgparity scripts/parity/pg_compare/
docker run -d --rm --name pgparity --shm-size=2g \
    -p 55432:5432 -e POSTGRES_PASSWORD=parity duckdb-geocoder-pgparity
bash scripts/parity/pg_compare/load_tiger_via_pg.sh MA,MN     # ~1 hour

# Run regress with PAGC enabled
( echo "SELECT tiger.set_geocode_setting('use_pagc_address_parser','true');"
  echo "\\pset format unaligned" "\\pset fieldsep '|'" "\\pset tuples_only on"
  cat scripts/parity/pg_compare/tiger_geocoder/src/regress/geocode_regress.sql
) | docker exec -i pgparity psql -U postgres -d parity
```

Net: the parity harness's surface "0 match" tally against the vendored expected file is heavily inflated by the parser-baseline mismatch (vendored built with the built-in normalizer, our outputs come from PAGC). With the no-ZIP +1 fix landed, T-series tests T2-T16 now match PG-with-PAGC exactly; remaining differences are T18a (tiebreak ordering — we and PG both score "26 Court Street, 02109" candidates equivalently after the misparse, but pick different streets) and the batched-VALUES `#1073…#1145…` tests, which mostly reflect TIGER vintage drift between PG's regress baseline and 2025 data.

### PG-2025-PAGC oracle (April 2026)

[`scripts/parity/pg_compare/regenerate_expected.sh`](../scripts/parity/pg_compare/regenerate_expected.sh) regenerates [`test/parity/upstream/geocode_regress_2025_pagc`](../test/parity/upstream/geocode_regress_2025_pagc) by running our test inputs through the PG container with `use_pagc_address_parser=true` against TIGER 2025. Output is byte-stable across re-runs (rows ordered by `(test_id, target, rating, geom WKT)`).

Comparing that oracle to our output (post +1 fix), 26 of 58 unique test IDs match PG-2025-PAGC exactly on first-row rating + picked address:

| ✓ Match (26) | ✗ Diverge (32) |
|---|---|
| T1-T17, T18b | T18a (tiebreak) |
| #TB1 (batched) | #1073a/b, #1076a-h, #1145a/b/d/e (numeric-named streets / batched edge cases) |
| #1074a/b | #1113a-e (prequalabr `Old` handling — PG keeps it, we strip it) |
| #1076g, #1087a/b/c | #1112a (we drop to Stage B where PG finds Stage A) |
| #1112b/c/d/e, #1113f, #1145c | |

**Two real buckets remain (everything else is upstream parser drift):**

- **Bucket 1 — T18a is intentionally better than PG, not a bug:** input "26 Court Street, 02109" has no city-before-ZIP. Both PAGC parsers misparse `city='STREET'`, no `suftype`. PG's [`pagc_normalize_address`](../scripts/parity/pg_compare/tiger_geocoder/src/pagc_normalize/pagc_normalize_address.sql) is a thin COALESCE wrapper around the two parsers — no validation, no recovery — so the misparse flows straight into scoring: `lev('STREET','BOSTON')` ≈ 6 plus `lev('','St')*5` = 10 type penalty per candidate → Court St and Court Sq both rate 18 (true tie, broken arbitrarily toward Court Sq). Our [`from_pagc`](../src/sql/from_pagc.sql.in) adds a post-PAGC validation step PG doesn't have: when both parsers agree the "city" is a street-type word (a hardcoded list of suffix abbreviations), infer `street_type` from that word and null out `location`. That changes our scoring so Court St rates 7 and Court Sq rates 12 (the type-match resolves the tie correctly). We pick Court St — same address as the vendored expected (rating 6), at rating 7. **The workaround is a DuckDB-specific value-add, not a port artifact** — do not remove it in any "bit-match PG exactly" cleanup; doing so would regress this case to PG's wrong-pick.

- **Bucket 2 — `us_address_standardizer` rule-file lineage divergence (~15 tests including #1076*, #1112*, #1113*, #1145*):** the DuckDB community extension's PAGC rule files (`us_lex`, `us_gaz`, `us_rules`) are a 1:1 faithful copy of *upstream* `postgis/address_standardizer`. PG's bundled `pagc_*` rule files are a *hand-modified fork* of that same upstream, tuned for TIGER's vocabulary (TIGER stores street types in abbreviated form like `SVC RD`, so PG's rules tokenize toward `SVC RD` instead of upstream's `SERVICE ROAD`, and add composite-token rules like `SERVICE DR → SVC DR` that upstream lacks). Same `address_standardizer` C library; different rule data because PG is the one that diverged from upstream. Verified against three concrete cases:

  | Input | Our parse | PG's parse |
  |-------|-----------|-----------|
  | `"8401 W 35W Service Dr NE"` | house=`35 W`, name=SERVICE, unit=`# 8401 W` (broken) | house=8401, name=`"35 W"`, suftype=`SVC DR`, sufdir=NE |
  | `"8040 OLD CEDAR AVE S"` | name=CEDAR, **qual**=OLD (split) | name=`"OLD CEDAR"` (kept together) |
  | `"16725 Co Rd 24"` | pretype=`COUNTY ROAD`, name=24 | suftype=`CO RD`, name=24 |

  These flow into our geocoder as different field assignments → different scoring/output formatting. **Our geocoder is correct given the parses it receives**; the divergence is upstream of us. Fix is to align the community extension's rule files with PG's, or to monkey-patch our rule-loader to override critical entries. Tracked in [project memory](../.claude/projects/-Users-ericmm-Documents-GitHub-duckdb-geocoder/memory/project_us_address_standardizer_rules.md).

The remaining surface-level divergences (T6, T12, T13, T16 multi-row outputs) reflect documented D7 (relaxed dedup) and float precision at the 5th decimal — first-row picks match.

The full PG-2025-PAGC oracle is the new parity baseline; the original vendored file is retained as historical reference (PG's built-in normalizer + ~2010-era TIGER) but should not be used for new parity work.

### Documented improvements over PG (deliberate divergences)

Two specific mechanisms in our pipeline produce *better* results than PG-with-PAGC on certain inputs. Both are intentional; **don't remove them in any "match PG bit-for-bit" cleanup**.

**Mechanism A — `from_pagc` post-PAGC validation** (resolves T18a-class).

PG's [`pagc_normalize_address`](../scripts/parity/pg_compare/tiger_geocoder/src/pagc_normalize/pagc_normalize_address.sql) is a thin COALESCE wrapper around two PAGC parsers — no validation, no recovery. Whatever PAGC says, PG accepts. For inputs with no city before the ZIP (e.g. `"26 Court Street, 02109"`), PAGC misparses `city='STREET'`; PG inflates the rating by paying `lev('STREET','BOSTON') ≈ 6` against every Boston candidate.

Our [`from_pagc`](../src/sql/from_pagc.sql.in) detects when both parsers agree the "city" is one of a hardcoded list of street-type words, infers `street_type` from that word, and nulls `location`. Triggers when: input has no recognizable city before the ZIP AND the trailing word is a street-type abbreviation.

**Mechanism B — `numeric_streets_equal` always on in candidate-finding** (resolves #1145a/b/e-class).

PAGC strips ordinal suffixes: `"27th"` → name=`'27'`, `"36th"` → `'36'`, `"18th"` → `'18'`. PG's primary stage_a uses ONLY exact `f.name = $2` for short streetnames (length ≤ 5), so it can't match TIGER's `name='27th'` from input `'27'`. PG has a `numeric_streets_equal` clause but only in its **fallback** stage_a, which runs only if primary's best rating ≥ 30. For `#1145a`, PG primary finds `Co Rd 27` at rating 27 (under threshold) → never tries fallback → never finds 27th Ave S.

Our [`name_match_tlids`](../src/sql/geocode_address.sql.in) always runs the `numeric_streets_equal` branch (a consequence of D7's collapsed primary/fallback). So we find both `name='27'` AND `name='27th'` candidates and pick the one that scores best.

Triggers when: PAGC strips ordinal/letter suffix from a numeric streetname (length ≤ 5 result) AND TIGER's name retains the suffix AND PG primary's best alternate would rate < 30.

### Post-investigation status (April 2026)

After landing the rule-data ship + sort key + pprint_addy + soundex length-gate + suftype-only rate_attributes + fullname-prefix LIKE + prequalabr-aware addy + clamp out-of-range + nested dedup + scaled house penalty fixes, **first-row pick parity is 43/51 against PG-2025-PAGC.** The remaining 8 first-row divergences split:

| Test | Category | Disposition |
|---|---|---|
| T18a | **Us better than PG** (Mechanism A — `from_pagc` recovery) | Don't fix |
| #1145a | **Us better** (Mechanism B — unconditional `numeric_streets_equal`) | Don't fix |
| #1145b | **Us better** (Mechanism B) | Don't fix |
| #1145e | **Us better** (Mechanism B) | Don't fix |
| #1076h | **D7 cost — recoverable in principle** | Same address picked, rating off by 2 (PG's `+3` no-input-ZIP fallback in fallback stage_a vs our `+1`). Closeable only by reverting D7 (porting PG's two-stage_a structure). Not worth it |
| #1113d | **PostgreSQL physical row-order non-determinism** | Both PG and we evaluate the IDENTICAL 3-candidate set for "Rockford Rd" — TLID 43606664 (one addr row) and TLID 43852639 (two addr rows, sides L and R). All three at sub_rating=5 (out-of-range scaled penalty). All three in the same `DISTINCT ON (predirabrv, fename, type, sufdir, place, state, zip)` partition. PG's `ORDER BY 1,2,3,4,5,6,7,9` documents sub_rating as the only non-partition tiebreak — but with sub_rating tied across all three, PG's pick depends on PostgreSQL's *physical heap row order* (disk insertion order). PG happens to pick the 43852639 side=R row (15899); we deterministically pick the 43852639 side=L row (15702) via our `a_fromhn ASC` tiebreak. **Not closeable in SQL** — PG's tiebreak isn't specified at the SQL level, just emerges from storage layout that differs between PostgreSQL heap and DuckDB columnar |
| #1073a | **D7 cost — multi-query zip_info iteration** | Input "212 3rd Ave N, MINNEAPOLIS, MN 553404" parses correctly (PG and us identical), but ZIP `55340` is Hanover, not Minneapolis. PG runs **3 sequential queries** with different `zip_info.zip` shapes: (1) primary stage_a with city-filtered ZIP window {55339,55340,55341}, (2) fallback stage_a with single-zip {55340} via `zip_state` lookup, (3) fallback stage_a with all 30 Minneapolis ZIPs via `zip_state_loc`/place lookup. Final pick comes from #1 (Hanover 55341 made the window). We collapse all three into one always-on pass with one `window_zips` shape, so we evaluate a different candidate set and pick differently. Closeable by D7 reversal |
| #1145d | **D7 cost — parser-broken input that PG salvages via fallback stage_a** | Input "8512 141 St Ct Apple Valley" has TWO numbers and PAGC can't tokenize it correctly. Both parsers produce `house=141, name='ST', type='Ct'` (we now also capture `internal='8512'` after the v0.x→v0.y geocode_input widening). With short streetname='ST', PG's primary stage_a finds nothing (uses exact match only for length≤5); PG then runs *fallback* stage_a after location-based ZIP expansion, and its fallback uses **unconditional soundex** when `zip_info.exact=false`. `soundex('ST')` matches every "St"-soundexed name in the expanded ZIP window, surfacing junk candidates like `141 W 121st St, Burnsville` at rating 51. Our pipeline length-gates soundex >5 (to prevent the digit-stem flood — see Mechanism B's `numeric_streets_equal` gate) AND has no fallback path, so we drop to Stage B (rating 100). Both implementations produce garbage; PG's is just lower-rated garbage. Closeable only by porting PG's primary/fallback structure (D7 reversal) AND adding a "fallback uses unconditional soundex" mode |

**Three "wrongness" categories:**
- **us-better-than-PG (4 tests)**: T18a, #1145a/b/e — our preprocessing mechanisms catch PAGC limitations PG carries through. Documented above; do not revert.
- **D7 cost (3 tests)**: #1076h, #1073a, #1145d — trace back to PG's primary/fallback stage_a structure that we deliberately collapsed in D7. Closeable only by D7 reversal (separate planned work).
- **PostgreSQL physical row-order non-determinism (1 test)**: #1113d — PG's `DISTINCT ON` has an explicit ORDER BY at the SQL level, but rating ties are resolved by PG's heap natural row order (disk insertion order). DuckDB's columnar storage has different natural ordering, so we deterministically pick a different row from the same tied set. Not closeable in SQL — would require replicating PostgreSQL's storage layout.

**Net: no remaining bugs in our code.** All 8 divergences are accounted for by deliberate design choices, D7, or storage-layout differences that aren't expressible in SQL. When D7 is revisited (as planned), the three D7-cost cases will all be re-evaluated — porting PG's primary/fallback split should resolve most or all of them. The four us-better-than-PG cases need careful handling at that time so they don't regress. #1113d will likely remain divergent regardless of D7 work.

### Roadmap

- Port `pagc_normalize_address_regress` as a CI-friendly sqllogic test (parser-only, no TIGER). The PG-vendored expected outputs become the test oracle for our `from_pagc` repack.
- Extend the harness to the batched-VALUES forms in `geocode_regress.sql`.
- Port `reverse_geocode_regress.sql` (8 tests, MA + MN).
