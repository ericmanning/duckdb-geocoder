#define DUCKDB_EXTENSION_MAIN

#include "us_geocoder_extension.hpp"
#include "us_geocoder_embed.hpp"
#include "us_geocoder_embedded_sql.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
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

	auto &db = loader.GetDatabaseInstance();
	Connection conn(db);

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
		conn.Commit();
	} catch (...) {
		conn.Rollback();
		throw;
	}

	// Optional-dependency registrations — best-effort.
	// spatial_macros, tiger_schema, and geocode_location need duckdb-spatial
	// (GEOMETRY type, ST_Centroid, ST_Intersects, ST_Transform...).
	// geocode_location additionally needs splink_udfs for soundex.
	// from_pagc needs us_address_standardizer. All three are attempted at
	// auto-load; if any is absent, the dependent macros/tables are absent
	// and users see "function not found" / "table not found" on call.
	ExtensionHelper::TryAutoLoadExtension(db, "spatial");
	ExtensionHelper::TryAutoLoadExtension(db, "splink_udfs");
	conn.BeginTransaction();
	TryRegisterOptional(conn, us_geocoder::SpatialMacrosSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::TigerSchemaSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeLocationSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeAddressSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::GeocodeIntersectionSql(), kDefaultTigerSchema);
	TryRegisterOptional(conn, us_geocoder::ReverseGeocodeSql(), kDefaultTigerSchema);
	conn.Commit();

	conn.BeginTransaction();
	TryRegisterOptional(conn, us_geocoder::FromPagcSql(), kDefaultTigerSchema);
	conn.Commit();
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
