# Geocode Flow — Reference for the DuckDB Port

This document traces what actually happens inside `postgis_tiger_geocoder` when its workhorse geocoding functions run against a single address or a batch of addresses. It is **not** an API reference — it is a semantic spec intended to let us rebuild the geocoder from scratch as a DuckDB community extension without transliterating the PL/pgSQL.

All file references are to paths under [src/](../src/) unless noted.

## 0. Big picture

The PostGIS tiger geocoder is not a single function — it is a cascade of nested SQL/PLpgSQL functions operating on three kinds of data:

1. **Lookup dictionaries** (small, static): direction, street-type, secondary-unit, state, place, county, countysub, and ZIP → state lookups. Used for parsing free-form input.
2. **TIGER reference tables** (large, per-state, loaded by the user): `featnames`, `edges`, `faces`, `addr`, `place`, `cousub`, `county`, `state`, `zcta5`, `zip_lookup_base`, `zip_state`, `zip_state_loc`.
3. **Candidate scoring** via `rate_attributes()` + `levenshtein_ignore_case()` + a handful of bespoke penalty functions.

The public entrypoints are:

| Function | Input | Output | Purpose |
| --- | --- | --- | --- |
| `normalize_address(varchar) → norm_addy` | free-form address string | parsed composite | parse into structured components only |
| `pagc_normalize_address(varchar) → norm_addy` | same | same | parse via the `address_standardizer` extension (PAGC) |
| `geocode(varchar, max_results, restrict_geom) → setof (addy, geomout, rating)` | free-form address string | N ranked matches | the main geocoder |
| `geocode(norm_addy, …)` | already-parsed composite | same | skip normalization, used for batching |
| `geocode_intersection(road1, road2, state, city, zip, n) → setof …` | two street names + location | N ranked matches at the intersection | cross-street geocoding |
| `reverse_geocode(geometry, include_strnum_range) → record` | point | arrays of `norm_addy` and cross-street names | reverse geocoding |
| `get_tract(geometry, output_field) → text` | point | census tract id/name | census enrichment |

Everything else (`geocode_address`, `geocode_location`, `rate_attributes`, `interpolate_from_address`, `includes_address`, helpers in `other_helper_functions.sql`) is internal.

All geometry is stored and returned in **SRID 4269** (NAD83). Any input geometry in another SRID is transformed to 4269 before use. Some interpolation math transiently projects to UTM to compute meter offsets.

Control flow is eager and row-oriented in PL/pgSQL: the outer geocoder calls the inner one, loops through the result cursor, and short-circuits whenever it sees a `rating = 0` (perfect) or has enough acceptable matches. This short-circuiting is the single biggest semantic quirk to preserve or consciously discard in the DuckDB port.

## 1. The `norm_addy` composite (Postgres's internal type — not our public contract)

> **Porting note.** This type exists in the reference description that follows only because every `geocode_*` function in PostGIS reads from and writes to it internally. The DuckDB port does **not** expose `norm_addy`. Our public input type is the 8-field `geocode_input` struct defined in §14, which strips out the parser-side artifacts (`parsed`, `zip4`, `address_alphanumeric`, `internal`) that the geocoder never actually consults. Don't build against `norm_addy`; build against `geocode_input`.

Defined in [sql_bits/norm_addy_create.sql.in](../sql_bits/norm_addy_create.sql.in):

```
norm_addy = (
    address              INTEGER,        -- numeric house number (e.g. 1731)
    preDirAbbrev         VARCHAR,        -- 'N', 'NE', etc., before street name
    streetName           VARCHAR,        -- 'New Hampshire' in 'New Hampshire Ave'
    streetTypeAbbrev     VARCHAR,        -- 'Ave', 'St', 'Hwy' (from street_type_lookup)
    postDirAbbrev        VARCHAR,        -- trailing direction
    internal             VARCHAR,        -- 'APT 301', etc. (from secondary_unit_lookup)
    location             VARCHAR,        -- city/place/countysub
    stateAbbrev          VARCHAR,        -- 'MA'
    zip                  VARCHAR,        -- 5-digit ZIP
    parsed               BOOLEAN,        -- true once normalization has run
    zip4                 VARCHAR(4),     -- the +4 part of ZIP+4
    address_alphanumeric VARCHAR         -- e.g. '1R' when house number has a letter
)
```

Everything downstream of normalization is driven by this record. `parsed = false` means the geocoder must bail. Fields being NULL drives whether branches of the geocoder run at all (e.g., "no street name" → skip address-level match).

## 2. Reference data actually consulted during geocoding

From [src/tables/lookup_tables_2011.sql](../src/tables/lookup_tables_2011.sql):

- **Parsing-only lookups (small, static):**
  - `direction_lookup(name, abbrev)` — e.g. `NORTHWEST` → `NW`
  - `street_type_lookup(name, abbrev, is_hw)` — e.g. `BOULEVARD` → `Blvd`, `is_hw` flag marks highway/route types whose name often follows the type
  - `secondary_unit_lookup(name, abbrev)` — e.g. `APARTMENT` → `APT`
  - `state_lookup(st_code, name, abbrev, statefp)` — e.g. `MA` ↔ FIPS `25`
  - `place_lookup`, `county_lookup`, `countysub_lookup` — currently unpopulated schemas; the live lookups are hit against the tiger data tables below
  - `zip_lookup_base(zip, state, county, city, statefp)` — ZIP ↔ city mapping, one row per ZIP
  - `zip_lookup` — aggregate table (not loaded by default)

- **TIGER reference data (large, per-state, loaded into inheriting tables):**
  - `state(statefp, stusps, the_geom, …)` — state polygons
  - `county(statefp, countyfp, cntyidfp, the_geom, …)`
  - `place(statefp, placefp, plcidfp, name, the_geom, …)` — incorporated/census places
  - `cousub(statefp, countyfp, cousubfp, cosbidfp, name, the_geom, …)` — county subdivisions
  - `zcta5(statefp, zcta5ce, the_geom, …)` — ZIP Code Tabulation Area polygons
  - `zip_state(zip, stusps, statefp)` — which states each ZIP touches
  - `zip_state_loc(zip, stusps, statefp, place)` — ZIP ↔ place
  - `edges(tlid, statefp, tfidl, tfidr, mtfcc, fullname, the_geom MULTILINESTRING(4269), tnidf, tnidt, zipl, zipr, …)` — the street network; `tlid` is the TIGER edge id, `tfidl`/`tfidr` are the face ids on each side, `tnidf`/`tnidt` are the endpoint node ids, `mtfcc` starts with `S` for street/road features
  - `faces(tfid, statefp, countyfp, placefp, cousubfp, the_geom MULTIPOLYGON(4269), …)` — the polygonal blocks that edges bound; used to resolve the `place` (city) for a given edge+side
  - `featnames(tlid, statefp, name, fullname, predirabrv, pretypabrv, prequalabr, suftypabrv, sufdirabrv, mtfcc, …)` — the searchable street-name vocabulary (one edge can have multiple names/aliases)
  - `addr(tlid, statefp, fromhn, tohn, side ∈ {'L','R'}, zip, plus4, …)` — the house-number ranges per edge and side-of-street

Key joins used ubiquitously:
- `featnames.tlid = addr.tlid AND featnames.statefp = addr.statefp` — to go from "a candidate street name" to "which blocks/ranges does it cover"
- `featnames.tlid = edges.tlid` — to get the line geometry
- `edges.tfidl` or `edges.tfidr = faces.tfid` — to get the place/city on the correct side of the street
- `faces.placefp = place.placefp` — to get the place *name*

Only street-type MTFCC features (`mtfcc LIKE 'S%'`) are considered. State-scoped (`statefp = $1`) is always applied first to prune — this is the dominant selectivity win in the Postgres implementation.

## 3. Settings that change behavior

From [src/geocode_settings.sql](../src/geocode_settings.sql) — stored in `tiger.geocode_settings`:

- `use_pagc_address_parser` (bool) — route `normalize_address` to the PAGC C library
- `zip_penalty` (numeric, default 2) — multiplier on ZIP drift in the rating formula
- `reverse_geocode_numbered_roads` (0/1/2) — prefer numbered highways, named roads, or neither
- Four `debug_*` flags that only control `RAISE NOTICE` output

Nothing else is user-tunable. For the DuckDB port these become table/macro options rather than a settings table.

## 4. `normalize_address(input_text) → norm_addy`

File: [src/normalize/normalize_address.sql](../src/normalize/normalize_address.sql) (with helpers in `src/normalize/*.sql` and `src/utility/*.sql`).

This is purely string parsing — no TIGER data, only the lookup dictionaries. It runs these steps sequentially:

1. **PAGC shortcut.** If `use_pagc_address_parser=true`, call `pagc_normalize_address()` and return.
2. **Leading house number** — regex `^([0-9].*?)[ ,/.]` captures the numeric address, plus an alphanumeric variant `^([0-9a-zA-Z].*?)[ ,/.]` for `1R`-style.
3. **Trailing ZIP** — five forms, in order: `XXXXX`, `XXXXX-YYYY`, partial `XX..XXXXX`, `6–14 digits` (garbage case), `^XXXXX$` (input is ZIP only → return immediately with just `zip` set).
4. **State extraction** via `state_extract()` ([src/normalize/state_extract.sql](../src/normalize/state_extract.sql)) against `state_lookup`. Fuzzy (soundex + Levenshtein) if no exact abbrev. Returns `"Full Name:AB"` format which the caller splits on `:`.
5. **Comma-delimited fast path.** If the remainder looks like `street, location[, state]`, split on comma; anything after the inner comma becomes `location`, the bit before becomes `fullStreet`.
6. **Location extraction** (non-comma case) via `location_extract(fullStreet, stateAbbrev)` ([src/normalize/location_extract.sql](../src/normalize/location_extract.sql)). It walks the string word-by-word from the end, soundex-matching accumulated suffixes against `place` and `cousub` (filtered by `statefp` when known), keeping the longest low-Levenshtein match. A match that exactly equals a street-type name is rejected.
7. **Internal address** via `secondary_unit_lookup` — regex-matches any of `APT`, `STE`, `FL`, etc. followed by `#?` and an optional digit/alnum suffix. Multiple matches → the rightmost wins.
8. **Street type** via `street_type_lookup`. If multiple types match, prefer the rightmost occurrence. `is_hw=true` entries (e.g. `RTE`, `HWY`) can legitimately precede the actual road name (`State Hwy 22a`), so they are also allowed at the start.
9. **Highway reduction.** If `is_hw` and a number-or-alnum token follows the type, that token becomes the `streetName` (e.g. `Country Road 24` → streetName `24`, type `Rd`).
10. **Pre/post direction** via `direction_lookup`. Post-direction is matched at the end of the reduced street; pre-direction at the start. Tie-breaking uses length and the raw input position (to distinguish e.g. `North East` as preDir from a subsequent `East` as postDir).
11. **Assembly** into `norm_addy` with `parsed = true`.

Important gotchas the DuckDB port must honor:
- Location extraction does **not** return the canonical name from the dictionary; it returns the slice of the **input string** that corresponds to the match. This is critical because later rating uses Levenshtein distance against the *input* form.
- `streetType` may be null even after parsing — the cascade has a "no street type found" branch.
- If only a ZIP was input, normalize returns `{zip, parsed=true}` and nothing else; the geocoder then has to operate purely from ZIP polygons.
- All comparison is `lower()` + `ignore_case`; case is not preserved for matching but is preserved on output where possible.

PAGC ([src/pagc_normalize/pagc_normalize_address.sql](../src/pagc_normalize/pagc_normalize_address.sql)) is a thin shim that calls the `address_standardizer` extension's `standardize_address()` + `parse_address()` and repacks the result into `norm_addy`. Semantically it is interchangeable with the built-in parser. **For the DuckDB port we are told to assume a DuckDB PAGC port exists** — so the DuckDB geocoder only needs to consume a normalized record; the complex PL/pgSQL parser above does not need to be rewritten unless we want a non-PAGC fallback.

## 5. `geocode(varchar, …)` — the main entrypoint

File: [src/geocode/geocode.sql](../src/geocode/geocode.sql).

