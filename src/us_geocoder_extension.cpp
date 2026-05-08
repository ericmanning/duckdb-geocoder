#define DUCKDB_EXTENSION_MAIN

#include "us_geocoder_extension.hpp"
#include "us_geocoder_embed.hpp"
#include "us_geocoder_embedded_sql.hpp"
#include "us_geocoder_loader.hpp"
#include "vendored_soundex.hpp"
#include "geocode_batch.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

// Default is unqualified so the schema is created in whatever catalog the
// extension is loaded into. Qualified usage (e.g. for portable-DB Mode 3)
// will come via set_tiger_reference() in a later phase.
static const char *const kDefaultTigerSchema = "tiger";

inline void UsGeocoderVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UsGeocoderExtension ext;
	result.SetValue(0, Value(ext.Version()));
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

// Vectorized wrapper around the vendored Soundex encoder. NULL inputs and
// empty strings both map to "0000" (matches Postgres' soundex() and the
// upstream splink_udfs implementation we vendored from). The encoder owns
// its 12-byte output buffer; we copy each chunk's result into the result
// vector via StringVector::AddString before the next call clobbers it.
static void SoundexScalarFn(DataChunk &args, ExpressionState &state, Vector &result) {
	const idx_t count = args.size();
	auto &input = args.data[0];
	::us_geocoder::phonetic::Soundex encoder;
	UnaryExecutor::Execute<string_t, string_t>(input, result, count, [&](string_t val) -> string_t {
		if (val.GetSize() == 0) {
			return StringVector::AddString(result, "0000");
		}
		// Copy the input to a NUL-terminated stack/heap buffer; string_t isn't
		// guaranteed to be NUL-terminated, but the encoder reads char-by-char
		// until it hits a NUL.
		std::string nul_terminated(val.GetData(), val.GetSize());
		const char *code = encoder.Encode(nul_terminated.c_str());
		return StringVector::AddString(result, code);
	});
}

static void ExecuteEmbeddedSql(Connection &conn, const std::string &sql, const std::string &tiger_schema) {
	auto rendered = us_geocoder::ApplySubstitutions(sql, {{"@TIGER@", tiger_schema}});
	auto result = conn.Query(rendered);
	if (result->HasError()) {
		throw IOException("us_geocoder: failed to load embedded SQL: " + result->GetError());
	}
}

// Try to register SQL that depends on an optional extension (spatial,
// us_address_standardizer). Swallows errors silently: if the dependency
// isn't loaded, the dependent macros are simply absent. Users get a
// "function not found" error on call — documented in README.
static void TryRegisterOptional(Connection &conn, const std::string &sql, const std::string &tiger_schema) {
	auto rendered = us_geocoder::ApplySubstitutions(sql, {{"@TIGER@", tiger_schema}});
	auto result = conn.Query(rendered);
	(void)result;
}

