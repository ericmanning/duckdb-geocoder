-- Bootstrap the PG-parity DB: PostGIS, fuzzystrmatch, address_standardizer,
-- and the postgis_tiger_geocoder extension. tiger_geocoder's CREATE EXTENSION
-- creates the tiger schema with all loader/geocoder functions.

CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS fuzzystrmatch;
CREATE EXTENSION IF NOT EXISTS postgis_tiger_geocoder;
CREATE EXTENSION IF NOT EXISTS address_standardizer;

-- The tiger_geocoder bundle ships pagc_tables.sql which defines and populates
-- the tiger.pagc_lex/pagc_gaz/pagc_rules tables that pagc_normalize_address
-- reads from. CREATE EXTENSION runs that automatically as part of the
-- extension install, so by this point the rule data is in place. Belt-and-
-- suspenders: explicitly call the install_pagc_tables() shim if it exists.
DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
        WHERE n.nspname = 'tiger' AND p.proname = 'install_pagc_tables'
    ) THEN
        PERFORM tiger.install_pagc_tables();
    END IF;
END $$;

-- PG's tiger loader expects a `tiger_data` schema for the partition tables
-- it creates per-state (state_all, county_all, ma_edges, mn_addr, etc.).
-- Per PG's README install steps.
CREATE SCHEMA IF NOT EXISTS tiger_data;

-- Set DB-level search_path so every connection (including the generated
-- loader script's psql -c calls) sees tiger/tiger_data without explicit SET.
ALTER DATABASE parity SET search_path = "$user", public, tiger, tiger_data, topology;
