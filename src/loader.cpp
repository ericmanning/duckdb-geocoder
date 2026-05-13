#include "us_geocoder_loader.hpp"
#include "us_geocoder_embed.hpp"
#include "us_geocoder_embedded_sql.hpp"
#include "us_geocoder_parallel_download.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace duckdb {
namespace us_geocoder {

// Live progress logging. Writes to stderr (and flushes) so a long-running
// CALL load_tiger_all_states() shows per-file progress as it runs, instead
// of the whole result vector flooding out at the end.
static void LogProgress(const std::string &prefix, const std::string &step, int64_t rows, double secs) {
	fprintf(stderr, "[us_geocoder %s] %s: %lld rows (%.1fs)\n", prefix.c_str(), step.c_str(),
	        static_cast<long long>(rows), secs);
	fflush(stderr);
}

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
static std::string ExtractSection(const std::string &all, const std::string &name) {
	const std::string marker = "-- @SECTION:" + name + "@";
	auto start = all.find(marker);
	if (start == std::string::npos) {
		throw InternalException("us_geocoder loader: missing template section '%s'", name);
	}
	// Advance to end of marker line.
	auto body_start = all.find('\n', start);
	if (body_start == std::string::npos) {
		return "";
	}
	++body_start;
	auto next = all.find("-- @SECTION:", body_start);
	if (next == std::string::npos) {
		return all.substr(body_start);
	}
	return all.substr(body_start, next - body_start);
}

static std::string RenderTemplate(const std::string &section,
                                  const std::vector<std::pair<std::string, std::string>> &subs) {
	return ApplySubstitutions(section, subs);
}

// =====================================================================
// Bind data + global state
// =====================================================================

struct LoaderResult {
	std::string step;
	int64_t rows;
};

struct LoaderBindData : public FunctionData {
	explicit LoaderBindData(std::string func_schema) : func_schema(std::move(func_schema)) {
		data_location = this->func_schema; // default: write to same place macros live
	}
	// Where the macros/lookup tables live (always local "tiger" in current catalog).
	std::string func_schema;
	// Where the TIGER data tables live. Equals func_schema for in-DB loads; becomes
	// "<target_db>.<target_schema>" when the user passes target_db. Used as @TIGER@
	// in templates; func_schema is used as @FUNC@.
	std::string data_location;
	// For bootstrap: the unqualified schema name on the target side (e.g. "tiger").
	// Used when rendering tiger_schema.sql.in against the target DB.
	std::string target_schema = "tiger";
	std::string target_db; // empty → current catalog

	// For load_tiger_nation
	std::string source;
	int32_t year = 2025;

	// For load_tiger_state / load_tiger_states / load_tiger_all_states:
	// one entry per state to load, resolved at bind time.
	struct StatePlan {
		std::string abbrev;
		std::string fips;
	};
	std::vector<StatePlan> states;

	// Whether to compute tiger.edge_containment inline at the end of each
	// state's load. Eager by default — the geocoder's GEOID output columns
	// and the `require_containment` filter both depend on it. Set to false
	// to skip (~1-2 min saved per state); then call build_edge_containment(states)
	// separately when you actually need the GEOIDs.
	bool build_containment = true;

	// Parallel download mode (default true). When true and `source` is an
	// HTTP URL, the loader downloads each state's (or the nation's) zips in
	// parallel into a temp dir, then ingests from local files, then deletes
	// the temp dir before moving to the next state. Bounds peak disk at
	// max(state_size) and bypasses GDAL's /vsicurl/ — fixes the Windows MSVC
	// SSL-cert-chain failure mode entirely.
	// `parallel := false` reverts to the legacy serial /vsicurl/ path.
	// Setting `parallel := true` with a local source emits a warning and
	// silently disables (parallel is meaningless when files are already local).
	bool parallel = true;
	std::string temp_dir; // base for state-suffixed scratch dirs; empty → OS temp
	int32_t parallel_workers = 16;

public:
	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<LoaderBindData>(func_schema);
		copy->data_location = data_location;
		copy->target_schema = target_schema;
		copy->target_db = target_db;
		copy->source = source;
		copy->year = year;
		copy->states = states;
		copy->build_containment = build_containment;
		copy->parallel = parallel;
		copy->temp_dir = temp_dir;
		copy->parallel_workers = parallel_workers;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LoaderBindData>();
		return func_schema == other.func_schema && data_location == other.data_location && source == other.source &&
		       year == other.year && states.size() == other.states.size() && parallel == other.parallel;
	}
};

struct LoaderGlobalState : public GlobalTableFunctionState {
	std::vector<LoaderResult> results;
	idx_t row_idx = 0;
	bool executed = false;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<LoaderGlobalState>();
	}
};

// Drain `gstate.results` into `output` a vector-size chunk at a time.
// Shared by every loader execute function; DuckDB re-invokes the execute
// callback until SetCardinality(0).
//
// Historical note: this used to be `EmitLoaderResults(gstate, output);`
// (recursive self-call). On macOS/Linux clang at -O2 silently optimized
// that to a `ret` since the body had no side effects, so every CALL
// load_tiger_* returned `0 rows` with no one noticing. On Windows MSVC
// the optimizer kept the loop and the function hung after analyze
// completed — fix applied May 2026.
template <typename State>
static void EmitLoaderResults(State &gstate, DataChunk &output) {
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
// Helpers
// =====================================================================

static std::string ZeroPad(const std::string &s, size_t width) {
	if (s.size() >= width) {
		return s;
	}
	return std::string(width - s.size(), '0') + s;
}

// Generate a unique cache-bust token. Cloudflare's cache key includes the
// query string, so appending ?cb=<token> guarantees a fresh fetch from the
// origin. Caught a real failure mode: tl_2025_02016_faces.zip had a 247-byte
// "Request Rejected" HTML cached at the edge, served instead of the zip.
// Cache-bust per-call sidesteps that without depending on edge eviction.
static uint64_t MakeCacheBust() {
	static std::atomic<uint64_t> counter {0};
	auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
	auto bump = counter.fetch_add(1, std::memory_order_relaxed);
	return static_cast<uint64_t>(now_ns) + bump;
}

// Build the /vsizip/ URI for a zip containing a single spatial file with
// the same base name. Most TIGER tables ship a full shapefile (.shp);
// featnames and addr are DBF-only (no geometry). GDAL can read both via
// ST_Read() — pass the correct inner extension.
//
// Both local and HTTP sources use the Census **nested** layout:
//   <source>/<SUBDIR>/<zip_base>.zip         — e.g. <root>/EDGES/tl_2025_44007_edges.zip
//
// HTTP form: /vsizip/{/vsicurl/<URL>?cb=<N>}/<inner>. The braces are
// required when ?cb=… is present so GDAL doesn't read past the .zip when
// splitting archive-vs-inner — without them GDAL treats the trailing
// /<inner> as part of the query string. Pass cache_bust=0 to disable
// (e.g. for local sources, where the query string would corrupt the path).
static std::string BuildVsiPath(const std::string &source, const std::string &subdir, const std::string &zip_base,
                                const std::string &inner_ext = "shp", uint64_t cache_bust = 0) {
	const bool is_http = source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
	std::string src = source;
	if (!src.empty() && src.back() != '/') {
		src += '/';
	}
	const std::string inner = zip_base + "." + inner_ext;
	if (is_http) {
		std::string url = src + subdir + "/" + zip_base + ".zip";
		if (cache_bust != 0) {
			url += "?cb=" + std::to_string(cache_bust);
		}
		return "/vsizip/{/vsicurl/" + url + "}/" + inner;
	}
	return "/vsizip/" + src + subdir + "/" + zip_base + ".zip/" + inner;
}

// Run one INSERT statement against a Connection, return the number of rows
// inserted (or 0 if the statement didn't produce a row count). Throws on SQL error.
static int64_t ExecuteInsert(Connection &conn, const std::string &sql, const std::string &step_label) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		throw IOException("us_geocoder loader (%s): %s", step_label, result->GetError());
	}
	// INSERT returns a single row with the count. Fetch it defensively.
	if (result->RowCount() == 0) {
		return 0;
	}
	auto row = result->Fetch();
	if (!row || row->size() == 0) {
		return 0;
	}
	try {
		return row->GetValue(0, 0).GetValue<int64_t>();
	} catch (...) {
		return 0;
	}
}