static void LoadInternal(ExtensionLoader &loader) {
	auto version_fn = ScalarFunction("us_geocoder_version", {}, LogicalType::VARCHAR, UsGeocoderVersionFun);
	loader.RegisterFunction(version_fn);

	// Vendored from splink_udfs (MIT) — see src/include/vendored_soundex.hpp.
	// Registered unconditionally so the geocoder's name-match branches and
	// the loader's name_soundex precompute work without any community-
	// extension dependency. Fixed 4-char encoding (matches Postgres).
	auto soundex_fn = ScalarFunction("soundex", {LogicalType::VARCHAR}, LogicalType::VARCHAR, SoundexScalarFn);
	loader.RegisterFunction(soundex_fn);

	auto &db = loader.GetDatabaseInstance();
	Connection conn(db);

	// User-tunable knob read by tiger.geocode_batch on each Bind. Default
	// 10000 was the empirical wall-clock optimum on a 5-run sweep at
	// 100K mixed input on 32-GB-RAM hardware (also robust under
	// memory_limit=8GB; cap=5000 OOM'd in a similar test where cap=10000
	// completed cleanly because fewer-larger dispatches stream more
	// cleanly than many-smaller through the buffer pool). Override via:
	//   SET us_geocoder_slice_cap = 5000;        -- session-wide
	//   SET LOCAL us_geocoder_slice_cap = 5000;  -- single statement
	// Power users on tighter RAM may want a smaller value; on big-RAM
	// (≥64 GB) machines, slightly larger may help.
	{
		auto &config = DBConfig::GetConfig(db);
		if (!config.HasExtensionOption("us_geocoder_slice_cap")) {
			config.AddExtensionOption(
			    "us_geocoder_slice_cap",
			    "Per-state slice cap for tiger.geocode_batch (input rows per per-state SQL dispatch).",
			    LogicalType::INTEGER, Value::INTEGER(10000));
		}
	}

	// core_functions provides lpad, regexp_matches, etc. — required by
	// canon_macros, scoring_macros, and lookup_tables. Enable autoinstall
	// + autoload so DuckDB loads core_functions on-demand when our SQL
	// references those builtins. Also issue an explicit LOAD as a belt.
	{
		auto r1 = conn.Query("SET autoinstall_known_extensions=1");
		auto r2 = conn.Query("SET autoload_known_extensions=1");
		auto r3 = conn.Query("LOAD core_functions");
		(void)r1;
		(void)r2;
		(void)r3;
	}

	conn.BeginTransaction();
	try {
		ExecuteEmbeddedSql(conn, us_geocoder::LookupTablesSql(), kDefaultTigerSchema);
		ExecuteEmbeddedSql(conn, us_geocoder::CanonMacrosSql(), kDefaultTigerSchema);
		ExecuteEmbeddedSql(conn, us_geocoder::ScoringMacrosSql(), kDefaultTigerSchema);
		ExecuteEmbeddedSql(conn, us_geocoder::GeocodeInputTypeSql(), kDefaultTigerSchema);
		ExecuteEmbeddedSql(conn, us_geocoder::PprintAdrSql(), kDefaultTigerSchema);
		conn.Commit();
	} catch (...) {
		conn.Rollback();
		throw;
	}

	// Optional-dependency registrations — best-effort.
	// spatial_macros, tiger_schema, and geocode_location need duckdb-spatial
	// (GEOMETRY type, ST_Centroid, ST_Intersects, ST_Transform...).
	// from_pagc needs us_address_standardizer. soundex is now built-in
	// (vendored from splink_udfs, registered above) — no community dep.
	ExtensionHelper::TryAutoLoadExtension(db, "spatial");
	conn.BeginTransaction();
	TryRegisterOptional(conn, us_geocoder::SpatialMacrosSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::TigerSchemaSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeLocationSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeAddressSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeAddressForStateSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeIntersectionSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::ReverseGeocodeSql(), kDefaultTigerSchema);
	conn.Commit();

	// us_address_standardizer's standardize_address() needs lex/gaz/rules
	// tables in `main` to resolve at call time. The community ext ships a
	// convenience loader that populates them from bundled defaults. We
	// auto-call it best-effort here so that `tiger.from_pagc()` and the
	// arity-1 `tiger.geocode(VARCHAR)` overload work out of the box once
	// us_address_standardizer is loaded.
	ExtensionHelper::TryAutoLoadExtension(db, "us_address_standardizer");
	{
		auto r = conn.Query("SELECT load_us_address_data()");
		(void)r; // silently no-op if the standardizer ext is absent.
	}
	// Populate our own TIGER-tuned PAGC tables (tiger.pagc_lex/gaz/rules)
	// alongside the community ext's upstream-faithful us_*. from_pagc reads
	// from tiger.pagc_* via standardize_address(); the us_* tables remain
	// untouched for any caller that wants the upstream rule set.
	conn.BeginTransaction();
	TryRegisterOptional(conn, us_geocoder::PagcTablesSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::FromPagcSql(), kDefaultTigerSchema);
	conn.Commit();

	// Register the C++ loader table functions (load_tiger_nation, load_tiger_state).
	// These don't need spatial at load time; ST_Read only fires at invocation.
	us_geocoder::RegisterLoaderFunctions(loader, kDefaultTigerSchema);

	// tiger.geocode_batch — C++ table-in-out function for batched geocoding
	// (works around the per-row LATERAL decorrelation that hangs the SQL
	// macro on nationwide TIGER. See src/geocode_batch.cpp). SCAFFOLD only
	// at present.
	us_geocoder::RegisterGeocodeBatchFunction(loader);
}

void UsGeocoderExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string UsGeocoderExtension::Name() {
	return "us_geocoder";
}

std::string UsGeocoderExtension::Version() const {
#ifdef EXT_VERSION_US_GEOCODER
	return EXT_VERSION_US_GEOCODER;
#else
	return "dev";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(us_geocoder, loader) {
	duckdb::LoadInternal(loader);
}
}
