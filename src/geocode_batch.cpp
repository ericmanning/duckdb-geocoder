// tiger.geocode_batch — C++ table-in-out function for batched geocoding.
// See src/include/geocode_batch.hpp for design context.
//
// API (LogicalType::TABLE input):
//   -- Form 1 (freeform addr_str + passthrough cols).
//   SELECT * FROM geocode_batch((SELECT id, addr_str FROM bench_input));
//
//   -- Form 2 (pre-parsed struct fields + passthrough cols). Input columns
//   -- map onto tiger.geocode_input fields by name: address (INT/BIGINT/
//   -- VARCHAR), street_name, street_type, internal, pre_dir, post_dir,
//   -- location, state_abbrev, zip (all VARCHAR). Missing fields → NULL.
//   SELECT * FROM geocode_batch((SELECT id, address, street_name,
//                                       state_abbrev, zip FROM bench_input));
//
// Output (current MVP):
//   <passthrough cols...>, rating BIGINT, lng DOUBLE, lat DOUBLE,
//   adr_text VARCHAR
// (Followups: geom GEOMETRY + adr STRUCT + GEOID columns.)
//
// Pipeline:
//   1. Buffer up to 100K input rows (NEED_MORE_INPUT until threshold or
//      end-of-input via in_out_function_final).
//   2. On flush: run a single resolution SQL that takes the buffered rows
//      as a VALUES list and (a) parses addr_str via tiger.from_pagc for
//      Form 1, (b) resolves each row's statefp from state_abbrev OR zip.
//      Read back (input_idx, 9 struct fields, resolved_statefp).
//   3. Partition resolved rows by statefp.
//   4. Per-state: build a LATERAL SQL with the literal statefp baked in,
//      call tiger.geocode_address_for_state, collect results.
//   5. Pair geocode results with passthrough values from the buffered row
//      via input_idx and emit chunked output.

#include "geocode_batch.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/connection.hpp"

#include <sstream>
#include <unordered_map>
#include <vector>

namespace duckdb {
namespace us_geocoder {

namespace {

constexpr idx_t kBufferThreshold = 100000;
constexpr idx_t kInvalidIdx = static_cast<idx_t>(-1);

// Recognized input column names (Form 1 + Form 2).
constexpr const char *kAddrStrCol = "addr_str";
constexpr const char *kAddressCol = "address";
constexpr const char *kStreetNameCol = "street_name";
constexpr const char *kStreetTypeCol = "street_type";
constexpr const char *kInternalCol = "internal";
constexpr const char *kPreDirCol = "pre_dir";
constexpr const char *kPostDirCol = "post_dir";
constexpr const char *kLocationCol = "location";
constexpr const char *kStateAbbrevCol = "state_abbrev";
constexpr const char *kZipCol = "zip";

struct BatchBindData : public TableFunctionData {
	idx_t addr_str_idx = kInvalidIdx;
	idx_t address_idx = kInvalidIdx;
	idx_t street_name_idx = kInvalidIdx;
	idx_t street_type_idx = kInvalidIdx;
	idx_t internal_idx = kInvalidIdx;
	idx_t pre_dir_idx = kInvalidIdx;
	idx_t post_dir_idx = kInvalidIdx;
	idx_t location_idx = kInvalidIdx;
	idx_t state_abbrev_idx = kInvalidIdx;
	idx_t zip_idx = kInvalidIdx;

	std::vector<idx_t> passthrough_input_indices;
	idx_t n_passthrough = 0;

	bool form_freeform = false;
};

struct BufferedRow {
	std::vector<Value> passthrough_values;
	std::string addr_str;        // Form 1
	std::vector<Value> parsed_fields; // Form 2 (9 fields, ordered)
};

// One geocoder result. Tagged with input_idx so we can pair with passthrough.
struct GeocodeResult {
	idx_t input_idx;
	Value rating;                 // BIGINT or NULL
	Value lng;                    // DOUBLE or NULL
	Value lat;                    // DOUBLE or NULL
	Value adr_text;               // VARCHAR or NULL — pprint_adr(adr) rendering
	Value block_geoid;            // VARCHAR or NULL — 15-digit FIPS block code
	Value tract_geoid;            // VARCHAR or NULL — 11-digit FIPS tract code
	Value blkgrp_geoid;           // VARCHAR or NULL — 12-digit FIPS block-group code
	Value containment_guaranteed; // BOOLEAN or NULL — true iff edge_containment
	                              // marked the matched side as fully inside the
	                              // returned face/block/tract/blkgrp polygon
};

struct BatchGlobalState : public GlobalTableFunctionState {
	BatchGlobalState() = default;