// Heuristic: is this loader error worth retrying? Network/GDAL transients
// (refused connection, corrupted-mid-download zip, /vsicurl/ open failure)
// retry; SQL syntax / catalog errors should fail fast. Hints are matched
// case-insensitively — error strings come from a mix of GDAL (paths use
// lowercase "gdal/"), DuckDB ("HTTP"), and curl, and we don't want a
// missed case to silently fail the whole load.
static bool IsRetriableLoaderError(const std::string &msg) {
	static const char *const kHints[] = {
	    "gdal",
	    "/vsicurl/",
	    "/vsizip/",
	    "http",
	    "curl",
	    "timeout",
	    "timed out",
	    "connection",
	    "decompression failed",
	    "z_err",
	    "cpl_vsil",
	    "premature end",
	    "unexpected end of",
	    "ssl",
	    "tls",
	    "reset by peer",
	    "broken pipe",
	};
	std::string lower;
	lower.reserve(msg.size());
	for (char c : msg) {
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	for (auto h : kHints) {
		if (lower.find(h) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// Wrap ExecuteInsert with retry. The Render callable is invoked once per
// attempt with a cache-bust token: 0 on the first attempt (CDN cache OK),
// fresh on every retry. The happy path keeps Cloudflare's edge cache; only
// failures pay the cache-miss + retry cost. Without this gating, every
// request hits origin and Census rate-limits us. Backoff: 2s, 5s, 15s;
// non-network errors fail fast.
template <typename RenderFn>
static int64_t RetryableExecuteInsert(Connection &conn, RenderFn render, const std::string &step_label) {
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

// DuckDB identifier quoting: wrap in double quotes and escape any embedded ".
// Used for target_db / target_schema, which may contain mixed case or awkward
// names the user chose for their ATTACH alias.
static std::string QuoteIdent(const std::string &s) {
	std::string out = "\"";
	for (char c : s) {
		if (c == '"') {
			out += "\"\"";
		} else {
			out += c;
		}
	}
	out += "\"";
	return out;
}

// Resolve target_db / target_schema named params into a qualified data location
// ("schema" or "db.schema"). Also stashes them on the bind data so the bootstrap
// pass knows where to create the schema.
static void ApplyTargetParams(LoaderBindData &bind, const TableFunctionBindInput &input) {
	auto db_it = input.named_parameters.find("target_db");
	if (db_it != input.named_parameters.end() && !db_it->second.IsNull()) {
		bind.target_db = StringValue::Get(db_it->second);
	}
	auto schema_it = input.named_parameters.find("target_schema");
	if (schema_it != input.named_parameters.end() && !schema_it->second.IsNull()) {
		auto s = StringValue::Get(schema_it->second);
		if (!s.empty()) {
			bind.target_schema = s;
		}
	}
	if (bind.target_db.empty()) {
		bind.data_location = bind.target_schema; // e.g. "tiger"
	} else {
		bind.data_location = QuoteIdent(bind.target_db) + "." + QuoteIdent(bind.target_schema);
	}
}

// Ensure the target catalog.schema has the 13 TIGER data tables (+ schema).
// Renders tiger_schema.sql.in with @TIGER@ → data_location. Idempotent: all
// statements are CREATE TABLE/SCHEMA IF NOT EXISTS. This makes the loader
// self-sufficient even when writing to a freshly-attached empty DB.
static void BootstrapTargetSchema(Connection &conn, const LoaderBindData &bind) {
	if (bind.data_location == bind.func_schema) {
		return; // default "tiger" location; already created by LoadInternal.
	}
	auto rendered = ApplySubstitutions(TigerSchemaSql(), {{"@TIGER@", bind.data_location}});
	auto result = conn.Query(rendered);
	if (result->HasError()) {
		throw IOException("us_geocoder loader (bootstrap %s): %s", bind.data_location, result->GetError());
	}
}

// =====================================================================
// Per-section completion ledger (tiger.loader_progress).
//
// Each loader step writes a row keyed by a structured section string only
// after its INSERT (or DELETE+INSERT) succeeds. Re-running the loader
// queries this table to skip already-completed work, supporting partial
// restart at per-(state, county, table) granularity.
//
// Section grammar:
//   nation:state | nation:county | nation:zcta5
//   state:<fp>:place | state:<fp>:cousub
//   state:<fp>:county:<cfp>:edges | …:faces | …:featnames | …:addr
//   state:<fp>:derived:zip_state | …:zip_state_loc | …:zip_lookup_base | …:zcta5_clip
//   state:<fp>:edge_containment
// =====================================================================

static bool IsProgressDone(Connection &conn, const std::string &data_loc, const std::string &section) {
	auto sql = "SELECT 1 FROM " + data_loc + ".loader_progress WHERE section = '" + section + "' LIMIT 1";
	auto result = conn.Query(sql);
	if (result->HasError()) {
		return false; // table missing or transient; let the caller proceed and surface the real error
	}
	return result->RowCount() > 0;
}

static void MarkProgressDone(Connection &conn, const std::string &data_loc, const std::string &section) {
	auto sql = "INSERT INTO " + data_loc + ".loader_progress (section, completed_at) VALUES ('" + section +
	           "', now()) ON CONFLICT (section) DO UPDATE SET completed_at = now()";
	auto result = conn.Query(sql);
	if (result->HasError()) {
		fprintf(stderr, "[us_geocoder] WARN: failed to mark progress '%s': %s\n", section.c_str(),
		        result->GetError().c_str());
		fflush(stderr);
	}
}

static void DeleteProgressLike(Connection &conn, const std::string &data_loc, const std::string &prefix) {
	auto sql = "DELETE FROM " + data_loc + ".loader_progress WHERE section LIKE '" + prefix + "%'";
	auto result = conn.Query(sql);
	if (result->HasError()) {
		fprintf(stderr, "[us_geocoder] WARN: failed to clear progress '%s%%': %s\n", prefix.c_str(),
		        result->GetError().c_str());
		fflush(stderr);
	}
}

// One-shot backfill. If loader_progress is empty but the data tables already
// have rows (existing DBs from before this migration), infer the completed
// sections from the data so re-runs skip already-loaded states/counties
// instead of duplicating rows.
//
// featnames/addr lacked countyfp before the schema migration; for legacy
// rows, we attribute their per-county completion via a join to edges on
// (statefp, tlid). TLIDs are unique per state to one edge → countyfp is
// unambiguous. This may over-attribute on the failure-county for partial
// state loads (e.g. AK county 016, where featnames/addr never ran but the
// join would mark them done if any featnames existed for AK at all). The
// risk is bounded: the next user-driven retry would skip the incomplete
// county; explicit `unload_tiger_state` is the escape hatch.
static void BackfillProgressIfNeeded(Connection &conn, const std::string &data_loc) {
	auto check = conn.Query("SELECT count(*) FROM " + data_loc + ".loader_progress");
	if (check->HasError() || check->RowCount() == 0) {
		return;
	}
	int64_t existing = 0;
	try {
		existing = check->Fetch()->GetValue(0, 0).GetValue<int64_t>();
	} catch (...) {
		return;
	}
	if (existing > 0) {
		return;
	}
	// Skip if nothing is loaded at all (fresh DB).
	auto data_check = conn.Query("SELECT count(*) FROM " + data_loc + ".state");
	if (data_check->HasError() || data_check->RowCount() == 0) {
		return;
	}
	int64_t state_rows = 0;
	try {
		state_rows = data_check->Fetch()->GetValue(0, 0).GetValue<int64_t>();
	} catch (...) {
		return;
	}
	if (state_rows == 0) {
		return;
	}
	fprintf(stderr, "[us_geocoder] backfilling loader_progress from existing data tables...\n");
	fflush(stderr);
	const char *const kBackfillStmts[] = {
	    // Nation steps: existence-of-rows on the canonical table.
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT 'nation:state', now()
	       WHERE EXISTS (SELECT 1 FROM @T@.state WHERE statefp IS NOT NULL))",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT 'nation:county', now()
	       WHERE EXISTS (SELECT 1 FROM @T@.county))",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT 'nation:zcta5', now()
	       WHERE EXISTS (SELECT 1 FROM @T@.zcta5 WHERE statefp IS NULL))",
	    // State-level: distinct statefp.
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':place', now()
	       FROM @T@.place WHERE statefp IS NOT NULL)",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':cousub', now()
	       FROM @T@.cousub WHERE statefp IS NOT NULL)",
	    // Per-county: edges/faces have countyfp directly.
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':county:' || countyfp || ':edges', now()
	       FROM @T@.edges WHERE statefp IS NOT NULL AND countyfp IS NOT NULL)",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':county:' || countyfp || ':faces', now()
	       FROM @T@.faces WHERE statefp IS NOT NULL AND countyfp IS NOT NULL)",
	    // featnames/addr post-migration: countyfp present.
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':county:' || countyfp || ':featnames', now()
	       FROM @T@.featnames WHERE statefp IS NOT NULL AND countyfp IS NOT NULL)",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':county:' || countyfp || ':addr', now()
	       FROM @T@.addr WHERE statefp IS NOT NULL AND countyfp IS NOT NULL)",
	    // featnames/addr legacy (countyfp NULL): attribute via tlid→edges.
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || e.statefp || ':county:' || e.countyfp || ':featnames', now()
	       FROM @T@.edges e
	       WHERE e.countyfp IS NOT NULL
	         AND EXISTS (SELECT 1 FROM @T@.featnames f
	                     WHERE f.statefp = e.statefp AND f.tlid = e.tlid AND f.countyfp IS NULL)
	       ON CONFLICT (section) DO NOTHING)",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || e.statefp || ':county:' || e.countyfp || ':addr', now()
	       FROM @T@.edges e
	       WHERE e.countyfp IS NOT NULL
	         AND EXISTS (SELECT 1 FROM @T@.addr a
	                     WHERE a.statefp = e.statefp AND a.tlid = e.tlid AND a.countyfp IS NULL)
	       ON CONFLICT (section) DO NOTHING)",
	    // Derived state-level + edge_containment.
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':derived:zip_state', now()
	       FROM @T@.zip_state WHERE statefp IS NOT NULL)",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':derived:zip_state_loc', now()
	       FROM @T@.zip_state_loc WHERE statefp IS NOT NULL)",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':derived:zip_lookup_base', now()
	       FROM @T@.zip_lookup_base WHERE statefp IS NOT NULL)",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':derived:zcta5_clip', now()
	       FROM @T@.zcta5 WHERE statefp IS NOT NULL)",
	    R"(INSERT INTO @T@.loader_progress (section, completed_at)
	       SELECT DISTINCT 'state:' || statefp || ':edge_containment', now()
	       FROM @T@.edge_containment WHERE statefp IS NOT NULL)",
	};
	for (const char *stmt : kBackfillStmts) {
		std::string sql = ApplySubstitutions(stmt, {{"@T@", data_loc}});
		auto result = conn.Query(sql);
		if (result->HasError()) {
			fprintf(stderr, "[us_geocoder] WARN: backfill step failed (continuing): %s\n", result->GetError().c_str());
			fflush(stderr);
		}
	}
	auto count = conn.Query("SELECT count(*) FROM " + data_loc + ".loader_progress");
	if (!count->HasError() && count->RowCount() > 0) {
		try {
			int64_t n = count->Fetch()->GetValue(0, 0).GetValue<int64_t>();
			fprintf(stderr, "[us_geocoder] backfill: marked %lld sections complete from existing data\n",
			        static_cast<long long>(n));
			fflush(stderr);
		} catch (...) {
		}
	}
}

