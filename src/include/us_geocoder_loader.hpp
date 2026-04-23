#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace us_geocoder {

// Registers load_tiger_nation and load_tiger_state table functions.
// Must be called after the TIGER schema SQL has been executed so the
// target tables exist.
void RegisterLoaderFunctions(ExtensionLoader &loader, const std::string &tiger_schema);

} // namespace us_geocoder
} // namespace duckdb
