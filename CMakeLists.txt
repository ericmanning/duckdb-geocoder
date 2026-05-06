cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME us_geocoder)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

# --- Embedded SQL ---------------------------------------------------------
# Each src/sql/*.sql.in is read at configure time and emitted as an inline
# accessor function in generated/us_geocoder_embedded_sql.hpp. Returns
# `const std::string &` so we stay C++11-compatible (DuckDB's default
# standard). The @TIGER@ placeholder is substituted at runtime via
# ApplySubstitutions() in us_geocoder_embed.hpp.
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/lookup_tables.sql.in LOOKUP_TABLES_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/canon_macros.sql.in CANON_MACROS_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/scoring_macros.sql.in SCORING_MACROS_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/spatial_macros.sql.in SPATIAL_MACROS_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/tiger_schema.sql.in TIGER_SCHEMA_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_location.sql.in GEOCODE_LOCATION_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_address.sql.in GEOCODE_ADDRESS_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode.sql.in GEOCODE_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_intersection.sql.in GEOCODE_INTERSECTION_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/reverse_geocode.sql.in REVERSE_GEOCODE_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/loader_templates.sql.in LOADER_TEMPLATES_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_input_type.sql.in GEOCODE_INPUT_TYPE_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/from_pagc.sql.in FROM_PAGC_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/pagc_tables.sql.in PAGC_TABLES_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/pprint_adr.sql.in PPRINT_ADR_SQL)

set(EMBED_HEADER_BODY "#pragma once
#include <string>
namespace duckdb {
namespace us_geocoder {
inline const std::string &LookupTablesSql() {
  static const std::string data(R\"USGSQLBLK(
${LOOKUP_TABLES_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &CanonMacrosSql() {
  static const std::string data(R\"USGSQLBLK(
${CANON_MACROS_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &ScoringMacrosSql() {
  static const std::string data(R\"USGSQLBLK(
${SCORING_MACROS_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &SpatialMacrosSql() {
  static const std::string data(R\"USGSQLBLK(
${SPATIAL_MACROS_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &TigerSchemaSql() {
  static const std::string data(R\"USGSQLBLK(
${TIGER_SCHEMA_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &GeocodeLocationSql() {
  static const std::string data(R\"USGSQLBLK(
${GEOCODE_LOCATION_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &GeocodeAddressSql() {
  static const std::string data(R\"USGSQLBLK(
${GEOCODE_ADDRESS_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &GeocodeSql() {
  static const std::string data(R\"USGSQLBLK(
${GEOCODE_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &GeocodeIntersectionSql() {
  static const std::string data(R\"USGSQLBLK(
${GEOCODE_INTERSECTION_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &ReverseGeocodeSql() {
  static const std::string data(R\"USGSQLBLK(
${REVERSE_GEOCODE_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &LoaderTemplatesSql() {
  static const std::string data(R\"USGSQLBLK(
${LOADER_TEMPLATES_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &GeocodeInputTypeSql() {
  static const std::string data(R\"USGSQLBLK(
${GEOCODE_INPUT_TYPE_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &FromPagcSql() {
  static const std::string data(R\"USGSQLBLK(
${FROM_PAGC_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &PagcTablesSql() {
  static const std::string data(R\"USGSQLBLK(
${PAGC_TABLES_SQL}
)USGSQLBLK\");
  return data;
}
inline const std::string &PprintAdrSql() {
  static const std::string data(R\"USGSQLBLK(
${PPRINT_ADR_SQL}
)USGSQLBLK\");
  return data;
}
} // namespace us_geocoder
} // namespace duckdb
")
file(WRITE ${GEN_DIR}/us_geocoder_embedded_sql.hpp "${EMBED_HEADER_BODY}")
include_directories(${GEN_DIR})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/lookup_tables.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/canon_macros.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/scoring_macros.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/spatial_macros.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/tiger_schema.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_location.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_address.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_intersection.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/reverse_geocode.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/loader_templates.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_input_type.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/from_pagc.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/pagc_tables.sql.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/pprint_adr.sql.in)
# -------------------------------------------------------------------------

set(EXTENSION_SOURCES
    src/us_geocoder_extension.cpp
    src/loader.cpp
    src/geocode_batch.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
