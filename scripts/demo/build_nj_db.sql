-- Build a portable NJ reference database by pulling TIGER/Line 2025 directly
-- from the Census CDN (via httpfs + GDAL /vsicurl/). Idempotent: re-running
-- is safe (the state loader DELETEs existing NJ rows before reinserting).
--
-- Usage:
--   ./build/release/duckdb tiger_nj_2025.duckdb -f scripts/demo/build_nj_db.sql
--
-- Timing on residential broadband (Census CDN, serial /vsicurl/ fetches):
--   load_tiger_nation:  ~30 s  (3 zips, nation-wide)
--   load_tiger_state NJ: ~5–8 min  (21 counties × 4 table types = 84 zips,
--                                  fetched serially inside GDAL)
--
-- After this finishes, tiger_nj_2025.duckdb is a standalone reference DB
-- you can attach READ_ONLY from anywhere:
--   ATTACH 'tiger_nj_2025.duckdb' AS ref (READ_ONLY);
--   CALL set_tiger_reference('ref');
--   SELECT * FROM tiger.geocode(...);

CALL load_tiger_nation();           -- defaults to https://www2.census.gov/geo/tiger/TIGER2025
CALL load_tiger_state('NJ');

-- Row counts for NJ — sanity check that data landed.
SELECT 'edges'        AS tbl, COUNT(*) AS n FROM tiger.edges     WHERE statefp = '34'
UNION ALL SELECT 'faces',            COUNT(*) FROM tiger.faces     WHERE statefp = '34'
UNION ALL SELECT 'featnames',        COUNT(*) FROM tiger.featnames WHERE statefp = '34'
UNION ALL SELECT 'addr',             COUNT(*) FROM tiger.addr      WHERE statefp = '34'
UNION ALL SELECT 'edge_containment', COUNT(*) FROM tiger.edge_containment WHERE statefp = '34'
UNION ALL SELECT 'zip_lookup_base',  COUNT(*) FROM tiger.zip_lookup_base  WHERE statefp = '34'
ORDER BY tbl;