// Census default source URL for a given year.
static std::string CensusUrl(int year) {
	return "https://www2.census.gov/geo/tiger/TIGER" + std::to_string(year);
}

// Read year + source + target_db/schema from a TableFunctionBindInput and
// populate the bind data. `source_input_index` is the positional slot that
// accepts the source string on this overload (-1 to disable positional).
// source is also accepted via the `source` named parameter.
static void ApplyYearSourceTarget(LoaderBindData &bind, const TableFunctionBindInput &input, int source_input_index) {
	auto year_it = input.named_parameters.find("year");
	if (year_it != input.named_parameters.end() && !year_it->second.IsNull()) {
		bind.year = year_it->second.GetValue<int32_t>();
	}
	ApplyTargetParams(bind, input);
	bool found_source = false;
	if (source_input_index >= 0 && static_cast<int>(input.inputs.size()) > source_input_index &&
	    !input.inputs[source_input_index].IsNull() && !StringValue::Get(input.inputs[source_input_index]).empty()) {
		bind.source = StringValue::Get(input.inputs[source_input_index]);
		found_source = true;
	}
	if (!found_source) {
		auto src_it = input.named_parameters.find("source");
		if (src_it != input.named_parameters.end() && !src_it->second.IsNull() &&
		    !StringValue::Get(src_it->second).empty()) {
			bind.source = StringValue::Get(src_it->second);
			found_source = true;
		}
	}
	if (!found_source) {
		bind.source = CensusUrl(bind.year);
	}
	auto bc_it = input.named_parameters.find("build_containment");
	if (bc_it != input.named_parameters.end() && !bc_it->second.IsNull()) {
		bind.build_containment = bc_it->second.GetValue<bool>();
	}
	auto par_it = input.named_parameters.find("parallel");
	if (par_it != input.named_parameters.end() && !par_it->second.IsNull()) {
		bind.parallel = par_it->second.GetValue<bool>();
	}
	auto tmp_it = input.named_parameters.find("temp_dir");
	if (tmp_it != input.named_parameters.end() && !tmp_it->second.IsNull()) {
		bind.temp_dir = StringValue::Get(tmp_it->second);
	}
	auto pw_it = input.named_parameters.find("parallel_workers");
	if (pw_it != input.named_parameters.end() && !pw_it->second.IsNull()) {
		auto v = pw_it->second.GetValue<int32_t>();
		if (v < 1) {
			throw BinderException("us_geocoder: parallel_workers must be >= 1 (got %d)", v);
		}
		bind.parallel_workers = v;
	}
	// `parallel` is meaningless when files are already local; warn + silently disable.
	const bool source_is_http = bind.source.rfind("http://", 0) == 0 || bind.source.rfind("https://", 0) == 0;
	if (bind.parallel && !source_is_http) {
		fprintf(stderr,
		        "[us_geocoder] WARN: parallel := true has no effect with a local source (%s); "
		        "ignored.\n",
		        bind.source.c_str());
		fflush(stderr);
		bind.parallel = false;
	}
}

// Resolve state abbrev → 2-digit FIPS via the lookup table.
static std::string LookupStateFips(Connection &conn, const std::string &schema, const std::string &abbrev) {
	auto sql = "SELECT statefp FROM " + schema + ".state_lookup WHERE upper(abbrev) = upper('" + abbrev + "') LIMIT 1";
	auto result = conn.Query(sql);
	if (result->HasError() || result->RowCount() == 0) {
		throw BinderException("us_geocoder: unknown state abbreviation '%s'", abbrev);
	}
	auto row = result->Fetch();
	if (!row || row->size() == 0) {
		throw BinderException("us_geocoder: unknown state abbreviation '%s'", abbrev);
	}
	return row->GetValue(0, 0).GetValue<std::string>();
}

// Refresh DuckDB's per-table sample stats so the planner stops misestimating
// joins through the wide tiger.* fan-outs (we observed 179M-row estimates
// vs 1.2M actual on featnames-side joins, picking suboptimal strategies).
// One ANALYZE pass over the 13 data tables is cheap (~20s nationwide) and
// shaves another ~37% off geocode wall-clock once stats are populated.
// Called once at the end of every loader entry point — running per-state
// would just re-do the same work N times.
static void RunAnalyzeOnTigerTables(ClientContext &context, const LoaderBindData &bind,
                                    std::vector<LoaderResult> &out) {
	static const char *const kAnalyzeTables[] = {
	    "state",           "county", "place", "cousub",    "zcta5", "zip_state",        "zip_state_loc",
	    "zip_lookup_base", "edges",  "faces", "featnames", "addr",  "edge_containment",
	};
	Connection conn(*context.db);
	const auto &data_loc = bind.data_location;
	auto t0 = std::chrono::steady_clock::now();
	fprintf(stderr, "[us_geocoder] analyze: refreshing planner stats on %s.*\n", data_loc.c_str());
	fflush(stderr);
	for (auto *tbl : kAnalyzeTables) {
		auto sql = std::string("ANALYZE ") + data_loc + "." + tbl;
		auto result = conn.Query(sql);
		if (result->HasError()) {
			// Don't fail the load — stats are an optimization, not correctness.
			fprintf(stderr, "[us_geocoder] WARN: ANALYZE %s.%s failed: %s\n", data_loc.c_str(), tbl,
			        result->GetError().c_str());
			fflush(stderr);
		}
	}
	auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	fprintf(stderr, "[us_geocoder] analyze: done in %.1fs\n", secs);
	fflush(stderr);
	out.push_back({"analyze", 0});
}

// =====================================================================
// load_tiger_nation(source VARCHAR, year INTEGER DEFAULT 2025)
// =====================================================================

static unique_ptr<FunctionData> LoadTigerNationBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	auto bind_data = make_uniq<LoaderBindData>("tiger"); // local func/lookup schema
	ApplyYearSourceTarget(*bind_data, input, /*source_input_index=*/0);
	return std::move(bind_data);
}

// If source is an HTTP URL, make sure httpfs is loaded so GDAL's vsicurl
// can reach it. Auto-load is a no-op if httpfs is already resident.
static void EnsureHttpfsIfRemote(DatabaseInstance &db, const std::string &source) {
	if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
		ExtensionHelper::TryAutoLoadExtension(db, "httpfs");
	}
}

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

static void DoLoadNationImpl(ClientContext &context, const LoaderBindData &bind, std::vector<LoaderResult> &out);

// Build the 3 nation-level download targets (STATE, COUNTY, ZCTA520).
// Filenames are known; no scraping needed.
// URLs use forward slashes (HTTP standard); local paths go through
// fs.JoinPath so Windows gets OS-correct separators.
static std::vector<DownloadTarget> BuildNationDownloadTargets(FileSystem &fs, const LoaderBindData &bind,
                                                              const std::string &dest_root) {
	const std::string year = std::to_string(bind.year);
	auto src_base = bind.source;
	if (!src_base.empty() && src_base.back() == '/') {
		src_base.pop_back();
	}
	struct NationFile {
		const char *subdir;
		std::string filename;
	};
	std::vector<NationFile> files = {
	    {"STATE", "tl_" + year + "_us_state.zip"},
	    {"COUNTY", "tl_" + year + "_us_county.zip"},
	    {"ZCTA520", "tl_" + year + "_us_zcta520.zip"},
	};
	std::vector<DownloadTarget> targets;
	targets.reserve(files.size());
	for (const auto &f : files) {
		std::string url = src_base + "/" + f.subdir + "/" + f.filename;
		std::string local = fs.JoinPath(dest_root, fs.JoinPath(f.subdir, f.filename));
		targets.push_back({url, local});
	}
	return targets;
}

