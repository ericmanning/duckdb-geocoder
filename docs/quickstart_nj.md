# Quickstart: geocode New Jersey addresses

End-to-end recipe for a single state, fetching TIGER over HTTP from Census directly. No local mirror.

## 1. Install + load extensions

One-time per machine:

```sql
INSTALL us_geocoder             FROM community;
INSTALL us_address_standardizer FROM community;
```

Each session:

```sql
LOAD us_geocoder;
```

`spatial`, `httpfs`, and `us_address_standardizer` auto-load. `soundex` is vendored — no `splink_udfs` dependency.

## 2. Persist into a real DB

```sql
ATTACH '/path/to/nj_geocoder.duckdb' AS db;
USE db;
```

In-memory works for one-off runs but you'll refetch ~835 MB of shapefiles every session.

## 3. Bootstrap nation-level lookups

```sql
CALL load_tiger_nation();
```

~30 s. Populates state/county/place/ZIP→state lookups. Required before any per-state load — those tables drive county-level fanout.

No `source` argument → defaults to `https://www2.census.gov/geo/tiger/TIGER2025/`.

## 4. Load NJ

```sql
CALL load_tiger_state('NJ');
```

~5–8 min on a fast connection. NJ is 21 counties, ~835 MB of shapefile zips total.

The loader is resumable: kill it mid-run and re-call — it skips already-completed work via the `loader_progress` table. `ANALYZE` runs at the end automatically so the planner has stats.

## 5. Batch geocode

Two input shapes accepted:

### Free-form `addr_str`

```sql
SELECT id, rating, lng, lat, adr_text, block_geoid
FROM geocode_batch((
    SELECT * FROM (VALUES
        (1, '60 Washington St, Hoboken NJ 07030'),
        (2, '101 Hudson St, Jersey City NJ 07302'),
        (3, '1 Boswell Rd, Princeton NJ 08540')
    ) AS t(id, addr_str)
));
```

The string is parsed via `tiger.from_pagc` internally.

### Pre-parsed fields

Skip `from_pagc` when you already have clean fields. Recognized column names: `address`, `street_name`, `street_type`, `pre_dir`, `post_dir`, `internal`, `location`, `state_abbrev`, `zip`.

```sql
SELECT id, rating, lng, lat, adr_text
FROM geocode_batch((
    SELECT 1 AS id,
           60 AS address,
           'Washington' AS street_name,
           'St' AS street_type,
           'Hoboken' AS location,
           'NJ' AS state_abbrev,
           '07030' AS zip
));
```

### Production: existing table

Any columns *not* in the recognized-input set are preserved as passthrough in the output, so you can carry your row id / customer id / etc. through:

```sql
SELECT *
FROM geocode_batch((
    SELECT id, customer_id, addr_str
    FROM my_addresses
    WHERE addr_str IS NOT NULL
));
```

## Notes for NJ-specific use

- Every input has `state='NJ'` (or resolves to NJ via ZIP) → all rows dispatch to one bucket. Max amortization: per-state slice cap (1000) is the only knob that fires.
- A 10K-row NJ-only batch runs in ~5–10 s on the shipped build.
- Multi-state-ZIP dispatch (~7% of US ZIPs cross state lines) only fires when `state_abbrev` is missing AND the ZIP appears in multiple states. If all your inputs say `'NJ'` explicitly, that path never fires.

## Output schema

```
<your passthrough columns>,
rating BIGINT,                  -- lower is better; 0 is exact, ≥100 is location-only
lng DOUBLE, lat DOUBLE,
adr_text VARCHAR,                -- pretty-printed canonical address
block_geoid VARCHAR,             -- 15-digit Census block GEOID
tract_geoid VARCHAR,             -- 11-digit
blkgrp_geoid VARCHAR,            -- 12-digit
containment_guaranteed BOOLEAN   -- true ⇒ block_geoid is correct without spatial test
```

See [api.md](api.md) for full reference, [parity.md](parity.md) for PostGIS-comparison notes.
