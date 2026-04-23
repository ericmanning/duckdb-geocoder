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
	explicit LoaderBindData(std::string tiger_schema) : tiger_schema(std::move(tiger_schema)) {
	}
	std::string tiger_schema;

	// For load_tiger_nation
	std::string source;
	int32_t year = 2025;

	// For load_tiger_state — adds these:
	std::string state_abbrev; // e.g. "RI"
	std::string state_fips;   // resolved at bind time

	bool is_state_loader = false;

public:
	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<LoaderBindData>(tiger_schema);
		copy->source = source;
		copy->year = year;
		copy->state_abbrev = state_abbrev;
		copy->state_fips = state_fips;
		copy->is_state_loader = is_state_loader;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LoaderBindData>();
		return tiger_schema == other.tiger_schema && source == other.source && year == other.year &&
		       state_abbrev == other.state_abbrev && is_state_loader == other.is_state_loader;
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
// Source handling:
//   * http:// or https:// URL → /vsizip//vsicurl/<url>/<SUBDIR>/<zip>/<inner>
//     Census organizes TIGER by table-type subdirectory (STATE/, EDGES/,
//     etc.), so HTTP paths always include the subdir. The double slash
//     after /vsizip/ is required; it tells GDAL the next token is another
//     VSI handler (vsicurl), not a local path.
//   * Local filesystem path → /vsizip/<path>/<zip>/<inner>
//     Local mode defaults to flat layout (all zips in one directory).
//     Users who mirrored the Census tree can pass `.../TIGER2025/STATE`
//     etc. as the source for each call, or flatten their local mirror.
static std::string BuildVsiPath(const std::string &source, const std::string &subdir,
                                const std::string &zip_base,
                                const std::string &inner_ext = "shp") {
	const bool is_http = source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
	std::string src = source;
	if (!src.empty() && src.back() != '/') {
		src += '/';
	}
	if (is_http) {
		return "/vsizip//vsicurl/" + src + subdir + "/" + zip_base + ".zip/" + zip_base + "." + inner_ext;
	}
	return "/vsizip/" + src + zip_base + ".zip/" + zip_base + "." + inner_ext;
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

// Census default source URL for a given year.
static std::string CensusUrl(int year) {
	return "https://www2.census.gov/geo/tiger/TIGER" + std::to_string(year);
}

static unique_ptr<FunctionData> LoadTigerNationBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types,
                                                     vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	auto bind_data = make_uniq<LoaderBindData>("tiger"); // schema fixed in v0.1
	auto year_it = input.named_parameters.find("year");
	if (year_it != input.named_parameters.end() && !year_it->second.IsNull()) {
		bind_data->year = year_it->second.GetValue<int32_t>();
	}

	// Positional arg: source (optional, defaults to Census URL).
	if (input.inputs.empty() || input.inputs[0].IsNull() || StringValue::Get(input.inputs[0]).empty()) {
		bind_data->source = CensusUrl(bind_data->year);
	} else {
		bind_data->source = StringValue::Get(input.inputs[0]);
	}

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
	const auto &schema = bind.tiger_schema;
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
		auto rendered = RenderTemplate(section, {{"@TIGER@", schema}, {"@VSIPATH@", vsi}});
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
// load_tiger_state(state_abbrev VARCHAR, source VARCHAR, year INT DEFAULT 2025)
// =====================================================================

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
	bind_data->state_abbrev = StringValue::Get(input.inputs[0]);
	auto it = input.named_parameters.find("year");
	if (it != input.named_parameters.end() && !it->second.IsNull()) {
		bind_data->year = it->second.GetValue<int32_t>();
	}
	// Positional source optional, defaults to Census URL.
	if (input.inputs.size() < 2 || input.inputs[1].IsNull() || StringValue::Get(input.inputs[1]).empty()) {
		bind_data->source = CensusUrl(bind_data->year);
	} else {
		bind_data->source = StringValue::Get(input.inputs[1]);
	}

	// Resolve state_fips now (fail fast if abbrev is invalid).
	Connection conn(*context.db);
	bind_data->state_fips = LookupStateFips(conn, bind_data->tiger_schema, bind_data->state_abbrev);

	return std::move(bind_data);
}