static void DoLoadNation(ClientContext &context, const LoaderBindData &bind, std::vector<LoaderResult> &out) {
	const bool source_is_http = bind.source.rfind("http://", 0) == 0 || bind.source.rfind("https://", 0) == 0;

	if (!bind.parallel || !source_is_http) {
		DoLoadNationImpl(context, bind, out);
		return;
	}

	// Entry-point diagnostic — if even THIS doesn't reach the user, the
	// crash is in bind, not execute. A Windows MSVC user reported a silent
	// no-stderr crash on `CALL load_tiger_nation()`; this print + the
	// try/catch fallback below should surface where exactly things die.
	fprintf(stderr, "[us_geocoder nation] starting parallel download path\n");
	fflush(stderr);

	EnsureHttpfsIfRemote(*context.db, bind.source);
	auto &fs = FileSystem::GetFileSystem(context);

	// Wrap the parallel prelude so a thrown exception (e.g. failed temp-dir
	// creation on a weird Windows path) becomes a stderr message + fallback
	// to the legacy /vsicurl/ path, rather than a silent crash.
	try {
		auto temp_base = ResolveTempBase(context, bind.temp_dir);
		auto state_dir = MakeStateTempDir(context, temp_base, "nation", bind.year);
		StateDirCleanup raii(fs, state_dir);

		auto t0 = std::chrono::steady_clock::now();
		auto targets = BuildNationDownloadTargets(fs, bind, state_dir);

		ParallelDownloadOptions opts;
		opts.workers = std::min(bind.parallel_workers, 3);
		opts.max_attempts = 3;
		opts.backoff_seconds = {2, 5};
		opts.log_prefix = "nation";
		auto result = ParallelDownload(context, targets, opts);
		auto dl_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		fprintf(stderr,
		        "[us_geocoder nation] parallel_download: %zu files (%zu downloaded, %zu skipped) "
		        "%.1f MB in %.1fs\n",
		        result.files_total, result.files_downloaded, result.files_skipped,
		        static_cast<double>(result.bytes_downloaded) / (1024.0 * 1024.0), dl_secs);
		fflush(stderr);
		out.push_back({"parallel_download:nation", static_cast<int64_t>(result.files_total)});

		LoaderBindData local_bind(bind.func_schema);
		local_bind.data_location = bind.data_location;
		local_bind.target_schema = bind.target_schema;
		local_bind.target_db = bind.target_db;
		local_bind.source = state_dir;
		local_bind.year = bind.year;
		local_bind.states = bind.states;
		local_bind.build_containment = bind.build_containment;
		local_bind.parallel = false;
		local_bind.temp_dir = bind.temp_dir;
		local_bind.parallel_workers = bind.parallel_workers;
		DoLoadNationImpl(context, local_bind, out);
	} catch (std::exception &ex) {
		fprintf(stderr, "[us_geocoder nation] parallel path failed: %s\n", ex.what());
		fprintf(stderr, "[us_geocoder nation] falling back to legacy /vsicurl/ path\n");
		fflush(stderr);
		DoLoadNationImpl(context, bind, out);
	}
}

static void DoLoadNationImpl(ClientContext &context, const LoaderBindData &bind, std::vector<LoaderResult> &out) {
	EnsureHttpfsIfRemote(*context.db, bind.source);
	Connection conn(*context.db);
	BootstrapTargetSchema(conn, bind);
	const auto &data_loc = bind.data_location;
	const auto &func_loc = bind.func_schema;
	const std::string year = std::to_string(bind.year);
	const auto &tmpl = LoaderTemplatesSql();
	BackfillProgressIfNeeded(conn, data_loc);

	struct NationStep {
		const char *section;
		const char *subdir;       // Census URL subdir (e.g. "STATE")
		const char *zip_base_fmt; // e.g. "tl_<year>_us_state"
		const char *progress_key; // section key for loader_progress
	};
	const NationStep steps[] = {
	    {"nation_state", "STATE", "tl_YEAR_us_state", "nation:state"},
	    {"nation_county", "COUNTY", "tl_YEAR_us_county", "nation:county"},
	    {"nation_zcta5", "ZCTA520", "tl_YEAR_us_zcta520", "nation:zcta5"},
	};

	for (const auto &step : steps) {
		std::string zip_base = step.zip_base_fmt;
		auto pos = zip_base.find("YEAR");
		if (pos != std::string::npos) {
			zip_base.replace(pos, 4, year);
		}
		const std::string label = std::string(step.section) + " (" + zip_base + ".zip)";
		if (IsProgressDone(conn, data_loc, step.progress_key)) {
			fprintf(stderr, "[us_geocoder nation] %s: already loaded (skip)\n", label.c_str());
			fflush(stderr);
			out.push_back({std::string(step.section) + ":skipped", 0});
			continue;
		}
		auto section = ExtractSection(tmpl, step.section);
		StepTimer t("nation", label);
		int64_t rows = RetryableExecuteInsert(
		    conn,
		    [&](uint64_t cb) {
			    auto vsi = BuildVsiPath(bind.source, step.subdir, zip_base, "shp", cb);
			    return RenderTemplate(section, {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}, {"@VSIPATH@", vsi}});
		    },
		    step.section);
		out.push_back({step.section, rows});
		t.Done(rows);
		MarkProgressDone(conn, data_loc, step.progress_key);
	}
}

static void LoadTigerNationExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<LoaderBindData>();
	auto &gstate = data_p.global_state->Cast<LoaderGlobalState>();

	if (!gstate.executed) {
		gstate.executed = true;
		DoLoadNation(context, bind, gstate.results);
		RunAnalyzeOnTigerTables(context, bind, gstate.results);
	}

	EmitLoaderResults(gstate, output);
}

// =====================================================================
// load_tiger_state(VARCHAR) — single state by abbrev (back-compat).
// load_tiger_states(LIST<VARCHAR>) — multiple states in one call.
// load_tiger_all_states() — every row of state_lookup with statefp ≤ 56
//   (= 50 states + DC; excludes PR, VI, Guam, AS, MP).
// All three land on the same LoaderBindData::states[] + DoLoadState per-state.
// =====================================================================

// Resolve a list of state abbreviations into LoaderBindData::states (in order).
// Dedups case-insensitively while preserving first-occurrence order; errors on
// any unknown abbrev.
static void ResolveStates(Connection &conn, LoaderBindData &bind, const std::vector<std::string> &abbrevs) {
	std::vector<std::string> seen;
	for (const auto &raw : abbrevs) {
		std::string up;
		up.reserve(raw.size());
		for (char c : raw) {
			up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		bool dup = false;
		for (const auto &s : seen) {
			if (s == up) {
				dup = true;
				break;
			}
		}
		if (dup)
			continue;
		seen.push_back(up);
		auto fips = LookupStateFips(conn, bind.func_schema, up);
		bind.states.push_back({up, fips});
	}
	if (bind.states.empty()) {
		throw BinderException("us_geocoder: no state abbreviations provided");
	}
}

// Extract varchar abbrevs from a LIST<VARCHAR> Value (or a single scalar VARCHAR).
static std::vector<std::string> AbbrevsFromValue(const Value &v, const char *context_fn) {
	std::vector<std::string> out;
	if (v.IsNull()) {
		throw BinderException("%s: states argument is NULL", context_fn);
	}
	if (v.type().id() == LogicalTypeId::LIST) {
		auto &children = ListValue::GetChildren(v);
		for (const auto &child : children) {
			if (child.IsNull())
				continue;
			auto s = StringValue::Get(child);
			if (!s.empty())
				out.push_back(s);
		}
	} else if (v.type().id() == LogicalTypeId::VARCHAR) {
		auto s = StringValue::Get(v);
		if (!s.empty())
			out.push_back(s);
	} else {
		throw BinderException("%s: expected VARCHAR or VARCHAR[], got %s", context_fn, v.type().ToString());
	}
	return out;
}

static unique_ptr<FunctionData> LoadTigerStateBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("load_tiger_state: state_abbrev is required");
	}
	auto bind_data = make_uniq<LoaderBindData>("tiger");
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "load_tiger_state");
	ApplyYearSourceTarget(*bind_data, input, /*source_input_index=*/1);
	Connection conn(*context.db);
	ResolveStates(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static unique_ptr<FunctionData> LoadTigerStatesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("load_tiger_states: states (VARCHAR[]) is required");
	}
	auto bind_data = make_uniq<LoaderBindData>("tiger");
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "load_tiger_states");
	ApplyYearSourceTarget(*bind_data, input, /*source_input_index=*/1);
	Connection conn(*context.db);
	ResolveStates(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static unique_ptr<FunctionData> LoadTigerAllStatesBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	auto bind_data = make_uniq<LoaderBindData>("tiger");
	ApplyYearSourceTarget(*bind_data, input, /*source_input_index=*/0);

	// Enumerate 50 states + DC from the local state_lookup. The Census FIPS
	// scheme reserves 01–56 for states/DC (with gaps at 03, 07, 14, 43, 52)
	// and 57+ for territories + freely-associated states (AS=60, FM=64, GU=66,
	// MH=68, MP=69, PW=70, PR=72, VI=78). No interspersing, so a BETWEEN filter
	// on CAST(statefp AS INTEGER) is sufficient. Users who want territories
	// pass the abbrev to load_tiger_states explicitly.
	Connection conn(*context.db);
	auto result = conn.Query("SELECT abbrev FROM " + bind_data->func_schema +
	                         ".state_lookup WHERE CAST(statefp AS INTEGER) BETWEEN 1 AND 56 "
	                         "ORDER BY CAST(statefp AS INTEGER)");
	if (result->HasError()) {
		throw IOException("us_geocoder load_tiger_all_states: %s", result->GetError());
	}
	std::vector<std::string> abbrevs;
	while (auto row = result->Fetch()) {
		for (idx_t i = 0; i < row->size(); ++i) {
			auto v = row->GetValue(0, i);
			if (!v.IsNull()) {
				abbrevs.push_back(v.GetValue<std::string>());
			}
		}
	}
	ResolveStates(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

// =====================================================================
// unload_tiger_state(state_abbrev VARCHAR | VARCHAR[], target_db := NULL,
//                    target_schema := 'tiger')
//
// Force-unload a state: deletes all per-statefp rows from the 13 data
// tables AND removes the matching loader_progress entries, so a
// subsequent load_tiger_state* call does a full re-load. Use when a
// state's existing rows are stale or partially corrupt.
// =====================================================================

static unique_ptr<FunctionData> UnloadTigerStateBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("unload_tiger_state: state abbrev is required");
	}
	auto bind_data = make_uniq<LoaderBindData>("tiger");
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "unload_tiger_state");
	ApplyTargetParams(*bind_data, input);
	Connection conn(*context.db);
	ResolveStates(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static void UnloadTigerStateExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<LoaderBindData>();
	auto &gstate = data_p.global_state->Cast<LoaderGlobalState>();
	if (!gstate.executed) {
		gstate.executed = true;
		Connection conn(*context.db);
		BootstrapTargetSchema(conn, bind);
		const auto &data_loc = bind.data_location;
		const auto &func_loc = bind.func_schema;
		const auto &tmpl = LoaderTemplatesSql();
		auto unload_template = ExtractSection(tmpl, "unload_state");
		for (const auto &state : bind.states) {
			StepTimer t(state.abbrev, "unload_state");
			auto rendered = RenderTemplate(unload_template,
			                               {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}, {"@STATEFP@", state.fips}});
			auto result = conn.Query(rendered);
			if (result->HasError()) {
				throw IOException("us_geocoder unload_tiger_state(%s): %s", state.abbrev, result->GetError());
			}
			DeleteProgressLike(conn, data_loc, "state:" + state.fips + ":");
			gstate.results.push_back({"unload:" + state.abbrev, 0});
			t.Done(0);
		}
	}
	EmitLoaderResults(gstate, output);
}

