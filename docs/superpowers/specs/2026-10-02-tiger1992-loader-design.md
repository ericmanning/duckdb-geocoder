# 1992 TIGER/Line loader — design

**Status:** approved, pending implementation plan
**Branch:** `feature/1992tiger`
**Date:** 2026-10-02

## Goal

Load the 1992 TIGER/Line files (<https://www2.census.gov/geo/tiger/TIGER1992/>) into the
existing `tiger` schema so that **forward geocoding works unchanged** — `tiger.geocode()`,
`geocode_address`, `geocode_address_for_state` and `geocode_intersection` run against
1992-vintage data with no macro edits.

The motivating use case is historical geocoding: resolving addresses as they existed in
1992 and linking them to 1990-census block/tract geography.

## Scope

In scope — populated by this work:

- `edges` (with assembled `LINESTRING` geometry, synthesized node IDs)
- `faces` — **attributes only**, `the_geom` NULL
- `featnames`, including all six precomputed equi-join columns
- `addr`
- `place`, `cousub`, `county`, `state` — **names only**, `the_geom` NULL
- derived `zip_state`, `zip_state_loc`, `zip_lookup_base`
- `edge_containment` — GEOIDs only, `guaranteed_* = false`

Out of scope — deliberately not built:

- Face / place / cousub / state polygons (no polygonization)
- `zcta5` (ZCTAs did not exist until 2000)
- `reverse_geocode`
- `geocode_location`'s ZCTA-centroid and place-centroid tiers (city/ZIP-only input)
- `containment_guaranteed` as a usable filter

## Decisions

| # | Decision | Rationale |
|---|---|---|
| D-A | Explicit `load_tiger_1992_*` functions, not `year := 1992` or a `vintage` param | The capability set genuinely differs; keeps the modern loader's behaviour and signature untouched |
| D-B | Vintage marker in `loader_progress` + refuse-to-mix guard | Real safety at zero hot-path cost; no data-table or macro changes. A `vintage` column would touch every macro's ART-pushdown hot path |
| D-C | `edge_containment` GEOIDs only, `guaranteed_* = false` | 1990 census linkage costs almost nothing and is the point of the vintage; `guaranteed_*` needs polygons, which are out of scope |
| D-D | Text fixtures + one small committed zip | Deterministic and offline. The zip covers the miniz path cross-platform |
| D-E | Sibling module `src/loader_1992.cpp`, shared helpers via a header | `loader.cpp` is already 1866 lines; the two paths share orchestration but nothing of their ingest logic |
| D-F | Fixed-width parse via `read_csv` + `substr`; **do not** use GDAL's native TIGER driver | GDAL has moved its TIGER driver to the deprecated-drivers set. Building on it would be a dead end |

### On D-F

GDAL *does* have a native `TIGER` driver, and it works well today: `ST_Read` over
`/vsizip/<county>.zip/TGR<ssccc>.F51` exposes `CompleteChain`, `Polygon`, `PolyChainLink`,
`AltName`, `FeatureIds`, `ZipCodes`, `PIP` and more as named layers, with pre-assembled
`LINESTRING` geometry and parsed columns. It would have removed the need for both the
fixed-width parse and the zip dependency.

It is rejected because the driver is deprecated upstream. Two secondary problems also
argued against it: it types `ZIPL`/`ZIPR`/`FPL`/`FMCD`/`CTBNA` as `integer`, silently
destroying leading zeros (ZIP `07825` → `7825`), and it presents a field set unified
across TIGER vintages, so 1992 semantics are not self-evident from the schema.

**Do not re-derive this.** If a future reader wonders whether GDAL can shortcut the parse:
it can, and we are choosing not to.

## API surface

Four new table functions, registered by a new `RegisterLoader1992Functions(ExtensionLoader &,
const std::string &tiger_schema)` called from `LoadInternal` alongside the existing
`RegisterLoaderFunctions`:

```
load_tiger_1992_state(state_abbrev VARCHAR [, source VARCHAR], ...)
load_tiger_1992_states(states VARCHAR[] [, source VARCHAR], ...)
load_tiger_1992_all_states([source VARCHAR], ...)
unload_tiger_1992_state(state_abbrev VARCHAR | VARCHAR[], ...)
```

Each load function is registered in both the with- and without-`source` arities, matching
the modern loader's overload pattern.

Named parameters, matching the modern `AddLoaderNamedParams` set **minus `year`** (which is
pinned to 1992):

`source`, `target_db`, `target_schema`, `build_containment`, `parallel`, `temp_dir`,
`parallel_workers`.

Note the parameter is `target_schema`, not `schema`.

`source` defaults to `https://www2.census.gov/geo/tiger/TIGER1992`.

`unload_tiger_1992_state` takes only `target_db` and `target_schema`, mirroring
`unload_tiger_state`.

There is **no** public per-county function. Per-county *restart* comes free from the
progress ledger, exactly as in the modern path.

One additional utility function, needed so the extraction layer is independently testable
and useful to anyone pre-extracting a local mirror:

```
us_geocoder_unzip(zip_path VARCHAR, dest_dir VARCHAR) -> TABLE(entry VARCHAR, bytes BIGINT)
```

It extracts every entry of `zip_path` into `dest_dir` and returns one row per entry.

## Architecture

```
load_tiger_1992_state(abbrev)
  └─ src/loader_1992.cpp
       ├─ resolve source layout      (remote zips | local zips | local extracted tree)
       ├─ ScrapeCensusIndex          [shared]  county list from <source>/<ss>/
       ├─ ParallelDownload           [shared]  zips -> temp dir
       ├─ src/zip_extract.cpp        [new]     miniz, memory-only APIs
       ├─ per-county INSERTs         loader_1992_templates.sql.in
       ├─ derived zip_* sections     [reused verbatim from loader_templates.sql.in]
       ├─ derived_edge_containment   loader_1992_templates.sql.in
       └─ RunAnalyzeOnTigerTables    [shared]
```

### Distribution layout

Verified uniform across every state directory (spot-checked AL, CA, HI, NY, RI, WY, PR —
county counts 67/58/5/62/5/23/78, matching reality):

```
TIGER1992/<ss>/<ssccc>.zip      one per county
TIGER1992/<ss>/OtherFiles.zip   contains TGR92S<ss>.NAM  (+ *.DBF, SCHOLD<ss>.NAM)
```

Each county zip holds 14 fixed-width ASCII files, `TGR<ssccc>.F5<rt>`.

### Source resolution

1. Remote (`http://`, `https://`) — scrape `<source>/<ss>/` with `^<ss>[0-9]{3}\.zip$`,
   download via `ParallelDownload`, extract.
2. Local nested zips — same `<source>/<ss>/<ssccc>.zip` layout, extract in place to the
   temp dir.
3. Local **extracted** tree — `<source>/<ss>/<ssccc>/TGR<ssccc>.F51`. Used by the text
   fixtures, and genuinely useful for anyone holding a CD-ROM mirror.

Detection order: if `<source>/<ss>/<ssccc>.zip` exists use (2), else look for the
directory form (3).

### Zip extraction — `src/zip_extract.cpp`

DuckDB bundles miniz as the `duckdb_miniz` static target. `MINIZ_NO_ARCHIVE_APIS` is
**not** defined, so the ZIP reader APIs are compiled in; `MINIZ_NO_STDIO` **is** defined,
so only the memory-based variants exist.

Linkage is settled empirically: every symbol we need is globally exported from the duckdb
binary, so the static build resolves it from `libduckdb_miniz.a` and the loadable build
from the host process at `dlopen` — **headers only, no extra linkage, and we do not
compile `miniz.cpp` ourselves** (that would duplicate code and risk a different set of
`MINIZ_*` defines than DuckDB built with). Note miniz is wrapped in
**`namespace duckdb_miniz`**, so every call must be qualified, exactly as DuckDB's own
`miniz_wrapper.hpp` does. That is sufficient and preferable — all I/O goes
through DuckDB's `FileSystem`, which keeps Windows behaviour consistent with the rest of
the loader.

```
read whole zip via FileSystem            ->  buffer
mz_zip_reader_init_mem(buffer)
for each entry: mz_zip_reader_get_filename, match case-insensitively
mz_zip_reader_extract_to_heap(index)     ->  bytes
write bytes via FileSystem
mz_zip_reader_end
```

Entry matching is done by enumerating and comparing ourselves rather than with
`mz_zip_reader_locate_file`, whose case sensitivity depends on flags and platform. 1992
archives store entries in uppercase.

Entries extracted per county: `.F51 .F52 .F54 .F55 .F56 .F5A .F5I`.
From `OtherFiles.zip`: `TGR92S<ss>.NAM`.

A new CMake include directory for `duckdb/third_party/miniz` and a link against
`duckdb_miniz` are required.

## Record layouts

Record lengths were verified byte-for-byte against the
[1992 documentation](https://www2.census.gov/geo/tiger/TIGER1992/Documentation/CHAPTER6.txt),
so `substr` offsets are exact. Positions are 1-indexed, inclusive; lengths below exclude
the CRLF terminator.

**RT1 — Complete Chain basic data** (`.F51`, 228 + CRLF)

| field | cols | field | cols |
|---|---|---|---|
| TLID | 6–15 | FPLL / FPLR | 161–165 / 166–170 |
| FEDIRP | 18–19 | CTBNAL / CTBNAR | 171–176 / 177–182 |
| FENAME | 20–49 | BLKL / BLKR | 183–186 / 187–190 |
| FETYPE | 50–53 | FRLONG | 191–200 |
| FEDIRS | 54–55 | FRLAT | 201–209 |
| CFCC | 56–58 | TOLONG | 210–219 |
| FRADDL / TOADDL | 59–69 / 70–80 | TOLAT | 220–228 |
| FRADDR / TOADDR | 81–91 / 92–102 | STATEL / STATER | 131–132 / 133–134 |
| ZIPL / ZIPR | 107–111 / 112–116 | COUNTYL / COUNTYR | 135–137 / 138–140 |
| FMCDL / FMCDR | 141–145 / 146–150 | | |

Coordinates are signed integers with **implied 6 decimal places** (microdegrees), NAD83,
longitude 10 wide and latitude 9 wide.

**RT2 — shape points** (`.F52`, 208 + CRLF). TLID 6–15, RTSQ 16–18, then 10 slots of
(longitude 10, latitude 9) beginning at col 19 — slot *i* at `19 + (i-1)*19` and
`29 + (i-1)*19`. Slots with a zero longitude are padding and must be skipped.

**RT4 — alternate-name index** (`.F54`, 58 + CRLF). TLID 6–15, RTSQ 16–18,
FEAT1–FEAT5 at 19–26, 27–34, 35–42, 43–50, 51–58.

**RT5 — feature identifiers** (`.F55`, 52 + CRLF). STATE 2–3, COUNTY 4–6, FEAT 7–14,
FEDIRP 15–16, FENAME 17–46, FETYPE 47–50, FEDIRS 51–52.

**RT6 — additional address ranges** (`.F56`, 76 + CRLF). TLID 6–15, RTSQ 16–18,
FRADDL 19–29, TOADDL 30–40, FRADDR 41–51, TOADDR 52–62, impute flags 63–66,
ZIPL 67–71, ZIPR 72–76.

**RTA — polygon geographic codes** (`.F5A`, 98 + CRLF). STATE 6–7, COUNTY 8–10,
CENID 11–15, POLYID 16–25, FAIR 26–30, FMCD 31–35, FPL 36–40, CTBNA 41–46, BLK 47–50.

**RTI — chain↔polygon link** (`.F5I`, 52 + CRLF). TLID 6–15, STATE 16–17, COUNTY 18–20,
RTLINK 21, CENIDL 22–26, POLYIDL 27–36, CENIDR 37–41, POLYIDR 42–51.

**NAM — Geographic Name File** (`TGR92S<ss>.NAM`, 80-char records). RT 1–2, STATE 3–4,
COUNTY 5–7, FIPS55 8–12, NAME 13–72, FIPSCLS 73–74, CENSUS 75–79, USAGE 80.
Record types: `01` state, `02` county, `03` county subdivision, `05` place.

## Transformation — `src/sql/loader_1992_templates.sql.in`

Follows the existing `-- @SECTION:name@` convention parsed by `GetLoaderTemplate()`.
Placeholders: `@TIGER@`, `@FUNC@`, `@STATEFP@`, `@COUNTYFP@`, plus `@F51@`, `@F52@`,
`@F54@`, `@F55@`, `@F56@`, `@F5A@`, `@F5I@`, `@NAM@` as **fully-resolved paths**, so C++
owns all path joining (Windows separators included).

Scan shape:

```sql
SELECT rtrim(line, chr(13)) AS line
FROM read_csv('@F51@', columns={'line':'VARCHAR'}, delim='\x1f',
              header=false, quote='', escape='')
```

`\x1f` (unit separator) cannot occur in printable-ASCII TIGER content, so no record is
ever split. `rtrim` strips the CRLF.

### Sections

| section | sources | notes |
|---|---|---|
| `county_edges` | RT1, RT2, RTI | geometry, node IDs, CFCC→MTFCC, `fullname` |
| `county_faces` | RTA | `the_geom` NULL |
| `county_featnames` | RT1, RT4⨝RT5 | 6 precomputed columns |
| `county_addr` | RT1, RT6 | L/R → `side` |
| `state_names` | NAM | `place`, `cousub`, `county`, `state` |
| `derived_edge_containment` | edges⨝faces | GEOIDs only; skipped entirely when `build_containment := false` |

The three derived `zip_*` sections (`derived_zip_state`, `derived_zip_state_loc`,
`derived_zip_lookup_base`) are **reused verbatim** from `loader_templates.sql.in`. They are
pure SQL over `edges`/`faces`/`place`/`state` and carry no vintage assumptions.

### Field mapping details

**Geometry.** `ST_MakeLine` over the from-node, the RT2 shape points ordered by
`(RTSQ, slot)`, and the to-node.

**Node IDs.** 1992 has no `TNIDF`/`TNIDT` — only endpoint coordinates. Node identity in
TIGER is coordinate identity, so IDs are derived arithmetically from the raw microdegree
integers already in the file:

```
nid = (lon_micro + 180000000) * 1000000000 + (lat_micro + 90000000)
```

Pure integer arithmetic (no float rounding), collision-free, and **stable across county
boundaries** — which matters because `geocode_intersection` joins `tnidf`/`tnidt` across
edges that may come from different county files. A per-load `row_number()` or a `hash()`
would break this; `hash()` additionally returns `UINT64`, which overflows `BIGINT`.
Maximum value ≈ 3.6×10¹⁷, comfortably inside `BIGINT`.

**`tfid`.** `CENID::BIGINT * 10000000000 + POLYID::BIGINT`. `(CENID, POLYID)` is the 1992
analogue of the modern `TFID`; CENID is 5 digits and POLYID 10, so the composite fits.

**CFCC → MTFCC.** The macros only ever test `mtfcc LIKE 'S%'`.

| CFCC | MTFCC | CFCC | MTFCC |
|---|---|---|---|
| `A1*` | `S1100` | `A5*`, `A6*`, `A7*` | `S1500` |
| `A2*` | `S1200` | other `A*` | `S1400` |
| `A3*`, `A4*` | `S1400` | non-`A` | `'X' \|\| cfcc` |

**`fullname`.** `concat_ws(' ', FEDIRP, FENAME, FETYPE, FEDIRS)`.

**`featnames`.** RT1 supplies the primary name per TLID; RT4 ⨝ RT5 supplies alternates.
`predirabrv` ← FEDIRP, `suftypabrv` ← FETYPE, `sufdirabrv` ← FEDIRS.
`pretypabrv` and `prequalabr` are **always NULL** — 1992 has only four name fields.
All six precomputed columns are populated exactly as the modern template does
(`name_lower`, `fullname_norm` via `@FUNC@.normalize_street_name`, `name_soundex`,
`numeric_stem`, `name_first_5`, `fullname_first_5`).

**`addr`.** RT1's L/R ranges plus RT6's additional ranges, unpivoted into `side` `'L'`/`'R'`,
filtered to `fromhn IS NOT NULL`. `zip` stays `VARCHAR` straight off `substr`, preserving
leading zeros. `plus4` is NULL — 1992 carries no ZIP+4 on these records.

**`faces`.** `placefp` ← FPL, `cousubfp` ← FMCD, `tractce20` ← CTBNA, `blockce20` ← BLK,
`blkgrpce20` ← first character of BLK. The `*20` column names are retained to avoid
touching the schema; the values are 1990-vintage.

**`state` / `county` / `place` / `cousub`.** Names from the NAM file — record type `01` →
`state`, `02` → `county`, `03` → `cousub`, `05` → `place`. `stusps` is **not** in the NAM
file and comes from the existing `state_lookup` seed table via `LookupStateFips`.
`cntyidfp` is `statefp || countyfp`; `plcidfp` is `statefp || placefp`; `cosbidfp` is
`statefp || countyfp || cousubfp`. Entity-type suffixes (` city`, ` borough`, ` township`,
` County`, …) are stripped to match modern TIGER's `NAME` convention.

**There is no 1992 nation step.** In the modern loader, `state` and `county` are
`nation_*` sections populated once by `load_tiger_nation()`. The 1992 distribution has no
nation-level product, so `load_tiger_1992_state` populates the `state` and `county` rows
for its own state from that state's NAM file. Both INSERTs therefore carry idempotency
guards in the same style as the modern nation sections — `WHERE statefp NOT IN (SELECT
statefp FROM @TIGER@.state WHERE statefp IS NOT NULL)` and the `cntyidfp` equivalent — so
loading a second state, or re-running a state, cannot duplicate rows.

## Vintage guard

Progress section keys are prefixed `tiger1992:`, e.g.
`tiger1992:state:34:county:041:edges`, `tiger1992:state:34:derived:zip_state`.

Guards, enforced in both directions:

- **1992 loader refuses** if `edges` has rows for that `statefp` **and** no
  `tiger1992:state:<fp>%` progress key exists.
- **Modern loader refuses** if a `tiger1992:state:<fp>%` progress key exists.

The data-presence half of the first check matters because `BackfillProgressIfNeeded` means
a database loaded before the progress ledger existed can hold rows with no keys; a
key-only check would wave it through.

Error text names the offending state and points at the remedy: use a separate `target_db`
or `target_schema`, or unload first.

`unload_tiger_1992_state(abbrev)` reuses the modern `unload_state` DELETE list unchanged
(11 statements, covering every per-state table including `edge_containment`) and clears
that state's `tiger1992:` progress keys. Note that list does **not** touch `state` or
`county`, which the modern loader treats as nation-level; for 1992 those rows are written
per-state, so unload additionally deletes the state's `county` rows and its own `state`
row.

Side-by-side use of both vintages remains available through the existing `target_db` /
`target_schema` parameters plus `set_tiger_reference`.

This guard is the **only** change to the modern loader path.

## Shared-helper extraction

New `src/include/us_geocoder_loader_internal.hpp` de-statics and declares, for reuse by
`loader_1992.cpp`:

`LoaderResult`, `StepTimer`, `ExtractSection`, `RenderTemplate`, `ExecuteInsert`,
`RetryableExecuteInsert`, `IsRetriableLoaderError`, `StateDirCleanup`, `ScrapeCensusIndex`,
`IsProgressDone`, `MarkProgressDone`, `DeleteProgressLike`, `ApplyTargetParams`,
`BootstrapTargetSchema`, `LookupStateFips`, `RunAnalyzeOnTigerTables`, `ZeroPad`,
`QuoteIdent`, `EnsureHttpfsIfRemote`.

Landed as its own commit, a **pure move with no behaviour change**, verified green against
the full suite before any 1992 code is added. This is the highest-risk commit in the work —
it touches a critical file — so it is kept mechanical and isolated.

## Tests

Fixtures live under `test/data/tiger1992/`, hand-built so every expected value is known:

```
test/data/tiger1992/34/34999/TGR34999.F51   extracted-tree fixture (text)
                            .F52 .F54 .F55 .F56 .F5A .F5I
test/data/tiger1992/34/TGR92S34.NAM
test/data/tiger1992/34/34998.zip            ~2 KB, extraction fixture
```

Fixture `featnames` rows must populate all six precomputed columns — see CLAUDE.md
§ Common traps.

| test | gated | covers |
|---|---|---|
| `tiger1992_fixedwidth.test` | **no** | `substr` field extraction, CFCC→MTFCC map, node-ID arithmetic, L/R address unpivot — pure SQL, no geometry, no `spatial` |
| `tiger1992_load.test` | yes | full loader against the extracted fixture tree; row counts and values per table |
| `tiger1992_zip.test` | yes | miniz extraction from the committed zip |
| `tiger1992_geocode.test` | yes | end-to-end `tiger.geocode()` — rating and coordinates |
| `tiger1992_guard.test` | yes | vintage-mixing refusal, both directions |

Gated tests sit behind the existing `TIGER_TEST_EXTENSIONS=1` sentinel, because building
`edges.the_geom` requires `ST_MakeLine`/`ST_Point` and `tiger_schema` requires the
`GEOMETRY` type — the 1992 loader depends on `spatial` exactly as the modern one does.
`tiger1992_fixedwidth.test` is deliberately structured to avoid geometry so that the parse
layer — where most of the new logic lives — has coverage in default CI.

Run the full suite after any SQL change; individual file runs miss regression interactions.

## Docs

- **`docs/tiger1992.md`** (new) — quickstart, capability matrix, vintage caveats.
- **`docs/api.md`** — function reference entries for the four new functions, with the
  `require_containment := 'none'` constraint called out.
- **`docs/pg_parity.md`** — new design decision **D15** recording the vintage path and its
  deliberate divergences.
- **`CLAUDE.md`** — the new module, and D-F above so nobody re-derives the GDAL TIGER driver.

## Known limitations — document, do not fix

- **Block GEOIDs are 1990-vintage and 14 *or* 15 characters.** 1990 block codes are three
  digits plus an optional suffix (`159` vs `159A`); modern `blockce20` is always four.
  Kept raw rather than padded, because padding would conflate distinct blocks.
- **`pretypabrv` / `prequalabr` always NULL.** Prefix types and qualifiers stay inside
  `FENAME`, so "Avenue B" / "Calle Ocho" / "County Road 5" shapes match less precisely than
  with modern TIGER.
- **`require_containment` must be `'none'`.** Any other value filters on
  `guaranteed_*`, which is always `false`, returning zero rows.
- **`reverse_geocode`, `zcta5`, and `geocode_location`'s city/ZIP-only tiers are
  non-functional.** All need area polygons.
- **Tract/block are 1990 census geography** living in columns named `*20`.

## Validation evidence

All of the following was measured on Warren County, NJ (`34041`) before this design was
written, loading 1992 data into the real `tiger` schema and geocoding against it.

- Parse: 14,460 RT1 records, 35,588 RT2 shape points, 14,460 RTI links, 4,377 RTA polygons,
  1,380 RT5 names. Bounding box 40.592–41.094 N, −75.204 to −74.768 W — exactly Warren
  County.
- Geometry: 14,460 linestrings, **all** `ST_IsValid`, 2–114 points each, ≈1,257 road miles
  for `CFCC LIKE 'A%'`.
- Node identity: 10,144 distinct coordinate-derived nodes, 8,913 shared by 2+ edges — a
  well-formed planar graph, confirming the synthesized-ID approach.
- Loaded schema: 14,460 `edges`, 4,377 `faces`, 9,670 `featnames`, 5,198 `addr`,
  500 `place`, 23 `cousub`, 20 `zip_state`, 32 `zip_lookup_base`.
- Forward geocoding, four addresses, all rating 15 with correct township and ZIP:

  | input | matched | lon / lat | block GEOID |
  |---|---|---|---|
  | 77 Stillwater Rd 07825 | Stillwater Road, Hardwick | −74.926869, 40.997688 | 34041031102142 |
  | 43 Mohican Rd 07825 | Mohican Road, Blairstown | −74.977716, 40.998912 | 34041031101148 |
  | 15 Cedarville Rd 07825 | Cedarville Road, Blairstown | −74.964387, 40.995051 | 34041031101159 |
  | 60 Maple Ln 07825 | Maple Lane, Hardwick | −74.962967, 41.007673 | 34041031102139 |

- `edge_containment`: 14,324 of 14,460 edges carry a tract GEOID.
- Negative controls, confirming the documented limitations: city-only and ZIP-only input
  returned 0 rows; `reverse_geocode` returned 0 rows.
- `read_csv` cannot read inside a zip (`zip://` and `/vsizip/` both fail — `/vsizip/` is a
  GDAL VSI path usable only by `ST_Read`), which is why extraction is required.

Additional facts established while planning, each of which removes a feared failure mode:

- **No TLID duplication across county files.** Warren (`34041`) and adjacent Sussex
  (`34037`) share **zero** TLIDs out of 14,460 and 19,236 records. Census splits complete
  chains at county boundaries, so each chain belongs to exactly one county file. **No
  dedupe step is needed**, and `(statefp, tlid)` stays unique across a state load.
- **Signed coordinate fields cast directly.** `'+40996043'::BIGINT` → `40996043` and
  `' -74960022'::BIGINT` → `-74960022`. DuckDB accepts both the leading `+` that TIGER
  writes on latitudes and the leading spaces from right-justified fields, so no
  sign-stripping is required.
- **Empty fixed-width files are safe.** `read_csv` over a zero-byte file returns 0 rows
  rather than erroring, which matters because some record types are absent or empty for
  some counties.

## Risks

| Risk | Mitigation |
|---|---|
| A DuckDB bump renames or un-exports the `duckdb_miniz` symbols | Verified exported today (`T`) from both the static binary and the host process, so the loadable and static builds both resolve them with headers only. A bump that breaks this is a link error on first build, not a runtime bug |
| Shared-helper extraction regresses the modern loader | Pure-move commit, full suite green before any 1992 code |
| Windows path and CRLF handling in the new ingest path | All I/O via DuckDB `FileSystem`; `rtrim(line, chr(13))` in the scan; committed zip fixture exercises extraction on every platform in the gated suite |
| Polygon-dependent code paths fail confusingly rather than cleanly | Documented in `docs/tiger1992.md`; negative-control assertions in the test suite |
| 1992 data quality varies by county (address coverage is "few/some/half/majority" per Appendix A) | Out of our control; note it in `docs/tiger1992.md` so low match rates in rural counties are not read as bugs |

## Non-goals

Polygonization is the single largest deferred piece. It was measured as feasible during the
feasibility pass — `ST_Polygonize` over each polygon's bounding chains reconstructed
4,259 / 4,377 (97.3%) of Warren County's faces as clean single rings, and **100% of those
contained the Census-published RTP internal point**, independently validating the
reconstruction. The remaining 2.7% are holes, islands, or boundaries clipped by the county
file edge. Dissolving by FMCD / FPL yielded 23 cousub and 10 place polygons; dissolving
faces by adjacent-edge ZIP produced usable pseudo-ZCTA centroids (08865 → Phillipsburg,
07840 → Hackettstown, 07825 → Blairstown).

That work would unlock `guaranteed_*`, `reverse_geocode`, and `geocode_location`'s
city/ZIP-only tiers. It is explicitly out of scope here and should be its own spec.