	std::vector<BufferedRow> buffered;
	std::vector<GeocodeResult> pending_output;
	idx_t output_emitted = 0;
	bool input_exhausted = false;

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
		return make_uniq<BatchGlobalState>();
	}
};

void RequireType(const std::string &col_name, const LogicalType &got, LogicalTypeId expected) {
	if (got.id() != expected) {
		throw BinderException("tiger.geocode_batch: input column '%s' must be %s, got %s", col_name,
		                      LogicalType(expected).ToString(), got.ToString());
	}
}

unique_ptr<FunctionData> Bind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<string> &names) {
	auto bind_data = make_uniq<BatchBindData>();

	for (idx_t i = 0; i < input.input_table_names.size(); i++) {
		const auto &col_name = input.input_table_names[i];
		const auto &col_type = input.input_table_types[i];
		if (col_name == kAddrStrCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->addr_str_idx = i;
		} else if (col_name == kAddressCol) {
			if (col_type.id() != LogicalTypeId::INTEGER && col_type.id() != LogicalTypeId::VARCHAR &&
			    col_type.id() != LogicalTypeId::BIGINT) {
				throw BinderException(
				    "tiger.geocode_batch: input column 'address' must be INTEGER, BIGINT, or VARCHAR (got %s)",
				    col_type.ToString());
			}
			bind_data->address_idx = i;
		} else if (col_name == kStreetNameCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->street_name_idx = i;
		} else if (col_name == kStreetTypeCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->street_type_idx = i;
		} else if (col_name == kInternalCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->internal_idx = i;
		} else if (col_name == kPreDirCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->pre_dir_idx = i;
		} else if (col_name == kPostDirCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->post_dir_idx = i;
		} else if (col_name == kLocationCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->location_idx = i;
		} else if (col_name == kStateAbbrevCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->state_abbrev_idx = i;
		} else if (col_name == kZipCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->zip_idx = i;
		}
	}

	bool has_addr_str = bind_data->addr_str_idx != kInvalidIdx;
	bool has_any_part =
	    (bind_data->address_idx != kInvalidIdx || bind_data->street_name_idx != kInvalidIdx ||
	     bind_data->street_type_idx != kInvalidIdx || bind_data->internal_idx != kInvalidIdx ||
	     bind_data->pre_dir_idx != kInvalidIdx || bind_data->post_dir_idx != kInvalidIdx ||
	     bind_data->location_idx != kInvalidIdx || bind_data->state_abbrev_idx != kInvalidIdx ||
	     bind_data->zip_idx != kInvalidIdx);
	if (!has_addr_str && !has_any_part) {
		throw BinderException(
		    "tiger.geocode_batch: input must include either 'addr_str' (VARCHAR) "
		    "OR some of the geocode_input fields {address INTEGER, street_name, street_type, "
		    "internal, pre_dir, post_dir, location, state_abbrev, zip — all VARCHAR}");
	}
	bind_data->form_freeform = has_addr_str;

	auto is_consumed = [&](idx_t i) {
		if (bind_data->form_freeform) {
			return i == bind_data->addr_str_idx;
		}
		return i == bind_data->address_idx || i == bind_data->street_name_idx ||
		       i == bind_data->street_type_idx || i == bind_data->internal_idx ||
		       i == bind_data->pre_dir_idx || i == bind_data->post_dir_idx ||
		       i == bind_data->location_idx || i == bind_data->state_abbrev_idx ||
		       i == bind_data->zip_idx;
	};
	for (idx_t i = 0; i < input.input_table_names.size(); i++) {
		if (is_consumed(i)) {
			continue;
		}
		bind_data->passthrough_input_indices.push_back(i);
		return_types.emplace_back(input.input_table_types[i]);
		names.emplace_back(input.input_table_names[i]);
	}
	bind_data->n_passthrough = bind_data->passthrough_input_indices.size();

	// Geocoder result columns. Output mirrors what tiger.geocode returns
	// minus geom GEOMETRY and adr STRUCT, which need catalog type lookups
	// to declare from C++ and are deferred to a follow-up. Users get geom
	// reconstituted via ST_Point(lng, lat) and pprint_adr-style rendering
	// via adr_text.
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("rating");
	return_types.emplace_back(LogicalType::DOUBLE);
	names.emplace_back("lng");
	return_types.emplace_back(LogicalType::DOUBLE);
	names.emplace_back("lat");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("adr_text");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("block_geoid");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("tract_geoid");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("blkgrp_geoid");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("containment_guaranteed");

	return std::move(bind_data);
}

// ---------------------------------------------------------------------------
// SQL helpers — string escaping + Value rendering as SQL literals.
// ---------------------------------------------------------------------------

static std::string EscapeSqlLiteral(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 2);
	out += "'";
	for (char c : s) {
		if (c == '\'') {
			out += "''";
		} else {
			out += c;
		}
	}
	out += "'";
	return out;
}

