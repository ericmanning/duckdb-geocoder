#pragma once

// tiger.geocode_batch — C++ table-in-out function for batched geocoding.
//
// Implements the LATERAL form `FROM bench_input b, LATERAL tiger.geocode_batch(b.addr_str)`
// with cross-call input buffering: rows are accumulated until a threshold,
// then partitioned by resolved statefp, then dispatched per-state with a
// literal `WHERE statefp = '<lit>'` so the planner uses ART + zone-map
// pruning on the unified TIGER tables instead of building giant hashes.
//
// Background on why this exists rather than the SQL macro: the SQL macro
// `tiger.geocode_address_impl` joins big tables on `f.statefp = p.statefp`.
// With dynamic per-row input via LATERAL, DuckDB can't push the runtime
// statefp value into the big-table scans, so it materializes 50+ GB of
// hash builds. With a literal statefp baked into the SQL text per state,
// the planner constant-folds and uses indexed scans.
//
// See CLAUDE.md "Roadmap / deferred" for the per-state-table sharding
// alternative (deferred to v0.2).
//
// Current status (May 2026): SCAFFOLD ONLY. The function is registered and
// works end-to-end as a pass-through; per-state dispatch + actual geocoding
// pipeline is the next milestone.

namespace duckdb {

class ExtensionLoader;

namespace us_geocoder {

void RegisterGeocodeBatchFunction(ExtensionLoader &loader);

} // namespace us_geocoder
} // namespace duckdb
