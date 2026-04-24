-- Geocode a CSV of free-form addresses via the 1-arg tiger.geocode(VARCHAR)
-- shortcut, which internally calls tiger.from_pagc() (PAGC standardizer).
--
-- Input format: two columns, `id` + `address` (single free-form string).
-- Writes geocoded_raw.csv with lat/lng and the 2020 block/tract/blkgrp GEOIDs.
--
-- Usage:
--   ./build/release/duckdb -f scripts/demo/geocode_raw.sql

-- Attach reference DB + repoint local tiger.* at it.
ATTACH 'tiger_nj_2025.duckdb' AS ref (READ_ONLY);
CALL set_tiger_reference('ref');

COPY (
    -- Rename the CSV column to `_raw` to avoid two DuckDB binder traps:
    --   (a) `i.address` through LATERAL is mis-parsed as struct-field access,
    --   (b) unqualified `address` clashes with the SELECT list alias below
    --       ("Column cannot be referenced before it is defined").
    -- Unqualified `_raw` resolves through LATERAL cleanly.
    WITH inputs AS (
        SELECT id, address AS _raw
        FROM read_csv('scripts/demo/sample_addresses_raw.csv',
                      header = true, auto_detect = true)
    ),
    candidates AS (
        SELECT
            inputs.id,
            inputs._raw AS address,
            g.rating,
            ST_X(g.geom) AS lng,
            ST_Y(g.geom) AS lat,
            g.block_geoid,
            g.tract_geoid,
            g.blkgrp_geoid,
            g.containment_guaranteed
        FROM inputs
        CROSS JOIN LATERAL tiger.geocode(_raw) AS g
    ),
    ranked AS (
        SELECT *, ROW_NUMBER() OVER (PARTITION BY id ORDER BY rating) AS rn
        FROM candidates
    )
    SELECT id, address, rating, lat, lng,
           block_geoid, tract_geoid, blkgrp_geoid, containment_guaranteed
    FROM ranked
    WHERE rn = 1
    ORDER BY id
) TO 'geocoded_raw.csv' (HEADER, DELIMITER ',');

-- Echo results.
SELECT * FROM read_csv('geocoded_raw.csv', header = true, auto_detect = true)
ORDER BY id;
