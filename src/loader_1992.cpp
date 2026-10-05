#include "us_geocoder_loader_1992.hpp"
#include "us_geocoder_loader_internal.hpp"
#include "us_geocoder_zip.hpp"
#include "us_geocoder_embed.hpp"
#include "us_geocoder_embedded_sql.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <regex>
#include <utility>

namespace duckdb {
namespace us_geocoder {

static const char *kDefaultSource1992 = "https://www2.census.gov/geo/tiger/TIGER1992";

// =====================================================================
// Bind data + global state
// =====================================================================

struct Loader1992BindData : public FunctionData {
	struct StatePlan {
		std::string abbrev;
		std::string fips;
	};
	std::string source = kDefaultSource1992;
	std::string target_db;                  // empty => current catalog
	std::string target_schema = "tiger";
	bool build_containment = true;
	bool parallel = true;
	int parallel_workers = 16;
	std::string temp_dir;
	std::vector<StatePlan> states;
	bool unload = false;

	// "tiger" locally, or "<db>.<schema>" when target_db is set. The local
	// form is deliberately NOT quoted: it has to compare equal to FuncLoc()
	// so BootstrapTargetSchema's `data_location == func_schema` early return
	// fires in the default case, exactly as the modern loader's
	// ApplyTargetParams arranges (loader.cpp: bind.data_location =
	// bind.target_schema). Quoting it here made every 1992 state re-render
	// and re-execute the whole TigerSchemaSql() DDL — harmless while every
	// statement is IF NOT EXISTS, but 51 redundant passes under
	// load_tiger_1992_all_states(), and the ALTER TABLE ... ADD COLUMN
	// statements would error against a set_tiger_reference view setup.
	std::string DataLoc() const {
		if (target_db.empty()) {
			return target_schema;
		}
		return QuoteIdent(target_db) + "." + QuoteIdent(target_schema);
	}
	// Macros always live locally.
	std::string FuncLoc() const {
		return "tiger";
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<Loader1992BindData>(*this);
	}
	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<Loader1992BindData>();
		return source == o.source && target_db == o.target_db && target_schema == o.target_schema &&
		       build_containment == o.build_containment && unload == o.unload;
	}
};

struct Loader1992GlobalState : public GlobalTableFunctionState {
	std::vector<LoaderResult> results;
	idx_t row_idx = 0;
	bool ran = false;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<Loader1992GlobalState>();
	}
};

// =====================================================================
// Source resolution
// =====================================================================

// Where a county's extracted files live for this load. Returns the directory
// holding TGR<ssccc>.F5*, extracting from a zip first if needed.
// `work_dir` is the loader's temp dir (used only for modes 1 and 2).
static std::string ResolveCountyDir(ClientContext &context, FileSystem &fs, const Loader1992BindData &bind,
                                    const std::string &fips, const std::string &cfp, const std::string &work_dir) {
	const std::string ssccc = fips + cfp;
	const std::vector<std::string> wanted = {"TGR" + ssccc + ".F51", "TGR" + ssccc + ".F52",
	                                         "TGR" + ssccc + ".F54", "TGR" + ssccc + ".F55",
	                                         "TGR" + ssccc + ".F56", "TGR" + ssccc + ".F5A",
	                                         "TGR" + ssccc + ".F5I"};
	const bool remote = bind.source.rfind("http://", 0) == 0 || bind.source.rfind("https://", 0) == 0;
	if (remote) {
		// Already downloaded into work_dir by the caller.
		const std::string local_zip = fs.JoinPath(work_dir, ssccc + ".zip");
		const std::string dest = fs.JoinPath(work_dir, ssccc);
		ExtractZipEntries(fs, local_zip, dest, wanted);
		return dest;
	}
	// Mode 2: local nested zip.
	const std::string zip_path = fs.JoinPath(fs.JoinPath(bind.source, fips), ssccc + ".zip");
	if (fs.FileExists(zip_path)) {
		const std::string dest = fs.JoinPath(work_dir, ssccc);
		ExtractZipEntries(fs, zip_path, dest, wanted);
		return dest;
	}
	// Mode 3: local already-extracted tree.
	const std::string dir = fs.JoinPath(fs.JoinPath(bind.source, fips), ssccc);
	if (fs.DirectoryExists(dir)) {
		return dir;
	}
	throw IOException("us_geocoder 1992: no county data for %s — looked for %s and %s", ssccc, zip_path, dir);
}