// Build the list of (URL, dest_path) pairs to download for one state.
// `dest_root` is the local state-temp-dir; output paths land at
// <dest_root>/<SUBDIR>/<zip>.zip. Counties are scraped from the Census HTML
// directory index for each per-county SUBDIR (EDGES, FACES, FEATNAMES, ADDR).
// URLs use forward slashes (HTTP standard); local paths go through fs.JoinPath
// so Windows gets OS-correct separators.
static std::vector<DownloadTarget> BuildStateDownloadTargets(ClientContext &context, FileSystem &fs,
                                                             const LoaderBindData &bind,
                                                             const LoaderBindData::StatePlan &state,
                                                             const std::string &dest_root) {
	const std::string &fips = state.fips;
	const std::string year = std::to_string(bind.year);
	std::vector<DownloadTarget> targets;

	// State-level (known filenames): PLACE, COUSUB.
	struct StateFile {
		const char *subdir;
		std::string filename;
	};
	auto src_base = bind.source;
	if (!src_base.empty() && src_base.back() == '/') {
		src_base.pop_back();
	}
	std::vector<StateFile> state_files = {
	    {"PLACE", "tl_" + year + "_" + fips + "_place.zip"},
	    {"COUSUB", "tl_" + year + "_" + fips + "_cousub.zip"},
	};
	for (const auto &sf : state_files) {
		std::string url = src_base + "/" + sf.subdir + "/" + sf.filename;
		std::string local = fs.JoinPath(dest_root, fs.JoinPath(sf.subdir, sf.filename));
		targets.push_back({url, local});
	}

	// Per-county (scraped): EDGES, FACES, FEATNAMES, ADDR.
	// Filename pattern: tl_<year>_<fips><cfp>_<type>.zip — cfp is 3 digits.
	ParallelDownloadOptions scrape_opts;
	scrape_opts.workers = 1; // scrape is a single GET per directory
	scrape_opts.max_attempts = 3;
	scrape_opts.log_prefix = state.abbrev;
	for (const char *sub : {"EDGES", "FACES", "FEATNAMES", "ADDR"}) {
		std::string lower_type(sub);
		for (auto &c : lower_type) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		std::string pattern_str = "tl_" + year + "_" + fips + "[0-9]{3}_" + lower_type + "\\.zip";
		std::regex pat(pattern_str);
		std::string idx_url = src_base + "/" + sub + "/";
		auto names = ScrapeCensusIndex(context, idx_url, pat, scrape_opts);
		if (names.empty()) {
			throw IOException("us_geocoder parallel_download (%s): index %s yielded no matching files (pattern %s)",
			                  state.abbrev, idx_url, pattern_str);
		}
		for (const auto &n : names) {
			std::string url = src_base + "/" + sub + "/" + n;
			std::string local = fs.JoinPath(dest_root, fs.JoinPath(sub, n));
			targets.push_back({url, local});
		}
	}
	return targets;
}

// RAII wrapper for a temp-dir lifetime. Destructor removes the directory
// unless Disarm() was called (e.g. on failure — keep zips for retry).
// Forward decl — defined below.
static void DoLoadStateImpl(ClientContext &context, const LoaderBindData &bind, const LoaderBindData::StatePlan &state,
                            std::vector<LoaderResult> &out);

static void DoLoadState(ClientContext &context, const LoaderBindData &bind, const LoaderBindData::StatePlan &state,
                        std::vector<LoaderResult> &out) {
	const bool source_is_http = bind.source.rfind("http://", 0) == 0 || bind.source.rfind("https://", 0) == 0;

	// Serial fallback: same as the legacy path.
	if (!bind.parallel || !source_is_http) {
		DoLoadStateImpl(context, bind, state, out);
		return;
	}

	// Entry-point diagnostic. If even this doesn't print, the crash is
	// before execute (in bind, or in DLL load on a buggy platform).
	fprintf(stderr, "[us_geocoder %s] starting parallel download path\n", state.abbrev.c_str());
	fflush(stderr);

	EnsureHttpfsIfRemote(*context.db, bind.source);
	auto &fs = FileSystem::GetFileSystem(context);

	// Wrap in try/catch so a thrown exception in the parallel prelude
	// surfaces as a clear stderr message + fallback to the legacy
	// /vsicurl/ path, rather than a silent crash.
	try {
		auto temp_base = ResolveTempBase(context, bind.temp_dir);
		auto state_dir = MakeStateTempDir(context, temp_base, state.abbrev, bind.year);
		StateDirCleanup raii(fs, state_dir);

		auto t0 = std::chrono::steady_clock::now();
		auto targets = BuildStateDownloadTargets(context, fs, bind, state, state_dir);

		ParallelDownloadOptions opts;
		opts.workers = bind.parallel_workers;
		opts.max_attempts = 3;
		opts.backoff_seconds = {2, 5};
		opts.log_prefix = state.abbrev;
		auto result = ParallelDownload(context, targets, opts);

		auto dl_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		fprintf(stderr,
		        "[us_geocoder %s] parallel_download: %zu files (%zu downloaded, %zu skipped) "
		        "%.1f MB in %.1fs\n",
		        state.abbrev.c_str(), result.files_total, result.files_downloaded, result.files_skipped,
		        static_cast<double>(result.bytes_downloaded) / (1024.0 * 1024.0), dl_secs);
		fflush(stderr);
		out.push_back({"parallel_download:" + state.abbrev, static_cast<int64_t>(result.files_total)});

		// Re-enter the existing ingest body with bind.source patched to the local
		// temp dir. Mutating via a local copy keeps the outer call's bind read-only.
		LoaderBindData local_bind(bind.func_schema);
		local_bind.data_location = bind.data_location;
		local_bind.target_schema = bind.target_schema;
		local_bind.target_db = bind.target_db;
		local_bind.source = state_dir; // <-- swap to local
		local_bind.year = bind.year;
		local_bind.states = bind.states;
		local_bind.build_containment = bind.build_containment;
		local_bind.parallel = false; // belt-and-suspenders
		local_bind.temp_dir = bind.temp_dir;
		local_bind.parallel_workers = bind.parallel_workers;
		DoLoadStateImpl(context, local_bind, state, out);

		// Successful ingest — let the RAII destructor clean up the temp dir.
	} catch (std::exception &ex) {
		fprintf(stderr, "[us_geocoder %s] parallel path failed: %s\n", state.abbrev.c_str(), ex.what());
		fprintf(stderr, "[us_geocoder %s] falling back to legacy /vsicurl/ path\n", state.abbrev.c_str());
		fflush(stderr);
		DoLoadStateImpl(context, bind, state, out);
	}
}

