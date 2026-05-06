#pragma once

// geocode_batch — C++ table-in-out function for batched geocoding.
//
// Takes a table-valued input (subquery), buffers rows up to a threshold,
// partitions by resolved statefp, and dispatches one per-state SQL
// (`tiger.geocode_address_for_state('<lit>', …)`) per partition. Literal
// statefp lets the planner use ART + zone-map pruning on the unified
// TIGER tables instead of materializing nationwide-sized hash builds.
//
// Why this exists rather than just the SQL macro: `tiger.geocode_address_impl`
// joins big tables on `f.statefp = p.statefp`. With per-row dynamic input via
// LATERAL, DuckDB can't push the runtime statefp value into the big-table
// scans, so it materializes 50+ GB of hash builds. Baking the statefp in
// per-state breaks that pattern.
//
// See CLAUDE.md "Roadmap / deferred" for the per-state-table sharding
// alternative (v0.2 candidate).

namespace duckdb {

class ExtensionLoader;

namespace us_geocoder {

void RegisterGeocodeBatchFunction(ExtensionLoader &loader);

} // namespace us_geocoder
} // namespace duckdb
