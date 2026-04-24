#include "us_geocoder_loader.hpp"
#include "us_geocoder_embed.hpp"
#include "us_geocoder_embedded_sql.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <sstream>
#include <utility>
#include <vector>

namespace duckdb {
namespace us_geocoder {

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
	struct StatePlan { std::string abbrev; std::string fips; };
	std::vector<StatePlan> states;

	// Whether to compute tiger.edge_containment inline at the end of each
	// state's load. Eager by default — the geocoder's GEOID output columns
	// and the `require_containment` filter both depend on it. Set to false
	// to skip (~1-2 min saved per state); then call build_edge_containment(states)
	// separately when you actually need the GEOIDs.
	bool build_containment = true;

	bool is_state_loader = false;

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
		copy->is_state_loader = is_state_loader;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LoaderBindData>();
		return func_schema == other.func_schema && data_location == other.data_location &&
		       source == other.source && year == other.year &&
		       is_state_loader == other.is_state_loader && states.size() == other.states.size();
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

// =====================================================================
// Helpers
// =====================================================================

static std::string ZeroPad(const std::string &s, size_t width) {
	if (s.size() >= width) {
		return s;
	}
	return std::string(width - s.size(), '0') + s;
}

// Build the /vsizip/ URI for a zip containing a single spatial file with
// the same base name. Most TIGER tables ship a full shapefile (.shp);
// featnames and addr are DBF-only (no geometry). GDAL can read both via
// ST_Read() — pass the correct inner extension.
//
// Both local and HTTP sources use the Census **nested** layout:
//   <source>/<SUBDIR>/<zip_base>.zip         — e.g. <root>/EDGES/tl_2025_44007_edges.zip
//
// For HTTP, we prefix /vsizip//vsicurl/ (double slash tells GDAL that the
// next token is another VSI handler, not a local path).
//
// For local sources, the user passes the root of a (partial) mirror of
// https://www2.census.gov/geo/tiger/TIGER<year>/ — exactly what `wget -r`
// or a manual download-by-directory script produces. There is no flat
// layout in v0.1; put the zips under STATE/, EDGES/, etc. directories.
static std::string BuildVsiPath(const std::string &source, const std::string &subdir,
                                const std::string &zip_base,
                                const std::string &inner_ext = "shp") {
	const bool is_http = source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
	std::string src = source;
	if (!src.empty() && src.back() != '/') {
		src += '/';
	}
	const std::string inner = zip_base + "." + inner_ext;
	if (is_http) {
		return "/vsizip//vsicurl/" + src + subdir + "/" + zip_base + ".zip/" + inner;
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

// Census default source URL for a given year.
static std::string CensusUrl(int year) {
	return "https://www2.census.gov/geo/tiger/TIGER" + std::to_string(year);
}

// Read year + source + target_db/schema from a TableFunctionBindInput and
// populate the bind data. `source_input_index` is the positional slot that
// accepts the source string on this overload (-1 to disable positional).
// source is also accepted via the `source` named parameter.
static void ApplyYearSourceTarget(LoaderBindData &bind, const TableFunctionBindInput &input,
                                   int source_input_index) {
	auto year_it = input.named_parameters.find("year");
	if (year_it != input.named_parameters.end() && !year_it->second.IsNull()) {
		bind.year = year_it->second.GetValue<int32_t>();
	}
	ApplyTargetParams(bind, input);
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
	if (!found_source) {
		bind.source = CensusUrl(bind.year);
	}
	auto bc_it = input.named_parameters.find("build_containment");
	if (bc_it != input.named_parameters.end() && !bc_it->second.IsNull()) {
		bind.build_containment = bc_it->second.GetValue<bool>();
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

// =====================================================================
// load_tiger_nation(source VARCHAR, year INTEGER DEFAULT 2025)
// =====================================================================

static unique_ptr<FunctionData> LoadTigerNationBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types,
                                                     vector<string> &names) {
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

static void DoLoadNation(ClientContext &context, const LoaderBindData &bind, std::vector<LoaderResult> &out) {
	EnsureHttpfsIfRemote(*context.db, bind.source);
	Connection conn(*context.db);
	BootstrapTargetSchema(conn, bind);
	const auto &data_loc = bind.data_location;
	const auto &func_loc = bind.func_schema;
	const std::string year = std::to_string(bind.year);
	const auto &tmpl = LoaderTemplatesSql();

	struct NationStep {
		const char *section;
		const char *subdir;       // Census URL subdir (e.g. "STATE")
		const char *zip_base_fmt; // e.g. "tl_<year>_us_state"
	};
	const NationStep steps[] = {
	    {"nation_state",  "STATE",    "tl_YEAR_us_state"},
	    {"nation_county", "COUNTY",   "tl_YEAR_us_county"},
	    {"nation_zcta5",  "ZCTA520",  "tl_YEAR_us_zcta520"},
	};

	for (const auto &step : steps) {
		std::string zip_base = step.zip_base_fmt;
		auto pos = zip_base.find("YEAR");
		if (pos != std::string::npos) {
			zip_base.replace(pos, 4, year);
		}
		auto vsi = BuildVsiPath(bind.source, step.subdir, zip_base);
		auto section = ExtractSection(tmpl, step.section);
		auto rendered = RenderTemplate(section, {{"@TIGER@", data_loc},
		                                         {"@FUNC@", func_loc},
		                                         {"@VSIPATH@", vsi}});
		int64_t rows = ExecuteInsert(conn, rendered, step.section);
		out.push_back({step.section, rows});
	}
}

static void LoadTigerNationExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<LoaderBindData>();
	auto &gstate = data_p.global_state->Cast<LoaderGlobalState>();

	if (!gstate.executed) {
		gstate.executed = true;
		DoLoadNation(context, bind, gstate.results);
	}

	idx_t emitted = 0;
	while (gstate.row_idx < gstate.results.size() && emitted < STANDARD_VECTOR_SIZE) {
		const auto &r = gstate.results[gstate.row_idx++];
		output.SetValue(0, emitted, Value(r.step));
		output.SetValue(1, emitted, Value::BIGINT(r.rows));
		++emitted;
	}
	output.SetCardinality(emitted);
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
static void ResolveStates(Connection &conn, LoaderBindData &bind,
                           const std::vector<std::string> &abbrevs) {
	std::vector<std::string> seen;
	for (const auto &raw : abbrevs) {
		std::string up;
		up.reserve(raw.size());
		for (char c : raw) { up += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
		bool dup = false;
		for (const auto &s : seen) { if (s == up) { dup = true; break; } }
		if (dup) continue;
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
			if (child.IsNull()) continue;
			auto s = StringValue::Get(child);
			if (!s.empty()) out.push_back(s);
		}
	} else if (v.type().id() == LogicalTypeId::VARCHAR) {
		auto s = StringValue::Get(v);
		if (!s.empty()) out.push_back(s);
	} else {
		throw BinderException("%s: expected VARCHAR or VARCHAR[], got %s",
		                      context_fn, v.type().ToString());
	}
	return out;
}

static unique_ptr<FunctionData> LoadTigerStateBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types,
                                                    vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("load_tiger_state: state_abbrev is required");
	}
	auto bind_data = make_uniq<LoaderBindData>("tiger");
	bind_data->is_state_loader = true;
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "load_tiger_state");
	ApplyYearSourceTarget(*bind_data, input, /*source_input_index=*/1);
	Connection conn(*context.db);
	ResolveStates(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static unique_ptr<FunctionData> LoadTigerStatesBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types,
                                                     vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("load_tiger_states: states (VARCHAR[]) is required");
	}
	auto bind_data = make_uniq<LoaderBindData>("tiger");
	bind_data->is_state_loader = true;
	auto abbrevs = AbbrevsFromValue(input.inputs[0], "load_tiger_states");
	ApplyYearSourceTarget(*bind_data, input, /*source_input_index=*/1);
	Connection conn(*context.db);
	ResolveStates(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static unique_ptr<FunctionData> LoadTigerAllStatesBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types,
                                                       vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	auto bind_data = make_uniq<LoaderBindData>("tiger");
	bind_data->is_state_loader = true;
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
			if (!v.IsNull()) { abbrevs.push_back(v.GetValue<std::string>()); }
		}
	}
	ResolveStates(conn, *bind_data, abbrevs);
	return std::move(bind_data);
}

static void DoLoadState(ClientContext &context, const LoaderBindData &bind,
                         const LoaderBindData::StatePlan &state,
                         std::vector<LoaderResult> &out) {
	EnsureHttpfsIfRemote(*context.db, bind.source);
	Connection conn(*context.db);
	BootstrapTargetSchema(conn, bind);
	const auto &data_loc = bind.data_location;
	const auto &func_loc = bind.func_schema;
	const std::string &fips = state.fips;
	const std::string year = std::to_string(bind.year);
	const auto &tmpl = LoaderTemplatesSql();

	// Wipe existing rows for this state so the load is idempotent.
	{
		auto sec = ExtractSection(tmpl, "unload_state");
		auto rendered = RenderTemplate(sec, {{"@TIGER@", data_loc},
		                                     {"@FUNC@", func_loc},
		                                     {"@STATEFP@", fips}});
		auto result = conn.Query(rendered);
		if (result->HasError()) {
			throw IOException("us_geocoder loader (unload_state): %s", result->GetError());
		}
		out.push_back({"unload_state", 0});
	}

	// State-level files: place, cousub.
	struct StateLevelStep { const char *section; const char *subdir; const char *zip_base; };
	const StateLevelStep state_level[] = {
	    {"state_place",  "PLACE",  "tl_YEAR_FIPS_place"},
	    {"state_cousub", "COUSUB", "tl_YEAR_FIPS_cousub"},
	};
	for (const auto &s : state_level) {
		std::string zip_base = s.zip_base;
		auto pos = zip_base.find("YEAR");
		if (pos != std::string::npos) zip_base.replace(pos, 4, year);
		pos = zip_base.find("FIPS");
		if (pos != std::string::npos) zip_base.replace(pos, 4, fips);
		auto vsi = BuildVsiPath(bind.source, s.subdir, zip_base);
		auto section = ExtractSection(tmpl, s.section);
		auto rendered = RenderTemplate(section, {{"@TIGER@", data_loc},
		                                         {"@FUNC@", func_loc},
		                                         {"@VSIPATH@", vsi}});
		int64_t rows = ExecuteInsert(conn, rendered, s.section);
		out.push_back({s.section, rows});
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

	// County-level files. Per (county, table-type): load one shapefile/DBF
	// via ST_Read. Templates are split into insert-prefix + branch sections
	// so a future genuinely-parallel loader can UNION ALL across counties;
	// for now DuckDB UNION ALL + GDAL /vsicurl/ don't parallelize HTTPS
	// fetches (measured: 8m16s batched vs ~5m serial on NJ via Census),
	// so we issue one INSERT per (county, table-type). Users who need
	// faster HTTP loads should pre-download in parallel via curl/xargs
	// and point at the local directory (see docs/api.md).
	struct CountyTable { const char *label; const char *subdir; const char *zip_base;
	                     const char *ext; const char *insert_section; const char *branch_section; };
	const CountyTable county_level[] = {
	    {"county_edges",     "EDGES",     "tl_YEAR_FIPSCOUNTY_edges",     "shp",
	     "county_edges_insert",     "county_edges_branch"},
	    {"county_faces",     "FACES",     "tl_YEAR_FIPSCOUNTY_faces",     "shp",
	     "county_faces_insert",     "county_faces_branch"},
	    {"county_featnames", "FEATNAMES", "tl_YEAR_FIPSCOUNTY_featnames", "dbf",
	     "county_featnames_insert", "county_featnames_branch"},
	    {"county_addr",      "ADDR",      "tl_YEAR_FIPSCOUNTY_addr",      "dbf",
	     "county_addr_insert",      "county_addr_branch"},
	};
	for (const auto &cfp : countyfps) {
		for (const auto &t : county_level) {
			std::string zip_base = t.zip_base;
			auto pos = zip_base.find("YEAR");
			if (pos != std::string::npos) zip_base.replace(pos, 4, year);
			pos = zip_base.find("FIPSCOUNTY");
			if (pos != std::string::npos) zip_base.replace(pos, 10, fips + cfp);
			auto vsi = BuildVsiPath(bind.source, t.subdir, zip_base, t.ext);
			auto insert_prefix = RenderTemplate(ExtractSection(tmpl, t.insert_section),
			                                     {{"@TIGER@", data_loc},
			                                      {"@FUNC@", func_loc}});
			auto branch = RenderTemplate(ExtractSection(tmpl, t.branch_section),
			                              {{"@TIGER@", data_loc},
			                               {"@FUNC@", func_loc},
			                               {"@VSIPATH@", vsi},
			                               {"@STATEFP@", fips},
			                               {"@COUNTYFP@", cfp}});
			std::string sql = insert_prefix + branch + ";";
			int64_t rows = ExecuteInsert(conn, sql, std::string(t.label) + ":" + cfp);
			out.push_back({std::string(t.label) + ":" + cfp, rows});
		}
	}

	// Build per-state derived tables. edge_containment is expensive
	// (ST_Within on ~150K rows) so it runs last; the zip_* tables are
	// pure INSERT…SELECT and finish in milliseconds.
	std::vector<const char *> derived = {
	    "derived_zip_state",
	    "derived_zip_state_loc",
	    "derived_zip_lookup_base",
	};
	if (bind.build_containment) {
		derived.push_back("derived_edge_containment");
	}
	for (const auto *name : derived) {
		auto section = ExtractSection(tmpl, name);
		auto rendered = RenderTemplate(section, {{"@TIGER@", data_loc},
		                                         {"@FUNC@", func_loc},
		                                         {"@STATEFP@", fips}});
		int64_t rows = ExecuteInsert(conn, rendered, name);
		out.push_back({name, rows});
	}
	if (!bind.build_containment) {
		out.push_back({"skipped:edge_containment", 0});
	}
}

static void LoadTigerStateExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<LoaderBindData>();
	auto &gstate = data_p.global_state->Cast<LoaderGlobalState>();

	if (!gstate.executed) {
		gstate.executed = true;
		// Prepend a "begin:<ABBREV>" marker per state so users tailing output
		// in a long batch (e.g. load_tiger_all_states) can see progress.
		for (const auto &st : bind.states) {
			gstate.results.push_back({"begin:" + st.abbrev, 0});
			DoLoadState(context, bind, st, gstate.results);
			gstate.results.push_back({"done:" + st.abbrev, 0});
		}
	}

	idx_t emitted = 0;
	while (gstate.row_idx < gstate.results.size() && emitted < STANDARD_VECTOR_SIZE) {
		const auto &r = gstate.results[gstate.row_idx++];
		output.SetValue(0, emitted, Value(r.step));
		output.SetValue(1, emitted, Value::BIGINT(r.rows));
		++emitted;
	}
	output.SetCardinality(emitted);
}

// =====================================================================
// Data-table catalog — the 13 tables that the macros read from, and that
// set_tiger_reference swaps between base-table and view forms.
// =====================================================================

static const char *const kDataTables[] = {
    "state",       "county",     "place",           "cousub",  "zcta5",
    "zip_state",   "zip_state_loc", "zip_lookup_base",
    "edges",       "faces",      "featnames",       "addr",
    "edge_containment",
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
                                                   vector<LogicalType> &return_types,
                                                   vector<string> &names) {
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

	idx_t emitted = 0;
	while (gstate.row_idx < gstate.results.size() && emitted < STANDARD_VECTOR_SIZE) {
		const auto &r = gstate.results[gstate.row_idx++];
		output.SetValue(0, emitted, Value(r.step));
		output.SetValue(1, emitted, Value::BIGINT(r.rows));
		++emitted;
	}
	output.SetCardinality(emitted);
}

// =====================================================================
// set_tiger_reference(database VARCHAR, schema VARCHAR DEFAULT 'tiger')
//   Repoints the 13 local tiger.<table> to views over <database>.<schema>.<table>.
//   Passing database := NULL (or empty) restores empty local base tables.
// =====================================================================

static unique_ptr<FunctionData> SetReferenceBind(ClientContext &, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types,
                                                  vector<string> &names) {
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
					if (r->HasError()) { /* swallow; fall through to DROP TABLE */ }
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
					auto view_sql = "CREATE VIEW " + local_ref + " AS SELECT * FROM " +
					                source_qualified + "." + QuoteIdent(tbl);
					auto r = conn.Query(view_sql);
					if (r->HasError()) {
						throw IOException("us_geocoder set_tiger_reference (create view %s): %s",
						                  tbl, r->GetError());
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
					throw IOException("us_geocoder set_tiger_reference (recreate base tables): %s",
					                  r->GetError());
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

static unique_ptr<FunctionData> BuildContainmentBind(ClientContext &context,
                                                      TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types,
                                                      vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("build_edge_containment: states (VARCHAR or VARCHAR[]) is required");
	}
	auto bind_data = make_uniq<LoaderBindData>("tiger");
	bind_data->is_state_loader = true; // reuse the state-resolution flow
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

		for (const auto &state : bind.states) {
			gstate.results.push_back({"begin:" + state.abbrev, 0});
			// Idempotent: wipe existing rows for this state first, then recompute.
			auto del_sql = "DELETE FROM " + data_loc + ".edge_containment WHERE statefp = '" +
			               state.fips + "'";
			auto del = conn.Query(del_sql);
			if (del->HasError()) {
				throw IOException("us_geocoder build_edge_containment (delete %s): %s",
				                  state.abbrev, del->GetError());
			}
			auto rendered = RenderTemplate(section, {{"@TIGER@", data_loc},
			                                         {"@FUNC@", func_loc},
			                                         {"@STATEFP@", state.fips}});
			int64_t rows = ExecuteInsert(conn, rendered, "edge_containment:" + state.abbrev);
			gstate.results.push_back({"edge_containment:" + state.abbrev, rows});
			gstate.results.push_back({"done:" + state.abbrev, 0});
		}
	}

	idx_t emitted = 0;
	while (gstate.row_idx < gstate.results.size() && emitted < STANDARD_VECTOR_SIZE) {
		const auto &r = gstate.results[gstate.row_idx++];
		output.SetValue(0, emitted, Value(r.step));
		output.SetValue(1, emitted, Value::BIGINT(r.rows));
		++emitted;
	}
	output.SetCardinality(emitted);
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
}

void RegisterLoaderFunctions(ExtensionLoader &loader, const std::string &) {
	// load_tiger_nation([source VARCHAR], year := 2025,
	//                   target_db := NULL, target_schema := 'tiger')
	// Source defaults to the Census TIGER URL for the given year.
	// target_db / target_schema control where the TIGER data tables live.
	TableFunction nation_fn0("load_tiger_nation", {}, LoadTigerNationExecute,
	                         LoadTigerNationBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(nation_fn0);
	loader.RegisterFunction(nation_fn0);

	TableFunction nation_fn1("load_tiger_nation", {LogicalType::VARCHAR}, LoadTigerNationExecute,
	                         LoadTigerNationBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(nation_fn1);
	loader.RegisterFunction(nation_fn1);

	// load_tiger_state(state_abbrev VARCHAR [, source VARCHAR], ...)
	TableFunction state_fn1("load_tiger_state", {LogicalType::VARCHAR}, LoadTigerStateExecute,
	                        LoadTigerStateBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(state_fn1);
	loader.RegisterFunction(state_fn1);

	TableFunction state_fn2("load_tiger_state", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LoadTigerStateExecute,
	                        LoadTigerStateBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(state_fn2);
	loader.RegisterFunction(state_fn2);

	// load_tiger_states(states VARCHAR[] [, source VARCHAR], ...)
	const auto list_vc = LogicalType::LIST(LogicalType::VARCHAR);
	TableFunction states_fn1("load_tiger_states", {list_vc}, LoadTigerStateExecute,
	                         LoadTigerStatesBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(states_fn1);
	loader.RegisterFunction(states_fn1);

	TableFunction states_fn2("load_tiger_states", {list_vc, LogicalType::VARCHAR},
	                         LoadTigerStateExecute, LoadTigerStatesBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(states_fn2);
	loader.RegisterFunction(states_fn2);

	// load_tiger_all_states([source VARCHAR], ...)
	//   Loads every row of state_lookup with statefp 01–56 (50 states + DC).
	TableFunction all_states_fn0("load_tiger_all_states", {}, LoadTigerStateExecute,
	                              LoadTigerAllStatesBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(all_states_fn0);
	loader.RegisterFunction(all_states_fn0);

	TableFunction all_states_fn1("load_tiger_all_states", {LogicalType::VARCHAR},
	                              LoadTigerStateExecute, LoadTigerAllStatesBind, LoaderGlobalState::Init);
	AddLoaderNamedParams(all_states_fn1);
	loader.RegisterFunction(all_states_fn1);

	// install_tiger_schema(database VARCHAR [, schema VARCHAR DEFAULT 'tiger'])
	//   Creates the TIGER data tables in a target catalog. Idempotent.
	TableFunction install_fn1("install_tiger_schema", {LogicalType::VARCHAR},
	                          InstallSchemaExecute, InstallSchemaBind, SchemaOpGlobalState::Init);
	loader.RegisterFunction(install_fn1);
	TableFunction install_fn2("install_tiger_schema", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                          InstallSchemaExecute, InstallSchemaBind, SchemaOpGlobalState::Init);
	loader.RegisterFunction(install_fn2);

	// set_tiger_reference([database VARCHAR, schema VARCHAR DEFAULT 'tiger'])
	//   No-arg / NULL / empty-string first arg → restore empty local base tables.
	//   Non-empty database → repoint local tiger.<table> to VIEWs over <db>.<schema>.<table>.
	TableFunction set_ref_fn0("set_tiger_reference", {},
	                          SetReferenceExecute, SetReferenceBind, SchemaOpGlobalState::Init);
	loader.RegisterFunction(set_ref_fn0);
	TableFunction set_ref_fn1("set_tiger_reference", {LogicalType::VARCHAR},
	                          SetReferenceExecute, SetReferenceBind, SchemaOpGlobalState::Init);
	loader.RegisterFunction(set_ref_fn1);
	TableFunction set_ref_fn2("set_tiger_reference", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                          SetReferenceExecute, SetReferenceBind, SchemaOpGlobalState::Init);
	loader.RegisterFunction(set_ref_fn2);

	// build_edge_containment(VARCHAR | VARCHAR[], target_db := NULL,
	//                        target_schema := 'tiger'])
	const auto list_vc2 = LogicalType::LIST(LogicalType::VARCHAR);
	TableFunction bec_varchar("build_edge_containment", {LogicalType::VARCHAR},
	                           BuildContainmentExecute, BuildContainmentBind,
	                           LoaderGlobalState::Init);
	bec_varchar.named_parameters["target_db"] = LogicalType::VARCHAR;
	bec_varchar.named_parameters["target_schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(bec_varchar);

	TableFunction bec_list("build_edge_containment", {list_vc2},
	                        BuildContainmentExecute, BuildContainmentBind,
	                        LoaderGlobalState::Init);
	bec_list.named_parameters["target_db"] = LogicalType::VARCHAR;
	bec_list.named_parameters["target_schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(bec_list);
}

} // namespace us_geocoder
} // namespace duckdb