// Render a duckdb::Value as a SQL literal. NULL → "NULL". Strings get
// single-quoted with escape. Numerics inline. We only handle the types we
// expect from the buffered input: VARCHAR, INTEGER, BIGINT, NULL.
static std::string ValueAsSqlLiteral(const Value &v) {
	if (v.IsNull()) {
		return "NULL";
	}
	switch (v.type().id()) {
	case LogicalTypeId::VARCHAR:
		return EscapeSqlLiteral(v.GetValue<std::string>());
	case LogicalTypeId::INTEGER:
		return std::to_string(v.GetValue<int32_t>());
	case LogicalTypeId::BIGINT:
		return std::to_string(v.GetValue<int64_t>());
	case LogicalTypeId::DOUBLE:
		return std::to_string(v.GetValue<double>());
	default:
		// Best-effort fallback: rely on Value::ToString. May not always be
		// safe SQL, but our buffered values come from the input table and
		// we've validated the types in Bind.
		return EscapeSqlLiteral(v.ToString());
	}
}

// Render a buffered Form-2 row's parsed_fields as a VALUES tuple suffix:
//   (input_idx, address, street_name, street_type, internal,
//    pre_dir, post_dir, location, state_abbrev, zip)
// address may be INTEGER (rendered as numeric) or NULL.
static std::string RenderParsedRowTuple(idx_t input_idx, const std::vector<Value> &fields) {
	std::ostringstream os;
	os << "(" << input_idx;
	for (const auto &v : fields) {
		os << ", " << ValueAsSqlLiteral(v);
	}
	os << ")";
	return os.str();
}

// ---------------------------------------------------------------------------
// Resolution: turn each buffered row into (input_idx, struct_fields, statefp).
// One Connection.Query per flush.
//
// Form 1: VALUES list of (input_idx, addr_str). Inside SQL, tiger.from_pagc
// extracts struct fields; state_lookup / zip_lookup_base resolve statefp.
//
// Form 2: VALUES list of (input_idx, address, street_name, …, zip). Same
// statefp resolution; struct fields are passed through.
//
// Returns a vector of resolved-row tuples in input order.
// ---------------------------------------------------------------------------

struct ResolvedRow {
	idx_t input_idx;
	Value statefp; // VARCHAR or NULL
	std::vector<Value> struct_fields; // 9 fields in geocode_input order
};

