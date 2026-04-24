-- Geocode a CSV of addresses against a prebuilt reference DB.
-- Expected input schema (edit the struct builder below if yours differs):
--   id, address_num, street_name, street_type, location, state, zip
--
-- Writes: geocoded.csv with one row per input (best match, lowest rating).
--
-- Usage:
--   ./build/release/duckdb -f scripts/demo/geocode_addresses.sql
--
-- Paths are hardcoded as literals because DuckDB's ATTACH and COPY TO
-- statements don't accept variables. Edit the three literal paths below
-- if yours are different.

-- ---- Attach the reference DB + repoint local tiger.* at it --------------
ATTACH 'tiger_nj_2025.duckdb' AS ref (READ_ONLY);
CALL set_tiger_reference('ref');

-- ---- Geocode ------------------------------------------------------------
COPY (
    WITH inputs AS (
        SELECT
            id,
            address_num || ' ' ||
                COALESCE(street_name || ' ', '') ||
                COALESCE(street_type || ', ', '') ||
                COALESCE(location || ', ', '') ||
                COALESCE(state || ' ', '') ||
                COALESCE(zip, '')                  AS input_address,
            CAST({
                address:      address_num,
                street_name:  street_name,
                street_type:  street_type,
                pre_dir:      NULL,
                post_dir:     NULL,
                location:     location,
                state_abbrev: state,
                zip:          zip
            } AS tiger.geocode_input)              AS gi
        FROM read_csv('scripts/demo/sample_addresses.csv',
                      header = true, auto_detect = true)
    ),
    candidates AS (
        -- Note: `gi` is referenced *unqualified* inside the LATERAL call.
        -- DuckDB's binder mis-parses `inputs.gi` / `i.gi` as struct-field
        -- access on the row when the outer source is a CTE. The unqualified
        -- column name resolves correctly.
        SELECT
            inputs.id,
            inputs.input_address,
            g.rating,
            ST_X(g.geom)                              AS lng,
            ST_Y(g.geom)                              AS lat,
            g.block_geoid,
            g.tract_geoid,
            g.blkgrp_geoid,
            g.containment_guaranteed
        FROM inputs
        CROSS JOIN LATERAL tiger.geocode(
            gi,
            3,       -- max_results
            NULL,    -- no spatial restriction
            'none'   -- require_containment: 'none' | 'block' | 'tract' | 'blkgrp'
        ) AS g
    ),
    ranked AS (
        SELECT *, ROW_NUMBER() OVER (PARTITION BY id ORDER BY rating, lat, lng) AS rn
        FROM candidates
    )
    SELECT id, input_address, rating, lat, lng,
           block_geoid, tract_geoid, blkgrp_geoid, containment_guaranteed
    FROM ranked
    WHERE rn = 1
    ORDER BY id
) TO 'geocoded.csv' (HEADER, DELIMITER ',');

-- Echo a summary so the CLI shows something useful.
SELECT * FROM (
    SELECT id, input_address, rating, lat, lng, containment_guaranteed
    FROM read_csv('geocoded.csv', header = true, auto_detect = true)
    ORDER BY id
);
