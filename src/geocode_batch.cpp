// tiger.geocode_batch — C++ table-in-out function for batched geocoding.
// See src/include/geocode_batch.hpp for design context.
//
// API: a single LogicalType::TABLE arg (the input subquery). The function
// supports two input shapes mirroring the existing tiger.geocode overloads:
//
//   -- Form 1 (freeform). One VARCHAR column named addr_str. Goes through
//   -- tiger.from_pagc internally to produce a tiger.geocode_input struct.
//   SELECT * FROM geocode_batch((SELECT id, addr_str FROM tbl));
//
//   -- Form 2 (pre-parsed). Subset of the tiger.geocode_input struct fields
//   -- as separate columns: address INT, street_name VARCHAR, street_type
//   -- VARCHAR, internal VARCHAR, pre_dir VARCHAR, post_dir VARCHAR, location
//   -- VARCHAR, state_abbrev VARCHAR, zip VARCHAR. Missing fields default to
//   -- NULL. The columns are assembled directly into the geocode_input
//   -- struct — *not* concatenated and *not* run through from_pagc, so
//   -- callers with already-parsed data don't lose information.
//   SELECT * FROM geocode_batch((SELECT id, address, street_name, street_type,
//                                       location, state_abbrev, zip
//                                FROM tbl));
//
// Form precedence: if addr_str is present, Form 1 wins and any pre-parsed
// fields fall through as passthrough columns.
//
// All other input columns are PASSTHROUGH — copied to the output rows
// alongside the geocoder result. Carries id / metadata through without an
// outer join.
//
// SCAFFOLD STATUS (May 2026):
//   * Bind validates the input shape; identifies passthrough columns.
//   * Execute buffers up to 100K rows then flushes via NEED_MORE_INPUT /
//     in_out_function_final. Verified working for the LogicalType::TABLE
//     input shape (LATERAL with project_input is not supported by DuckDB's
//     PhysicalTableInOutFunction::FinalExecute — see commit history).
//   * Output is a single VARCHAR `geocode_input_echo` column showing the
//     freeform string (Form 1) or a synthetic rendering of the struct
//     (Form 2). Real output schema (next milestone): the same 7 columns
//     tiger.geocode returns — adr, geom, rating, block_geoid, tract_geoid,
//     blkgrp_geoid, containment_guaranteed.
//   * Per-state SQL dispatch: NOT YET. Currently flush is a no-op.

#include "geocode_batch.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <vector>

namespace duckdb {
namespace us_geocoder {

namespace {

constexpr idx_t kBufferThreshold = 100000;
constexpr idx_t kInvalidIdx = static_cast<idx_t>(-1);

// Recognized input column names. Form 1 uses addr_str; Form 2 uses the
// nine geocode_input field names (which match tiger.geocode_input.*). All
// other columns pass through.
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
	// Form 1 column index (kInvalidIdx if not present).
	idx_t addr_str_idx = kInvalidIdx;

	// Form 2 column indices (kInvalidIdx if not present). These map onto
	// the fields of tiger.geocode_input exactly.
	idx_t address_idx = kInvalidIdx;
	idx_t street_name_idx = kInvalidIdx;
	idx_t street_type_idx = kInvalidIdx;
	idx_t internal_idx = kInvalidIdx;
	idx_t pre_dir_idx = kInvalidIdx;
	idx_t post_dir_idx = kInvalidIdx;
	idx_t location_idx = kInvalidIdx;
	idx_t state_abbrev_idx = kInvalidIdx;
	idx_t zip_idx = kInvalidIdx;

	// Passthrough column input indices, in input order. Excludes whichever
	// form's columns are consumed; columns not consumed by either form
	// always pass through.
	std::vector<idx_t> passthrough_input_indices;
	idx_t n_passthrough = 0;