static void DoLoadStateImpl(ClientContext &context, const LoaderBindData &bind, const LoaderBindData::StatePlan &state,
                            std::vector<LoaderResult> &out) {
	EnsureHttpfsIfRemote(*context.db, bind.source);
	Connection conn(*context.db);
	BootstrapTargetSchema(conn, bind);
	const auto &data_loc = bind.data_location;
	const auto &func_loc = bind.func_schema;
	const std::string &fips = state.fips;
	const std::string year = std::to_string(bind.year);
	const auto &tmpl = LoaderTemplatesSql();
	BackfillProgressIfNeeded(conn, data_loc);

	const std::string state_pfx = "state:" + fips + ":";

	// State-level files: place, cousub. Per-section progress check; skip
	// without downloading if already complete.
	struct StateLevelStep {
		const char *section;
		const char *subdir;
		const char *zip_base;
		const char *progress_suffix; // appended to state_pfx
	};
	const StateLevelStep state_level[] = {
	    {"state_place", "PLACE", "tl_YEAR_FIPS_place", "place"},
	    {"state_cousub", "COUSUB", "tl_YEAR_FIPS_cousub", "cousub"},
	};
	for (const auto &s : state_level) {
		std::string zip_base = s.zip_base;
		auto pos = zip_base.find("YEAR");
		if (pos != std::string::npos)
			zip_base.replace(pos, 4, year);
		pos = zip_base.find("FIPS");
		if (pos != std::string::npos)
			zip_base.replace(pos, 4, fips);
		const std::string section_key = state_pfx + s.progress_suffix;
		const std::string label = std::string(s.section) + " (" + zip_base + ".zip)";
		if (IsProgressDone(conn, data_loc, section_key)) {
			fprintf(stderr, "[us_geocoder %s] %s: already loaded (skip)\n", state.abbrev.c_str(), label.c_str());
			fflush(stderr);
			out.push_back({std::string(s.section) + ":skipped", 0});
			continue;
		}
		auto section = ExtractSection(tmpl, s.section);
		StepTimer t(state.abbrev, label);
		int64_t rows = RetryableExecuteInsert(
		    conn,
		    [&](uint64_t cb) {
			    auto vsi = BuildVsiPath(bind.source, s.subdir, zip_base, "shp", cb);
			    return RenderTemplate(section, {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}, {"@VSIPATH@", vsi}});
		    },
		    s.section);
		out.push_back({s.section, rows});
		t.Done(rows);
		MarkProgressDone(conn, data_loc, section_key);
	}

	// Enumerate counties for this state from <data_loc>.county (must be loaded first).
	std::vector<std::string> countyfps;
	{
		auto sql = "SELECT countyfp FROM " + data_loc + ".county WHERE statefp = '" + fips + "' ORDER BY countyfp";
		auto result = conn.Query(sql);
		if (result->HasError()) {
			throw IOException("us_geocoder loader (enumerate counties): %s", result->GetError());
		}
		while (auto row = result->Fetch()) {
			for (idx_t i = 0; i < row->size(); ++i) {
				auto v = row->GetValue(0, i);
				if (!v.IsNull()) {
					countyfps.push_back(v.GetValue<std::string>());
				}
			}
		}
	}
	if (countyfps.empty()) {
		throw IOException("us_geocoder loader: no counties found for state %s (statefp=%s) at %s; "
		                  "run load_tiger_nation() first (with matching target_db/target_schema) to populate .county",
		                  state.abbrev, fips, data_loc);
	}

	// County-level files. One INSERT per (county, table-type) — see
	// CLAUDE.md's loader-perf notes for why UNION-ALL across counties
	// doesn't pay off here (parse parallelizes; INSERT-write serializes).
	// Per (statefp, countyfp, table) we keep a loader_progress entry,
	// which is what gives us partial-restart: a re-run skips counties
	// already done and only redoes the missing per-table inserts.
	struct CountyTable {
		const char *label;
		const char *subdir;
		const char *zip_base;
		const char *ext;
		const char *insert_section;
		const char *branch_section;
		const char *progress_table; // table name for progress key (lowercase)
	};
	const CountyTable county_level[] = {
	    {"county_edges", "EDGES", "tl_YEAR_FIPSCOUNTY_edges", "shp", "county_edges_insert", "county_edges_branch",
	     "edges"},
	    {"county_faces", "FACES", "tl_YEAR_FIPSCOUNTY_faces", "shp", "county_faces_insert", "county_faces_branch",
	     "faces"},
	    {"county_featnames", "FEATNAMES", "tl_YEAR_FIPSCOUNTY_featnames", "dbf", "county_featnames_insert",
	     "county_featnames_branch", "featnames"},
	    {"county_addr", "ADDR", "tl_YEAR_FIPSCOUNTY_addr", "dbf", "county_addr_insert", "county_addr_branch", "addr"},
	};
	bool any_county_inserted = false;
	for (size_t ci = 0; ci < countyfps.size(); ++ci) {
		const auto &cfp = countyfps[ci];
		auto t0 = std::chrono::steady_clock::now();
		int n_skipped = 0;
		int n_loaded = 0;
		for (const auto &t : county_level) {
			const std::string section_key = state_pfx + "county:" + cfp + ":" + t.progress_table;
			if (IsProgressDone(conn, data_loc, section_key)) {
				++n_skipped;
				out.push_back({std::string(t.label) + ":" + cfp + ":skipped", 0});
				continue;
			}
			std::string zip_base = t.zip_base;
			auto pos = zip_base.find("YEAR");
			if (pos != std::string::npos)
				zip_base.replace(pos, 4, year);
			pos = zip_base.find("FIPSCOUNTY");
			if (pos != std::string::npos)
				zip_base.replace(pos, 10, fips + cfp);
			auto insert_prefix =
			    RenderTemplate(ExtractSection(tmpl, t.insert_section), {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}});
			auto branch_section = ExtractSection(tmpl, t.branch_section);
			int64_t rows = RetryableExecuteInsert(
			    conn,
			    [&](uint64_t cb) {
				    auto vsi = BuildVsiPath(bind.source, t.subdir, zip_base, t.ext, cb);
				    auto branch = RenderTemplate(branch_section, {{"@TIGER@", data_loc},
				                                                  {"@FUNC@", func_loc},
				                                                  {"@VSIPATH@", vsi},
				                                                  {"@STATEFP@", fips},
				                                                  {"@COUNTYFP@", cfp}});
				    return insert_prefix + branch + ";";
			    },
			    std::string(t.label) + ":" + cfp);
			out.push_back({std::string(t.label) + ":" + cfp, rows});
			MarkProgressDone(conn, data_loc, section_key);
			++n_loaded;
			any_county_inserted = true;
		}
		auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (n_loaded == 0) {
			fprintf(stderr, "[us_geocoder %s] county %s (%zu/%zu) skipped — already loaded\n", state.abbrev.c_str(),
			        cfp.c_str(), ci + 1, countyfps.size());
		} else {
			fprintf(stderr, "[us_geocoder %s] county %s (%zu/%zu) (%.1fs%s)\n", state.abbrev.c_str(), cfp.c_str(),
			        ci + 1, countyfps.size(), secs, n_skipped > 0 ? ", partial" : "");
		}
		fflush(stderr);
	}

	// Per-state derived tables. Each is INSERT…SELECT keyed on statefp;
	// progress-tracked individually. If progress entry is absent (fresh
	// load OR partial-restart that just filled in counties), we DELETE
	// the prior per-state rows first to avoid duplication, then INSERT.
	struct DerivedStep {
		const char *section;
		const char *table;        // for the pre-INSERT delete (per-statefp scope)
		const char *progress_key; // suffix appended to state_pfx + "derived:"
	};
	std::vector<DerivedStep> derived = {
	    {"derived_zip_state", "zip_state", "zip_state"},
	    {"derived_zip_state_loc", "zip_state_loc", "zip_state_loc"},
	    {"derived_zip_lookup_base", "zip_lookup_base", "zip_lookup_base"},
	    {"derived_zcta5_clip", "zcta5", "zcta5_clip"},
	};
	for (const auto &d : derived) {
		const std::string section_key = state_pfx + "derived:" + d.progress_key;
		if (!any_county_inserted && IsProgressDone(conn, data_loc, section_key)) {
			out.push_back({std::string(d.section) + ":skipped", 0});
			continue;
		}
		// Idempotent re-run: DELETE pre-existing per-state rows so re-INSERT
		// doesn't double-up. zcta5 is special — only delete the per-state
		// clipped rows (statefp NOT NULL); the nation_zcta5 baseline keeps
		// statefp NULL and must survive.
		std::string del_sql = "DELETE FROM " + data_loc + "." + d.table + " WHERE statefp = '" + fips + "'";
		auto del_result = conn.Query(del_sql);
		if (del_result->HasError()) {
			throw IOException("us_geocoder loader (pre-derived delete %s): %s", d.table, del_result->GetError());
		}
		auto section = ExtractSection(tmpl, d.section);
		auto rendered = RenderTemplate(section, {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}, {"@STATEFP@", fips}});
		StepTimer timer(state.abbrev, d.section);
		int64_t rows = ExecuteInsert(conn, rendered, d.section);
		out.push_back({d.section, rows});
		timer.Done(rows);
		MarkProgressDone(conn, data_loc, section_key);
	}

	// edge_containment last (expensive: ST_Within over ~150K edges per
	// state). Same idempotency rules as the other derived steps.
	if (bind.build_containment) {
		const std::string section_key = state_pfx + "edge_containment";
		if (!any_county_inserted && IsProgressDone(conn, data_loc, section_key)) {
			out.push_back({"derived_edge_containment:skipped", 0});
		} else {
			std::string del_sql = "DELETE FROM " + data_loc + ".edge_containment WHERE statefp = '" + fips + "'";
			auto del_result = conn.Query(del_sql);
			if (del_result->HasError()) {
				throw IOException("us_geocoder loader (pre-edge_containment delete): %s", del_result->GetError());
			}
			auto section = ExtractSection(tmpl, "derived_edge_containment");
			auto rendered = RenderTemplate(section, {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}, {"@STATEFP@", fips}});
			StepTimer timer(state.abbrev, "derived_edge_containment");
			int64_t rows = ExecuteInsert(conn, rendered, "derived_edge_containment");
			out.push_back({"derived_edge_containment", rows});
			timer.Done(rows);
			MarkProgressDone(conn, data_loc, section_key);
		}
	} else {
		out.push_back({"skipped:edge_containment", 0});
	}
}

