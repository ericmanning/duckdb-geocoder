# 1992 TIGER/Line vintage

`us_geocoder` can load historical 1992-vintage TIGER/Line files
(<https://www2.census.gov/geo/tiger/TIGER1992/>) into the same `tiger` schema the modern
loader uses, so `tiger.geocode()` runs unchanged against 1990s-era streets and links results
to 1990-census block/tract geography. The motivating use case is historical geocoding:
resolving an address as it existed in 1992 rather than as it exists today. 1992 TIGER predates
area polygons entirely, so this path is **forward geocoding only** — see the capability matrix
below for exactly what that excludes.

## Quickstart

```sql
CALL load_tiger_1992_state('NJ');
```

Loads every county in New Jersey from the Census CDN into local `tiger.*`. No `load_tiger_nation`
equivalent is needed first — unlike the modern loader, 1992 has no nation-level product, so
`state` and `county` rows are populated per-state as part of this call.

```sql
SELECT g.rating, (g.adr).street_name, ST_AsText(g.geom), g.block_geoid
FROM tiger.geocode(
    {'address': 50, 'street_name': 'Fixture', 'street_type': 'Rd',
     'pre_dir': NULL, 'post_dir': NULL, 'internal': NULL,
     'location': NULL, 'state_abbrev': 'NJ', 'zip': '07825'}::tiger.geocode_input,
    1, NULL, 'none'
) g;
```

`require_containment` must stay `'none'` for 1992 data — see [Vintage caveats](#vintage-caveats).

## Capability matrix

| Entry point | Status | Reason |
|---|---|---|
| `tiger.geocode()` (full forward path) | **Works** | Edges, featnames, addr all populated |
| `geocode_batch` | **Works** | Same tables, dispatches like any other state |
| `tiger.geocode_intersection()` | **Works** | Node IDs are synthesized arithmetically from endpoint coordinates (1992 has no `TNIDF`/`TNIDT`), so the shared-node join works exactly as with modern data |
| `block_geoid` / `tract_geoid` / `blkgrp_geoid` on a match | **Works** | Sourced from `edge_containment`, computed from 1990 census codes carried in RTA |
| `tiger.reverse_geocode()` | **Does not work** — returns zero rows | 1992 ships no face polygons; there is nothing to search outward from |
| `geocode_location`'s ZCTA-centroid tier (ZIP-only input) | **Does not work** | ZCTAs did not exist until 2000; `zcta5` is not populated for 1992 |
| `geocode_location`'s place-centroid tier (city-only input) | **Does not work** — returns zero rows | `place.the_geom` is NULL for 1992, and the tier filters on `the_geom IS NOT NULL` so a NULL-geometry row is never emitted as a match |
| `require_containment := 'block'` / `'tract'` / `'blkgrp'` | **Does not work** — returns zero rows | Filters on `containment_guaranteed`, which is a hard `false` for every 1992 row (no polygons to prove containment against) |

## Vintage caveats

- **Block GEOIDs are 1990-vintage and 14 *or* 15 characters.** 1990 block codes are three
  digits plus an optional suffix (`159` vs `159A`); modern `blockce20` is always four. Kept
  raw rather than padded, because padding would conflate distinct blocks. `tract_geoid` is
  always 11 and `blkgrp_geoid` always 12. Verified on real 1992 Rhode Island data
  (83,588 `edge_containment` rows): block 14 chars ×78,130 / 15 chars ×4,824, tract 11 chars
  ×82,954, block group 12 chars ×82,954 — nothing outside those widths.
- **The 1990 tract suffix is zero-filled, not trimmed.** 1992 RTA records store `CTBNA` as a
  4-digit basic tract plus a 2-char suffix that is *blank*-filled when the tract has no
  suffix (`'0301  '`), while the canonical 1990 tract code zero-fills it (`030100`). The
  loader zero-fills; `150 Westminster St, Providence RI 02903` resolves to
  `tract_geoid = 44007000800`, which joins 1990 census tract tables directly.
- **`pretypabrv` / `prequalabr` always NULL.** 1992 has only four name fields (FEDIRP, FENAME,
  FETYPE, FEDIRS) — prefix types and qualifiers stay inside `FENAME`, so "Avenue B" /
  "Calle Ocho" / "County Road 5" shapes match less precisely than with modern TIGER.
- **`require_containment` must be `'none'`.** Any other value filters on `guaranteed_*`,
  which is always `false`, returning zero rows.
- **`reverse_geocode`, `zcta5`, and `geocode_location`'s city/ZIP-only tiers are
  non-functional.** All need area polygons.
- **Tract/block are 1990 census geography** living in columns named `*20` — the column names
  are retained from the modern schema to avoid a schema fork, but the values underneath are
  1990-vintage, not 2020.

## Source layouts

`load_tiger_1992_state` / `_states` / `_all_states` accept a `source` parameter in three
forms, auto-detected in this order:

1. **Remote HTTP** (`source` starts with `http://` or `https://`, or is omitted — default
   `https://www2.census.gov/geo/tiger/TIGER1992`) — scrapes the per-state directory index for
   county zip names, downloads them, and extracts in a temp dir.
2. **Local nested zips** — `<source>/<ss>/<ssccc>.zip`, the same layout as the Census mirror
   itself, just already on disk. `<ss>` is the 2-digit state FIPS code (e.g. `34` for NJ, not
   the postal abbreviation).
3. **Local already-extracted tree** — `<source>/<ss>/<ssccc>/TGR<ssccc>.F51` (and its sibling
   `.F52`/`.F54`/`.F55`/`.F56`/`.F5A`/`.F5I` files in the same directory). Detected when the
   zip from mode 2 isn't present but the directory is. Useful for a CD-ROM mirror or any
   pre-extracted local copy — this is also what the test fixtures use.

   Modes 2 and 3 both additionally need the state's **Geographic Name File** at the state
   level: either `<source>/<ss>/TGR92S<ss>.NAM` or `<source>/<ss>/OtherFiles.zip` (which
   contains it). It is what populates `state`, `county`, `place` and `cousub`, and it does
   *not* live inside the per-county zips — so pre-extracting only the `TGR<ssccc>.F5*` files
   gives a load that fails with `no TGR92S<ss>.NAM`. On the Census mirror it is
   `TIGER1992/<ss>/OtherFiles.zip`; copying that file next to the county directories is
   enough.

[`us_geocoder_unzip(zip_path, dest_dir)`](api.md#us_geocoder_unzipzip_path-varchar-dest_dir-varchar--tableentry-varchar-bytes-bigint)
is a standalone extraction utility (not specific to 1992) if you want to pre-extract a local
mirror into mode-3 form yourself.

## Data quality varies by county

1992 address-range coverage is not uniform. Appendix A of the Census TIGER/Line 1992
documentation rates each county's address coverage as "few", "some", "half", or "majority" —
rural counties in particular often fall in the lower bands. A low match rate against a rural
county's 1992 data is expected behavior reflecting the source data's real coverage, not a bug
in the loader or the geocoder.