	// True iff Form 1 is in use (addr_str present). Form 2 otherwise.
	bool form_freeform = false;
};

// One buffered input row: passthrough column values + the parsed input.
// For Form 1 we keep the raw addr_str; from_pagc gets called at flush time.
// For Form 2 we keep the pre-parsed Values directly.
struct BufferedRow {
	std::vector<Value> passthrough_values;
	// Form 1 input: the raw freeform string (or empty if NULL). Only populated
	// when bind_data.form_freeform == true.
	std::string addr_str;
	// Form 2 input: each Value carries the column's type (INTEGER for address,
	// VARCHAR for the rest) plus NULLability. Only populated when
	// bind_data.form_freeform == false. Indices match the geocode_input
	// struct field order: address, street_name, street_type, internal,
	// pre_dir, post_dir, location, state_abbrev, zip.
	std::vector<Value> parsed_fields;
};

struct BatchGlobalState : public GlobalTableFunctionState {
	BatchGlobalState() = default;

	std::vector<BufferedRow> buffered;
	std::vector<BufferedRow> pending_output;
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

	// First pass: identify recognized columns and validate types.
	for (idx_t i = 0; i < input.input_table_names.size(); i++) {
		const auto &col_name = input.input_table_names[i];
		const auto &col_type = input.input_table_types[i];
		if (col_name == kAddrStrCol) {
			RequireType(col_name, col_type, LogicalTypeId::VARCHAR);
			bind_data->addr_str_idx = i;
		} else if (col_name == kAddressCol) {
			// Accept INTEGER or VARCHAR. Real-world data often has house
			// numbers as strings (CSV import, etc.); from_pagc itself does
			// TRY_CAST(regexp_extract(...) AS INTEGER), so we tolerate the
			// same shapes and coerce at ingest time.
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

	// Form selection: addr_str wins if present.
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

	// Second pass: passthrough columns. Skip whichever input columns are
	// consumed by the active form; others pass through unchanged.
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

	// Geocoder result column. Scaffold echo only — real shape (next
	// milestone) appends adr, geom, rating, block_geoid, tract_geoid,
	// blkgrp_geoid, containment_guaranteed.
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("geocode_input_echo");

	return std::move(bind_data);
}

// Helper: read a Value out of an input chunk cell. Returns Value() (NULL)
// if col_idx is kInvalidIdx (column not present in input).
Value ReadCell(DataChunk &chunk, idx_t col_idx, idx_t row_idx) {
	if (col_idx == kInvalidIdx) {
		return Value();
	}
	return chunk.GetValue(col_idx, row_idx);
}

// Helper: render the buffered Form-2 fields as a debug string. Used by the
// scaffold's echo output. Real impl will assemble the actual geocode_input
// struct and run it through the geocoder.
std::string RenderParsedFields(const std::vector<Value> &fields) {
	const char *labels[] = {"address",  "street_name", "street_type", "internal",     "pre_dir",
	                        "post_dir", "location",    "state_abbrev", "zip"};
	std::string out;
	for (idx_t i = 0; i < fields.size(); i++) {
		if (fields[i].IsNull()) continue;
		if (!out.empty()) out += " ";
		out += labels[i];
		out += "=";
		out += fields[i].ToString();
	}
	return out;
}

void FlushBuffer(BatchGlobalState &gstate) {
	// Real implementation: partition gstate.buffered by resolved statefp,
	// dispatch per-state SQL with a literal `WHERE statefp='<lit>'`, collect
	// results, pair with passthrough values via the original input position.
	// Scaffold: just promote buffered → pending_output verbatim.
	for (auto &row : gstate.buffered) {
		gstate.pending_output.push_back(std::move(row));
	}
	gstate.buffered.clear();
	gstate.output_emitted = 0;
}

idx_t EmitFromQueue(const BatchBindData &bind_data, BatchGlobalState &gstate, DataChunk &output) {
	idx_t avail = gstate.pending_output.size() - gstate.output_emitted;
	idx_t n = MinValue<idx_t>(avail, STANDARD_VECTOR_SIZE);
	for (idx_t out_row = 0; out_row < n; out_row++) {
		const auto &buffered = gstate.pending_output[gstate.output_emitted + out_row];
		// Passthrough columns first.
		for (idx_t pt = 0; pt < bind_data.n_passthrough; pt++) {
			output.SetValue(pt, out_row, buffered.passthrough_values[pt]);
		}
		// Then the geocoder result column (scaffold).
		std::string echo;
		if (bind_data.form_freeform) {
			echo = buffered.addr_str;
		} else {
			echo = RenderParsedFields(buffered.parsed_fields);
		}
		output.SetValue(bind_data.n_passthrough, out_row, Value(echo));
	}
	output.SetCardinality(n);
	gstate.output_emitted += n;
	if (gstate.output_emitted >= gstate.pending_output.size()) {
		gstate.pending_output.clear();
		gstate.output_emitted = 0;
	}
	return n;
}

OperatorResultType Execute(ExecutionContext &, TableFunctionInput &data, DataChunk &input, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<BatchBindData>();
	auto &gstate = data.global_state->Cast<BatchGlobalState>();

	// Phase 1: drain pending output from a prior flush before accepting more input.
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
			// Order matches the geocode_input struct field order; missing
			// columns yield NULL Values so the struct construction at the
			// real-geocode milestone gets the right shape.
			row.parsed_fields.reserve(9);
			// `address`: coerce to INTEGER for the geocode_input struct
			// (which has `address INTEGER`). Mirrors from_pagc's
			// TRY_CAST(regexp_extract(..., '\\d+') AS INTEGER) behavior:
			// non-numeric inputs yield NULL rather than an error. Use
			// DefaultTryCastAs so VARCHAR / BIGINT / INTEGER all funnel to
			// the same INTEGER value the downstream macro expects.
			{
				Value v = ReadCell(input, bind_data.address_idx, i);
				if (!v.IsNull() && v.type().id() != LogicalTypeId::INTEGER) {
					Value casted;
					string err;
					if (v.DefaultTryCastAs(LogicalType::INTEGER, casted, &err)) {
						v = casted;
					} else {
						v = Value(LogicalType::INTEGER); // NULL
					}
				}
				row.parsed_fields.emplace_back(std::move(v));
			}
			row.parsed_fields.emplace_back(ReadCell(input, bind_data.street_name_idx, i));
			row.parsed_fields.emplace_back(ReadCell(input, bind_data.street_type_idx, i));
			row.parsed_fields.emplace_back(ReadCell(input, bind_data.internal_idx, i));
			row.parsed_fields.emplace_back(ReadCell(input, bind_data.pre_dir_idx, i));
			row.parsed_fields.emplace_back(ReadCell(input, bind_data.post_dir_idx, i));
			row.parsed_fields.emplace_back(ReadCell(input, bind_data.location_idx, i));
			row.parsed_fields.emplace_back(ReadCell(input, bind_data.state_abbrev_idx, i));
			row.parsed_fields.emplace_back(ReadCell(input, bind_data.zip_idx, i));
		}
		gstate.buffered.emplace_back(std::move(row));
	}