static void LoadTigerStateExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<LoaderBindData>();
	auto &gstate = data_p.global_state->Cast<LoaderGlobalState>();

	if (!gstate.executed) {
		gstate.executed = true;
		// Per-state begin/done markers in the result stream; LogProgress
		// also emits them to stderr so users tailing a multi-hour CALL see
		// real-time progress.
		for (size_t i = 0; i < bind.states.size(); ++i) {
			const auto &st = bind.states[i];
			char counter[64];
			snprintf(counter, sizeof(counter), "(%zu/%zu)", i + 1, bind.states.size());
			fprintf(stderr, "[us_geocoder %s] begin %s\n", st.abbrev.c_str(), counter);
			fflush(stderr);
			auto t0 = std::chrono::steady_clock::now();
			gstate.results.push_back({"begin:" + st.abbrev, 0});
			DoLoadState(context, bind, st, gstate.results);
			gstate.results.push_back({"done:" + st.abbrev, 0});
			auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			fprintf(stderr, "[us_geocoder %s] done %s in %.1fs\n", st.abbrev.c_str(), counter, secs);
			fflush(stderr);
		}
		RunAnalyzeOnTigerTables(context, bind, gstate.results);
	}

	EmitLoaderResults(gstate, output);
}

// =====================================================================
// Data-table catalog — the 13 tables that the macros read from, and that
// set_tiger_reference swaps between base-table and view forms.
// =====================================================================

static const char *const kDataTables[] = {
    "state",           "county", "place", "cousub",    "zcta5", "zip_state",        "zip_state_loc",
    "zip_lookup_base", "edges",  "faces", "featnames", "addr",  "edge_containment",
};
static constexpr size_t kDataTableCount = sizeof(kDataTables) / sizeof(kDataTables[0]);

// =====================================================================
// install_tiger_schema(database VARCHAR, schema VARCHAR DEFAULT 'tiger')
//   Creates the 13 TIGER data tables + schema in <database>.<schema>.
//   Idempotent. Used to pre-build a portable reference DB.
// =====================================================================

struct SchemaOpBindData : public FunctionData {
	std::string database;
	std::string schema = "tiger";
	std::string op; // "install" or "set_reference"

	unique_ptr<FunctionData> Copy() const override {
		auto c = make_uniq<SchemaOpBindData>();
		c->database = database;
		c->schema = schema;
		c->op = op;
		return std::move(c);
	}
	bool Equals(const FunctionData &o_p) const override {
		auto &o = o_p.Cast<SchemaOpBindData>();
		return database == o.database && schema == o.schema && op == o.op;
	}
};

struct SchemaOpGlobalState : public GlobalTableFunctionState {
	std::vector<LoaderResult> results;
	idx_t row_idx = 0;
	bool executed = false;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<SchemaOpGlobalState>();
	}
};

static unique_ptr<FunctionData> InstallSchemaBind(ClientContext &, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	auto bind = make_uniq<SchemaOpBindData>();
	bind->op = "install";
	// Positional (database) is required; schema optional.
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("install_tiger_schema: database (catalog name) is required; "
		                      "pass '' to target the current catalog");
	}
	bind->database = StringValue::Get(input.inputs[0]);
	if (input.inputs.size() >= 2 && !input.inputs[1].IsNull() && !StringValue::Get(input.inputs[1]).empty()) {
		bind->schema = StringValue::Get(input.inputs[1]);
	}
	return std::move(bind);
}

static std::string QualifyLocation(const std::string &database, const std::string &schema) {
	if (database.empty()) {
		return QuoteIdent(schema);
	}
	return QuoteIdent(database) + "." + QuoteIdent(schema);
}

static void InstallSchemaExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<SchemaOpBindData>();
	auto &gstate = data_p.global_state->Cast<SchemaOpGlobalState>();
	if (!gstate.executed) {
		gstate.executed = true;
		Connection conn(*context.db);
		auto qualified = QualifyLocation(bind.database, bind.schema);
		auto rendered = ApplySubstitutions(TigerSchemaSql(), {{"@TIGER@", qualified}});
		auto result = conn.Query(rendered);
		if (result->HasError()) {
			throw IOException("us_geocoder install_tiger_schema (%s): %s", qualified, result->GetError());
		}
		gstate.results.push_back({"installed:" + qualified, (int64_t)kDataTableCount});
	}

	EmitLoaderResults(gstate, output);
}

// =====================================================================
// set_tiger_reference(database VARCHAR, schema VARCHAR DEFAULT 'tiger')
//   Repoints the 13 local tiger.<table> to views over <database>.<schema>.<table>.
//   Passing database := NULL (or empty) restores empty local base tables.
// =====================================================================

static unique_ptr<FunctionData> SetReferenceBind(ClientContext &, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("table");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("kind"); // 'view' | 'base_table'

	auto bind = make_uniq<SchemaOpBindData>();
	bind->op = "set_reference";
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		bind->database = StringValue::Get(input.inputs[0]);
	}
	if (input.inputs.size() >= 2 && !input.inputs[1].IsNull() && !StringValue::Get(input.inputs[1]).empty()) {
		bind->schema = StringValue::Get(input.inputs[1]);
	}
	return std::move(bind);
}

static void SetReferenceExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<SchemaOpBindData>();
	auto &gstate = data_p.global_state->Cast<SchemaOpGlobalState>();

	if (!gstate.executed) {
		gstate.executed = true;
		Connection conn(*context.db);

		// Always operate on the local "tiger" schema (where macros live and read from).
		const std::string local = "tiger";
		const bool resetting = bind.database.empty();
		std::string source_qualified;
		if (!resetting) {
			source_qualified = QualifyLocation(bind.database, bind.schema);
		}

		conn.BeginTransaction();
		try {
			for (size_t i = 0; i < kDataTableCount; ++i) {
				const std::string tbl = kDataTables[i];
				const std::string local_ref = QuoteIdent(local) + "." + QuoteIdent(tbl);
				// Drop whichever form is currently there. DROP TABLE / DROP VIEW
				// are not mutually forgiving, so we try both.
				{
					auto r = conn.Query("DROP VIEW IF EXISTS " + local_ref);
					if (r->HasError()) { /* swallow; fall through to DROP TABLE */
					}
				}
				{
					auto r = conn.Query("DROP TABLE IF EXISTS " + local_ref);
					if (r->HasError()) {
						throw IOException("us_geocoder set_tiger_reference (drop %s): %s", tbl, r->GetError());
					}
				}

				std::string kind;
				if (resetting) {
					// Recreate empty local base tables from the schema template.
					// Render tiger_schema.sql.in but pull out just the single table.
					// Easier: render the whole template once per call (idempotent).
					// We do this ONCE outside the loop — see post-loop below.
					kind = "base_table";
				} else {
					auto view_sql =
					    "CREATE VIEW " + local_ref + " AS SELECT * FROM " + source_qualified + "." + QuoteIdent(tbl);
					auto r = conn.Query(view_sql);
					if (r->HasError()) {
						throw IOException("us_geocoder set_tiger_reference (create view %s): %s", tbl, r->GetError());
					}
					kind = "view";
				}
				gstate.results.push_back({tbl, 0});
				(void)kind; // captured into results' step field? No — we return tbl+kind below.
				            // Store kind in rows field as 0/1 to keep schema simple? Use a side vector.
				            // Simpler: re-run the tiger_schema template once after the loop to recreate
				            // base tables when resetting. We'll just report "view" or "base_table" via
				            // a second results field — extend LoaderResult below? Easier: encode in step.
				            // Keep step = tbl, rows = 0; we emit a "kind" column by looking up resetting.
			}

			if (resetting) {
				// Recreate all 13 base tables in one render.
				auto rendered = ApplySubstitutions(TigerSchemaSql(), {{"@TIGER@", QuoteIdent(local)}});
				auto r = conn.Query(rendered);
				if (r->HasError()) {
					throw IOException("us_geocoder set_tiger_reference (recreate base tables): %s", r->GetError());
				}
			}
			conn.Commit();
		} catch (...) {
			conn.Rollback();
			throw;
		}
	}

	idx_t emitted = 0;
	const std::string kind = bind.database.empty() ? "base_table" : "view";
	while (gstate.row_idx < gstate.results.size() && emitted < STANDARD_VECTOR_SIZE) {
		const auto &r = gstate.results[gstate.row_idx++];
		output.SetValue(0, emitted, Value(r.step));
		output.SetValue(1, emitted, Value(kind));
		++emitted;
	}
	output.SetCardinality(emitted);
}

// =====================================================================
// build_edge_containment(states VARCHAR | VARCHAR[], target_db := NULL,
//                        target_schema := 'tiger')
//   Run the derived_edge_containment step for one or more already-loaded
//   states. Idempotent — DELETEs existing rows for each statefp first.
//   Used after `load_tiger_*(build_containment := false)` to populate the
//   GEOIDs on demand.
// =====================================================================