Thin SQL wrapper:
```
geocode(text) = geocode(normalize_address(text))    -- filtered to parsed=true, sorted by rating
```

The batching shape here is interesting: it's a `CROSS JOIN LATERAL`. For a table of N addresses, Postgres fans out row-at-a-time through the structured overload. **This is where the DuckDB port has the biggest opportunity** — normalization and TIGER lookups should be expressible as set-oriented joins over the whole batch.

## 6. `geocode(norm_addy, max_results, restrict_geom)` — the cascade

Same file, `(norm_addy, …)` overload. Two-stage cascade:

**Stage A — full address match.** Runs only if `streetName IS NOT NULL AND (zip IS NOT NULL OR stateAbbrev IS NOT NULL)`:
1. Call `geocode_address(parsed, max_results, restrict_geom)` (see §7).
2. Deduplicate on the full addy tuple (`address, predirabbrev, streetname, streettypeabbrev, postdirabbrev, internal, location, stateabbrev, zip`), keeping the best rating per duplicate.
3. Sort by rating, yield up to `max_results`. **Return immediately if any result has `rating = 0`** (exact match).
4. If Stage A produced at least one result, return.

**Stage B — location fallback.** Runs only if Stage A produced nothing *and* (`zip IS NOT NULL OR (stateAbbrev AND location)`):
1. Call `geocode_location(parsed, restrict_geom)` (see §8).
2. Sort by rating, yield up to `max_results`. **Return on `rating = 100`** (the perfect location-match rating; see §12).

Rating discontinuity: Stage A returns integers clustered near 0 for good, 50+ for poor. Stage B returns integers starting at 100 (location matches are *always* marked worse than any real address match). Consumers use rating as a total order.

## 7. `geocode_address(parsed, max_results, restrict_geom)` — the heart of the geocoder

File: [src/geocode/geocode_address.sql](../src/geocode/geocode_address.sql). This is the most complex function in the repo. It runs in two sub-stages, both dynamic SQL.

### 7.1 Preliminaries

- Bail if `streetName IS NULL`.
- Resolve `in_statefp`: from `state_lookup.abbrev = parsed.stateAbbrev`, or fallback to the first `zip_lookup_base.statefp` for the input ZIP.
- Normalize `restrict_geom`: if SRID is 0 or 4236 (≈WGS84), reinterpret as 4269; otherwise transform to 4269 and `ST_SnapToGrid` it. The snap is defensive — it prevents floating-point intersection misses.
- Build `var_bfilter` — a predicate fragment that filters `tiger.zcta5` to ZCTAs inside `restrict_geom`, used to scope later ZIP lookups.
- Expand the input ZIP into a candidate window using `zip_range(zip, -2, +2)` for long street names or `(-1, +1)` for short ones. Rationale: typos in ZIP are usually ±1–2 numeric off; longer street names give enough additional selectivity to widen the window without blowing up false positives.
- If the ZIP looks bad (length ≠ 5) but a location is present, re-derive the ZIP window by joining `zip_lookup_base` on the location name (`lower(city) LIKE lower(parsed.location) || '%'`).
- If still no ZIPs, widen to any ZIP in the state whose city starts with the parsed location.
- Cache all the ZIP candidates in `zip_info.zip varchar[]`.

### 7.2 Sub-stage A: brute-force exact-match-first

Builds one giant SQL statement (parameterized) that:

1. **Inner CTE `a`** — pulls candidate edges from `featnames ⨝ addr`, restricted to `statefp = in_statefp`, ranked by a composite penalty:

    ```
    rank = diff_zip(addr.zip, parsed.zip) * zip_penalty
         + (name=input ? 0 : levenshtein_ignore_case(name, input))
         + levenshtein_ignore_case(fullname, input_street + ' ' + streetType)
         + (parity of max(fromhn,tohn) == parity of input address ? 0 : 1)
         + (input address ∈ [least_hn(fromhn,tohn), greatest_hn(fromhn,tohn)] ? 0 : 4)
         + (input type matches suftyp or pretyp ? 0 : 1)
         + rate_attributes(preDir, predirabrv, input_street, name, streetType,
                           suftypabrv, postDir, sufdirabrv, prequalabr)
    ```

    Street-name matching is strict `lower(name) = lower(input)` for short names (≤5 chars); for longer names it is `fullname LIKE input || '%' OR name = input OR soundex(name) = soundex(input)`. ZIP filter: `addr.zip = ANY(zip_info.zip)`.

    Limit 3× max_results to keep the candidate set tight.

2. **Outer select** — joins to `edges`, `faces`, `place` on `faces.placefp = place.placefp` with the correct side-of-street rule:

    ```
    (edges.tfidl = faces.tfid AND addr.side = 'L') OR
    (edges.tfidr = faces.tfid AND addr.side = 'R')
    ```

    For each edge, computes `interpolate_from_address(parsed.address, fromhn, tohn, edges.the_geom, side)` as the output point (see §13). Computes a `sub_rating`:

    ```
    sub_rating = rate_attributes(...)
               + CASE                                       -- house-number penalty
                   input_address NULL or no range                             → 20
                   in range AND same parity                                   → 0
                   in range (wrong parity)                                    → 2
                   out of range                                               → 5 + scaled_distance
                 END
               + CASE ZIP penalty: least(diff_zip(input, candidate)*zip_penalty, 20*zip_penalty) END
               + levenshtein_ignore_case(parsed.location, candidate.place)    (coalesced to 5 if null)
    ```

    `DISTINCT ON (predirabrv, fename, coalesced suffix type, sufdirabrv, place, state, zip)` collapses duplicates. Ordered by `sub_rating, exact_address DESC`.

3. **Result loop.** Iterates with this control flow:
    - First result's rating is captured as `var_bestrating`.
    - Results with `rating < 90` are yielded.
    - `rating = 0` → return immediately.
    - If `var_n >= max_results AND rating < 10` → return.
    - After the loop, if `var_bestrating < 30` → skip sub-stage B entirely.
    - If any `exact_address = true` result was seen, set `exact_street = true`.

The `exact_street` flag matters because **sub-stage B is skipped for non-exact ZIP windows if we already have a solid exact hit** — this is the biggest early-exit in the geocoder.

### 7.3 Sub-stage B: soft-match cascade over ZIP/place candidates

If sub-stage A produced no good-enough match, the function enumerates alternative `(statefp, location, zip[], exact)` candidate buckets via a UNION:

1. `zip_state.zip = parsed.zip AND statefp matches` — exact ZIP match, `pref = 1`
2. `zip_state_loc` with `lower($1) = lower(place)` — exact city match, `pref = 1 + abs(diff_zip)*zip_penalty`
3. `zip_state_loc` with `soundex(place) = soundex($1)` — fuzzy city, `pref = 3`
4. `zip_lookup_base` with `soundex(city or county) = soundex($1)` — even fuzzier, `pref = 4`
5. Fallback: `(in_statefp, parsed.location, NULL, exact=false)`, `pref = 5`

These are ordered `exact DESC, pref, zip`.

For each bucket, a second dynamic SQL does essentially the same join as 7.2 but with relaxed matching on the street name:
- **exact bucket:** `lower(input) = lower(a.name)` OR prequal-stripped match OR `numeric_streets_equal(input, name)`.
- **non-exact bucket:** `soundex(input) = soundex(name)` OR (long-name prefix) OR `numeric_streets_equal`.

The place is resolved via `COALESCE(place.name, cousub.name, zip_lookup_base.city, county.name)` — this is the strict priority for the output `location`. The rating adds ZIP Levenshtein (not just numeric diff, because typos in leading digits matter).

**Loop short-circuits:**
- `rating > 99` → bail (too far).
- `rating = 0` → return immediately.
- `zip_info.exact == false AND exact_street (from A)` → don't even run this bucket.
- `var_n > max_results` → done.

### 7.4 What comes back

One row per candidate match: `{addy: norm_addy, geomout: geometry(POINT, 4269), rating: integer}`. The addy has its `address`, `preDirAbbrev`, `streetName`, `streetTypeAbbrev`, `postDirAbbrev`, `location`, `stateAbbrev`, `zip`, `parsed=true` populated from the winning row. If the input address fell outside the range but still matched a street, the addy reports the nearest range endpoint (not the original input) — this is intentional and signals "we know about this street but not this number".

## 8. `geocode_location(parsed, restrict_geom)` — location-only fallback

File: [src/geocode/geocode_location.sql](../src/geocode/geocode_location.sql). Runs when there is no street name to work with.

1. Try `zcta5 ⨝ zip_lookup_base` using either `zip_lookup_base.zip = parsed.zip` OR `soundex(city) = soundex(parsed.location) AND statefp = in_statefp`. Emit `ST_Centroid(zcta5.the_geom)` as the point. Rating = `100 + levenshtein_ignore_case(city, parsed.location)`.
2. If that yielded no `rating = 100`, try `place` (filtered to state and optionally `ST_Intersects(restrict_geom, the_geom)`): match by `soundex`, rating again `100 + levenshtein`.

So **location-only matches always have rating ≥ 100**. This is the convention that puts them strictly below any successful address match in Stage A.

## 9. `geocode_intersection(road1, road2, state, city, zip, n)`

File: [src/geocode/geocode_intersection.sql](../src/geocode/geocode_intersection.sql).

1. Normalize each road by calling `normalize_address('0 <road>, <city>, <state> <zip>')`. The `0` forces the parser into its street-level branch.
2. Resolve `in_statefp` via `state_lookup`.
3. Build the ZIP filter: `zip_range(in_zip, -2, +2)` if ZIP given; otherwise ZIPs for the city from `zip_lookup_base`.
4. Two CTEs (`a1`, `a2`) gather candidate edges from `featnames ⨝ addr` for each road, matching on `lower(name) = input_name` OR (if name >5 chars) `fullname LIKE fullname_input || '%'` OR `normalize_street_name(fullname) = normalize_street_name(input)`. The last one is the fix for "I-635" vs "I- 635" collapsing.
5. Expand each to `edges` (CTEs `e1`, `e2`), keeping `tfidl` or `tfidr` matching `side`.
6. Join `e1` to `e2` on **shared endpoint** using `ARRAY[e1.tnidf, e1.tnidt] && ARRAY[e2.tnidf, e2.tnidt]` — TIGER node ids, not geometric intersection. This is the key trick: TIGER edges already carry topological node ids (`tnidf`, `tnidt`), so finding intersections is a set-overlap join on integers, not an expensive `ST_Intersects`.
7. For each intersecting pair:
    - Pick the `fromhn` or `tohn` from `e1` based on which endpoint is shared with `e2`.
    - The output point is that endpoint's geometry: `ST_StartPoint(ST_GeometryN(ST_Multi(e1.the_geom),1))` or `ST_EndPoint(...)`.
    - Rate = `levenshtein(place, city) + levenshtein(e1.name, road1) + levenshtein(e1.fullname, road1_full) + levenshtein(e2.name, road2)` (with short-circuits for exact name matches).
8. `DISTINCT ON (e1.tlid, e1.side)` to dedupe, order by rating.

Note: The function is declared `IMMUTABLE` but it reads from reference tables — this is a knowing violation used to get a planner boost; do not replicate as-is in DuckDB.

## 10. `reverse_geocode(pt, include_strnum_range)`

File: [src/geocode/reverse_geocode.sql](../src/geocode/reverse_geocode.sql).

Output is a single record: `{intpt: geometry[], addy: norm_addy[], street: varchar[]}`. Walks from coarse to fine:

1. **Normalize input.** If SRID is 4269, keep; if known, transform to 4269; if 0, assume 4269. `ST_SnapToGrid(..., 0.00005)` to defeat float noise.
2. **State** via `ST_Intersects(state.the_geom, pt) LIMIT 1`. If none → return empty.
3. **County** via `ST_Intersects(county.the_geom, pt) AND statefp=$state`.
4. **ZIP** via `ST_Intersects(zcta5.the_geom, pt) AND statefp=$state`. Set as initial `addy.zip`.
5. **Place** via `place` then fall back to `cousub`. Set as `addy.location`.
6. **Edges near the point** via `ST_DWithin(edges.the_geom, pt, 0.01)` restricted to `mtfcc LIKE 'S%'` and further constrained by `faces` (the edge's face on the correct side must contain or be near the point). Left-join to `addr` so edges without ranges still surface. Compute `center_pt = ST_ClosestPoint(edges.the_geom, pt)`, distance via `ST_DistanceSphere` in meters.
7. Order candidates by `reverse_geocode_numbered_roads` preference (highway-number vs named-road priority), then name, then distance. Top 50.
8. **Primary edge** is the nearest; its geometry/fullname anchor the answer.
9. **Interpolate house number** on the primary edge: `nstrnum = fromhn + ST_LineLocatePoint(line, pt) * (tohn - fromhn)`. Parity correction: if the interpolated number's parity doesn't match the range's parity, nudge by ±1.
10. **Collect candidates.** For every nearby edge whose line geometrically intersects the primary:
    - Append the edge's center point to `intpt[]`.
    - Append a `norm_addy` built from the edge to `addy[]`.
    - If the edge name differs from the primary, append a cross-street string to `street[]` (optionally prefixed with the house-number range).
    - Back-patch the previous addy's street name if it was missing but this one has one (handles ramps and weird topology).

So reverse geocode is more of an enumeration than a ranking — caller gets the primary address first, then alternates.

## 11. Rating semantics consolidated

The rating is an *ordered penalty* — lower is better, 0 is a perfect match. There is no upper bound, but in practice:

| Range | Source | Meaning |
| --- | --- | --- |
| **0** | Stage A | Exact house number, street, type, direction, ZIP, place |
| 1–29 | Stage A | "Strong" match — typos or minor mismatches |
| 30–89 | Stage A | Acceptable; geocoder may still emit |
| 90–99 | Stage A | Marginal; emitted but deprioritized |
| ≥ 100 | Stage B, `geocode_location` | Location-only (no street-level confidence) |

Components that contribute to the Stage A rating (see [src/geocode/rate_attributes.sql](../src/geocode/rate_attributes.sql) and the two big SQL strings in `geocode_address.sql`):

- `levenshtein_ignore_case(input_preDir, candidate_preDir) * 2` — direction weight
- Street-name Levenshtein `* 10` — name weight (0 penalty if both names are numeric street equivalents per `numeric_streets_equal`; special handling for `prequalabr` like `Old` in `Old Main St`)
- `levenshtein_ignore_case(input_type, candidate_type) * 5`
- `levenshtein_ignore_case(input_postDir, candidate_postDir) * 2`
- `+ levenshtein_ignore_case(input_location, candidate_location)` (when both non-null)
- House-number penalty: 0 (in range + parity) / 2 (in range wrong parity) / 5–10 (out of range, scaled by how far) / 20 (no range available)
- ZIP penalty: `min(diff_zip, levenshtein_ignore_case) * zip_penalty`, capped per code path; `diff_zip` is numeric distance on the first five digits
- Place Levenshtein (coalesced to 5 if null)
- Occasional +1 fixed penalties for parity mismatches, type-in-wrong-slot, etc.

`levenshtein_ignore_case` = `levenshtein(lower(a), lower(b))`. `cull_null(x)` = `coalesce(x, '')` — keeps Levenshtein from returning NULL when one side is missing.

This is where a DuckDB-native implementation can cleanly vectorize: every component is a pairwise distance computable per-candidate-row in a SELECT.

## 12. House-number interpolation

File: [src/geocode/interpolate_from_address.sql](../src/geocode/interpolate_from_address.sql).

Given `(given_address, addr1, addr2, road_geom, side, offset_m=10)`:

1. Parse both range endpoints as integers; if non-numeric, coerce to 0.
2. `part = (given_address - min(addr1, addr2)) / (max-min)`, clamped to `[0,1]`; swapped if the range runs backwards.
3. Transform the road to UTM (`utmzone(ST_StartPoint(road))`) to work in meters.
4. `center_pt = ST_LineInterpolatePoint(utm_road, part)`.
5. **Side-of-street offset:** compute the local azimuth (handling endpoints and interior differently via the nearest `ST_PointN` pair), then translate `offset_m` perpendicular. `'L'` uses negative offset, `'R'` positive. Quadrant adjustments for the azimuth.
6. Transform the result back to the original SRID.

Ancillary house-number helpers in [src/geocode/other_helper_functions.sql](../src/geocode/other_helper_functions.sql):
- `least_hn(fromhn, tohn)` / `greatest_hn(fromhn, tohn)` — numeric min/max that tolerates non-numeric strings (coerced to 0), wrapped as IMMUTABLE so they index-cache.
- `diff_zip(a, b)` — numeric difference on first 5 digits.
- `zip_range(zip, offset1, offset2)` — returns `varchar[]` of ZIPs in a numeric neighborhood.
- `numeric_streets_equal(a, b)` — treats `15th` ≡ `15rd` as equal for rating purposes, only when both start with digits and are <10 chars.
- `normalize_street_name(s)` — canonicalizes whitespace around `-` so `I-635` and `I- 635` collapse.
- `includes_address(given, addr1, addr2, addr3, addr4)` — range+parity check for two-sided streets (left pair vs right pair); used historically for pre-filtering but not in the current hot path.

## 13. Data loading pipeline — how the reference data gets into Postgres

The loader is the other half of the story. The geocoder above is *useless* without data, and the data does not come from the extension install — it has to be pulled down per-state from the Census TIGER/Line FTP site. The loader machinery lives in [src/tiger_loader.sql](../src/tiger_loader.sql) and is substantial (~500 lines of PL/pgSQL + SQL). The single most important design fact to internalize:

> **The loader does not load data. It generates shell/batch scripts that the *user* runs outside Postgres, which then pipe `shp2pgsql` output back into `psql`.**

This is because TIGER distributes ~3500 shapefile zips (one per county per file-type plus per-state plus nation-wide), totalling tens of GB. Streaming that through `COPY` from a PL/pgSQL function would be slow and fragile. Instead, the extension uses a small templating engine (`loader_macro_replace`) and three control tables to emit bespoke scripts the user inspects and runs.

For the DuckDB port this whole architecture can be collapsed into one or two table functions that use DuckDB spatial's shapefile reader directly — but it is worth understanding what the Postgres version actually does, because the *post-load* work (derived tables, indexes, column pruning, constraint partitioning) is exactly what we still need to reproduce.

### 13.1 The three control tables

All live in schema `tiger` (the extension's schema).

**`loader_platform(os, declare_sect, pgbin, wget, unzip_command, psql, path_sep, loader, environ_set_command, county_process_command)`** — one row per target OS. Ships with two rows: `windows` and `sh`. Each row is essentially a set of shell-snippet templates:

- `declare_sect` — the env-var preamble (e.g. `set PGBIN=...`, `cd ${staging_fold}`). Users are expected to edit this to match their machine — this is the only hand-configuration step.
- `wget` — path to wget (`%WGETTOOL%` on Windows, `wget` on sh).
- `unzip_command` — shell snippet to loop over `*.zip` in the staging dir and extract everything to `TMPDIR`. Literally different between Windows (`for /r %%z in (*.zip) do %UNZIPTOOL% e %%z -o%TMPDIR%`) and Linux (`for z in *.zip; do $UNZIPTOOL -o -d $TMPDIR $z; done`).
- `psql`, `loader` — the `${PSQL}` and `${SHP2PGSQL}` placeholders.
- `path_sep` — `\\` vs `/`; used by `replace(..., '/', platform.path_sep)` at generation time to swap slashes.
- `county_process_command` — a templated inner loop that runs `${loader} ... ${table_name} | ${psql}` for every `*${table_name}*.dbf` present; used for county-level files (featnames, edges, addr, faces, addrfeat).

Non-trivial behavior to note: the `declare_sect` contains dummy credentials (`set PGPASSWORD=yourpasswordhere`). Users **must** edit this before running. There is no validation.

**`loader_variables(tiger_year, website_root, staging_fold, data_schema, staging_schema)`** — a single row. In the 2025 build this is:

```
tiger_year      = '2025'
website_root    = 'https://www2.census.gov/geo/tiger/TIGER2025'
staging_fold    = '/gisdata'
data_schema     = 'tiger_data'
staging_schema  = 'tiger_staging'
```

`tiger_year` and `website_root` bump with each annual TIGER release (see [NEWS.md](../NEWS.md) — "update the geocoder to load TIGER 2025 data"). `staging_fold` is where zips land and get extracted; the user creates this directory with a `temp/` subdirectory. `data_schema` is where loaded data lives (parent tables are in `tiger`, per-state children in `tiger_data`). `staging_schema` is a scratch schema that gets dropped and recreated on every run.

**`loader_lookuptables(process_order, lookup_name, table_name, single_mode, load, level_county, level_state, level_nation, post_load_process, single_geom_mode, insert_mode, pre_load_process, columns_exclude, website_root_override)`** — one row per TIGER table-type (`state`, `county`, `place`, `cousub`, `tract`, `tabblock20`, `bg`, `zcta5_raw`, `faces`, `featnames`, `edges`, `addr`, `addrfeat`, `county_all`, `state_all`). Roughly 14 rows. The column semantics are spelled out in `COMMENT ON COLUMN` but the non-obvious ones:

- `process_order` — integer sort key. Dictates the order of blocks in the emitted script. `state_all=1`, `county_all=2`, then `place=3`, `cousub=4`, `faces=6`, `featnames=7`, `edges=8`, `addr=9`, `addrfeat=9`, `tract=10`, `tabblock20=11`, `bg=12`, `zcta5_raw=13`. The gaps are intentional — `edges` must run before `addr`'s post-load (because `addr` queries `edges` to build `zip_state`), and `featnames` depends on nothing but must run before `edges` for index-cache coherence, etc.
- `lookup_name` vs `table_name` — `lookup_name` is the name of the *parent* table (and the suffix of the per-state child). `table_name` is the filename fragment TIGER uses. For most rows they are identical; they differ for `zcta5_raw/zcta520`, `state_all/state`, `county_all/county`.
- `level_state` / `level_county` / `level_nation` — boolean tags controlling which generator function picks this row up. Nation-level rows (`state_all`, `county_all`, `zcta5_raw`) appear only in `loader_generate_nation_script`; state-level rows appear in the state-level section of `loader_generate_script`; county-level rows expand into one wget-per-county in the same script.
- `load` — master switch. `bg`, `addrfeat`, `zcta5_raw` ship with `load = false` so they are not emitted by default. Users flip these to true if they want block groups or `addrfeat` (the TIGER 2010+ address-feature shapefile, which duplicates `addr+edges` data in a single table).
- `insert_mode` — `c` or `a`, feeds directly into `shp2pgsql`'s `-c` (create) or `-a` (append). `featnames`, `edges`, `addr`, `addrfeat` use `a` because a county-level loop appends into one per-state table.
- `single_geom_mode` — maps to `shp2pgsql -S` (produce single-type geometry instead of multi-). Used for `addrfeat` and `zcta5_raw`.
- `columns_exclude` — text array of source columns to *drop* when `INSERT`ing from the staging table into the final table. This is substantial: the `faces` row excludes 40+ columns (every legacy `_00` and `_10` variant). Rationale: TIGER shapefiles accumulate generational columns forever; the geocoder's parent table only defines the current ones.
- `website_root_override` — used to point specific rows at a different URL than the year-wide `loader_variables.website_root`. Historically used for zcta5 (Census only published that directory sporadically).
- `pre_load_process` — shell snippet that runs *before* `shp2pgsql` pipes data in. Always a `${psql} -c "CREATE TABLE ${data_schema}.${state_abbrev}_${lookup_name}(...) INHERITS(tiger.${lookup_name})"`. This is where the per-state child table gets created with its primary key.
- `post_load_process` — shell snippet that runs *after* data is inserted. This is where all the work happens:
  - `${psql} -c "SELECT loader_load_staged_data(...)"` to copy from staging to data schema (see §13.4).
  - `${psql} -c "ALTER TABLE ... ADD CONSTRAINT chk_statefp CHECK (statefp = '${state_fips}')"` — critical for constraint exclusion (see §13.6).
  - `${psql} -c "CREATE INDEX ... USING gist(the_geom)"`, and other indexes.
  - For `edges` and `addr`: SQL that *derives* `zip_state_loc`, `zip_lookup_base`, and `zip_state` per-state tables from the just-loaded edges+faces+place joins. This is how the non-spatial ZIP→city lookups get populated — TIGER itself doesn't publish them.
  - For `zcta5_raw`: a big `INSERT ... SELECT` that clips each ZCTA to each state polygon (`ST_Intersection` or `ST_Covers`) and writes to `zcta5_all`, then drops `zcta5_raw`. ZCTAs cross state boundaries, so the clip is what produces the per-state-indexed ZCTA polygons the geocoder expects.
  - `${psql} -c "VACUUM ANALYZE ..."`.

This table **is the spec** for what needs to happen per-table-type. For the DuckDB port, this is the checklist.

### 13.2 Source data layout

TIGER/Line shapefiles are published at:

```
${website_root}/
    STATE/tl_${year}_us_state.zip                 # nation-level
    COUNTY/tl_${year}_us_county.zip               # nation-level
    ZCTA520/tl_${year}_us_zcta520.zip             # nation-level (formerly zcta510)
    PLACE/tl_${year}_${state_fips}_place.zip      # state-level
    COUSUB/tl_${year}_${state_fips}_cousub.zip    # state-level
    TRACT/tl_${year}_${state_fips}_tract.zip      # state-level
    TABBLOCK20/tl_${year}_${state_fips}_tabblock20.zip   # state-level
    BG/tl_${year}_${state_fips}_bg.zip            # state-level
    FACES/tl_${year}_${state_fips}${county_fips}_faces.zip       # county-level
    FEATNAMES/tl_${year}_${state_fips}${county_fips}_featnames.zip  # county-level
    EDGES/tl_${year}_${state_fips}${county_fips}_edges.zip       # county-level
    ADDR/tl_${year}_${state_fips}${county_fips}_addr.zip         # county-level
    ADDRFEAT/tl_${year}_${state_fips}${county_fips}_addrfeat.zip # county-level (opt)
```

State FIPS is 2 digits (`25` for MA); county FIPS is 3 digits. A state load for MA with 14 counties pulls 14 × 5 county-level zips + 5 state-level zips = 75 zips.

The directory names in the URL (`STATE`, `FEATNAMES`, etc.) are derived in-code as `upper(lookup_name)` — with a hardcoded special case for `zcta5 → ZCTA5` on top of the already-renamed `zcta5_raw`. Keep that case in mind if pointing at a different year's layout.

### 13.3 The three generator functions

All return `SETOF text` — one giant shell-script string. Users typically `\o script.sh` + `\a` + `\t` in psql before calling, so the result lands in a runnable file.

**`loader_generate_nation_script(os text) → SETOF text`** ([src/tiger_loader.sql:313](../src/tiger_loader.sql#L313))

Emits the nation-level script for one platform. Contains rows where `level_nation = true AND load = true` (i.e. `state_all`, `county_all`, and optionally `zcta5_raw`). Output structure:

```
<declare_sect from loader_platform, with env vars substituted>
<environ_set_command>
<for each nation-level row in process_order>:
    cd ${staging_fold}
    ${wget} ${website_root}/<UPPER_TABLE>/tl_${year}_us_<table_name>.zip --mirror --reject=html
    cd ${staging_fold}/<host-derived-subpath>/<UPPER_TABLE>
    <unzip_command for tl_*<table_name>.zip>
    <pre_load_process>                         # CREATE TABLE ... INHERITS(tiger.xxx)
    ${loader} -D -<c|a> -s 4269 -g the_geom [-S] -W "latin1" tl_${year}_us_<table_name>.dbf tiger_staging.<table_name> | ${psql}
    <post_load_process>                        # loader_load_staged_data, CREATE INDEX, VACUUM
```

This must run **once, first**, because state-level scripts depend on `tiger.county` being populated to enumerate county FIPS codes per state.

**`loader_generate_script(param_states text[], os text) → SETOF text`** ([src/tiger_loader.sql:356](../src/tiger_loader.sql#L356))

Emits the per-state script. For each state in the input array, produces two blocks:

1. **State-level block.** One `wget`+`unzip`+`shp2pgsql`+`psql`+`post_load_process` sequence per row where `level_state = true AND load = true`, in `process_order` order. Target table becomes `tiger_data.<state_abbrev>_<lookup_name>`, staging table `tiger_staging.<state_abbrev>_<table_name>`.

2. **County-level block.** For each row where `level_county = true AND load = true`:
    - A `wget --mirror` loop over *every county FIPS in that state*, enumerated by joining to `tiger.county` (which is why nation must run first).
    - A single unzip that globs `tl_*_${state_fips}*_${table_name}*.zip`.
    - The platform's `county_process_command` — which is itself a loop over `*${table_name}*.dbf` files that runs `${loader} | ${psql}` per county and then calls `loader_load_staged_data` to move each into the per-state table.
    - Post-load: the big derived-table SQL for `edges` (creates `<state>_zip_state_loc` and `<state>_zip_lookup_base`) and for `addr` (creates `<state>_zip_state`).

Typical usage from the README:
```sql
SELECT loader_generate_script(ARRAY['DC','RI'], 'sh');
```

**`loader_generate_census_script(param_states text[], os text) → SETOF text`** ([src/tiger_loader.sql:459](../src/tiger_loader.sql#L459))

A narrowed variant that only emits blocks for `bg`, `tract`, `tabblock` (census enumeration geographies). Used when you already have the geocoder data loaded and only want to add census tables later, e.g. for `get_tract()`. Also calls `create_census_base_tables()` as its first SQL statement (note: this function was dropped in the Makefile preamble; the call is legacy and currently `SELECT create_census_base_tables()` resolves to the dropped shim — a small latent bug).

### 13.4 `loader_macro_replace` and `loader_load_staged_data`

**`loader_macro_replace(input text, keys text[], values text[]) → text`** is the templating primitive. Walks `keys` and `values` in parallel and does `replace(input, '${'||keys[i]||'}', values[i])`. Every generator function double-wraps calls to this with slightly different variable sets — because some variables (`state_abbrev`, `state_fips`) aren't resolved until the state loop, and others (`psql`, `data_schema`) are resolved from `loader_platform`/`loader_variables`.

The full variable set seen in templates:

| Variable | Source | Example |
| --- | --- | --- |
| `${staging_fold}` | loader_variables | `/gisdata` |
| `${website_root}` | loader_variables | `https://www2.census.gov/geo/tiger/TIGER2025` |
| `${tiger_year}` | loader_variables (inline) | `2025` |
| `${data_schema}` | loader_variables | `tiger_data` |
| `${staging_schema}` | loader_variables | `tiger_staging` |
| `${psql}` | loader_platform | `${PSQL}` |
| `${loader}` | loader_platform | `${SHP2PGSQL}` |
| `${state_abbrev}` | state_lookup | `ma` |
| `${state_fips}` | state_lookup | `25` |
| `${state_fold}` | state_lookup | `25_Massachusetts` |
| `${lookup_name}` | loader_lookuptables | `edges` |
| `${table_name}` | loader_lookuptables | `edges` |

Note the `${...}` substitution happens *in SQL* during script generation — the emitted script has all of these already replaced with literal values. The only `${...}`-style variables surviving into the shell script are the ones that the shell itself will expand (e.g. `${TMPDIR}`, `${PSQL}`), written with `$$` in the PL/pgSQL string literal.

**`loader_load_staged_data(staging_table, target_table [, exclude_columns])` → integer** ([src/tiger_loader.sql:410](../src/tiger_loader.sql#L410)) is the per-row staging→data mover:

1. Introspect the columns of the *target* table from `information_schema.columns`, excluded by `exclude_columns` (defaulted from the lookup table's `columns_exclude` if omitted — the 2-arg overload resolves the exclusion list by matching the target table name against `loader_lookuptables.lookup_name` with `LIKE`).
2. Introspect the columns of the *staging* table likewise.
3. Build and execute `INSERT INTO data_schema.target (col1,col2,...) SELECT col1,col2,... FROM staging_schema.staging` — both column lists are built from `information_schema` in alphabetical order, so they will line up iff the column *names* match. TIGER column names are stable year-to-year so this works in practice, but it is a latent footgun.
4. `DropGeometryTable()` the staging table.
5. Return row count.

The implicit default `columns_exclude` list in the 2-arg overload is ~50 columns long, covering every generational legacy column the geocoder doesn't care about (`statefp00`, `statefp10`, `uace00`, etc.).

### 13.5 The lookup dictionaries are separate

None of the above loads the small lookup dictionaries that `normalize_address()` consults (`direction_lookup`, `street_type_lookup`, `secondary_unit_lookup`, `state_lookup`). Those are static seed data, **created and populated at `CREATE EXTENSION` time** by [src/tables/lookup_tables_2011.sql](../src/tables/lookup_tables_2011.sql) running ~1300 lines of `INSERT INTO` statements.

Three related tables, `place_lookup`, `county_lookup`, `countysub_lookup`, are created empty and never populated by the current codebase — their `INSERT` statements in `lookup_tables_2011.sql` are commented out (`/** INSERT INTO place_lookup SELECT ... pl99_d00 ... **/`). They reference long-deleted `pl99_d00`/`co99_d00` tables from Census 2000 Shapefile pilots. At runtime the geocoder's location extractor instead joins directly against the loaded `tiger.place` and `tiger.cousub` tables — so these `_lookup` tables are dead weight. For the DuckDB port we can drop them entirely.

The `zip_lookup_base` table is a hybrid case: declared empty by the lookup-tables script, but populated **per-state** by the `edges` row's `post_load_process` via an `INSERT ... SELECT DISTINCT e.zipl, ..., p.name, ..., c.name FROM ${state}_edges e JOIN tiger.county c ... JOIN ${state}_faces f ... JOIN ${state}_place p ...`. So "load tiger data for MA" implicitly builds "tiger_data.ma_zip_lookup_base" as a child of `tiger.zip_lookup_base`.

### 13.6 Inheritance and constraint exclusion

Every per-state table in `tiger_data` is created with `INHERITS(tiger.<parent>)` plus `CHECK (statefp = '<FIPS>')`. For example:

```
CREATE TABLE tiger_data.ma_edges (
    CONSTRAINT pk_ma_edges PRIMARY KEY (gid),
    CONSTRAINT chk_statefp CHECK (statefp = '25')
) INHERITS (tiger.edges);
```

This achieves two things:

1. **Transparent fan-out.** Queries like `SELECT ... FROM tiger.edges WHERE ...` read from every inheriting child. The geocoder never hardcodes state-specific table names — it always queries the parent.
2. **Constraint exclusion pruning.** When the query contains `WHERE statefp = '25'` (which the geocoder adds whenever it knows the state from the input ZIP/state), the planner sees the CHECK constraint on `tiger_data.ma_edges` and physically excludes every other state's child from the plan. This is why the geocoder's `WHERE statefp = $in_statefp` is the dominant selectivity win — without it, every query scans nationwide edge data.

`tiger_data.state_all`, `tiger_data.county_all`, `tiger_data.zcta5_all` are nation-wide children (no `statefp` CHECK); the geocoder scans them uniformly.

The `create_census_base_tables()` function referenced in the Makefile and in `loader_generate_census_script` is the original installer for the census subset (`tract`, `bg`, `tabblock20`). It's been dropped-and-recreated inline in `src/tiger_loader.sql` and `src/tables/census_tables.sql`, and those files' table definitions are what live in the `tiger` schema.

### 13.7 Post-load housekeeping functions

Run **once**, by the user, from a psql session after scripts finish:

- **`install_missing_indexes() → boolean`** ([src/geocode/other_helper_functions.sql:219](../src/geocode/other_helper_functions.sql#L219)) — executes the SQL returned by `missing_indexes_generate_script()`. This is the safety net: if any expected index didn't get created (e.g. a partial load, a user-added state), this function creates it.
- **`missing_indexes_generate_script() → text`** ([src/geocode/other_helper_functions.sql:77](../src/geocode/other_helper_functions.sql#L77)) — introspects `information_schema.columns` and `pg_catalog.pg_indexes` to find missing indexes on both `tiger.*` and `tiger_data.*`. It generates DDL for:
  - `UNIQUE INDEX (tfid)` on every `*faces` table.
  - `btree` indexes on `countyfp, tlid, tfidl, tfidr, tfid, zip, placefp, cousubfp`.
  - `gist` spatial indexes on `the_geom` / `geom`.
  - `btree(soundex(col))` and `btree(lower(col))` on `name`, `place`, `city` of `*county*`, `*featnames*`, `*place*`, `*zip*`, `*cousub*`.
  - `btree(least_hn(fromhn, tohn))` on every `*addr*` table (accelerates range checks).
  - `btree(lower(col) varchar_pattern_ops)` for LIKE-prefix lookups.
  - `btree(zipl)`, `btree(zipr)` on every `*edges*` table.

These are *exactly* the indexes the geocoder queries depend on — the `ORDER BY soundex(name)` in `location_extract`, the `lower(name) = lower(input)` joins in `geocode_address`, the `least_hn(fromhn, tohn)` comparisons. Missing any of them pushes a given query from index scan to seq scan on ~150M rows.

- **`drop_state_tables_generate_script(state text, schema text = 'tiger_data') → text`** ([src/tiger_loader.sql:66](../src/tiger_loader.sql#L66)) — emits `DROP TABLE tiger_data.<state>_*;` for every table prefixed with the state abbrev. Used to reload a single state.
- **`drop_nation_tables_generate_script(schema text = 'tiger_data') → text`** ([src/tiger_loader.sql:79](../src/tiger_loader.sql#L79)) — emits DROPs for the nation-level children (`state_all`, `county_all`, `zcta5_all`, plus any stray two-letter-prefixed `county`/`state` tables).
- **`drop_dupe_featnames_generate_script() → text`** ([src/geocode/other_helper_functions.sql:230](../src/geocode/other_helper_functions.sql#L230)) — emits a per-table `CREATE TEMPORARY TABLE dup AS ... DELETE ... DROP` block that deduplicates `(tlid, lower(fullname))` rows in `*featnames` tables, then creates a tlid btree. TIGER publishes the same feature name multiple times per edge occasionally; this cleans it up.

### 13.8 End-to-end user workflow (canonical)

The README's six-step procedure, translated into what's actually happening:

1. Create `${staging_fold}` and `${staging_fold}/temp/` on disk.
2. Edit `loader_platform.declare_sect` to match your PG binary paths and creds.
3. `psql -c "SELECT loader_generate_nation_script('sh')"` → run the script → loads `state_all`, `county_all`, and (if enabled) `zcta5_all`.
4. `psql -c "SELECT loader_generate_script(ARRAY['DC','RI'], 'sh')"` → run the script → loads per-state `place`, `cousub`, `tract`, `tabblock20`, `bg` (if enabled), `faces`, `featnames`, `edges`, `addr`. Derives per-state `zip_state`, `zip_state_loc`, `zip_lookup_base`.
5. `psql -c "SELECT install_missing_indexes()"` → belt-and-suspenders indexing.
6. Sanity-check with a `SELECT * FROM geocode('...')`.

Step 3 must precede step 4. Step 4 can be run incrementally per state.

### 13.9 Runtime behavior that depends on the loader

Several quirks of the geocoder only make sense in light of the loader's layout:

- `WHERE statefp = $1` everywhere — to get the constraint-exclusion prune.
- `parsed.zip` → `zip_lookup_base` → `statefp` fallback in `geocode_address` — because ZIPs cross state boundaries but the geocoder needs to pick *one* state's child table to scan efficiently.
- `restrict_geom` → intersected with `zcta5` at 4269 — because ZCTA polygons are the coarsest, smallest, pre-indexed geometries available for a rough cut.
- The per-state `zip_state`, `zip_state_loc`, `zip_lookup_base` tables exist at all — they're the "static lookup" alternative to actually joining `edges ⨝ faces ⨝ place`, and since they're derived at load-time, query-time is cheap. The DuckDB port should keep these as materialized tables for the same reason.
- `lower(name)`, `soundex(name)`, `levenshtein_ignore_case(name, ...)` — matched by the exact indexes the loader creates. Changing the case handling in the geocoder would force an index rebuild.

### 13.10 What the DuckDB port loader should look like

Don't replicate the shell-script-generator pattern. Replace it with a thin C++ shim (per D11) that drives a per-state × per-table loop, with all the SQL expressed as macros or inline statements. API sketch:

```sql
CALL load_tiger_nation(year := 2025, temp_dir := '/tmp/duckdb_tiger');   -- one-off; loads state_all, county_all, zcta5_all
CALL load_tiger_states(states := ['DC','RI'], year := 2025,             -- per-state, idempotent
                       temp_dir := '/tmp/duckdb_tiger');
```

Per the D1 recipe, the C++ driver, per (state, table):

1. Build the TIGER URL from `year` + `state_fips` + `table_name` conventions (§13.2), with a per-year config struct allowing year-specific URL tweaks (per D12).
2. `HTTP GET` the zip into `temp_dir` via `httpfs` / libcurl.
3. `INSERT INTO <target_table> (<non-excluded columns>) SELECT <same cols> FROM ST_Read('/vsizip/<temp_dir>/tl_2025_25_edges.zip/tl_2025_25_edges.shp')` — the column-exclusion list from PG's `loader_lookuptables.columns_exclude` is preserved as a static per-table constant in the shim.
4. `std::filesystem::remove` the zip.
5. After the state's base tables finish, run the derived-table SQL from §13 (per-state `zip_state`, `zip_state_loc`, `zip_lookup_base` build queries, translated essentially unchanged from the PG `post_load_process` snippets).
6. For `zcta5` clipping: DuckDB spatial has `ST_Intersection` / `ST_Covers` / `ST_SimplifyPreserveTopology`; port the PG SQL verbatim.

Locked port decisions (cross-reference D1–D13 above):

- **Partitioning.** Single table per entity. No explicit sort needed: each state loads in its own INSERT, so row groups are naturally contiguous per `statefp` and zonemaps prune on `WHERE statefp = '25'` without intervention. (D2)
- **Indexing.** No user-managed indexes for `soundex(name)` / `lower(name)` — instead precompute `name_lower`, `name_soundex` columns at load time and `ORDER BY name_lower` (or cluster by it) so zonemaps prune effectively. DuckDB spatial R-tree index on `the_geom` for every table with a geometry column. ART indexes on `tlid`, `statefp`, `tfid` where equality-joined.
- **Keep:**
  - Column-exclusion lists. TIGER really does have 40+ legacy columns on `faces`. Drop them at load time, not every query.
  - Post-load derivation SQL for `zip_state`, `zip_state_loc`, `zip_lookup_base`. These are the fastest path to the location-only fallback (§8) and the ZIP-window expansion in `geocode_address` (§7.1).
  - The `zcta5` state-clipping step. Without it, `geocode_location` can't do the per-state `ST_Intersects(zcta5.the_geom, ...)` prune cheaply.
- **Drop:**
  - `tract`, `bg`, `tabblock20`, `addrfeat` tables. Not consulted by the geocoder. (D8)
  - The `loader_platform` / `loader_variables` / `loader_lookuptables` control tables. Their content becomes C++ constants in the shim. (D11)
  - The shell-script generators (`loader_generate_*`). The extension loads data in-process.
  - `loader_load_staged_data` — there's no staging schema; `ST_Read` streams directly into final tables.
  - `loader_macro_replace` — DuckDB has parameterized SQL.
  - `drop_*_tables_generate_script` — implement as `DELETE FROM tiger.edges WHERE statefp = ?` etc. inside an `unload_tiger_state(state)` table function.
  - `install_missing_indexes` / `missing_indexes_generate_script` — the loader creates expected indexes itself; no safety net needed.
  - `create_census_base_tables` — legacy; the current tables are declared inline (and we're not loading census tables anyway per D8).
  - The `CHECK (statefp=...)` constraints — DuckDB can't use them for planning and we're single-table, so they're documentation-only noise.
  - Per-OS templating. Everything in-process; no shell to escape. (D11 cross-platform note)

- **Preserve:**
  - TIGER filename/URL conventions (§13.2). These are external to us.
  - Column names — downstream users of `tiger.edges`, `tiger.faces`, `tiger.addr`, `tiger.featnames` expect the same columns. Deviating makes ported Postgres queries break silently.
  - SRID 4269 on all geometry in storage. Transform at query time, not load time.
  - The `(zip, stusps, statefp)` uniqueness of `zip_state`, the `(zip, stusps, place)` uniqueness of `zip_state_loc`, the `(zip, state, county, city, statefp)` uniqueness of `zip_lookup_base`. The geocoder's `DISTINCT ON` relies on these.

## 14. What the DuckDB port actually needs to do

Based on the flow above, here is the minimum semantic scaffolding a DuckDB port must provide. Everything else is optimization.

### 14.0 Locked design decisions

Before the scaffolding, the committed choices from design discussion:

| # | decision |
| --- | --- |
| D1 | **Data distribution.** Three first-class source modes, selected by the `source` parameter: **(i) Census HTTP** (default) — download-on-demand via `httpfs`, read with GDAL `/vsizip/` so shapefiles are never fully extracted. Loader takes a `temp_dir` argument (default `std::filesystem::temp_directory_path() / "duckdb_tiger_<pid>"`); each zip is downloaded to `temp_dir`, read via `ST_Read('/vsizip/…')`, and deleted as soon as its INSERT completes. No `shellfs`. **(ii) Local folder** — `source := '/path/to/tiger/'` reads from pre-downloaded zips (or extracted `.shp` files) on local disk, skipping HTTP. Required for air-gapped / secure environments. **(iii) Portable DuckDB file** — TIGER data is loaded into an ATTACH-able DuckDB file in a networked environment, then the file is transferred to and attached READ_ONLY in the secure environment. See §14.2 for the full API, layout rules, and schema contract. |
| D2 | **Partitioning.** One DuckDB table per entity (`tiger.edges`, `tiger.addr`, etc.). Each state loads in its own INSERT, so row groups end up naturally contiguous per `statefp` — zonemap pruning on `WHERE statefp = '25'` handles per-state selectivity with no explicit sort or partitioning. No Hive layout, no UNION views. |
| D3 | **Public API.** `geocode()` has a `VARCHAR` overload that calls `from_pagc(raw_text)` internally — users who pass a raw string get a sensible default. Explicit-struct callers use `geocode(from_pagc(raw_text))` or `geocode(my_adapter(...))`. |
| D4 | **Standardizer dependency.** Soft. `from_pagc` calls `standardize_address()` / `parse_address()` at runtime; emit a clear "install duckdb-address-standardizer" error if the functions aren't resolvable. DuckDB community extensions don't have a cross-extension dep declaration in the manifest. |
| D5 | **`restrict_geom`.** Ported as-is; accepts any DuckDB spatial `GEOMETRY`. |
| D6 | **`reverse_geocode` output.** DuckDB-idiomatic: one row per candidate edge with a `rank` column, not PG's parallel-array record. |
| D7 | **Stage A → Stage B short-circuits.** Relaxed for vectorization — compute all candidates in one vectorized pass, LIMIT at the end. Documented deviation: which marginal candidates get emitted may differ from PG at tie boundaries. (Wishlist: revisit if users report parity issues.) |
| D8 | **Optional TIGER tables.** Drop `tract`, `bg`, `tabblock20`, `addrfeat` entirely. Not used by the geocoder. If a `get_tract()`-equivalent is needed later, add it as a separate optional load. |
| D9 | **Configuration knobs** (`zip_penalty`, `reverse_geocode_numbered_roads`). Function parameters with sensible defaults. No settings table, no session variables. |
| D10 | **SRID policy.** Loader data: always 4269, no conversion. User-input geometry (`restrict_geom`, the point to `reverse_geocode`): auto-transform to 4269 at the boundary if SRID is set and different. SRID 0 is treated as "assume 4269" (matches PG). Do not silently misinterpret 4326 as 4269 — that's a 1–3m wrong-answer bug on short blocks. |
| D11 | **Extension language.** Thin C++ shim (~100 LOC) for extension registration, HTTP orchestration, and the state-×-table-×-county loop (which needs iteration that DuckDB SQL doesn't offer). 95% of logic stays in SQL macros — `from_pagc`, `geocode`, `rate_attributes`, `interpolate_from_address`, per-table column projections, and all derived-table build queries. |
| D12 | **Year handling.** `2025` default; `year` is a runtime parameter override. Per-year URL patterns and per-year schema tweaks live in a small internal config structure so year-to-year TIGER drift can be handled without a new extension release. |
| D13 | **Parity testing.** v0.1 ships with (a) a hand-curated ~50-address corpus covering stress classes (numbered highways, `prequalabr` like `Old`, short street names, ZIP typos, cross-state ZIPs, unit suffixes, `I-635`/`I- 635` highway spacing), plus (b) ports of PG's regression tests from [src/regress/](../src/regress/): `pagc_normalize_address_regress`, `geocode_regress`, `reverse_geocode_regress`, and `test-geocode_intersection_spacing`. We skip `normalize_address_regress` — that parser isn't ported. |
| D14 | **Census block/tract containment guarantees.** Precompute per-edge-per-side whether the interpolated offset point is guaranteed to land inside the adjacent face (implying block, tract, block group, county, state). Exposed at geocode time via `block_geoid`/`tract_geoid`/`blkgrp_geoid` output columns (always populated) plus a `containment_guaranteed` boolean, and a `require_containment` filter parameter. Guarantee holds at the default offset only. See §14.3 for the full spec and known limitations. |

**Cross-platform note.** Unlike PG, which spends ~350 lines on `loader_platform` with per-OS paths for `wget` / `unzip` / `7z.exe` / `shp2pgsql` / `psql`, our loader does everything in-process via libraries that are already cross-platform: `httpfs` uses libcurl, `/vsizip/` is in GDAL, `ST_Read` is in DuckDB spatial, temp-path resolution uses C++17 `std::filesystem`. No shell escaping, no OS-specific unzip invocation, no `PATH` hunting. Builds and runs identically on Windows, macOS, and Linux through DuckDB's standard cross-compiled extension CI.

### 14.1 Scaffolding (informed by the decisions above)


**Data ingestion.** See §13 for the full treatment — the Postgres loader is a metadata-driven shell-script generator that we should *not* replicate. Instead, provide one or two DuckDB table functions that read TIGER shapefiles directly and write permanent tables. At minimum the loader must materialize these tables, all with geometry in EPSG:4269:

- `state(statefp, stusps, geom)`
- `county(statefp, countyfp, geom)`
- `place(statefp, placefp, plcidfp, name, geom)`
- `cousub(statefp, countyfp, cousubfp, cosbidfp, name, geom)`
- `zcta5(statefp, zcta5ce, geom)`
- `zip_state(zip, stusps, statefp)`
- `zip_state_loc(zip, statefp, place)`
- `zip_lookup_base(zip, state, county, city, statefp)`
- `edges(statefp, tlid, tfidl, tfidr, tnidf, tnidt, mtfcc, fullname, geom, zipl, zipr)`
- `faces(statefp, tfid, countyfp, placefp, cousubfp, geom)`
- `featnames(statefp, tlid, name, fullname, predirabrv, pretypabrv, prequalabr, suftypabrv, sufdirabrv, mtfcc)`
- `addr(statefp, tlid, fromhn, tohn, side, zip, plus4)`

TIGER distributes this as per-state shapefiles; the single-table decision is locked (D2). Per-state partitioning is what makes the PostGIS version usably fast. In DuckDB we get the same effect for free: each state is loaded in its own INSERT, so row groups are naturally contiguous per `statefp` and zonemap pruning handles the per-state selectivity with no explicit sort. See §13.10 for the full loader-design discussion.

### 14.2 Loading modes and the portable-database pattern

Three real-world scenarios drive the loader's API surface. The same C++ shim handles all of them — they're just argument permutations, not separate code paths.

#### Mode 1: Census HTTP (default, networked envs)

```sql
CALL load_tiger_nation(year := 2025);                              -- once
CALL load_tiger_states(states := ['MA','RI'], year := 2025);       -- per-state, idempotent
```

Downloads from `https://www2.census.gov/geo/tiger/TIGER<year>/...` to `temp_dir`, reads via `/vsizip/`, deletes each zip as its INSERT completes. Happy path for dev and networked prod.

#### Mode 2: Local folder (offline, air-gapped, secure envs)

```sql
CALL load_tiger_nation(year := 2025, source := '/mnt/tiger_2025/');
CALL load_tiger_states(['MA','RI'], year := 2025, source := '/mnt/tiger_2025/');
```

When `source` is a filesystem path instead of a URL base, the shim skips `httpfs` and reads directly. Three accepted layouts, detected by probing:

1. **Flat zips** — all `tl_<year>_<fips>_<table>.zip` files in one directory (e.g. what you'd get from downloading by hand or with a simple script). Easiest to hand-curate.
2. **Census-style nested** — `<source>/EDGES/tl_2025_25_edges.zip`, `<source>/FACES/tl_2025_25_faces.zip`, etc. — mirrors Census's FTP layout. Convenient if the user ran `wget --mirror` against the Census site and shipped the whole tree.
3. **Pre-extracted shapefiles** — `tl_2025_25_edges.shp` plus its sidecars (`.shx`, `.dbf`, `.prj`, …) on disk. Loader skips `/vsizip/` and passes the `.shp` path directly to `ST_Read`.

Users with an even weirder layout can pass an explicit file-list override (path per `(state, table)` tuple) — edge case, not a primary API.

`temp_dir` is unused in Mode 2 (nothing to extract into).

#### Mode 3: Portable DuckDB file (build once, attach anywhere)

The canonical pattern for secure environments where neither HTTPS egress nor filesystem copies of raw TIGER shapefiles are allowed, but an opaque data file is. Step 1, in a networked staging environment, build the reference DB:

```sql
-- Staging env (has internet):
ATTACH 'tiger_us_2025.duckdb' AS tiger_ref;
CALL load_tiger_nation(year := 2025, target_db := 'tiger_ref');
CALL load_tiger_states(states := all_us_states(), year := 2025, target_db := 'tiger_ref');
DETACH tiger_ref;
-- Ship tiger_us_2025.duckdb to the secure environment via approved transport.
```

Step 2, in the secure env, attach the reference DB next to whatever DB contains the addresses to geocode:

```sql
-- Secure env (no internet, no shapefile copies allowed):
ATTACH 'my_data.duckdb' AS my_data;                        -- addresses to geocode (read-write)
ATTACH 'tiger_us_2025.duckdb' AS tiger_ref (READ_ONLY);    -- reference data, immutable

SELECT g.*
FROM my_data.addresses a
CROSS JOIN LATERAL geocode(
    from_pagc(a.raw),
    reference_db := 'tiger_ref'      -- tells geocoder which catalog to read from
) AS g;
```

Why this is the right workflow for secure envs:
- Single-file artifact, easy to transfer, hash, version, sign.
- `READ_ONLY` attach makes the reference data physically immutable — matches the trust model.
- No re-loading, no `httpfs`, no `/vsizip/` in the secure env. The secure env only runs DuckDB.
- The same `tiger_us_2025.duckdb` serves many users in the secure env without re-load.

#### Target and reference configuration

Both the loader and the geocoder accept a `(database, schema)` pair that tells them where TIGER tables live:

| function | parameter | default | meaning |
| --- | --- | --- | --- |
| `load_tiger_*` | `target_db` | `main` (current DB) | which attached catalog to write to |
| `load_tiger_*` | `target_schema` | `tiger` | schema name inside that catalog |
| `geocode`, `reverse_geocode`, `geocode_intersection` | `reference_db` | `main` | which attached catalog to read from |
| `reference_schema` | `tiger` | | schema name inside that catalog |

Since passing these to every call gets noisy, there's a session-level helper that sets both at once:

```sql
CALL set_tiger_reference(database := 'tiger_ref', schema := 'tiger');
-- now geocode() reads from tiger_ref.tiger.* without explicit args
```

This is a thin wrapper around DuckDB session variables — effectively `SET tiger_db = 'tiger_ref'; SET tiger_schema = 'tiger'`. The geocoder reads these at call time.

#### Minimum schema contract

Whatever path users take to populate the reference data — our loader, a third-party distribution, a hand-curated subset, an ETL from some other source — `geocode()` expects these tables to exist in `<reference_db>.<reference_schema>` with the shapes described in §2. This is the contract that the ATTACH-portability pattern rests on, and it's the spec anyone wanting to build a reference DB from scratch needs.

**TIGER-derived tables** (populated by the loader from Census data, or by the user's equivalent):

| table | minimum columns used by the geocoder |
| --- | --- |
| `state` | `statefp`, `stusps`, `the_geom` |
| `county` | `statefp`, `countyfp`, `cntyidfp`, `name`, `the_geom` |
| `place` | `statefp`, `placefp`, `plcidfp`, `name`, `the_geom` |
| `cousub` | `statefp`, `countyfp`, `cousubfp`, `cosbidfp`, `name`, `the_geom` |
| `zcta5` | `statefp`, `zcta5ce`, `the_geom` |
| `zip_state` | `zip`, `stusps`, `statefp` |
| `zip_state_loc` | `zip`, `stusps`, `statefp`, `place` |
| `zip_lookup_base` | `zip`, `state`, `county`, `city`, `statefp` |
| `edges` | `statefp`, `tlid`, `tfidl`, `tfidr`, `tnidf`, `tnidt`, `mtfcc`, `fullname`, `the_geom`, `zipl`, `zipr`, `countyfp` |
| `faces` | `statefp`, `tfid`, `countyfp`, `placefp`, `cousubfp`, `the_geom` |
| `featnames` | `statefp`, `tlid`, `name`, `fullname`, `predirabrv`, `pretypabrv`, `prequalabr`, `suftypabrv`, `sufdirabrv`, `mtfcc` |
| `addr` | `statefp`, `tlid`, `fromhn`, `tohn`, `side`, `zip`, `plus4` |

Geometry columns are `GEOMETRY` in SRID 4269 (NAD83).

**Static lookup dictionaries** (created and seeded by the extension at install time, *not* from TIGER data):

| table | columns | source |
| --- | --- | --- |
| `direction_lookup` | `name`, `abbrev` | hardcoded; compass directions and abbreviations |
| `street_type_lookup` | `name`, `abbrev`, `is_hw` | hardcoded; USPS street type abbreviations |
| `secondary_unit_lookup` | `name`, `abbrev` | hardcoded; APT/STE/FL/etc. |
| `state_lookup` | `st_code`, `name`, `abbrev`, `statefp` | hardcoded; US states + DC + territories |

These live in the extension itself, not in TIGER. In the portable-DB pattern they're automatically created in whichever database loads the extension. The loader uses them for canonicalization (`canon_street_type`, `canon_dir`, `canon_state` in §14); the geocoder reads them at query time. For attached read-only reference DBs, the lookup tables just need to exist in the attached DB — the loader copies them alongside the TIGER data.

A user who wants to build a reference DB from *non-TIGER* sources (e.g., cleaned-up data from a state DOT, or a synthetic test fixture) just needs to produce tables matching the columns above with plausible values. The geocoder doesn't care how they got there.

### 14.3 Census block/tract containment guarantees

A TIGER-based geocoder interpolates a point along a street centerline and offsets it perpendicular to one side. That point may or may not fall in the same census block the Census geocoder (which uses rooftop parcel data) would assign — small blocks, narrow streets, block corners, and certain address-range topologies can all produce cases where the interpolated point leaks into a neighboring block.

For many downstream use cases (demographic linkage, policy analysis, redistricting, benefits eligibility) the block/tract assignment matters more than the exact point. This extension precomputes, per edge per side, whether the interpolation is *guaranteed* to land in the adjacent face — which implies guarantees at block, tract, block group, county, and state levels simultaneously, since each is a superset of the next.

#### Load-time precomputation

After per-state `edges`/`addr`/`faces` are populated, build one derived table per state:

```sql
CREATE TABLE tiger.edge_containment AS
SELECT
    e.statefp,
    e.tlid,
    -- Is the one-sided offset strip entirely within the adjacent face?
    -- If yes, any interpolated point at the default offset is guaranteed to land
    -- inside that face's block (and therefore its tract, blkgrp, county, state).
    ST_Within(
        ST_Buffer(e.the_geom, <default_offset_m>, 'side=left'),
        f_l.the_geom
    ) AS guaranteed_l,
    ST_Within(
        ST_Buffer(e.the_geom, <default_offset_m>, 'side=right'),
        f_r.the_geom
    ) AS guaranteed_r,
    -- GEOIDs from the adjacent face, *always* populated regardless of guarantee.
    f_l.statefp || f_l.countyfp || f_l.tractce20 || f_l.blockce20   AS block_geoid_l,
    f_l.statefp || f_l.countyfp || f_l.tractce20                     AS tract_geoid_l,
    f_l.statefp || f_l.countyfp || f_l.tractce20 || f_l.blkgrpce20   AS blkgrp_geoid_l,
    f_r.statefp || f_r.countyfp || f_r.tractce20 || f_r.blockce20   AS block_geoid_r,
    f_r.statefp || f_r.countyfp || f_r.tractce20                     AS tract_geoid_r,
    f_r.statefp || f_r.countyfp || f_r.tractce20 || f_r.blkgrpce20   AS blkgrp_geoid_r
FROM tiger.edges e
LEFT JOIN tiger.faces f_l ON e.tfidl = f_l.tfid AND e.statefp = f_l.statefp
LEFT JOIN tiger.faces f_r ON e.tfidr = f_r.tfid AND e.statefp = f_r.statefp;

-- Indexed on (statefp, tlid) for O(1) join at geocode time.
```

If DuckDB spatial's `ST_Buffer` doesn't support `side=left`/`side=right`, fallbacks in order of preference:

1. `ST_OffsetCurve(line, ±offset_m)` + polygonize between the offset curve and the centerline to form the strip.
2. Sample N points along the centerline, offset each perpendicular, take the convex hull over {offset points, edge endpoints}. Conservative and cheap.

Benchmark on one dense state (CA or NY) before productionizing — containment computation is the single heaviest step of the loader.

#### Geocoder output additions

The `geocode()` result rows carry four new columns per the signature in §14.1:

| column | type | meaning |
| --- | --- | --- |
| `block_geoid` | VARCHAR(15) | 2020 census block GEOID (`statefp`‖`countyfp`‖`tractce20`‖`blockce20`) of the chosen side |
| `tract_geoid` | VARCHAR(11) | census tract GEOID of the chosen side |
| `blkgrp_geoid` | VARCHAR(12) | block group GEOID of the chosen side |
| `containment_guaranteed` | BOOLEAN | true if the interpolated point is provably inside `block_geoid` at the default offset |

"Chosen side" is the `addr.side` (`'L'` or `'R'`) of the winning candidate. The three `*_geoid` columns are *always* populated from the adjacent face — even when the guarantee doesn't hold, these are the geocoder's best-guess GEOIDs, and they're correct for the majority of addresses. `containment_guaranteed` tells you whether to trust them as hard truth.

#### `require_containment` parameter

New parameter on `geocode()` (also on `geocode_intersection` and `reverse_geocode` where applicable):

| value | behavior |
| --- | --- |
| `'none'` (default) | Return all candidates; `containment_guaranteed` is informational. |
| `'block'` | Filter to candidates where `containment_guaranteed = true`. |
| `'tract'` | Filter to candidates where the offset strip is provably within the tract (looser; higher hit rate). |
| `'blkgrp'` | Same at block group level. |

For v0.1, `'tract'` and `'blkgrp'` resolve to the same face-level check as `'block'` — the conservative sufficient condition. Looser-but-still-sound checks at those levels require the materialized-dissolved-polygons work under (L2) below.

#### Known limitations (document, defer the fixes)

All three of these are **conservative**: they produce *false negatives* (refusing to certify cases that would actually be fine), never false positives. A `containment_guaranteed = true` result is genuinely guaranteed.

- **(L1) Guarantee only applies at the default offset.** The precomputed flag is built against a single offset value (our default, ~10m). If we later expose a per-call `offset_m` parameter, the flag is invalidated for non-default offsets. Document this clearly in the function reference. **Future remedy:** precompute flags for a small set of canonical offsets (e.g. 5m / 10m / 15m) stored as separate columns; select the right one at query time based on the requested offset.
- **(L2) Conservative false negatives at intra-block face boundaries.** A census block is typically composed of *multiple* topological faces joined along internal edges (driveways, utility easements, TIGER topology artifacts). An edge whose offset strip crosses from face A into adjacent face B — where A and B share the same `blockce20` — will fail the "within a single face" check even though the block-level guarantee actually holds. v0.1 accepts the false negatives for simplicity. **Future remedy:** during load, materialize block / tract / blkgrp polygons by dissolving faces on matching codes (`ST_Union` GROUP BY `blockce20`/`tractce20`/etc.); run the `ST_Within` check against those dissolved polygons instead of the single adjacent face. Gives a strictly tighter `'tract'` and `'blkgrp'` answer, and a tighter `'block'` answer at faces that straddle multi-face blocks.
- **(L3) Buffer endpoint artifacts.** `ST_Buffer` produces rounded or squared caps at line endpoints; at street intersections these can cause the strip to leak across into neighboring faces even when the middle of the edge is comfortably inside. The result is more false negatives near intersections. **Future remedy:** trim the buffer polygon back from both endpoints by `offset_m` before the `ST_Within` test, or construct the offset strip manually via `ST_OffsetCurve` + flat endpoints.

#### Storage cost

~50M edges nationwide × ~60 bytes per row ≈ 3 GB added to a full-nation portable reference DB. A single-state load adds on the order of 10–100 MB depending on state density and urbanization. This is dominated by the TIGER data itself (~30 GB for all states).

**Input contract — the 8-field `geocode_input` struct.** Postgres couples the geocoder to its internal `norm_addy` composite because PL/pgSQL needs it to pass values between functions. That coupling is an implementation detail, not a geocoding requirement. The geocoder's SQL only ever reads 8 fields. That is the entire public contract:

```sql
CREATE TYPE geocode_input AS STRUCT(
    address       INTEGER,    -- house number
    street_name   VARCHAR,    -- 'New Hampshire' (no type, no directions embedded)
    street_type   VARCHAR,    -- TIGER Title-Case abbrev: 'Ave', 'Blvd', 'Hwy'
    pre_dir       VARCHAR,    -- uppercase 2-letter: 'N', 'NW', 'SE'
    post_dir      VARCHAR,
    location      VARCHAR,    -- city / place, free-form
    state_abbrev  VARCHAR,    -- uppercase 2-letter: 'MA', 'NY'
    zip           VARCHAR     -- 5-character string with leading zeros: '02109'
);
```

We do **not** port `normalize_address`, we do **not** expose `norm_addy`, and we do **not** ship the `use_pagc_address_parser` setting. The fields `norm_addy` carries beyond these 8 (`parsed`, `zip4`, `address_alphanumeric`, `internal`) are either never read by the geocoder or are parser-side breadcrumbs we don't need. Users produce a `geocode_input` from whatever source they prefer — PAGC, a different parser, a form submission, a row already in a warehouse table — and hand it to `geocode()`.

**Field format contract.** Each field has an expected format that the geocoder's SQL depends on. Derived from reading every usage in [src/geocode/geocode_address.sql](../src/geocode/geocode_address.sql), [src/geocode/geocode_location.sql](../src/geocode/geocode_location.sql), and [src/geocode/rate_attributes.sql](../src/geocode/rate_attributes.sql):

| field | type | expected format | why the geocoder cares | strictness |
| --- | --- | --- | --- | --- |
| `address` | INTEGER | numeric house number, e.g. `1731` (strip any trailing letter like `1731A`) | `$1 % 2` parity check, `BETWEEN least_hn(fromhn,tohn) AND greatest_hn(fromhn,tohn)` range check, feeds `interpolate_from_address($1, fromhn, tohn, line, side)` interpolation math | NULL allowed → rating penalty of **+20** (out-of-range fallback) |
| `street_name` | VARCHAR | free-form, any case, any punctuation; **must not contain** the street type or direction words (they live in their own fields) | `lower(f.name) = lower($2)`, `soundex(f.name) = soundex($2)`, `levenshtein_ignore_case(f.name, $2)` for rating | NULL → geocoder's Stage A returns empty immediately |
| `street_type` | VARCHAR | **TIGER Title-Case abbreviation** (`Ave`, `Blvd`, `Hwy`, `Ct`, `St`, `Rd`, etc.) | `lower($4) = lower(f.suftypabrv) OR lower($4) = lower(f.pretypabrv)` match check; rating adds `5 × levenshtein_ignore_case(input_type, tiger_type)` | **soft** — any case OK (compared lowercased); unabbreviated (`Avenue` vs `Ave`) costs ~+15 rating |
| `pre_dir` | VARCHAR | **uppercase 2-letter** (`N`, `NW`, `SE`) | rating adds `2 × levenshtein_ignore_case(input_dir, tiger_dir)` | **soft** — any case OK; unabbreviated (`Northwest` vs `NW`) costs ~+14 rating |
| `post_dir` | VARCHAR | same as `pre_dir` | same | **soft** |
| `location` | VARCHAR | free-form city name, any case | `lower(city) LIKE lower($3) \|\| '%'`, `levenshtein_ignore_case`, `soundex` | NULL adds 5 to rating but is allowed |
| `state_abbrev` | VARCHAR | **UPPERCASE 2-letter** (`MA`, `NY`, `DC`) | `state_lookup.abbrev = parsed.stateAbbrev` **exact-equality** compare; `state_lookup.abbrev` is uppercase | **STRICT** — `Ma` or `Massachusetts` fails the state match, falls back to ZIP-based statefp lookup, loses the per-state spatial prune |
| `zip` | VARCHAR | **5-character string with leading zeros preserved** (`'02109'`) | `addr.zip = ANY(...)` **exact-equality** compare against TIGER's `addr.zip varchar(5)`; `diff_zip` numeric compare on first 5 digits | **STRICT** — INT or stripped `'2109'` silently misses every MA/NJ/CT/RI/VT/NH/MA ZIP |

Three fields are strict (`address` → INTEGER, `state_abbrev` → uppercase 2-letter, `zip` → 5-char with leading zeros). Three fields are soft-penalty-only (`street_type`, `pre_dir`, `post_dir` — use TIGER's abbreviation vocabulary for best rating). Two are free-form (`street_name`, `location`). That is the entire contract.

The soft penalties are cumulative. An input like `{street_type: 'AVENUE', pre_dir: 'NORTHWEST'}` against TIGER's `{'Ave', 'NW'}` eats roughly +29 rating — the difference between a top match and an also-ran.

**Canonicalization helpers.** The adapter is responsible for coercing arbitrary parser output into the expected formats. Four macros (built against the existing `street_type_lookup`, `direction_lookup`, `state_lookup` tables) do the work:

```sql
CREATE MACRO canon_street_type(t) AS (
    SELECT COALESCE(abbrev, t) FROM street_type_lookup
    WHERE upper(name) = upper(t)                    -- 'AVENUE' → 'Ave', 'Ave' → 'Ave', 'AVE' → 'Ave'
    LIMIT 1
);

CREATE MACRO canon_dir(d) AS (
    SELECT COALESCE(abbrev, upper(d)) FROM direction_lookup
    WHERE upper(name) = upper(d)                    -- 'Northwest' → 'NW', 'nw' → 'NW'
    LIMIT 1
);

CREATE MACRO canon_state(s) AS (
    -- Accept either abbrev or full name; emit uppercase abbrev.
    SELECT abbrev FROM state_lookup
    WHERE upper(s) IN (upper(abbrev), upper(name))  -- 'MA' or 'Massachusetts' or 'massachusetts'
    LIMIT 1
);

CREATE MACRO canon_zip(z) AS (
    CASE
        WHEN z IS NULL THEN NULL
        WHEN length(z) >= 5 AND left(z, 5) ~ '^\d{5}$' THEN left(z, 5)
        WHEN z ~ '^\d{1,4}$' THEN lpad(z, 5, '0')   -- recover stripped leading zeros
        ELSE NULL
    END
);
```

**The one official adapter: `from_pagc(raw_text)`.** PostGIS's install docs recommend PAGC over the regex parser (*"The bundled address_standardizer extension provides superior address normalization…"*) and the recent repo activity is overwhelmingly on the PAGC side. We do the same. The adapter mirrors the exact repack rules in [src/pagc_normalize/pagc_normalize_address.sql](../src/pagc_normalize/pagc_normalize_address.sql) — including the `parse_address` fallback for missing `city`/`state`/`zip` and the unit regex:

```sql
CREATE MACRO from_pagc(raw_text) AS (
    SELECT STRUCT_PACK(
        address      := TRY_CAST(regexp_extract(sa.r.house_num, '\d+') AS INTEGER),
        street_name  := NULLIF(trim(sa.r.name), ''),
        street_type  := canon_street_type(COALESCE(sa.r.suftype, sa.r.pretype)),
        pre_dir      := canon_dir(sa.r.predir),
        post_dir     := canon_dir(sa.r.sufdir),
        location     := COALESCE(NULLIF(trim(sa.r.city), ''),
                                 NULLIF(trim(pa.r.city), '')),
        state_abbrev := canon_state(COALESCE(NULLIF(trim(sa.r.state), ''),
                                             NULLIF(trim(pa.r.state), ''))),
        zip          := canon_zip(COALESCE(
                           NULLIF(split_part(COALESCE(sa.r.postcode, ''), '-', 1), ''),
                           NULLIF(pa.r.zip, '')))
    )::geocode_input
    FROM (SELECT standardize_address(raw_text) AS r) sa,
         (SELECT parse_address(raw_text) AS r) pa
);
```

The COALESCE cascade for `location`, `state_abbrev`, and `zip` is **load-bearing for PostGIS parity** — `standardize_address` sometimes leaves those empty on addresses where `parse_address`'s blunter regex picks them up. Omitting the cascade is the single biggest silent divergence vs. `pagc_normalize_address`.

Usage:

```sql
-- Point-and-shoot
SELECT * FROM geocode(from_pagc('1731 New Hampshire Ave NW, Washington DC 20010'));

-- Batch
SELECT g.*
FROM addresses a
CROSS JOIN LATERAL geocode(from_pagc(a.raw)) g;

-- Already-parsed input (from a form, warehouse table, spreadsheet)
SELECT * FROM geocode(STRUCT_PACK(
    address := 1731,
    street_name := 'New Hampshire',
    street_type := 'Ave',
    pre_dir := NULL,
    post_dir := 'NW',
    location := 'Washington',
    state_abbrev := 'DC',
    zip := '20010'
)::geocode_input);
```

**Bring your own adapter.** Any other standardizer — a different PAGC rules set, a hand-rolled Perl script, an LLM-powered parser, `addrust_parse()`, `parse_address()` standalone — is fine as long as it produces a `geocode_input` that satisfies the format table above. The `canon_*` macros are the supported escape hatch for non-canonical output. The geocoder itself does not care where the input came from.

**Parity target.** Given (a) the same PAGC binaries + rules our extension uses and PostGIS uses, (b) the COALESCE-cascade adapter above, and (c) a faithful port of the geocoder SQL on identically-loaded TIGER data, `geocode(from_pagc(x))` should produce the same ratings Postgres does when `use_pagc_address_parser = true`. We explicitly do **not** pursue parity with the built-in `normalize_address()` path — it's data-dependent (location extraction queries loaded TIGER `place`/`cousub`, so results are not reproducible across installs) and is not the PostGIS team's recommended workflow.

**Intersection geocoding.** `geocode_intersection(road1, road2, state, city, zip)` takes plain varchars already; no `geocode_input` round-trip is needed. Port it directly — take five varchars, do light inline cleanup (lowercase, whitespace-normalize via the existing `normalize_street_name` logic for highway hyphen handling), and hit `featnames`/`edges`/`faces`.

**Geocode.** A table function
```
geocode(
    input              geocode_input,
    max_results        INT     DEFAULT 10,
    restrict_geom      GEOMETRY DEFAULT NULL,
    require_containment VARCHAR DEFAULT 'none',    -- 'none' | 'block' | 'tract' | 'blkgrp'
    reference_db       VARCHAR DEFAULT NULL,        -- default: session setting, else 'main'
    reference_schema   VARCHAR DEFAULT NULL         -- default: session setting, else 'tiger'
) → TABLE(
    addy                    geocode_input,
    geom                    GEOMETRY,
    rating                  INT,
    block_geoid             VARCHAR,   -- 15-char 2020 block GEOID, always populated
    tract_geoid             VARCHAR,   -- 11-char tract GEOID, always populated
    blkgrp_geoid            VARCHAR,   -- 12-char block group GEOID, always populated
    containment_guaranteed  BOOLEAN    -- true ⇒ geom provably inside block_geoid (see §14.3)
)
```
that conceptually does:

```sql
WITH
     -- Stage A: full-address candidates per row of input
     candidates AS (
       SELECT
           p, f.tlid, f.statefp, f.name, f.fullname, ..., a.fromhn, a.tohn, a.side, a.zip,
           e.geom AS edge_geom, place.name AS place_name, state.stusps,
           rating_expr(...) AS rating
       FROM input p
       JOIN featnames f        ON f.statefp = state_of(p) AND name_match(f, p)
       JOIN addr a              ON a.tlid = f.tlid AND a.statefp = f.statefp
                               AND (p.zip IS NULL OR zip_in_window(a.zip, p.zip))
       JOIN edges e             ON e.tlid = f.tlid AND e.statefp = f.statefp
       JOIN faces fc            ON fc.statefp = e.statefp
                               AND ((e.tfidl = fc.tfid AND a.side='L')
                                 OR (e.tfidr = fc.tfid AND a.side='R'))
       LEFT JOIN place          ON place.statefp = fc.statefp AND place.placefp = fc.placefp
       JOIN state               ON state.statefp = e.statefp
     ),
     ranked AS (
       SELECT *,
              interpolate(p.address, fromhn, tohn, edge_geom, side) AS point,
              ROW_NUMBER() OVER (PARTITION BY p ORDER BY rating) AS rn
       FROM candidates
     )
SELECT * FROM ranked WHERE rn <= max_results;
```

(There is no `SELECT normalize_address(address)` step — the caller has already produced a `geocode_input`, whether via `from_pagc(raw)` or their own adapter.)

This is the set-oriented rewrite the PL/pgSQL *cannot* express cleanly. The short-circuits (`rating = 0` → stop, `exact_street` → skip sub-stage) become `LIMIT` + `WHERE` clauses in the vectorized version; we should be willing to compute a few extra candidates per row in exchange for keeping the whole batch in one vectorized pass.

**Stage B (location fallback)** only needs to run for rows of the batch that produced no Stage A candidate. A `LEFT JOIN` followed by a `WHERE stage_a_rating IS NULL` subquery on `zcta5` / `place` / `cousub` handles this cleanly.

**Rating function.** All components are pure scalar functions of two string inputs (plus integer house number). Implement `rate_attributes(...)`, `diff_zip(...)`, `numeric_streets_equal(...)`, `normalize_street_name(...)` as deterministic DuckDB UDFs (scalar or macros) with the exact weights above. `levenshtein` is built-in; `soundex` is available via a small C implementation or a UDF.

**Interpolation.** Port `interpolate_from_address` more or less literally against DuckDB spatial: project to a local metric CRS (UTM zone from the starting point), line-interpolate, optionally offset perpendicular using the local azimuth, project back. DuckDB spatial's `ST_Transform`, `ST_LineInterpolatePoint`, `ST_Azimuth`, `ST_Translate` are all the primitives needed.

**Reverse geocode.** Separate function; the flow in §10 translates directly. No row-at-a-time loop needed if we're willing to emit one row per candidate edge instead of a single record with arrays.

**What we can drop / simplify:**
- The entire `normalize_address` PL/pgSQL parser — replaced by `from_pagc()` + BYO adapters.
- The `norm_addy` composite — replaced by the 8-field `geocode_input` struct.
- The `pagc_normalize_address` wrapper — its logic lives in `from_pagc()` directly; no indirection needed.
- The `use_pagc_address_parser` setting and the whole `geocode_settings` table. Expose `zip_penalty`, `reverse_geocode_numbered_roads` etc. as function parameters.
- The `internal`, `zip4`, `address_alphanumeric`, `parsed` fields — the geocoder never reads them.
- `install_missing_indexes()` and the loader script generators — DuckDB handles indexing differently and we control ingestion, not the user's shell.
- The state-partition table inheritance hierarchy — use regular tables; rely on DuckDB's pruning.
- The two exact/fuzzy sub-stages inside `geocode_address` can be folded into a single rating expression where "this matches exactly" just contributes 0 to the penalty. The Postgres version split them for plan-stability reasons that don't apply to DuckDB.
- The `IMMUTABLE`-but-actually-reads-tables trick on `geocode_intersection`.

**What we must preserve:**
- The `geocode_input` public contract and per-field format rules (§14 table above) — ratings depend on it.
- Rating weight ratios (direction 2, name 10, type 5) — consumers depend on these.
- The "location-only ratings are always ≥ 100" invariant, or an equivalent that preserves total order with address matches.
- SRID 4269 for all output geometry (or at minimum, document when we don't).
- House-number parity rule (`even addresses on the even side`).
- `side` interpretation (`'L'`/`'R'`) relative to the edge direction — this is what the faces join encodes.
- The PAGC-path parity target: `geocode(from_pagc(x))` = PostGIS's `geocode(x)` with `use_pagc_address_parser=true`, on identical TIGER data.

### 14.9 Wishlist / deferred

Items explicitly punted out of v0.1 but worth building toward:

- **Pre-built Parquet distribution (D1 follow-up).** Publish per-state Parquet releases from our own bucket (GitHub release assets or S3). First-run UX becomes `CALL load_tiger_states(['MA'])` → ~200 MB of Parquet instead of ~500 MB of zips + shapefile-read + derived-table builds. Turns a multi-minute load into ~30 seconds.
- **`/vsizip//vsicurl/` remote-direct reads (D1 follow-up).** Skip the download-to-disk step entirely by letting GDAL do ranged HTTPS reads of the remote zip. Zero disk footprint. Latency and range-request-count concerns make this a benchmark-then-maybe-default; keep as an opt-in flag initially.
- **Short-circuit parity (D7 follow-up).** Revisit if users report rating/ordering drift at tie boundaries vs PG. Could reintroduce the Stage A "if best rating ≥ 0, return" early-exit as an optional flag; otherwise tolerate the wider candidate set in exchange for vectorized throughput.
- **`get_tract()` / census enrichment (D8 follow-up).** If users want tract / block group / tabblock joins, add an optional `load_tiger_census(['MA'], include=['tract','bg','tabblock20'])` that loads those tables. Separate from the geocoding load path.
- **Parallel state downloads.** Current plan loads states sequentially. For users adding 50 states, parallelizing the HTTP+ST_Read across a thread pool would help — but needs care around DuckDB's single-writer model.
- **Larger parity corpus (D13 follow-up).** Scale from the v0.1 ~50-address hand-curated set to a 10K-row randomly-sampled corpus (pick `addr` rows, reconstruct strings, round-trip). Catches regressions the hand corpus misses.
- **Country-aware normalization.** PG's recent `normalize_address` fix (#1599, per [NEWS.md](../NEWS.md)) and `pagc_normalize_address`'s `country` field hint that non-US is on the roadmap upstream. If we go there, it's a separate conversation — everything in this doc assumes US / TIGER.
- **Backwards-compat `norm_addy` shim.** If anyone wants to port Postgres SQL that references `norm_addy` unchanged, ship a thin SQL macro that casts between `norm_addy`-shaped STRUCT and `geocode_input`. Only add this if users ask — we don't need it for our own code.