	// Phase 3: flush only when the buffer hits the threshold. Otherwise
	// ask DuckDB for more input. End-of-input is handled by Finalize.
	if (gstate.buffered.size() < kBufferThreshold) {
		output.SetCardinality(0);
		return OperatorResultType::NEED_MORE_INPUT;
	}

	FlushBuffer(gstate);
	EmitFromQueue(bind_data, gstate, output);
	return gstate.pending_output.empty() ? OperatorResultType::NEED_MORE_INPUT
	                                     : OperatorResultType::HAVE_MORE_OUTPUT;
}

OperatorFinalizeResultType Finalize(ExecutionContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<BatchBindData>();
	auto &gstate = data.global_state->Cast<BatchGlobalState>();
	gstate.input_exhausted = true;
	if (!gstate.buffered.empty()) {
		FlushBuffer(gstate);
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
	// Single LogicalType::TABLE arg — accepts any subquery whose result
	// includes either addr_str (Form 1) or some of the geocode_input fields
	// (Form 2, by name). Other columns pass through.
	TableFunction fn("geocode_batch", {LogicalType::TABLE}, /*function=*/nullptr, Bind, BatchGlobalState::Init);
	fn.in_out_function = Execute;
	fn.in_out_function_final = Finalize;
	loader.RegisterFunction(fn);
}

} // namespace us_geocoder
} // namespace duckdb