static void DoLoadState(ClientContext &context, const LoaderBindData &bind, std::vector<LoaderResult> &out) {
	EnsureHttpfsIfRemote(*context.db, bind.source);
	Connection conn(*context.db);
	const auto &schema = bind.tiger_schema;
	const std::string &fips = bind.state_fips;
	const std::string year = std::to_string(bind.year);
	const auto &tmpl = LoaderTemplatesSql();

	// Wipe existing rows for this state so the load is idempotent.
	{
		auto sec = ExtractSection(tmpl, "unload_state");
		auto rendered = RenderTemplate(sec, {{"@TIGER@", schema}, {"@STATEFP@", fips}});
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
		auto rendered = RenderTemplate(section, {{"@TIGER@", schema}, {"@VSIPATH@", vsi}});
		int64_t rows = ExecuteInsert(conn, rendered, s.section);
		out.push_back({s.section, rows});
	}

	// Enumerate counties for this state from tiger.county (must be loaded first).
	std::vector<std::string> countyfps;
	{
		auto sql = "SELECT countyfp FROM " + schema + ".county WHERE statefp = '" + fips + "' ORDER BY countyfp";
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
		throw IOException("us_geocoder loader: no counties found for state %s (statefp=%s); "
		                  "run load_tiger_nation() first to populate tiger.county",
		                  bind.state_abbrev, fips);
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
			                                     {{"@TIGER@", schema}});
			auto branch = RenderTemplate(ExtractSection(tmpl, t.branch_section),
			                              {{"@TIGER@", schema},
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
	const char *derived[] = {
	    "derived_zip_state",
	    "derived_zip_state_loc",
	    "derived_zip_lookup_base",
	    "derived_edge_containment",
	};
	for (const auto *name : derived) {
		auto section = ExtractSection(tmpl, name);
		auto rendered = RenderTemplate(section, {{"@TIGER@", schema}, {"@STATEFP@", fips}});
		int64_t rows = ExecuteInsert(conn, rendered, name);
		out.push_back({name, rows});
	}
}

static void LoadTigerStateExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<LoaderBindData>();
	auto &gstate = data_p.global_state->Cast<LoaderGlobalState>();

	if (!gstate.executed) {
		gstate.executed = true;
		DoLoadState(context, bind, gstate.results);
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

void RegisterLoaderFunctions(ExtensionLoader &loader, const std::string &) {
	// load_tiger_nation([source VARCHAR], year := 2025)
	// Source defaults to the Census TIGER URL for the given year.
	TableFunction nation_fn0("load_tiger_nation", {}, LoadTigerNationExecute,
	                         LoadTigerNationBind, LoaderGlobalState::Init);
	nation_fn0.named_parameters["year"] = LogicalType::INTEGER;
	loader.RegisterFunction(nation_fn0);

	TableFunction nation_fn1("load_tiger_nation", {LogicalType::VARCHAR}, LoadTigerNationExecute,
	                         LoadTigerNationBind, LoaderGlobalState::Init);
	nation_fn1.named_parameters["year"] = LogicalType::INTEGER;
	loader.RegisterFunction(nation_fn1);

	// load_tiger_state(state_abbrev VARCHAR [, source VARCHAR], year := 2025)
	TableFunction state_fn1("load_tiger_state", {LogicalType::VARCHAR}, LoadTigerStateExecute,
	                        LoadTigerStateBind, LoaderGlobalState::Init);
	state_fn1.named_parameters["year"] = LogicalType::INTEGER;
	loader.RegisterFunction(state_fn1);

	TableFunction state_fn2("load_tiger_state", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LoadTigerStateExecute,
	                        LoadTigerStateBind, LoaderGlobalState::Init);
	state_fn2.named_parameters["year"] = LogicalType::INTEGER;
	loader.RegisterFunction(state_fn2);
}

} // namespace us_geocoder
} // namespace duckdb