// RT4/RT5/RT6 are optional per county. read_csv errors on a missing file, so
// substitute a known-empty file when the real one is absent.
static std::string PathOrEmpty(FileSystem &fs, const std::string &dir, const std::string &name,
                               const std::string &work_dir) {
	const std::string p = fs.JoinPath(dir, name);
	if (fs.FileExists(p)) {
		return p;
	}
	const std::string stub = fs.JoinPath(work_dir, "__empty__");
	if (!fs.FileExists(stub)) {
		auto h = fs.OpenFile(stub, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		h.reset();
	}
	return stub;
}

// Regex matching a state's per-county zip filenames inside a Census HTML
// directory index.
//
// ScrapeCensusIndex applies this with std::sregex_iterator over the WHOLE
// index document. C++11 std::regex has no multiline flag, so `^` and `$`
// anchor to the start and end of the entire subject string — an anchored
// pattern can only match when the whole HTML body IS the bare filename,
// i.e. never against a real index. The pattern must therefore be
// unanchored, like the modern loader's equivalent in
// BuildStateDownloadTargets.
//
// Unanchored matching on 1992's short "<ssccc>.zip" names is more
// collision-prone than the modern "tl_<year>_<fips>_<type>.zip" form, so
// both ends get a boundary:
//   * leading — start-of-subject, or a *consumed* non-alphanumeric
//     character. std::regex has no lookbehind and ScrapeCensusIndex returns
//     group 0, so the delimiter lands inside the match and
//     CountyFpFrom1992IndexName strips it (which is why that helper counts
//     back from the end of the match, not forward from the start — the
//     `^` arm of the alternation consumes nothing). Excluding only
//     [0-9A-Za-z] (not '.' or '/') keeps hrefs such as "./34999.zip"
//     matching while rejecting "134999.zip" and "x34999.zip". `^` here is
//     start-of-*subject*, not start-of-line, which is exactly what a bare
//     newline-delimited listing whose first line is a county zip needs;
//     std::sregex_iterator sets match_prev_avail on later iterations so it
//     cannot fire again mid-document.
//   * trailing — a negative lookahead, so "34998.zip.decoy" does not yield
//     a bogus county 998.
static std::string CountyZipPattern1992(const std::string &fips) {
	return "(?:^|[^0-9A-Za-z])" + fips + "[0-9]{3}\\.zip(?![0-9A-Za-z._-])";
}

// One CountyZipPattern1992 match -> the 3-digit county FIPS. A match is
// "<ss><ccc>.zip" optionally preceded by one consumed delimiter, so the
// name is always the trailing len(fips)+3+len(".zip") characters.
static bool CountyFpFrom1992IndexName(const std::string &match, const std::string &fips, std::string &out) {
	const size_t name_len = fips.size() + 3 + 4; // <ss><ccc>.zip
	if (match.size() < name_len) {
		return false;
	}
	const size_t start = match.size() - name_len;
	if (match.compare(start, fips.size(), fips) != 0) {
		return false;
	}
	out = match.substr(start + fips.size(), 3);
	return true;
}

// Apply CountyZipPattern1992 to an index document and return the raw group-0
// matches, exactly as ScrapeCensusIndex would (same std::sregex_iterator
// walk over the whole body, same group 0). Factored out so the
// us_geocoder_1992_county_index test hook exercises the real matching
// without needing an HTTP server. Keep in step with ScrapeCensusIndex.
static std::vector<std::string> MatchCountyZips1992(const std::string &index_html, const std::string &fips) {
	std::regex pat(CountyZipPattern1992(fips));
	std::vector<std::string> out;
	auto begin = std::sregex_iterator(index_html.begin(), index_html.end(), pat);
	auto end = std::sregex_iterator();
	for (auto it = begin; it != end; ++it) {
		out.push_back(it->str(0));
	}
	return out;
}

// Index matches -> sorted, deduped county FIPS codes. Dedup is required
// rather than cosmetic: an Apache index lists each file twice (anchor href
// and visible text) and the two matches carry different leading delimiters,
// so ScrapeCensusIndex's own string-level dedup cannot collapse them.
static std::vector<std::string> CountyFpsFrom1992IndexNames(const std::vector<std::string> &names,
                                                            const std::string &fips) {
	std::vector<std::string> out;
	for (size_t i = 0; i < names.size(); ++i) {
		std::string cfp;
		if (CountyFpFrom1992IndexName(names[i], fips, cfp)) {
			out.push_back(cfp);
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

// County FIPS list for a state. Remote: scrape <source>/<ss>/ for
// <ssccc>.zip. Local: list <source>/<ss>/ for either <ssccc>.zip files or
// <ssccc>/ directories, so both local layouts work.
static std::vector<std::string> ListCounties1992(ClientContext &context, FileSystem &fs,
                                                 const Loader1992BindData &bind, const std::string &fips) {
	std::vector<std::string> out;
	const bool remote = bind.source.rfind("http://", 0) == 0 || bind.source.rfind("https://", 0) == 0;
	if (remote) {
		std::string base = bind.source;
		if (!base.empty() && base.back() == '/') {
			base.pop_back();
		}
		ParallelDownloadOptions opts;
		opts.workers = 1; // one GET for the directory index
		opts.max_attempts = 3;
		opts.log_prefix = fips;
		const std::string pattern_str = CountyZipPattern1992(fips);
		std::regex pat(pattern_str);
		auto names = ScrapeCensusIndex(context, base + "/" + fips + "/", pat, opts);
		out = CountyFpsFrom1992IndexNames(names, fips);
		if (out.empty()) {
			throw IOException("us_geocoder 1992: index %s/%s/ yielded no county zips (pattern %s)", base, fips,
			                  pattern_str);
		}
	} else {
		const std::string state_dir = fs.JoinPath(bind.source, fips);
		if (!fs.DirectoryExists(state_dir)) {
			throw IOException("us_geocoder 1992: no such directory: %s", state_dir);
		}
		fs.ListFiles(state_dir, [&](const string &name, bool is_dir) {
			if (is_dir) {
				if (name.size() == 5 && name.compare(0, 2, fips) == 0) {
					out.push_back(name.substr(2, 3));
				}
			} else if (name.size() == 9 && name.compare(0, 2, fips) == 0 &&
			           name.compare(5, 4, ".zip") == 0) {
				out.push_back(name.substr(2, 3));
			}
		});
		if (out.empty()) {
			throw IOException("us_geocoder 1992: no county zips or county directories under %s", state_dir);
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

// Directory holding TGR92S<ss>.NAM. Extracted from OtherFiles.zip for
// remote and local-zip sources; for an already-extracted tree it is just
// <source>/<ss>.
static std::string ResolveStateOtherDir(ClientContext &context, FileSystem &fs, const Loader1992BindData &bind,
                                        const std::string &fips, const std::string &work_dir) {
	const std::vector<std::string> wanted = {"TGR92S" + fips + ".NAM"};
	const bool remote = bind.source.rfind("http://", 0) == 0 || bind.source.rfind("https://", 0) == 0;
	if (remote) {
		// Downloaded by the prelude in DoLoadState1992.
		const std::string zip = fs.JoinPath(work_dir, "OtherFiles.zip");
		const std::string dest = fs.JoinPath(work_dir, "other");
		ExtractZipEntries(fs, zip, dest, wanted);
		return dest;
	}
	const std::string zip = fs.JoinPath(fs.JoinPath(bind.source, fips), "OtherFiles.zip");
	if (fs.FileExists(zip)) {
		const std::string dest = fs.JoinPath(work_dir, "other");
		ExtractZipEntries(fs, zip, dest, wanted);
		return dest;
	}
	const std::string dir = fs.JoinPath(bind.source, fips);
	if (fs.FileExists(fs.JoinPath(dir, "TGR92S" + fips + ".NAM"))) {
		return dir;
	}
	throw IOException("us_geocoder 1992: no TGR92S%s.NAM — looked in %s and %s", fips, zip, dir);
}

// =====================================================================
// Bind-input helpers
// =====================================================================

// Extract varchar abbrevs from a LIST<VARCHAR> Value (or a single scalar
// VARCHAR). Local copy of loader.cpp's AbbrevsFromValue (static there too —
// internal linkage means no ODR conflict across translation units).
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

// Resolve a list of state abbreviations into Loader1992BindData::states[]
// (in order). Dedups case-insensitively while preserving first-occurrence
// order; errors on any unknown abbrev. Macros/lookup tables for the 1992
// loader always live in the local "tiger" schema (FuncLoc()), independent
// of target_db/target_schema.
static void ResolveStates1992(Connection &conn, Loader1992BindData &bind, const std::vector<std::string> &abbrevs) {
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
		auto fips = LookupStateFips(conn, "tiger", up);
		bind.states.push_back({up, fips});
	}
	if (bind.states.empty()) {
		throw BinderException("us_geocoder: no state abbreviations provided");
	}
}

// target_db / target_schema named params, shared by every 1992 entry point.
static void ApplyTargetParams1992(Loader1992BindData &bind, const TableFunctionBindInput &input) {
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
}

// Full named-parameter set for load_tiger_1992_*: target params plus
// source/build_containment/parallel/temp_dir/parallel_workers.
// `source_input_index` is the positional slot that accepts the source
// string on this overload (-1 to disable positional).
static void Apply1992Params(Loader1992BindData &bind, const TableFunctionBindInput &input, int source_input_index) {
	ApplyTargetParams1992(bind, input);
	bool found_source = false;
	if (source_input_index >= 0 && static_cast<int>(input.inputs.size()) > source_input_index &&
	    !input.inputs[source_input_index].IsNull() &&
	    !StringValue::Get(input.inputs[source_input_index]).empty()) {
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
	// else: bind.source keeps its struct-default (kDefaultSource1992).
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
	// `parallel` only controls download worker count for the 1992 loader
	// (unlike the modern loader, there's no separate /vsicurl/-direct path
	// to fall back to); still meaningless with a local source.
	const bool source_is_http = bind.source.rfind("http://", 0) == 0 || bind.source.rfind("https://", 0) == 0;
	if (bind.parallel && !source_is_http) {
		fprintf(stderr,
		        "[us_geocoder 1992] WARN: parallel := true has no effect with a local source (%s); "
		        "ignored.\n",
		        bind.source.c_str());
		fflush(stderr);
		bind.parallel = false;
	}
}

// =====================================================================
// DoLoadState1992 — per-state ingest
// =====================================================================

static void DoLoadState1992(ClientContext &context, const Loader1992BindData &bind,
                            const Loader1992BindData::StatePlan &state, std::vector<LoaderResult> &out) {
	Connection conn(*context.db);
	auto &fs = FileSystem::GetFileSystem(context);
	const std::string data_loc = bind.DataLoc();
	const std::string func_loc = bind.FuncLoc();
	const std::string &fips = state.fips;
	const std::string tmpl = Loader1992TemplatesSql();
	const std::string state_pfx = "tiger1992:state:" + fips + ":";
	const bool remote = bind.source.rfind("http://", 0) == 0 || bind.source.rfind("https://", 0) == 0;

	EnsureHttpfsIfRemote(*context.db, bind.source);

	// Create the TIGER tables in the target catalog when target_db is set.
	// The modern loader does this at the head of DoLoadStateImpl; without it,
	// `target_db := 'hist92'` would try to INSERT into tables that do not exist.
	BootstrapTargetSchema(conn, data_loc, func_loc);

	// Vintage guard goes here — see Task 6, Step 4.

	std::vector<std::string> countyfps = ListCounties1992(context, fs, bind, fips);

	struct StateStep {
		const char *section;
		const char *progress;
		const char *table;
	};
	const StateStep state_steps[] = {
	    {"state_state", "state", "state"},
	    {"state_county", "county", "county"},
	    {"state_place", "place", "place"},
	    {"state_cousub", "cousub", "cousub"}};
	struct CountyStep {
		const char *section;
		const char *progress_table;
	};
	const CountyStep county_steps[] = {{"county_edges", "edges"},
	                                   {"county_faces", "faces"},
	                                   {"county_featnames", "featnames"},
	                                   {"county_addr", "addr"}};

	// Consult the progress ledger BEFORE touching the network. Re-running
	// against a remote source used to re-download the whole state and then
	// report every step ":skipped"; now only the counties with pending work
	// are fetched, and a fully-loaded state does no HTTP at all beyond the
	// one directory-index GET that ListCounties1992 needs to enumerate
	// counties in the first place.
	bool state_steps_pending = false;
	for (const auto &s : state_steps) {
		if (!IsProgressDone(conn, data_loc, state_pfx + s.progress)) {
			state_steps_pending = true;
			break;
		}
	}
	std::vector<std::string> pending_counties;
	for (size_t ci = 0; ci < countyfps.size(); ++ci) {
		for (const auto &t : county_steps) {
			if (!IsProgressDone(conn, data_loc, state_pfx + "county:" + countyfps[ci] + ":" + t.progress_table)) {
				pending_counties.push_back(countyfps[ci]);
				break;
			}
		}
	}
	const bool have_work = state_steps_pending || !pending_counties.empty();

	// work_dir: scratch for extraction (always) and downloads (remote only).
	// Reuse the modern loader's helpers so temp_dir resolution and the
	// per-state directory naming stay identical across both paths. Left empty
	// when there is nothing to do, in which case StateDirCleanup is a no-op
	// and no step below ever consults it (both the .NAM and the per-county
	// directory are resolved lazily, only for a pending step).
	std::string work_dir;
	if (have_work) {
		const std::string temp_base = ResolveTempBase(context, bind.temp_dir);
		work_dir = MakeStateTempDir(context, temp_base, state.abbrev, 1992);
	}
	// Always-armed, like the modern loader (loader.cpp's DoLoadState /
	// DoLoadNation preludes). Deliberately no Disarm() on failure: the
	// directory name embeds the pid and a steady_clock timestamp, so nothing
	// ever looks for a retained one — the next run makes a fresh dir and
	// re-downloads regardless, leaving the old one to leak disk forever.
	// Not re-downloading already-ingested counties (above) is what actually
	// makes a restart cheap. Resumable downloads would need a deterministic
	// directory name plus a lock or completion marker; out of scope here.
	StateDirCleanup cleanup(fs, work_dir);

	if (remote && have_work) {
		std::string src = bind.source;
		if (!src.empty() && src.back() == '/') {
			src.pop_back();
		}
		std::vector<DownloadTarget> targets;
		for (size_t i = 0; i < pending_counties.size(); ++i) {
			const std::string ssccc = fips + pending_counties[i];
			targets.push_back({src + "/" + fips + "/" + ssccc + ".zip", fs.JoinPath(work_dir, ssccc + ".zip")});
		}
		// Unconditional (not gated on `state_steps_pending`): we're already
		// inside `if (remote && have_work)`, and the state-level loop below
		// force-rebuilds whenever any county inserted this call (see its
		// comment), even if the state-level progress keys were already
		// marked done — so a pending-counties-only restart still needs the
		// NAM file fetched.
		targets.push_back({src + "/" + fips + "/OtherFiles.zip", fs.JoinPath(work_dir, "OtherFiles.zip")});
		ParallelDownloadOptions opts;
		opts.workers = bind.parallel ? bind.parallel_workers : 1;
		opts.log_prefix = state.abbrev;
		auto dl = ParallelDownload(context, targets, opts);
		(void)dl;
	}

	// Per-county first, so `any_county_inserted` is known before the
	// state-level and derived steps run. Those steps gate on their own
	// progress key, but a progress key marked "done" on a prior call must
	// not suppress a rebuild when this call just ingested a county that
	// call hadn't seen yet (e.g. a partial local mirror completed between
	// runs) — county data has nothing the state-level steps or NAM parse
	// depend on, so running counties first is safe.
	bool any_county_inserted = false;
	for (size_t ci = 0; ci < countyfps.size(); ++ci) {
		const auto &cfp = countyfps[ci];
		const std::string ssccc = fips + cfp;
		std::string dir; // resolved lazily, only if some step is pending
		for (const auto &t : county_steps) {
			const std::string key = state_pfx + "county:" + cfp + ":" + t.progress_table;
			if (IsProgressDone(conn, data_loc, key)) {
				out.push_back({std::string(t.section) + ":" + cfp + ":skipped", 0});
				continue;
			}
			if (dir.empty()) {
				dir = ResolveCountyDir(context, fs, bind, fips, cfp, work_dir);
			}
			auto sql =
			    RenderTemplate(ExtractSection(tmpl, t.section),
			                   {{"@TIGER@", data_loc},
			                    {"@FUNC@", func_loc},
			                    {"@STATEFP@", fips},
			                    {"@COUNTYFP@", cfp},
			                    {"@F51@", fs.JoinPath(dir, "TGR" + ssccc + ".F51")},
			                    {"@F52@", PathOrEmpty(fs, dir, "TGR" + ssccc + ".F52", work_dir)},
			                    {"@F54@", PathOrEmpty(fs, dir, "TGR" + ssccc + ".F54", work_dir)},
			                    {"@F55@", PathOrEmpty(fs, dir, "TGR" + ssccc + ".F55", work_dir)},
			                    {"@F56@", PathOrEmpty(fs, dir, "TGR" + ssccc + ".F56", work_dir)},
			                    {"@F5A@", fs.JoinPath(dir, "TGR" + ssccc + ".F5A")},
			                    {"@F5I@", fs.JoinPath(dir, "TGR" + ssccc + ".F5I")}});
			int64_t rows = ExecuteInsert(conn, sql, std::string(t.section) + ":" + cfp);
			out.push_back({std::string(t.section) + ":" + cfp, rows});
			MarkProgressDone(conn, data_loc, key);
			any_county_inserted = true;
		}
	}

	// Per-state names. The county sections above do not depend on them, but
	// derived_zip_lookup_base/derived_zip_state_loc below join place/county,
	// so this has to run before the derived loop even though it now runs
	// after the county loop.
	//
	// Gated like the derived steps: `!any_county_inserted` is required (not
	// just the section's own progress key) so a newly-ingested county's
	// place/county/cousub rows get parsed out of the (already-fully-listing)
	// NAM file even when this section previously completed. state_state and
	// state_county re-insert idempotently via their own `NOT IN` guards, but
	// state_place/state_cousub have no such guard — they re-parse the whole
	// NAM file unconditionally — so every re-run DELETEs this state's rows
	// first, the same idempotency rule the derived loop below uses.
	const std::string stusps = state.abbrev;
	std::string nam; // resolved lazily, only if some state step is pending
	for (const auto &s : state_steps) {
		const std::string key = state_pfx + s.progress;
		if (!any_county_inserted && IsProgressDone(conn, data_loc, key)) {
			out.push_back({std::string(s.section) + ":skipped", 0});
			continue;
		}
		if (nam.empty()) {
			const std::string nam_dir = ResolveStateOtherDir(context, fs, bind, fips, work_dir);
			nam = fs.JoinPath(nam_dir, "TGR92S" + fips + ".NAM");
		}
		conn.Query("DELETE FROM " + data_loc + "." + s.table + " WHERE statefp = '" + fips + "'");
		auto sql = RenderTemplate(ExtractSection(tmpl, s.section), {{"@TIGER@", data_loc},
		                                                            {"@FUNC@", func_loc},
		                                                            {"@STATEFP@", fips},
		                                                            {"@STUSPS@", stusps},
		                                                            {"@NAM@", nam}});
		int64_t rows = ExecuteInsert(conn, sql, s.section);
		out.push_back({s.section, rows});
		MarkProgressDone(conn, data_loc, key);
	}

	// Derived per-state tables. The zip_* sections are reused verbatim from
	// the modern templates — they are pure SQL over edges/faces/place/state
	// and carry no vintage assumptions.
	const std::string modern_tmpl = LoaderTemplatesSql();
	struct DerivedStep {
		const char *section;
		const char *table;
		const char *progress;
		bool from_1992_templates;
	};
	std::vector<DerivedStep> derived = {
	    {"derived_zip_state", "zip_state", "zip_state", false},
	    {"derived_zip_state_loc", "zip_state_loc", "zip_state_loc", false},
	    {"derived_zip_lookup_base", "zip_lookup_base", "zip_lookup_base", false},
	};
	// build_containment is accepted and stored by Task 4; this loop is what
	// reads it: `true` (the default) appends the containment section below,
	// `false` leaves it out, so edge_containment is never touched this call.
	if (bind.build_containment) {
		derived.push_back({"derived_edge_containment", "edge_containment", "edge_containment", true});
	}
	for (const auto &d : derived) {
		const std::string key = state_pfx + "derived:" + d.progress;
		// `!any_county_inserted` forces a rebuild even when this section's
		// own progress key says "done" — otherwise a county added since the
		// last call would ingest cleanly but leave the derived tables (and
		// edge_containment) silently stale for that county's rows.
		if (!any_county_inserted && IsProgressDone(conn, data_loc, key)) {
			out.push_back({std::string(d.section) + ":skipped", 0});
			continue;
		}
		// Absent progress entry means either a fresh load or a partial
		// restart that just filled in counties; DELETE first so a rebuild
		// cannot duplicate.
		conn.Query("DELETE FROM " + data_loc + "." + d.table + " WHERE statefp = '" + fips + "'");
		auto section = ExtractSection(d.from_1992_templates ? tmpl : modern_tmpl, d.section);
		auto sql = RenderTemplate(section, {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}, {"@STATEFP@", fips}});
		int64_t rows = ExecuteInsert(conn, sql, d.section);
		out.push_back({d.section, rows});
		MarkProgressDone(conn, data_loc, key);
	}
}

static void Load1992Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<Loader1992BindData>();
	auto &gstate = data_p.global_state->Cast<Loader1992GlobalState>();

	if (!gstate.ran) {
		gstate.ran = true;
		for (size_t i = 0; i < bind.states.size(); ++i) {
			const auto &st = bind.states[i];
			char counter[64];
			snprintf(counter, sizeof(counter), "(%zu/%zu)", i + 1, bind.states.size());
			fprintf(stderr, "[us_geocoder 1992 %s] begin %s\n", st.abbrev.c_str(), counter);
			fflush(stderr);
			auto t0 = std::chrono::steady_clock::now();
			gstate.results.push_back({"begin:" + st.abbrev, 0});
			DoLoadState1992(context, bind, st, gstate.results);
			gstate.results.push_back({"done:" + st.abbrev, 0});
			auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			fprintf(stderr, "[us_geocoder 1992 %s] done %s in %.1fs\n", st.abbrev.c_str(), counter, secs);
			fflush(stderr);
		}
		RunAnalyzeOnTigerTables(context, bind.DataLoc(), gstate.results);
	}

	EmitLoaderResults(gstate, output);
}

// =====================================================================
// load_tiger_1992_state / _states / _all_states — bind
// =====================================================================

static unique_ptr<FunctionData> Load1992StateBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("load_tiger_1992_state: state_abbrev is required");
	}
	auto bind_data = make_uniq<Loader1992BindData>();
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "load_tiger_1992_state");
	Apply1992Params(*bind_data, input, /*source_input_index=*/1);
	Connection conn(*context.db);
	ResolveStates1992(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static unique_ptr<FunctionData> Load1992StatesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("load_tiger_1992_states: states (VARCHAR[]) is required");
	}
	auto bind_data = make_uniq<Loader1992BindData>();
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "load_tiger_1992_states");
	Apply1992Params(*bind_data, input, /*source_input_index=*/1);
	Connection conn(*context.db);
	ResolveStates1992(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static unique_ptr<FunctionData> Load1992AllStatesBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	auto bind_data = make_uniq<Loader1992BindData>();
	Apply1992Params(*bind_data, input, /*source_input_index=*/0);

	// Same enumeration the modern load_tiger_all_states uses: 50 states + DC
	// (FIPS 01-56, with gaps) from the local state_lookup.
	Connection conn(*context.db);
	auto result = conn.Query("SELECT abbrev, statefp FROM tiger.state_lookup "
	                         "WHERE statefp::INT BETWEEN 1 AND 56 ORDER BY statefp");
	if (result->HasError()) {
		throw IOException("us_geocoder load_tiger_1992_all_states: %s", result->GetError());
	}
	while (auto row = result->Fetch()) {
		for (idx_t i = 0; i < row->size(); ++i) {
			auto abbrev_v = row->GetValue(0, i);
			auto fips_v = row->GetValue(1, i);
			if (!abbrev_v.IsNull() && !fips_v.IsNull()) {
				bind_data->states.push_back({abbrev_v.GetValue<std::string>(), fips_v.GetValue<std::string>()});
			}
		}
	}
	if (bind_data->states.empty()) {
		throw BinderException("us_geocoder: no state abbreviations provided");
	}
	return std::move(bind_data);
}

// =====================================================================
// unload_tiger_1992_state(state_abbrev VARCHAR | VARCHAR[],
//                        target_db := NULL, target_schema := 'tiger')
// =====================================================================

static unique_ptr<FunctionData> Unload1992Bind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("unload_tiger_1992_state: state abbrev is required");
	}
	auto bind_data = make_uniq<Loader1992BindData>();
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "unload_tiger_1992_state");
	ApplyTargetParams1992(*bind_data, input);
	Connection conn(*context.db);
	ResolveStates1992(conn, *bind_data, abbrevs);
	bind_data->unload = true;
	return std::move(bind_data);
}

