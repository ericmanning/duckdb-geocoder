#pragma once

#include "duckdb/common/string.hpp"
#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace duckdb {
class ClientContext;
class DatabaseInstance;

namespace us_geocoder {

// One file to fetch: URL + the local path it should land at.
struct DownloadTarget {
	std::string url;
	std::string dest_path;
};

struct ParallelDownloadOptions {
	int workers = 16;
	int max_attempts = 3; // total attempts per file (1 initial + 2 retries)
	// Backoff seconds between attempts. Indices into [0..max_attempts-1).
	// Defaults to {2, 5}; mirrors RetryableExecuteInsert.
	std::vector<int> backoff_seconds = {2, 5};
	// Label for log messages (e.g. "RI" or "nation"). Used only in fprintf.
	std::string log_prefix;
};

struct ParallelDownloadResult {
	size_t files_total = 0;
	size_t files_downloaded = 0;
	size_t files_skipped = 0; // already on disk, size > 0
	int64_t bytes_downloaded = 0;
	double elapsed_seconds = 0;
};

// Resolve a Census-CDN HTML directory index to a list of matching filenames.
// `name_pattern` is matched against each anchor's href in the returned HTML.
// Throws IOException on transport failure (after retries).
std::vector<std::string> ScrapeCensusIndex(ClientContext &context, const std::string &index_url,
                                           const std::regex &name_pattern, const ParallelDownloadOptions &opts);

// Download every (url, dest_path) in `targets` in parallel.
// - dest_path's parent directory is created if missing.
// - Each file is written via <dest>.tmp and atomic-renamed on success.
// - Skips files that already exist on disk with size > 0 (idempotency).
// - On any file exhausting all retries: in-flight workers drain their current
//   file, the function throws IOException naming the first failed file.
// - The temp dir is NOT cleaned up here; that's the caller's RAII concern.
ParallelDownloadResult ParallelDownload(ClientContext &context, const std::vector<DownloadTarget> &targets,
                                        const ParallelDownloadOptions &opts);

// Helper: resolve a usable temp-dir base. If `explicit_temp_dir` is non-empty,
// returns it verbatim (caller-pinned). Else tries the OS temp dir conventions:
// $TMPDIR, $TEMP, $TMP, "/tmp", "."
std::string ResolveTempBase(ClientContext &context, const std::string &explicit_temp_dir);

// Helper: make a state-specific scratch dir under `base`, named
// `us_geocoder_tiger_<YEAR>_<STATE>_<pid>_<ts>`. Creates the directory and
// any missing parents. Returns the absolute path. The caller is responsible
// for `FileSystem::RemoveDirectory(path)` when done (RAII-friendly).
std::string MakeStateTempDir(ClientContext &context, const std::string &base, const std::string &state_label, int year);

} // namespace us_geocoder
} // namespace duckdb
