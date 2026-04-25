# Parity inputs

Hand-transcribed CSV mirroring PG's `geocode_regress.sql` test queries. One row per geocode call: `test_id, raw_address, max_n`. Used by [`scripts/parity/run_geocode_regress.sh`](../../../scripts/parity/run_geocode_regress.sh) instead of regex-extracting from the SQL — robust against PG SQL syntax variations and easier to extend.

## Coverage

- All `T*` tests (T1–T18b) — simple `geocode('addr', N)` calls.
- All `#TB1`, `#1073*`, `#1074*`, `#1076*`, `#1087*`, `#1112*`, `#1113*`, `#1145*` — batched-VALUES tests transcribed as one row per VALUES entry (so `#TB1` appears twice, once per address).
- `#2899` — Westbrook CT (won't match unless CT data is loaded into the reference DB).

## Skipped from PG

- `#1070a`, `#1070b` — pass a `restrict_geom` polygon. The harness doesn't support that arg yet; would need MA `place WHERE name='Lynn'` geometry pre-resolved.
- `#1333a`, `#1333b`, `#1392a`, `#1392b` — call `geocode_intersection`, not `geocode`. Need a separate harness.

## Updating

When upstream PG adds new test cases, transcribe by hand into this CSV. The PG file in [`../upstream/geocode_regress.sql`](../upstream/geocode_regress.sql) is vendored verbatim (read-only); this directory holds our extraction.