static void Unload1992Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<Loader1992BindData>();
	auto &gstate = data_p.global_state->Cast<Loader1992GlobalState>();
	if (!gstate.ran) {
		gstate.ran = true;
		Connection conn(*context.db);
		const std::string data_loc = bind.DataLoc();
		const std::string func_loc = bind.FuncLoc();
		BootstrapTargetSchema(conn, data_loc, func_loc);
		const auto &tmpl = LoaderTemplatesSql();
		auto unload_template = ExtractSection(tmpl, "unload_state");
		for (const auto &state : bind.states) {
			StepTimer t(state.abbrev, "unload_1992_state");
			auto rendered = RenderTemplate(unload_template,
			                               {{"@TIGER@", data_loc}, {"@FUNC@", func_loc}, {"@STATEFP@", state.fips}});
			auto result = conn.Query(rendered);
			if (result->HasError()) {
				throw IOException("us_geocoder unload_tiger_1992_state(%s): %s", state.abbrev, result->GetError());
			}
			// 1992 writes its own county + state rows per-state (unlike the
			// modern nation-level load_tiger_nation path), so the shared
			// unload_state template's coverage of the other 11 tables needs
			// two extra per-state deletes here.
			auto del_county = conn.Query("DELETE FROM " + data_loc + ".county WHERE statefp = '" + state.fips + "'");
			if (del_county->HasError()) {
				throw IOException("us_geocoder unload_tiger_1992_state(%s): %s", state.abbrev,
				                  del_county->GetError());
			}
			auto del_state = conn.Query("DELETE FROM " + data_loc + ".state WHERE statefp = '" + state.fips + "'");
			if (del_state->HasError()) {
				throw IOException("us_geocoder unload_tiger_1992_state(%s): %s", state.abbrev, del_state->GetError());
			}
			DeleteProgressLike(conn, data_loc, "tiger1992:state:" + state.fips + ":");
			gstate.results.push_back({"unload:" + state.abbrev, 0});
			t.Done(0);
		}
	}
	EmitLoaderResults(gstate, output);
}

