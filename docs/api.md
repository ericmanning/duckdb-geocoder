# API reference

All functions live in the `tiger` schema of whatever database `LOAD us_geocoder` was first invoked against. The schema name is fixed at `tiger` in the local catalog — that's where the macros, lookup tables, and (by default) the 13 TIGER data tables live.

The **data tables** can be repointed to an attached catalog via [`set_tiger_reference`](#set_tiger_referencedatabase-varchar-schema-varchar-default-tiger--table) without touching the macros. See [§ Reference databases](#reference-databases).

## Loaders

### `load_tiger_nation([source VARCHAR], year INT DEFAULT 2025, target_db VARCHAR DEFAULT NULL, target_schema VARCHAR DEFAULT 'tiger') → TABLE(step VARCHAR, rows_loaded BIGINT)`

Loads three nation-wide TIGER tables into the target schema: `state` (56 rows), `county` (~3,200), `zcta5` (~33,800). Must run **before** any `load_tiger_state` call — the state loader enumerates counties from the `county` table populated here.

- `source` — optional URL or local filesystem path. When omitted (or empty), defaults to `https://www2.census.gov/geo/tiger/TIGER<year>` and auto-loads the `httpfs` extension.
- `year` — TIGER vintage; default `2025`.
- `target_db` — attached catalog name to write into. `NULL` (default) means the current database.
- `target_schema` — schema name inside the target catalog; default `tiger`. Created idempotently.

Emits one summary row per loaded file. Idempotent: re-running skips rows that already exist (checked by FIPS / GEOID).

Examples:
```sql
CALL load_tiger_nation(year := 2025);                     -- from Census CDN
CALL load_tiger_nation('/data/tiger_2025', year := 2025); -- from local dir
```

### `load_tiger_state(state_abbrev VARCHAR, [source VARCHAR], year INT DEFAULT 2025, target_db VARCHAR DEFAULT NULL, target_schema VARCHAR DEFAULT 'tiger') → TABLE(step VARCHAR, rows_loaded BIGINT)`

Loads a single state's TIGER data: state-level `place` and `cousub`, county-level `edges`, `faces`, `featnames`, `addr` (for every county in the state), and the derived `zip_state`, `zip_state_loc`, `zip_lookup_base`, `edge_containment` tables.

- `state_abbrev` — 2-letter postal code, case-insensitive (`'RI'`, `'ri'`, `'Ri'` all work).
- `source` / `year` — same as `load_tiger_nation`.
- `target_db` / `target_schema` — same as `load_tiger_nation`. Must match the target used for `load_tiger_nation` (the state loader reads `<target>.county` to enumerate counties).
- `build_containment BOOLEAN DEFAULT true` — when `false`, skip the eager `edge_containment` precompute at the end of the state load (saves ~1-2 min per state). Populate it later with [`build_edge_containment()`](#build_edge_containmentstates-varchar--varchar-target_db-varchar-default-null-target_schema-varchar-default-tiger--table).

Idempotent via **DELETE-first**: re-running wipes all rows for that state before reinserting (including `edge_containment`).

```sql
CALL load_tiger_state('RI');                                 -- from Census, into tiger.* locally
CALL load_tiger_state('RI', '/data/tiger_2025', year := 2025);
CALL load_tiger_state('RI', target_db := 'work');            -- write to attached 'work' catalog
```

### `load_tiger_states(states VARCHAR[], [source VARCHAR], year INT DEFAULT 2025, target_db VARCHAR DEFAULT NULL, target_schema VARCHAR DEFAULT 'tiger') → TABLE(step VARCHAR, rows_loaded BIGINT)`

Same as `load_tiger_state` but accepts a list of abbreviations. States are loaded sequentially in the order provided; the output row stream interleaves `begin:<ABBREV>` / per-file / `done:<ABBREV>` markers so you can tail progress.

Case-insensitive and deduped — `['ri','RI']` loads RI once. Unknown abbreviations fail at bind time.

```sql
CALL load_tiger_states(['RI','MA','CT']);
CALL load_tiger_states(['RI','MA','CT'], '/data/tiger_2025');
CALL load_tiger_states(['RI','MA','CT'], target_db := 'work');
```

### `load_tiger_all_states([source VARCHAR], year INT DEFAULT 2025, target_db VARCHAR DEFAULT NULL, target_schema VARCHAR DEFAULT 'tiger') → TABLE(step VARCHAR, rows_loaded BIGINT)`

Convenience wrapper over `load_tiger_states` that enumerates the 50 states + DC from `state_lookup`. Concretely: `SELECT abbrev FROM tiger.state_lookup WHERE CAST(statefp AS INTEGER) BETWEEN 1 AND 56` — the Census FIPS scheme reserves 01–56 for the 50 states + DC (with gaps at 03/07/14/43/52) and ≥60 for territories (AS, GU, MP, PR, VI) and freely-associated states (FM, MH, PW). Call `load_tiger_states(['PR',...])` explicitly if you need those.

```sql
CALL load_tiger_all_states();                                    -- ~3-4 hours over Census CDN
CALL load_tiger_all_states('/data/tiger_2025');                  -- from local mirror, faster
CALL load_tiger_all_states(target_db := 'tiger_us_2025');        -- build a portable reference DB
```

You'll typically want to run `load_tiger_nation` first with the same target; the state loader reads from `<target>.county`. A full nation + all-states load to a single `.duckdb` file is the canonical way to build a portable reference (§ Reference databases).

### Local source layout

Local sources use the **Census-nested** layout — a (partial) mirror of `https://www2.census.gov/geo/tiger/TIGER<year>/` with one subdirectory per table-type:

```
/data/tiger_2025/
  STATE/tl_2025_us_state.zip
  COUNTY/tl_2025_us_county.zip
  ZCTA520/tl_2025_us_zcta520.zip
  PLACE/tl_2025_44_place.zip
  COUSUB/tl_2025_44_cousub.zip
  EDGES/tl_2025_44007_edges.zip
  FACES/tl_2025_44007_faces.zip
  FEATNAMES/tl_2025_44007_featnames.zip
  ADDR/tl_2025_44007_addr.zip
  …
```

This is what `wget --mirror` against the Census FTP produces out of the box. The HTTP source mode uses the same subdirectory convention, so swapping between HTTP and local is a one-line change.

Other layouts are **not** supported in v0.1:

- Flat-in-one-directory (all zips in `/data/tiger_2025/` with no `EDGES/`, `FACES/`, … subdirs) — fails because the loader always inserts the Census subdir when constructing the `/vsizip/` path.
- Pre-extracted shapefiles (unzipped `.shp` + `.dbf` + `.shx` on disk) — fails because the loader always wraps in `/vsizip/`, which requires a real zip.

Unzip and flattening are explicit out-of-scope; a user who wants either can either rewrap (zip the files back up, or create symlinks with the expected nested structure) or open an issue for explicit support.

---

## Reference databases

The geocoder macros (`tiger.geocode`, `tiger.reverse_geocode`, `tiger.geocode_intersection`, and their helpers) always live in the local `tiger` schema. The 13 **data tables** (`state`, `county`, `place`, `cousub`, `zcta5`, `zip_state`, `zip_state_loc`, `zip_lookup_base`, `edges`, `faces`, `featnames`, `addr`, `edge_containment`) can be either base tables in the current catalog or views pointing at an attached catalog.

### `install_tiger_schema(database VARCHAR, schema VARCHAR DEFAULT 'tiger') → TABLE(step VARCHAR, rows_loaded BIGINT)`

Creates the TIGER data schema + 13 empty tables in an attached catalog. Idempotent (safe to re-run; existing rows are preserved). Pass `database := ''` to target the current catalog.

```sql
ATTACH 'tiger_ref.duckdb' AS ref;
CALL install_tiger_schema('ref');          -- creates ref.tiger.*
CALL install_tiger_schema('ref', 'tgr25'); -- creates ref.tgr25.*
```

The loaders call this automatically when `target_db` is set, so you only need `install_tiger_schema` explicitly if you want an empty-but-valid schema (e.g. to hand-populate for tests) or are building a reference DB via some other ETL.

### `build_edge_containment(states VARCHAR | VARCHAR[], target_db VARCHAR DEFAULT NULL, target_schema VARCHAR DEFAULT 'tiger') → TABLE(step VARCHAR, rows_loaded BIGINT)`

Compute (or recompute) `tiger.edge_containment` for one or more already-loaded states. Useful after `load_tiger_state(..., build_containment := false)` when you want to defer the ~1-2-min-per-state precompute and fill it in later.

Idempotent: DELETEs existing rows for each `statefp` before reinserting. `target_db` / `target_schema` route the writes to an attached catalog, same semantics as the loaders.

```sql
-- Fast initial load, defer containment:
CALL load_tiger_nation();
CALL load_tiger_states(['NJ','NY','PA'], build_containment := false);

-- ...later, when you want GEOIDs:
CALL build_edge_containment(['NJ','NY','PA']);
-- or a single state:
CALL build_edge_containment('NJ');
```

Prerequisite: the state's `edges` and `faces` rows must be loaded (via any `load_tiger_*` call). If they're missing, the INSERT runs but produces 0 rows.

### `set_tiger_reference([database VARCHAR [, schema VARCHAR DEFAULT 'tiger']]) → TABLE(table VARCHAR, kind VARCHAR)`

Swaps the 13 local `tiger.<table>` between base-table form and view form.

- **No args / `NULL` / empty string first arg** — drops any existing local `tiger.<data_table>` (view or base table) and recreates empty base tables. Use this to "unbind" from an attached reference.
- **`database` non-empty** — drops the local tables and recreates them as `VIEW tiger.<t> AS SELECT * FROM <database>.<schema>.<t>`. Reads through those views land in the attached catalog; macros, lookup tables, and everything else stay local.

Emits one row per data table with `kind ∈ {'base_table', 'view'}`.

```sql
-- Mode B: attached read-write
ATTACH 'work.duckdb' AS work;
CALL load_tiger_state('RI', target_db := 'work');
CALL set_tiger_reference('work');
SELECT * FROM tiger.geocode(...);          -- reads from work.tiger.*

-- Mode C: portable, read-only
ATTACH 'tiger_us_2025.duckdb' AS ref (READ_ONLY);
CALL set_tiger_reference('ref');
SELECT * FROM tiger.geocode(...);

-- Reset to empty local base tables:
CALL set_tiger_reference();
```

**Caveats.**

- `set_tiger_reference()` with no args is **destructive to local data tables** — any rows previously loaded via `load_tiger_state()` into the local `tiger.*` are dropped. Reload after resetting.
- The attached catalog must already contain the 13 tables with the expected columns. `install_tiger_schema` + the loader's `target_db` are the sanctioned way to build one.
- Lookup dictionaries (`state_lookup`, `street_type_lookup`, `direction_lookup`, `secondary_unit_lookup`) are not affected — they live in the local `tiger` schema and are seeded by the extension at load time.

---

## Input type

### `tiger.geocode_input`

```sql
CREATE TYPE tiger.geocode_input AS STRUCT(
    address      INTEGER,
    street_name  VARCHAR,
    street_type  VARCHAR,
    pre_dir      VARCHAR,
    post_dir     VARCHAR,
    location     VARCHAR,
    state_abbrev VARCHAR,
    zip          VARCHAR
);
```

The 8-field public contract for every geocoder entry point. Users produce these from any parser (PAGC, warehouse columns, forms, …) and hand them to `geocode()`.

| field | format | strict? | notes |
|---|---|---|---|
| `address` | INTEGER house number | NULL allowed | parity check + interpolation; NULL → +20 rating penalty |
| `street_name` | free-form, any case; **no** street type or direction | NULL → Stage A returns empty | `Benefit`, `I-95`, `15th` |
| `street_type` | TIGER title-case abbrev | soft penalty | `Ave`, `St`, `Blvd`, `Hwy` |
| `pre_dir` | uppercase 2-letter | soft penalty | `N`, `NW`, `SE` |
| `post_dir` | same as `pre_dir` | soft penalty | |
| `location` | free-form city name | NULL → +5 | |
| `state_abbrev` | **UPPERCASE** 2-letter | **strict** | `MA`, `DC`; lowercase or full name fails the state prune |
| `zip` | **5-char** with leading zeros | **strict** | `'02109'`, not `2109`; wrong → silent miss on MA/NJ/CT/RI |

The `canon_*` macros coerce arbitrary parser output into this contract — see [Helpers](#helpers).

---

## Core geocoding

### `tiger.geocode(input geocode_input, max_results INT, restrict_geom GEOMETRY, require_containment VARCHAR) → TABLE`

The main geocoder. Returns up to `max_results` candidate matches, ordered by `rating` ascending (0 = best).

**Parameters:**
- `input` — a `tiger.geocode_input` struct (see above).
- `max_results` — integer cap on returned rows; typical values 1–10.
- `restrict_geom` — optional `GEOMETRY`. If non-NULL, only edges intersecting this polygon are considered. Auto-transformed to EPSG:4269 if the input SRID is different (SRID 0 treated as 4269).
- `require_containment` — `'none'` | `'block'` | `'tract'` | `'blkgrp'`.
  - `'none'` — return all candidates; `containment_guaranteed` is informational.
  - `'block'`/`'tract'`/`'blkgrp'` — filter to `containment_guaranteed = true`. In v0.1 these three are equivalent (conservative single-face check); looser tract/blkgrp dissolve-polygon checks are a v0.2 follow-up.

**Returns:**
| column | type | meaning |
|---|---|---|
| `addy` | `tiger.geocode_input` | canonicalized matched address (name/type/dir from TIGER; location resolved via place → cousub → county; zip from `addr.zip`) |
| `geom` | `GEOMETRY` | interpolated point, EPSG:4269, 10m offset to the matched side of the street |
| `rating` | `INTEGER` | quality score, lower is better (see [Rating scale](#rating-scale)) |
| `block_geoid` | `VARCHAR(15)` | 2020 census block GEOID of the matched side (always populated when `edge_containment` is built) |
| `tract_geoid` | `VARCHAR(11)` | census tract GEOID |
| `blkgrp_geoid` | `VARCHAR(12)` | block group GEOID |
| `containment_guaranteed` | `BOOLEAN` | `true` iff the 10m-offset midpoint provably lies inside `block_geoid`'s face (see [Containment](#containment)) |

Batch geocoding via `CROSS JOIN LATERAL`:
```sql
SELECT a.id, g.rating, ST_AsText(g.geom)
FROM my_addresses a, LATERAL tiger.geocode(a.input, 1, NULL, 'none') g;
```

### `tiger.geocode(raw_text VARCHAR) → TABLE`

One-arg shortcut for free-form strings. Equivalent to `tiger.geocode(tiger.from_pagc(raw_text), 10, NULL, 'none')`.

```sql
SELECT rating, ST_AsText(geom), block_geoid
FROM tiger.geocode('120 Benefit St, Providence RI 02903');
```

**Availability.** Registered only when the `us_address_standardizer` community extension is present. The extension auto-loads and populates its `us_lex` / `us_gaz` / `us_rules` tables via `load_us_address_data()` when `us_geocoder` is loaded — no manual setup required if the ext is already `INSTALL`ed. If `us_address_standardizer` isn't installed, call [`from_pagc`](#tigerfrom_pagcraw_text-varchar--tigergeocode_input) yourself or pass a `geocode_input` struct.

For per-row `max_results` / `restrict_geom` / `require_containment` control with free-form input, call the 4-arg form explicitly:

```sql
SELECT * FROM tiger.geocode(
    tiger.from_pagc('120 Benefit St, Providence RI 02903'),
    1,
    NULL,
    'block'
);
```

### `tiger.from_pagc(raw_text VARCHAR) → tiger.geocode_input`

Standardizer adapter. Parses a free-form address string into a `geocode_input` struct suitable for `tiger.geocode()`. Built on the `us_address_standardizer` community extension's `standardize_address('us_lex', 'us_gaz', 'us_rules', …)` (PAGC) + `parse_address(…)` fallback for fields the standardizer leaves empty.

See [§ geocode_input](#tigergeocode_input) for the output shape and the `canon_*` macros for individual field normalization.

### `tiger.geocode_intersection(road1 VARCHAR, road2 VARCHAR, state VARCHAR, city VARCHAR, zip VARCHAR, max_results INT) → TABLE`

Finds intersections of two streets. Joins candidate edges on shared TIGER node IDs (`tnidf`/`tnidt`) — not `ST_Intersects` — which means intersecting edges are found in O(joins) rather than O(spatial).

```sql
SELECT rating, ST_AsText(geom)
FROM tiger.geocode_intersection('Benefit', 'Meeting', 'RI', 'Providence', '02903', 3);
```

Returns `(addy, geom, rating)`. Output geom is the shared endpoint of the first road's edge.

### `tiger.reverse_geocode(pt GEOMETRY, max_results INT) → TABLE`

Given a point, returns the nearest street candidates.

```sql
SELECT rank, street, (addy).address, (addy).location, dist_m
FROM tiger.reverse_geocode(ST_Point(-71.40882, 41.82993), 5)
ORDER BY rank;
```

Returns one row per candidate edge with a `rank` column (1 = nearest), distance in meters, interpolated house number, and the geocode_input struct populated from state/place/zip containing the point. Per geocode_flow.md D6, this is one-row-per-candidate rather than PG's parallel-array output.

---

## Helpers

### Canonicalization macros

For coercing arbitrary parser output into the `geocode_input` contract:

| macro | output |
|---|---|
| `canon_street_type(t)` | TIGER title-case abbrev: `'AVENUE'` / `'Avenue'` / `'ave'` → `'Ave'` |
| `canon_dir(d)` | uppercase 2-letter: `'Northwest'` / `'nw'` → `'NW'` |
| `canon_state(s)` | 2-letter uppercase abbrev: `'Massachusetts'` → `'MA'`; unknown → `NULL` |
| `canon_zip(z)` | 5-char, leading-zero preserved: `'2109'` → `'02109'`; `'02109-1234'` → `'02109'` |

### `from_pagc(raw_text VARCHAR) → geocode_input`

Adapter that repacks [`us_address_standardizer`](https://duckdb.org/community_extensions/extensions/us_address_standardizer)'s `standardize_address()` + `parse_address()` output into a `geocode_input`, applying the `canon_*` functions and the PostGIS-parity COALESCE cascade for `location`/`state_abbrev`/`zip`.

Registered only if `us_address_standardizer` was loaded at extension-load time. If absent, calls to `from_pagc()` return "function does not exist" — load the standardizer then reload the extension:
```sql
LOAD us_address_standardizer;
LOAD us_geocoder;
SELECT * FROM tiger.geocode(tiger.from_pagc('1731 New Hampshire Ave NW, Washington DC 20010'));
```

---

## Rating scale

The rating is an ordered penalty — **lower is better**, 0 is a perfect match.

| range | source | meaning |
|---|---|---|
| **0** | Stage A | exact house number, street, type, direction, ZIP, place |
| 1–29 | Stage A | strong match; typos or minor mismatches |
| 30–89 | Stage A | acceptable |
| 90–99 | Stage A | marginal; emitted but deprioritized |
| ≥ 100 | Stage B | location-only fallback (no street-level confidence) |

Per-component penalty weights (see `rate_attributes` in [src/sql/scoring_macros.sql.in](../src/sql/scoring_macros.sql.in)):

- Direction mismatch: `levenshtein * 2`
- Street name: `levenshtein * 10` (with a `0.75` discount when a `prequalabr` like `Old` is dropped; zero when both names are numerically equivalent via `numeric_streets_equal`)
- Street type: `levenshtein * 5`
- House number: `0` (in range + right parity), `2` (in range wrong parity), `5` (out of range), `20` (no range at all)
- ZIP: `min(diff_zip * zip_penalty, 20 * zip_penalty)` — `zip_penalty` defaults to 2.0
- Location (city): raw levenshtein against the resolved place/cousub/county name

These ratios are derived straight from PostGIS's `rate_attributes`; consumers that sort by `rating` or threshold at specific values will see the same relative ordering.

---

## Containment

The `block_geoid`, `tract_geoid`, `blkgrp_geoid` columns are always populated from the adjacent face of the matched edge side. `containment_guaranteed` is a conservative check:

- `true` — the 10m-offset midpoint of the matched edge is provably inside the GEOID's face polygon. Use when downstream linkage (demographics, policy) requires high confidence.
- `false` — the check failed. The GEOID is still the best-guess assignment; for the majority of addresses it's correct, but the offset point may land near a face boundary.

The guarantee is only valid at the default 10m offset and the default 0.5 interpolation fraction. v0.1 limitations (all conservative, producing false negatives only):

- **L1** offset-sensitive — a per-call custom offset invalidates the flag.
- **L2** intra-block faces — an edge whose offset strip crosses from one face into a neighboring face in the same block reads false even though the block-level truth holds.
- **L3** endpoint caps — the midpoint check passes near intersections even when the buffer would otherwise leak into a neighboring face.

`require_containment='tract'` and `'blkgrp'` currently resolve to the same face-level check as `'block'`; looser (but still sound) checks via dissolved-polygon pre-aggregation are a v0.2 follow-up.

---

## Schema

All tables in the `tiger` schema. See [src/sql/tiger_schema.sql.in](../src/sql/tiger_schema.sql.in) for the full DDL. Geometry is stored in EPSG:4269 (NAD83). User-supplied geometry passed to `geocode`/`reverse_geocode` is auto-transformed to 4269; SRID 0 is treated as "assume 4269".

The reference tables (`featnames`, `edges`, `faces`, `addr`, `state`, `county`, `place`, `cousub`, `zcta5`) carry the minimum columns the geocoder reads, plus a handful of loader-precomputed acceleration columns on `featnames` (`name_lower`, `fullname_norm`, `name_soundex`). The derived tables (`zip_state`, `zip_state_loc`, `zip_lookup_base`, `edge_containment`) are built per-state at load time.