static std::vector<ResolvedRow> RunResolution(Connection &conn, const BatchBindData &bind_data,
                                              const std::vector<BufferedRow> &buffered) {
	std::ostringstream sql;
	if (bind_data.form_freeform) {
		// Form 1 SQL: VALUES of (idx, addr_str), parse + resolve.
		sql << "WITH input(input_idx, addr_str) AS (VALUES ";
		for (idx_t i = 0; i < buffered.size(); i++) {
			if (i > 0) sql << ",";
			sql << "(" << i << "," << EscapeSqlLiteral(buffered[i].addr_str) << ")";
		}
		sql << "),"
		    << " parsed AS (SELECT input_idx, tiger.from_pagc(addr_str) AS s FROM input)"
		    << " SELECT input_idx,"
		    << " s['address'], s['street_name'], s['street_type'], s['internal'],"
		    << " s['pre_dir'], s['post_dir'], s['location'], s['state_abbrev'], s['zip'],"
		    << " COALESCE("
		    << "(SELECT statefp FROM tiger.state_lookup WHERE abbrev = s['state_abbrev'] LIMIT 1),"
		    << "(SELECT statefp FROM tiger.zip_lookup_base WHERE zip = s['zip'] LIMIT 1)"
		    << ") AS resolved_statefp"
		    << " FROM parsed ORDER BY input_idx";
	} else {
		// Form 2 SQL: VALUES of (idx, address, ..., zip), then resolve.
		sql << "WITH input(input_idx, address, street_name, street_type, internal,"
		    << " pre_dir, post_dir, location, state_abbrev, zip) AS (VALUES ";
		for (idx_t i = 0; i < buffered.size(); i++) {
			if (i > 0) sql << ",";
			sql << RenderParsedRowTuple(i, buffered[i].parsed_fields);
		}
		sql << ")"
		    << " SELECT input_idx, address, street_name, street_type, internal,"
		    << " pre_dir, post_dir, location, state_abbrev, zip,"
		    << " COALESCE("
		    << "(SELECT statefp FROM tiger.state_lookup WHERE abbrev = state_abbrev LIMIT 1),"
		    << "(SELECT statefp FROM tiger.zip_lookup_base WHERE input.zip = zip_lookup_base.zip LIMIT 1)"
		    << ") AS resolved_statefp"
		    << " FROM input ORDER BY input_idx";
	}

	auto result = conn.Query(sql.str());
	if (result->HasError()) {
		throw IOException("tiger.geocode_batch resolution: %s", result->GetError());
	}
	std::vector<ResolvedRow> out;
	out.reserve(buffered.size());
	while (auto row = result->Fetch()) {
		for (idx_t r = 0; r < row->size(); r++) {
			ResolvedRow rr;
			rr.input_idx = row->GetValue(0, r).GetValue<int64_t>();
			rr.struct_fields.reserve(9);
			for (idx_t f = 1; f <= 9; f++) {
				rr.struct_fields.emplace_back(row->GetValue(f, r));
			}
			rr.statefp = row->GetValue(10, r);
			out.push_back(std::move(rr));
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Per-state dispatch: run tiger.geocode_address_for_state with literal
// statefp + the state's struct values.
// ---------------------------------------------------------------------------

static std::string RenderGeocodeInputCast(const std::vector<Value> &fields) {
	std::ostringstream os;
	os << "CAST({"
	   << "'address':" << ValueAsSqlLiteral(fields[0]) << ","
	   << "'street_name':" << ValueAsSqlLiteral(fields[1]) << ","
	   << "'street_type':" << ValueAsSqlLiteral(fields[2]) << ","
	   << "'internal':" << ValueAsSqlLiteral(fields[3]) << ","
	   << "'pre_dir':" << ValueAsSqlLiteral(fields[4]) << ","
	   << "'post_dir':" << ValueAsSqlLiteral(fields[5]) << ","
	   << "'location':" << ValueAsSqlLiteral(fields[6]) << ","
	   << "'state_abbrev':" << ValueAsSqlLiteral(fields[7]) << ","
	   << "'zip':" << ValueAsSqlLiteral(fields[8])
	   << "} AS tiger.geocode_input)";
	return os.str();
}

static void RunPerStateGeocode(Connection &conn, const std::string &statefp_lit,
                               const std::vector<ResolvedRow> &state_rows,
                               std::vector<GeocodeResult> &out) {
	if (state_rows.empty()) return;

	// Build VALUES of (input_idx, struct_arg) for this state. Then LATERAL
	// the macro per row — the planner constant-folds statefp_lit and uses
	// ART pushdown on big tables.
	std::ostringstream sql;
	sql << "WITH input(input_idx, struct_arg) AS (VALUES ";
	for (idx_t i = 0; i < state_rows.size(); i++) {
		if (i > 0) sql << ",";
		sql << "(" << state_rows[i].input_idx << ", "
		    << RenderGeocodeInputCast(state_rows[i].struct_fields) << ")";
	}
	sql << ")"
	    << " SELECT input.input_idx,"
	    << " g.rating,"
	    << " ST_X(g.geom) AS lng, ST_Y(g.geom) AS lat,"
	    << " tiger.pprint_adr(g.adr) AS adr_text,"
	    << " g.block_geoid, g.tract_geoid, g.blkgrp_geoid,"
	    << " g.containment_guaranteed"
	    << " FROM input,"
	    << " LATERAL tiger.geocode_address_for_state("
	    << EscapeSqlLiteral(statefp_lit) << ", struct_arg, 1, NULL, 2.0) g"
	    << " QUALIFY ROW_NUMBER() OVER (PARTITION BY input.input_idx ORDER BY g.rating) = 1";

	auto result = conn.Query(sql.str());
	if (result->HasError()) {
		throw IOException("tiger.geocode_batch dispatch (statefp=%s): %s", statefp_lit, result->GetError());
	}
	while (auto row = result->Fetch()) {
		for (idx_t r = 0; r < row->size(); r++) {
			GeocodeResult gr;
			gr.input_idx = row->GetValue(0, r).GetValue<int64_t>();
			gr.rating = row->GetValue(1, r);
			gr.lng = row->GetValue(2, r);
			gr.lat = row->GetValue(3, r);
			gr.adr_text = row->GetValue(4, r);
			gr.block_geoid = row->GetValue(5, r);
			gr.tract_geoid = row->GetValue(6, r);
			gr.blkgrp_geoid = row->GetValue(7, r);
			gr.containment_guaranteed = row->GetValue(8, r);
			out.push_back(std::move(gr));
		}
	}
}

// ---------------------------------------------------------------------------
// Flush: resolve, partition, dispatch, pair with passthrough, queue output.
// ---------------------------------------------------------------------------

static void FlushBuffer(ClientContext &context, const BatchBindData &bind_data, BatchGlobalState &gstate) {
	if (gstate.buffered.empty()) return;

	Connection conn(*context.db);

	// Step 1: resolve struct fields + statefp for every buffered row.
	auto resolved = RunResolution(conn, bind_data, gstate.buffered);

	// Step 2: partition by statefp (NULL statefp = no anchor → no match,
	// matches the existing macro's behavior).
	std::unordered_map<std::string, std::vector<ResolvedRow>> by_statefp;
	for (auto &r : resolved) {
		if (r.statefp.IsNull()) continue;
		by_statefp[r.statefp.GetValue<std::string>()].push_back(std::move(r));
	}

	// Step 3: per-state dispatch.
	std::vector<GeocodeResult> all_results;
	all_results.reserve(resolved.size());
	for (auto &kv : by_statefp) {
		RunPerStateGeocode(conn, kv.first, kv.second, all_results);
	}

	// Step 4: index results by input_idx for fast pairing with the buffered
	// passthrough values. A row that produced no geocode output (e.g. NULL
	// statefp, or no candidate above gate) gets a NULL result row so the
	// caller still sees their input_idx in the output.
	std::unordered_map<idx_t, GeocodeResult> by_input_idx;
	for (auto &g : all_results) {
		by_input_idx[g.input_idx] = std::move(g);
	}

	gstate.pending_output.clear();
	gstate.pending_output.reserve(gstate.buffered.size());
	for (idx_t i = 0; i < gstate.buffered.size(); i++) {
		auto it = by_input_idx.find(i);
		GeocodeResult gr;
		gr.input_idx = i;
		if (it != by_input_idx.end()) {
			gr = std::move(it->second);
			gr.input_idx = i; // re-stamp after move (paranoia)
		} // else: leave defaults (all-NULL Values).
		gstate.pending_output.push_back(std::move(gr));
	}

	gstate.output_emitted = 0;
}

// ---------------------------------------------------------------------------
// Output emission. Pairs pending_output[i] with gstate.buffered[i]'s passthrough.
// ---------------------------------------------------------------------------

static idx_t EmitFromQueue(const BatchBindData &bind_data, BatchGlobalState &gstate, DataChunk &output) {
	idx_t avail = gstate.pending_output.size() - gstate.output_emitted;
	idx_t n = MinValue<idx_t>(avail, STANDARD_VECTOR_SIZE);
	for (idx_t out_row = 0; out_row < n; out_row++) {
		const auto &gr = gstate.pending_output[gstate.output_emitted + out_row];
		const auto &buffered = gstate.buffered[gr.input_idx];
		// Passthrough columns first.
		for (idx_t pt = 0; pt < bind_data.n_passthrough; pt++) {
			output.SetValue(pt, out_row, buffered.passthrough_values[pt]);
		}
		idx_t base = bind_data.n_passthrough;
		output.SetValue(base + 0, out_row, gr.rating);
		output.SetValue(base + 1, out_row, gr.lng);
		output.SetValue(base + 2, out_row, gr.lat);
		output.SetValue(base + 3, out_row, gr.adr_text);
		output.SetValue(base + 4, out_row, gr.block_geoid);
		output.SetValue(base + 5, out_row, gr.tract_geoid);
		output.SetValue(base + 6, out_row, gr.blkgrp_geoid);
		output.SetValue(base + 7, out_row, gr.containment_guaranteed);
	}
	output.SetCardinality(n);
	gstate.output_emitted += n;
	if (gstate.output_emitted >= gstate.pending_output.size()) {
		gstate.pending_output.clear();
		gstate.output_emitted = 0;
		gstate.buffered.clear(); // safe to drop now that output is queued/emitted
	}
	return n;
}

OperatorResultType Execute(ExecutionContext &context, TableFunctionInput &data, DataChunk &input, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<BatchBindData>();
	auto &gstate = data.global_state->Cast<BatchGlobalState>();

	// Phase 1: drain pending output before accepting more input.
	if (!gstate.pending_output.empty()) {
		EmitFromQueue(bind_data, gstate, output);
		return gstate.pending_output.empty() ? OperatorResultType::NEED_MORE_INPUT
		                                     : OperatorResultType::HAVE_MORE_OUTPUT;
	}

	// Phase 2: ingest this call's input chunk into the buffer.
	for (idx_t i = 0; i < input.size(); i++) {
		BufferedRow row;
		row.passthrough_values.reserve(bind_data.n_passthrough);
		for (auto src_idx : bind_data.passthrough_input_indices) {
			row.passthrough_values.emplace_back(input.GetValue(src_idx, i));
		}
		if (bind_data.form_freeform) {
			auto v = input.GetValue(bind_data.addr_str_idx, i);
			row.addr_str = v.IsNull() ? std::string() : v.ToString();
		} else {
			row.parsed_fields.reserve(9);
			Value addr_val;
			if (bind_data.address_idx != kInvalidIdx) {
				addr_val = input.GetValue(bind_data.address_idx, i);
				if (!addr_val.IsNull() && addr_val.type().id() != LogicalTypeId::INTEGER) {
					Value casted;
					string err;
					if (addr_val.DefaultTryCastAs(LogicalType::INTEGER, casted, &err)) {
						addr_val = casted;
					} else {
						addr_val = Value(LogicalType::INTEGER);
					}
				}
			} else {
				addr_val = Value(LogicalType::INTEGER);
			}
			row.parsed_fields.emplace_back(std::move(addr_val));
			auto cell = [&](idx_t col_idx) -> Value {
				return col_idx == kInvalidIdx ? Value(LogicalType::VARCHAR) : input.GetValue(col_idx, i);
			};
			row.parsed_fields.emplace_back(cell(bind_data.street_name_idx));
			row.parsed_fields.emplace_back(cell(bind_data.street_type_idx));
			row.parsed_fields.emplace_back(cell(bind_data.internal_idx));
			row.parsed_fields.emplace_back(cell(bind_data.pre_dir_idx));
			row.parsed_fields.emplace_back(cell(bind_data.post_dir_idx));
			row.parsed_fields.emplace_back(cell(bind_data.location_idx));
			row.parsed_fields.emplace_back(cell(bind_data.state_abbrev_idx));
			row.parsed_fields.emplace_back(cell(bind_data.zip_idx));
		}
		gstate.buffered.emplace_back(std::move(row));
	}

	// Phase 3: flush only when buffer threshold hit.
	if (gstate.buffered.size() < kBufferThreshold) {
		output.SetCardinality(0);
		return OperatorResultType::NEED_MORE_INPUT;
	}

	FlushBuffer(context.client, bind_data, gstate);
	EmitFromQueue(bind_data, gstate, output);
	return gstate.pending_output.empty() ? OperatorResultType::NEED_MORE_INPUT
	                                     : OperatorResultType::HAVE_MORE_OUTPUT;
}

OperatorFinalizeResultType Finalize(ExecutionContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<BatchBindData>();
	auto &gstate = data.global_state->Cast<BatchGlobalState>();
	gstate.input_exhausted = true;
	if (!gstate.buffered.empty() && gstate.pending_output.empty()) {
		FlushBuffer(context.client, bind_data, gstate);
	}
	if (gstate.pending_output.empty()) {
		output.SetCardinality(0);
		return OperatorFinalizeResultType::FINISHED;
	}
	EmitFromQueue(bind_data, gstate, output);
	return gstate.pending_output.empty() ? OperatorFinalizeResultType::FINISHED
	                                     : OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
}

} // namespace

void RegisterGeocodeBatchFunction(ExtensionLoader &loader) {
	TableFunction fn("geocode_batch", {LogicalType::TABLE}, /*function=*/nullptr, Bind, BatchGlobalState::Init);
	fn.in_out_function = Execute;
	fn.in_out_function_final = Finalize;
	loader.RegisterFunction(fn);
}

} // namespace us_geocoder
} // namespace duckdb