// =====================================================================
// us_geocoder_1992_county_index(index_html, statefp) -> TABLE(countyfp)
// =====================================================================
//
// Test hook, not documented API. A pure function over an index document:
// it runs exactly the pattern match + group-0 extraction that
// ListCounties1992's remote branch performs on a scraped Census directory
// index, with the HTTP fetch removed. It exists because the sqllogictest
// harness cannot stand up an HTTP server, and remote county discovery had
// no coverage at all — the anchored pattern this replaced could never
// match a real index, so `CALL load_tiger_1992_state('NJ')` against the
// default source was unconditionally broken and no test noticed. Same
// spirit as us_geocoder_unzip, which exposes the loader's zip-extraction
// internals for test/sql/tiger1992_zip.test.

struct CountyIndex1992BindData : public FunctionData {
	std::vector<std::string> countyfps;
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CountyIndex1992BindData>(*this);
	}
	bool Equals(const FunctionData &other) const override {
		return countyfps == other.Cast<CountyIndex1992BindData>().countyfps;
	}
};

struct CountyIndex1992GlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<CountyIndex1992GlobalState>();
	}
};

static unique_ptr<FunctionData> CountyIndex1992Bind(ClientContext &, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("countyfp");
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("us_geocoder_1992_county_index: index_html and statefp are both required");
	}
	auto result = make_uniq<CountyIndex1992BindData>();
	const auto html = StringValue::Get(input.inputs[0]);
	const auto fips = StringValue::Get(input.inputs[1]);
	result->countyfps = CountyFpsFrom1992IndexNames(MatchCountyZips1992(html, fips), fips);
	return std::move(result);
}

