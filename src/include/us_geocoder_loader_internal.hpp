#pragma once

// Shared, bind-data-free loader helpers used by BOTH ingestion modules:
// src/loader.cpp (modern TIGER) and src/loader_1992.cpp (1992-vintage
// TIGER/Line). Template rendering, INSERT execution, the progress ledger,
// temp-dir orchestration, the vintage-mixing guard, schema bootstrap and
// ANALYZE all live here so the two modules share one implementation.
//
// This started as a verbatim de-static'ing of helpers from loader.cpp, but
// it is no longer a pure extraction — do not read it as one:
//
//   * BootstrapTargetSchema / RunAnalyzeOnTigerTables moved here with
//     NARROWED signatures (plain strings instead of `const LoaderBindData
//     &`), because LoaderBindData itself stays private to loader.cpp and
//     the 1992 loader has its own bind-data struct.
//   * HasVintage1992 / HasStateRows / RefuseIfVintageMismatch /
//     RefuseIfVintage1992Present are NEW with the 1992 path; they never
//     existed in loader.cpp.
//
// Still deliberately NOT here: LoaderBindData and ApplyTargetParams. Both
// are bind-data-shaped and stay private to loader.cpp; the 1992 loader
// defines its own struct and its own narrower target-param application.

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "us_geocoder_parallel_download.hpp"

#include <algorithm>
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
// Result emission
// =====================================================================

// Drain `gstate.results` into `output` a vector-size chunk at a time.
// Shared by every loader execute function (modern and 1992); DuckDB
// re-invokes the execute callback until SetCardinality(0).
//
// Historical note: this used to be `EmitLoaderResults(gstate, output);`
// (recursive self-call). On macOS/Linux clang at -O2 silently optimized
// that to a `ret` since the body had no side effects, so every CALL
// load_tiger_* returned `0 rows` with no one noticing. On Windows MSVC
// the optimizer kept the loop and the function hung after analyze
// completed — fix applied May 2026.
//
// Defined (not just declared) here, not `static`: `State` makes this a
// template, so it must live in a header — with external linkage — to be
// instantiable from another translation unit (loader_1992.cpp). Moved
// (not reimplemented) from loader.cpp so this fix can't regress.
template <typename State>
void EmitLoaderResults(State &gstate, DataChunk &output) {
	const idx_t total = gstate.results.size();
	if (gstate.row_idx >= total) {
		output.SetCardinality(0);
		return;
	}
	const idx_t avail = total - gstate.row_idx;
	const idx_t n = std::min<idx_t>(STANDARD_VECTOR_SIZE, avail);
	auto step_data = FlatVector::GetData<string_t>(output.data[0]);
	auto rows_data = FlatVector::GetData<int64_t>(output.data[1]);
	for (idx_t i = 0; i < n; ++i) {
		auto &r = gstate.results[gstate.row_idx + i];
		step_data[i] = StringVector::AddString(output.data[0], r.step);
		rows_data[i] = r.rows;
	}
	gstate.row_idx += n;
	output.SetCardinality(n);
}

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
// Vintage-mixing guard
//
// The 13 tiger.* tables carry no vintage column, so loading 1992 data and
// modern data into the same location would silently interleave them (a
// 1992 edge joining a 2025 face produces plausible-looking wrong
// coordinates). The predicates below let each loader refuse the other
// vintage's data before writing anything. Both fail OPEN on a query error
// (loader_progress missing/unreadable => "no 1992 data"): a fresh
// target_db has no loader_progress table until BootstrapTargetSchema
// creates it, and failing closed there would break ordinary first loads.
//
// Every caller goes through RefuseIfVintageMismatch (per-state) or
// RefuseIfVintage1992Present (nation-scope) rather than re-expressing the
// predicate + throw shape inline. There are five guard sites across the
// two loaders (modern load/unload, 1992 load/unload, modern nation load)
// and the fifth was missed for exactly as long as each site spelled the
// condition out for itself.
// =====================================================================

// True iff this location holds 1992-vintage data for `fips`, i.e. at least
// one progress key under "tiger1992:state:<fips>:".
bool HasVintage1992(Connection &conn, const std::string &data_loc, const std::string &fips);

// True iff this location holds ANY 1992-vintage data, for any state.
// Nation-scope callers (load_tiger_nation writes state/county/zcta5, which
// are not per-state) need this rather than the per-fips predicate.
bool HasAnyVintage1992(Connection &conn, const std::string &data_loc);

// True iff this location holds any edges rows for `fips`.
bool HasStateRows(Connection &conn, const std::string &data_loc, const std::string &fips);

// Which vintage the calling loader deals in, and what it is about to do.
enum class LoaderVintage { MODERN, TIGER1992 };
enum class LoaderVintageOp { LOAD, UNLOAD };

// Throw InvalidInputException if `data_loc` already holds the OTHER
// vintage's rows for `fips`. Call after BootstrapTargetSchema (a fresh
// target_db has no loader_progress before it) and before any row write,
// DELETE, filesystem read or network fetch.
//
// MODERN callers trip on a "tiger1992:state:<fips>:" progress key.
// TIGER1992 callers trip on "edges rows present AND no tiger1992 key" —
// the data-presence half matters because a database loaded before the
// progress ledger existed holds modern rows with no keys at all, which a
// key-only check would wave straight through.
void RefuseIfVintageMismatch(Connection &conn, const std::string &data_loc, const std::string &fips,
                             const std::string &abbrev, LoaderVintage caller, LoaderVintageOp op);

// Nation-scope variant: throw if `data_loc` holds 1992 data for ANY state.
// load_tiger_nation writes modern nation-level state/county/zcta5 rows,
// which are not keyed per-state, so any 1992 state in this location is a
// mismatch. Without it, a modern 2020-vintage zcta5 row lands next to 1992
// streets and ZIP-only input silently returns a 2020 ZCTA centroid.
void RefuseIfVintage1992Present(Connection &conn, const std::string &data_loc);

// =====================================================================
// Bind-data-free bootstrap/analyze helpers
// =====================================================================

// Ensure the target catalog.schema has the 13 TIGER data tables (+ schema).
// Renders tiger_schema.sql.in with @TIGER@ → data_location. Idempotent: all
// statements are CREATE TABLE/SCHEMA IF NOT EXISTS. No-op when data_location
// equals func_schema (the default "tiger" location, already created by
// LoadInternal). De-static'd + signature narrowed to plain strings (was
// `const LoaderBindData &`) so loader_1992.cpp can call it without
// depending on LoaderBindData, which stays private to loader.cpp.
void BootstrapTargetSchema(Connection &conn, const std::string &data_location, const std::string &func_schema);

// Refresh DuckDB's per-table sample stats so the planner stops misestimating
// joins through the wide tiger.* fan-outs. One ANALYZE pass over the 13 data
// tables is cheap and shaves wall-clock off geocode queries once stats are
// populated. De-static'd + signature narrowed to a plain string (was
// `const LoaderBindData &`) so loader_1992.cpp can call it too.
void RunAnalyzeOnTigerTables(ClientContext &context, const std::string &data_location,
                             std::vector<LoaderResult> &out);

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
