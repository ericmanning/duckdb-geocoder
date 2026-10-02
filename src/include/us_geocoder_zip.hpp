#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace us_geocoder {

struct ZipEntry {
	std::string name; // basename as stored in the archive
	int64_t bytes;    // uncompressed size written
};

// Extract entries from `zip_path` into `dest_dir`, creating `dest_dir` if
// needed. `wanted` holds basenames to extract, matched case-insensitively;
// an empty `wanted` extracts every entry. Directory entries are skipped.
// All I/O goes through `fs` so Windows paths and permissions behave the
// same as the rest of the loader. Throws IOException on a malformed or
// unreadable archive.
std::vector<ZipEntry> ExtractZipEntries(FileSystem &fs, const std::string &zip_path,
                                        const std::string &dest_dir,
                                        const std::vector<std::string> &wanted);

// Registers us_geocoder_unzip(zip_path, dest_dir) -> TABLE(entry, bytes).
void RegisterZipFunctions(ExtensionLoader &loader);

} // namespace us_geocoder
} // namespace duckdb