static void CountyIndex1992Execute(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<CountyIndex1992BindData>();
	auto &gstate = data_p.global_state->Cast<CountyIndex1992GlobalState>();
	idx_t count = 0;
	while (gstate.offset < bind.countyfps.size() && count < STANDARD_VECTOR_SIZE) {
		output.SetValue(0, count, Value(bind.countyfps[gstate.offset]));
		++gstate.offset;
		++count;
	}
	output.SetCardinality(count);
}

// =====================================================================
// Registration
// =====================================================================

static void Add1992NamedParams(TableFunction &fn) {
	fn.named_parameters["source"] = LogicalType::VARCHAR;
	fn.named_parameters["target_db"] = LogicalType::VARCHAR;
	fn.named_parameters["target_schema"] = LogicalType::VARCHAR;
	fn.named_parameters["build_containment"] = LogicalType::BOOLEAN;
	fn.named_parameters["parallel"] = LogicalType::BOOLEAN;
	fn.named_parameters["temp_dir"] = LogicalType::VARCHAR;
	fn.named_parameters["parallel_workers"] = LogicalType::INTEGER;
}

void RegisterLoader1992Functions(ExtensionLoader &loader, const std::string &) {
	const auto list_vc = LogicalType::LIST(LogicalType::VARCHAR);

	TableFunction st1("load_tiger_1992_state", {LogicalType::VARCHAR}, Load1992Execute, Load1992StateBind,
	                  Loader1992GlobalState::Init);
	Add1992NamedParams(st1);
	loader.RegisterFunction(st1);

	TableFunction st2("load_tiger_1992_state", {LogicalType::VARCHAR, LogicalType::VARCHAR}, Load1992Execute,
	                  Load1992StateBind, Loader1992GlobalState::Init);
	Add1992NamedParams(st2);
	loader.RegisterFunction(st2);

	TableFunction sts1("load_tiger_1992_states", {list_vc}, Load1992Execute, Load1992StatesBind,
	                   Loader1992GlobalState::Init);
	Add1992NamedParams(sts1);
	loader.RegisterFunction(sts1);

	TableFunction sts2("load_tiger_1992_states", {list_vc, LogicalType::VARCHAR}, Load1992Execute,
	                   Load1992StatesBind, Loader1992GlobalState::Init);
	Add1992NamedParams(sts2);
	loader.RegisterFunction(sts2);

	TableFunction all0("load_tiger_1992_all_states", {}, Load1992Execute, Load1992AllStatesBind,
	                   Loader1992GlobalState::Init);
	Add1992NamedParams(all0);
	loader.RegisterFunction(all0);

	TableFunction all1("load_tiger_1992_all_states", {LogicalType::VARCHAR}, Load1992Execute,
	                   Load1992AllStatesBind, Loader1992GlobalState::Init);
	Add1992NamedParams(all1);
	loader.RegisterFunction(all1);

	TableFunction ul1("unload_tiger_1992_state", {LogicalType::VARCHAR}, Unload1992Execute, Unload1992Bind,
	                  Loader1992GlobalState::Init);
	ul1.named_parameters["target_db"] = LogicalType::VARCHAR;
	ul1.named_parameters["target_schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(ul1);

	TableFunction ul2("unload_tiger_1992_state", {list_vc}, Unload1992Execute, Unload1992Bind,
	                  Loader1992GlobalState::Init);
	ul2.named_parameters["target_db"] = LogicalType::VARCHAR;
	ul2.named_parameters["target_schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(ul2);

	// Test hook — see the comment above CountyIndex1992Bind.
	TableFunction cidx("us_geocoder_1992_county_index", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                   CountyIndex1992Execute, CountyIndex1992Bind, CountyIndex1992GlobalState::Init);
	loader.RegisterFunction(cidx);
}

} // namespace us_geocoder
} // namespace duckdb
