#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace us_geocoder {

// Registers load_tiger_1992_state / _states / _all_states and
// unload_tiger_1992_state. Must be called after the TIGER schema SQL has
// run so the target tables exist.
void RegisterLoader1992Functions(ExtensionLoader &loader, const std::string &tiger_schema);

} // namespace us_geocoder
} // namespace duckdb