static unique_ptr<FunctionData> BuildContainmentBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("build_edge_containment: states (VARCHAR or VARCHAR[]) is required");
	}
	auto bind_data = make_uniq<LoaderBindData>("tiger");
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "build_edge_containment");
	ApplyTargetParams(*bind_data, input);
	Connection conn(*context.db);
	ResolveStates(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static void BuildContainmentExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<LoaderBindData>();
	auto &gstate = data_p.global_state->Cast<LoaderGlobalState>();

	if (!gstate.executed) {
		gstate.executed = true;
		Connection conn(*context.db);
		const auto &data_loc = bind.data_location;
		const auto &func_loc = bind.func_schema;
		const auto &tmpl = LoaderTemplatesSql();
		auto section = ExtractSection(tmpl, "derived_edge_containment");

		for (size_t i = 0; i < bind.states.size(); ++i) {
			const auto &state = bind.states[i];
			char counter[64];
			snprintf(counter, sizeof(counter), "(%zu/%zu)", i + 1, bind.states.size());
			fprintf(stderr, "[us_geocoder %s] begin edge_containment %s\n", state.abbrev.c_str(), counter);
			fflush(stderr);
			auto t0 = std::chrono::steady_clock::now();
			gstate.results.push_back({"begin:" + state.abbrev, 0});
			// Idempotent: wipe existing rows for this state first, then recompute.
			auto del_sql = "DELETE FROM " + data_loc + ".edge_containment WHERE statefp = '" + state.fips + "'";
			auto del = conn.Query(del_sql);
			if (del->HasError()) {
				throw IOException("us_geocoder build_edge_containment (delete %s): %s", state.abbrev, del->GetError());
			}
			auto rendered =
			    RenderTemplate(section, {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}, {"@STATEFP@", state.fips}});
			StepTimer timer(state.abbrev, "edge_containment");
			int64_t rows = ExecuteInsert(conn, rendered, "edge_containment:" + state.abbrev);
			gstate.results.push_back({"edge_containment:" + state.abbrev, rows});
			gstate.results.push_back({"done:" + state.abbrev, 0});
			timer.Done(rows);
			auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			fprintf(stderr, "[us_geocoder %s] done edge_containment %s in %.1fs\n", state.abbrev.c_str(), counter,
			        secs);
			fflush(stderr);
		}
	}

	EmitLoaderResults(gstate, output);
}

// =====================================================================
// Registration
// =====================================================================

// Attach the standard named parameters to a load_tiger_* TableFunction
// before registration. `source` is accepted positionally on overloads that
// reserve a slot for it and as a named parameter everywhere.
static void AddLoaderNamedParams(TableFunction &fn) {
	fn.named_parameters["year"] = LogicalType::INTEGER;
	fn.named_parameters["source"] = LogicalType::VARCHAR;
	fn.named_parameters["target_db"] = LogicalType::VARCHAR;
	fn.named_parameters["target_schema"] = LogicalType::VARCHAR;
	fn.named_parameters["build_containment"] = LogicalType::BOOLEAN;
	fn.named_parameters["parallel"] = LogicalType::BOOLEAN;
	fn.named_parameters["temp_dir"] = LogicalType::VARCHAR;
	fn.named_parameters["parallel_workers"] = LogicalType::INTEGER;
}

void RegisterLoaderFunctions(ExtensionLoader &loader, const std::string &) {
	// load_tiger_nation([source VARCHAR], year := 2025,
	//                   target_db := NULL, target_schema := 'tiger')
	// Source defaults to the Census TIGER URL for the given year.
	// target_db / target_schema control where the TIGER data tables live.
	TableFunction nation_fn0("load_tiger_nation", {}, LoadTigerNationExecute, LoadTigerNationBind,
	                         LoaderGlobalState::Init);
	AddLoaderNamedParams(nation_fn0);
	loader.RegisterFunction(nation_fn0);

	TableFunction nation_fn1("load_tiger_nation", {LogicalType::VARCHAR}, LoadTigerNationExecute, LoadTigerNationBind,
	                         LoaderGlobalState::Init);
	AddLoaderNamedParams(nation_fn1);
	loader.RegisterFunction(nation_fn1);

	// load_tiger_state(state_abbrev VARCHAR [, source VARCHAR], ...)
	TableFunction state_fn1("load_tiger_state", {LogicalType::VARCHAR}, LoadTigerStateExecute, LoadTigerStateBind,
	                        LoaderGlobalState::Init);
	AddLoaderNamedParams(state_fn1);
	loader.RegisterFunction(state_fn1);

	TableFunction state_fn2("load_tiger_state", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LoadTigerStateExecute,
	                        LoadTigerStateBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(state_fn2);
	loader.RegisterFunction(state_fn2);

	// load_tiger_states(states VARCHAR[] [, source VARCHAR], ...)
	const auto list_vc = LogicalType::LIST(LogicalType::VARCHAR);
	TableFunction states_fn1("load_tiger_states", {list_vc}, LoadTigerStateExecute, LoadTigerStatesBind,
	                         LoaderGlobalState::Init);
	AddLoaderNamedParams(states_fn1);
	loader.RegisterFunction(states_fn1);

	TableFunction states_fn2("load_tiger_states", {list_vc, LogicalType::VARCHAR}, LoadTigerStateExecute,
	                         LoadTigerStatesBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(states_fn2);
	loader.RegisterFunction(states_fn2);

	// load_tiger_all_states([source VARCHAR], ...)
	//   Loads every row of state_lookup with statefp 01–56 (50 states + DC).
	TableFunction all_states_fn0("load_tiger_all_states", {}, LoadTigerStateExecute, LoadTigerAllStatesBind,
	                             LoaderGlobalState::Init);
	AddLoaderNamedParams(all_states_fn0);
	loader.RegisterFunction(all_states_fn0);

	TableFunction all_states_fn1("load_tiger_all_states", {LogicalType::VARCHAR}, LoadTigerStateExecute,
	                             LoadTigerAllStatesBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(all_states_fn1);
	loader.RegisterFunction(all_states_fn1);

	// unload_tiger_state(state_abbrev VARCHAR | VARCHAR[])
	const auto list_vc_unload = LogicalType::LIST(LogicalType::VARCHAR);
	TableFunction unload_fn1("unload_tiger_state", {LogicalType::VARCHAR}, UnloadTigerStateExecute,
	                         UnloadTigerStateBind, LoaderGlobalState::Init);
	unload_fn1.named_parameters["target_db"] = LogicalType::VARCHAR;
	unload_fn1.named_parameters["target_schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(unload_fn1);
	TableFunction unload_fn2("unload_tiger_state", {list_vc_unload}, UnloadTigerStateExecute, UnloadTigerStateBind,
	                         LoaderGlobalState::Init);
	unload_fn2.named_parameters["target_db"] = LogicalType::VARCHAR;
	unload_fn2.named_parameters["target_schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(unload_fn2);

	// install_tiger_schema(database VARCHAR [, schema VARCHAR DEFAULT 'tiger'])
	//   Creates the TIGER data tables in a target catalog. Idempotent.
	TableFunction install_fn1("install_tiger_schema", {LogicalType::VARCHAR}, InstallSchemaExecute, InstallSchemaBind,
	                          SchemaOpGlobalState::Init);
	loader.RegisterFunction(install_fn1);
	TableFunction install_fn2("install_tiger_schema", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                          InstallSchemaExecute, InstallSchemaBind, SchemaOpGlobalState::Init);
	loader.RegisterFunction(install_fn2);

	// set_tiger_reference([database VARCHAR, schema VARCHAR DEFAULT 'tiger'])
	//   No-arg / NULL / empty-string first arg → restore empty local base tables.
	//   Non-empty database → repoint local tiger.<table> to VIEWs over <db>.<schema>.<table>.
	TableFunction set_ref_fn0("set_tiger_reference", {}, SetReferenceExecute, SetReferenceBind,
	                          SchemaOpGlobalState::Init);
	loader.RegisterFunction(set_ref_fn0);
	TableFunction set_ref_fn1("set_tiger_reference", {LogicalType::VARCHAR}, SetReferenceExecute, SetReferenceBind,
	                          SchemaOpGlobalState::Init);
	loader.RegisterFunction(set_ref_fn1);
	TableFunction set_ref_fn2("set_tiger_reference", {LogicalType::VARCHAR, LogicalType::VARCHAR}, SetReferenceExecute,
	                          SetReferenceBind, SchemaOpGlobalState::Init);
	loader.RegisterFunction(set_ref_fn2);

	// build_edge_containment(VARCHAR | VARCHAR[], target_db := NULL,
	//                        target_schema := 'tiger'])
	const auto list_vc2 = LogicalType::LIST(LogicalType::VARCHAR);
	TableFunction bec_varchar("build_edge_containment", {LogicalType::VARCHAR}, BuildContainmentExecute,
	                          BuildContainmentBind, LoaderGlobalState::Init);
	bec_varchar.named_parameters["target_db"] = LogicalType::VARCHAR;
	bec_varchar.named_parameters["target_schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(bec_varchar);

	TableFunction bec_list("build_edge_containment", {list_vc2}, BuildContainmentExecute, BuildContainmentBind,
	                       LoaderGlobalState::Init);
	bec_list.named_parameters["target_db"] = LogicalType::VARCHAR;
	bec_list.named_parameters["target_schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(bec_list);
}

} // namespace us_geocoder
} // namespace duckdb
