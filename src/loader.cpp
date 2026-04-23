#include "us_geocoder_loader.hpp"
#include "us_geocoder_embed.hpp"
#include "us_geocoder_embedded_sql.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

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
static std::string BuildVsiPath(const std::string &source, const std::string &zip_base,
                                const std::string &inner_ext = "shp") {
	std::string src = source;
	if (!src.empty() && src.back() != '/') {
		src += '/';
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

static unique_ptr<FunctionData> LoadTigerNationBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types,
                                                     vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("step");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rows_loaded");

	// Positional args: source (required), year (optional, default 2025).
	// Named args: year := 2025
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("load_tiger_nation: source path is required");
	}

	auto bind_data = make_uniq<LoaderBindData>("tiger"); // schema fixed in v0.1
	bind_data->source = StringValue::Get(input.inputs[0]);
	auto it = input.named_parameters.find("year");
	if (it != input.named_parameters.end() && !it->second.IsNull()) {
		bind_data->year = it->second.GetValue<int32_t>();
	}

	return std::move(bind_data);
}

static void DoLoadNation(ClientContext &context, const LoaderBindData &bind, std::vector<LoaderResult> &out) {
	Connection conn(*context.db);
	const auto &schema = bind.tiger_schema;
	const std::string year = std::to_string(bind.year);
	const auto &tmpl = LoaderTemplatesSql();

	struct NationStep {
		const char *section;
		const char *zip_base_fmt; // "tl_<year>_us_state" etc.
	};
	const NationStep steps[] = {
	    {"nation_state", "tl_YEAR_us_state"},
	    {"nation_county", "tl_YEAR_us_county"},
	    {"nation_zcta5", "tl_YEAR_us_zcta520"},
	};

	for (const auto &step : steps) {
		std::string zip_base = step.zip_base_fmt;
		// Replace "YEAR" token with the year string.
		auto pos = zip_base.find("YEAR");
		if (pos != std::string::npos) {
			zip_base.replace(pos, 4, year);
		}
		auto vsi = BuildVsiPath(bind.source, zip_base);
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

	if (input.inputs.size() < 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("load_tiger_state: (state_abbrev, source) are required");
	}

	auto bind_data = make_uniq<LoaderBindData>("tiger");
	bind_data->is_state_loader = true;
	bind_data->state_abbrev = StringValue::Get(input.inputs[0]);
	bind_data->source = StringValue::Get(input.inputs[1]);
	auto it = input.named_parameters.find("year");
	if (it != input.named_parameters.end() && !it->second.IsNull()) {
		bind_data->year = it->second.GetValue<int32_t>();
	}

	// Resolve state_fips now (fail fast if abbrev is invalid).
	Connection conn(*context.db);
	bind_data->state_fips = LookupStateFips(conn, bind_data->tiger_schema, bind_data->state_abbrev);

	return std::move(bind_data);
}

static void DoLoadState(ClientContext &context, const LoaderBindData &bind, std::vector<LoaderResult> &out) {
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
	const std::pair<const char *, const char *> state_level[] = {
	    {"state_place", "tl_YEAR_FIPS_place"},
	    {"state_cousub", "tl_YEAR_FIPS_cousub"},
	};
	for (const auto &s : state_level) {
		std::string zip_base = s.second;
		auto pos = zip_base.find("YEAR");
		if (pos != std::string::npos) zip_base.replace(pos, 4, year);
		pos = zip_base.find("FIPS");
		if (pos != std::string::npos) zip_base.replace(pos, 4, fips);
		auto vsi = BuildVsiPath(bind.source, zip_base);
		auto section = ExtractSection(tmpl, s.first);
		auto rendered = RenderTemplate(section, {{"@TIGER@", schema}, {"@VSIPATH@", vsi}});
		int64_t rows = ExecuteInsert(conn, rendered, s.first);
		out.push_back({s.first, rows});
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

	// County-level files. For each county, load edges, faces, featnames, addr.
	// (section, zip_base, inner_ext).
	struct CountyStep { const char *section; const char *zip_base; const char *ext; };
	const CountyStep county_level[] = {
	    {"county_edges",     "tl_YEAR_FIPSCOUNTY_edges",     "shp"},
	    {"county_faces",     "tl_YEAR_FIPSCOUNTY_faces",     "shp"},
	    {"county_featnames", "tl_YEAR_FIPSCOUNTY_featnames", "dbf"},  // DBF-only
	    {"county_addr",      "tl_YEAR_FIPSCOUNTY_addr",      "dbf"},  // DBF-only
	};
	for (const auto &cfp : countyfps) {
		for (const auto &s : county_level) {
			std::string zip_base = s.zip_base;
			auto pos = zip_base.find("YEAR");
			if (pos != std::string::npos) zip_base.replace(pos, 4, year);
			pos = zip_base.find("FIPSCOUNTY");
			if (pos != std::string::npos) zip_base.replace(pos, 10, fips + cfp);
			auto vsi = BuildVsiPath(bind.source, zip_base, s.ext);
			auto section = ExtractSection(tmpl, s.section);
			auto rendered = RenderTemplate(section, {{"@TIGER@", schema}, {"@VSIPATH@", vsi},
			                                         {"@STATEFP@", fips}, {"@COUNTYFP@", cfp}});
			int64_t rows = ExecuteInsert(conn, rendered, std::string(s.section) + ":" + cfp);
			out.push_back({std::string(s.section) + ":" + cfp, rows});
		}
	}

	// Build per-state derived tables (zip_state, zip_state_loc, zip_lookup_base).
	const char *derived[] = {"derived_zip_state", "derived_zip_state_loc", "derived_zip_lookup_base"};
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
	// load_tiger_nation(source VARCHAR, year := 2025)
	TableFunction nation_fn("load_tiger_nation", {LogicalType::VARCHAR}, LoadTigerNationExecute,
	                        LoadTigerNationBind, LoaderGlobalState::Init);
	nation_fn.named_parameters["year"] = LogicalType::INTEGER;
	loader.RegisterFunction(nation_fn);

	// load_tiger_state(state_abbrev VARCHAR, source VARCHAR, year := 2025)
	TableFunction state_fn("load_tiger_state", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LoadTigerStateExecute,
	                       LoadTigerStateBind, LoaderGlobalState::Init);
	state_fn.named_parameters["year"] = LogicalType::INTEGER;
	loader.RegisterFunction(state_fn);
}

} // namespace us_geocoder
} // namespace duckdb
