#pragma once

// Shared, bind-data-free helpers extracted from loader.cpp so a second
// ingestion module (loader_1992.cpp, for 1992-vintage TIGER/Line files) can
// reuse the modern loader's template-rendering, INSERT-execution,
// progress-ledger, and temp-dir orchestration helpers instead of
// duplicating them. Pure extraction: every declaration/definition here is
// copied verbatim (less `static`) from src/loader.cpp; no behaviour
// changed.
//
// Deliberately NOT moved here: LoaderBindData, ApplyTargetParams,
// BootstrapTargetSchema, RunAnalyzeOnTigerTables. Those take
// LoaderBindData (or a reference to it), which stays private to
// loader.cpp — the 1992 loader defines its own bind-data struct and its
// own narrower target-param application instead of sharing this one.

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "us_geocoder_parallel_download.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace duckdb {
namespace us_geocoder {

// One row of a loader table function's output.
struct LoaderResult {
	std::string step;
	int64_t rows;
};

// Live progress logging. Writes to stderr (and flushes) so a long-running
// CALL load_tiger_all_states() shows per-file progress as it runs, instead
// of the whole result vector flooding out at the end.
//
// Not in task-1-brief.md's symbol list, but promoted here (de-static'd)
// because StepTimer::Done() below calls it, and StepTimer is required to
// move in full. See task-1-report.md for the rationale.
void LogProgress(const std::string &prefix, const std::string &step, int64_t rows, double secs);

// RAII helper: time an ExecuteInsert call and log it on completion. Use as:
//   { auto _t = StepTimer(prefix, step_label);
//     int64_t rows = ExecuteInsert(...);
//     out.push_back(...);
//     _t.Done(rows); }
struct StepTimer {
	std::string prefix;
	std::string step;
	std::chrono::steady_clock::time_point t0;
	bool done = false;
	StepTimer(std::string p, std::string s)
	    : prefix(std::move(p)), step(std::move(s)), t0(std::chrono::steady_clock::now()) {
	}
	void Done(int64_t rows) {
		auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		LogProgress(prefix, step, rows, secs);
		done = true;
	}
	~StepTimer() {
		// Intentionally silent on the failure path. The IOException thrown
		// by ExecuteInsert already includes the step label (e.g. "us_geocoder
		// loader (county_faces:019): IO Error..."), so a separate FAILED log
		// here was redundant and noisy under sqllogictest's `statement error`
		// path (where exceptions are an expected outcome).
	}
};

// =====================================================================
// Template extraction
// =====================================================================

// Extracts one section from loader_templates.sql.in. Sections are delimited
// by "-- @SECTION:name@" lines. Returns the text between the marker and the
// next "-- @SECTION:" marker (or EOF). Throws if the section is missing.
std::string ExtractSection(const std::string &all, const std::string &name);

std::string RenderTemplate(const std::string &section,
                           const std::vector<std::pair<std::string, std::string>> &subs);

// Run one INSERT statement against a Connection, return the number of rows
// inserted (or 0 if the statement didn't produce a row count). Throws on SQL error.
int64_t ExecuteInsert(Connection &conn, const std::string &sql, const std::string &step_label);

// Heuristic: is this loader error worth retrying? Network/GDAL transients
// (refused connection, corrupted-mid-download zip, /vsicurl/ open failure)
// retry; SQL syntax / catalog errors should fail fast. Hints are matched
// case-insensitively — error strings come from a mix of GDAL (paths use
// lowercase "gdal/"), DuckDB ("HTTP"), and curl, and we don't want a
// missed case to silently fail the whole load.
bool IsRetriableLoaderError(const std::string &msg);

// Generate a unique cache-bust token. Cloudflare's cache key includes the
// query string, so appending ?cb=<token> guarantees a fresh fetch from the
// origin (see RetryableExecuteInsert below).
//
// Not in task-1-brief.md's symbol list, but promoted here (de-static'd)
// because RetryableExecuteInsert's template body calls it, and that body
// must live in this header. See task-1-report.md for the rationale.
uint64_t MakeCacheBust();

// Wrap ExecuteInsert with retry. The Render callable is invoked once per
// attempt with a cache-bust token: 0 on the first attempt (CDN cache OK),
// fresh on every retry. The happy path keeps Cloudflare's edge cache; only
// failures pay the cache-miss + retry cost. Without this gating, every
// request hits origin and Census rate-limits us. Backoff: 2s, 5s, 15s;
// non-network errors fail fast.
//
// Defined (not just declared) here, not `static`: RenderFn makes this a
// template, so it must live in a header — with external linkage — to be
// instantiable from another translation unit (loader_1992.cpp).
template <typename RenderFn>
int64_t RetryableExecuteInsert(Connection &conn, RenderFn render, const std::string &step_label) {
	constexpr int kAttempts = 3;
	const int kBackoffSec[] = {2, 5, 15};
	for (int attempt = 0; attempt < kAttempts; ++attempt) {
		uint64_t cb = (attempt == 0) ? 0 : MakeCacheBust();
		try {
			return ExecuteInsert(conn, render(cb), step_label);
		} catch (const IOException &e) {
			std::string msg = e.what();
			bool last = (attempt + 1 == kAttempts);
			if (last || !IsRetriableLoaderError(msg)) {
				throw;
			}
			fprintf(stderr, "[us_geocoder] %s: HTTP/GDAL error, retry %d/%d in %ds\n", step_label.c_str(), attempt + 1,
			        kAttempts - 1, kBackoffSec[attempt]);
			fflush(stderr);
			std::this_thread::sleep_for(std::chrono::seconds(kBackoffSec[attempt]));
		}
	}
	throw IOException("us_geocoder loader (%s): unreachable retry loop", step_label);
}

// =====================================================================
// Progress ledger
// =====================================================================

bool IsProgressDone(Connection &conn, const std::string &data_loc, const std::string &section);
void MarkProgressDone(Connection &conn, const std::string &data_loc, const std::string &section);
void DeleteProgressLike(Connection &conn, const std::string &data_loc, const std::string &prefix);

// =====================================================================
// Misc helpers
// =====================================================================

std::string ZeroPad(const std::string &s, size_t width);

// DuckDB identifier quoting: wrap in double quotes and escape any embedded ".
// Used for target_db / target_schema, which may contain mixed case or awkward
// names the user chose for their ATTACH alias.
std::string QuoteIdent(const std::string &s);

// Resolve state abbrev → 2-digit FIPS via the lookup table.
std::string LookupStateFips(Connection &conn, const std::string &schema, const std::string &abbrev);

// If source is an HTTP URL, make sure httpfs is loaded so GDAL's vsicurl
// can reach it. Auto-load is a no-op if httpfs is already resident.
void EnsureHttpfsIfRemote(DatabaseInstance &db, const std::string &source);

// RAII wrapper for a temp-dir lifetime. Destructor removes the directory
// unless Disarm() was called (e.g. on failure — keep zips for retry).
// Shared by both DoLoadState and DoLoadNation parallel preludes.
class StateDirCleanup {
public:
	StateDirCleanup(FileSystem &fs, std::string path) : fs_(fs), path_(std::move(path)) {
	}
	~StateDirCleanup() {
		if (!armed_ || path_.empty()) {
			return;
		}
		try {
			fs_.RemoveDirectory(path_);
		} catch (...) {
			fprintf(stderr, "[us_geocoder] WARN: failed to remove temp dir %s\n", path_.c_str());
			fflush(stderr);
		}
	}
	void Disarm() {
		armed_ = false;
	}

private:
	FileSystem &fs_;
	std::string path_;
	bool armed_ = true;
};

// Temp-dir resolution + Census-index scraping, shared so both loaders name
// scratch dirs the same way. These were never `static` in loader.cpp —
// they already live with external linkage in parallel_download.cpp,
// declared in the header included above. Re-declared here (identical
// signatures) purely so this header is a self-describing single surface
// for loader_1992.cpp; see task-1-report.md for why task-1-brief.md's
// framing of these three as loader.cpp statics was incorrect.
// `temp_dir`/`explicit_temp_dir` empty => OS temp. MakeStateTempDir's
// `year` is 1992 for the vintage path.
std::string ResolveTempBase(ClientContext &context, const std::string &explicit_temp_dir);
std::string MakeStateTempDir(ClientContext &context, const std::string &base, const std::string &state_label,
                             int year);
std::vector<std::string> ScrapeCensusIndex(ClientContext &context, const std::string &index_url,
                                           const std::regex &name_pattern, const ParallelDownloadOptions &opts);

} // namespace us_geocoder
} // namespace duckdb
