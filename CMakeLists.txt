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
#
# MSVC caps individual string literals at 16,380 single-byte chars (error
# C2026 — yes, much smaller than the C standard's effective 64 KB). Several
# of our .sql.in files exceed that — lookup_tables.sql.in (~1300 INSERTs)
# and the macro files are well past it. emit_chunked_literals() splits
# each file's content into ~8 KB pieces and emits them as adjacent C++ raw
# string literals; the preprocessor concatenates them at compile time, so
# the resulting std::string contents are identical on every compiler.
# Target is held well under 16,380 to leave headroom for the +1024 newline
# scan AND CRLF expansion on Windows runners (where checked-out .sql.in
# files may have CRLF endings, doubling the byte count of each newline in
# the embedded literal). Pure build-time fix; no runtime impact.
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

# CMake function: take an input variable name holding raw SQL, walk it in
# ~16 KB blocks aligned to newline boundaries, and emit a sequence of
# `R"USGSQLBLK(<chunk>)USGSQLBLK"` literals separated by newlines (so
# adjacent C++ literal concatenation kicks in).
function(emit_chunked_literals INPUT_VAR OUT_VAR)
    set(content "${${INPUT_VAR}}")
    string(LENGTH "${content}" total_len)
    set(result "")
    set(pos 0)
    set(target 8000)
    while(pos LESS total_len)
        math(EXPR remaining "${total_len} - ${pos}")
        if(remaining LESS_EQUAL ${target})
            string(SUBSTRING "${content}" ${pos} -1 chunk)
            set(pos ${total_len})
        else()
            # Look for a newline near the target boundary so we don't split
            # mid-line. Search up to 1 KB past target before giving up and
            # cutting at exactly target.
            math(EXPR scan_start "${pos} + ${target}")
            string(SUBSTRING "${content}" ${scan_start} 1024 tail)
            string(FIND "${tail}" "\n" nl_offset)
            if(nl_offset EQUAL -1)
                set(chunk_len ${target})
            else()
                math(EXPR chunk_len "${target} + ${nl_offset} + 1")
            endif()
            string(SUBSTRING "${content}" ${pos} ${chunk_len} chunk)
            math(EXPR pos "${pos} + ${chunk_len}")
        endif()
        string(APPEND result "R\"USGSQLBLK(${chunk})USGSQLBLK\"\n  ")
    endwhile()
    if(result STREQUAL "")
        # Empty input: emit one empty literal so the C++ initializer is valid.
        set(result "R\"USGSQLBLK()USGSQLBLK\"")
    endif()
    set(${OUT_VAR} "${result}" PARENT_SCOPE)
endfunction()

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/lookup_tables.sql.in LOOKUP_TABLES_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/canon_macros.sql.in CANON_MACROS_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/scoring_macros.sql.in SCORING_MACROS_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/spatial_macros.sql.in SPATIAL_MACROS_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/tiger_schema.sql.in TIGER_SCHEMA_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_location.sql.in GEOCODE_LOCATION_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_address.sql.in GEOCODE_ADDRESS_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_address_for_state.sql.in GEOCODE_ADDRESS_FOR_STATE_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode.sql.in GEOCODE_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_intersection.sql.in GEOCODE_INTERSECTION_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/reverse_geocode.sql.in REVERSE_GEOCODE_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/loader_templates.sql.in LOADER_TEMPLATES_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_input_type.sql.in GEOCODE_INPUT_TYPE_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/from_pagc.sql.in FROM_PAGC_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/pagc_tables.sql.in PAGC_TABLES_SQL)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/pprint_adr.sql.in PPRINT_ADR_SQL)

emit_chunked_literals(LOOKUP_TABLES_SQL              LOOKUP_TABLES_LITS)
emit_chunked_literals(CANON_MACROS_SQL               CANON_MACROS_LITS)
emit_chunked_literals(SCORING_MACROS_SQL             SCORING_MACROS_LITS)
emit_chunked_literals(SPATIAL_MACROS_SQL             SPATIAL_MACROS_LITS)
emit_chunked_literals(TIGER_SCHEMA_SQL               TIGER_SCHEMA_LITS)
emit_chunked_literals(GEOCODE_LOCATION_SQL           GEOCODE_LOCATION_LITS)
emit_chunked_literals(GEOCODE_ADDRESS_SQL            GEOCODE_ADDRESS_LITS)
emit_chunked_literals(GEOCODE_ADDRESS_FOR_STATE_SQL  GEOCODE_ADDRESS_FOR_STATE_LITS)
emit_chunked_literals(GEOCODE_SQL                    GEOCODE_LITS)
emit_chunked_literals(GEOCODE_INTERSECTION_SQL       GEOCODE_INTERSECTION_LITS)
emit_chunked_literals(REVERSE_GEOCODE_SQL            REVERSE_GEOCODE_LITS)
emit_chunked_literals(LOADER_TEMPLATES_SQL           LOADER_TEMPLATES_LITS)
emit_chunked_literals(GEOCODE_INPUT_TYPE_SQL         GEOCODE_INPUT_TYPE_LITS)
emit_chunked_literals(FROM_PAGC_SQL                  FROM_PAGC_LITS)
emit_chunked_literals(PAGC_TABLES_SQL                PAGC_TABLES_LITS)
emit_chunked_literals(PPRINT_ADR_SQL                 PPRINT_ADR_LITS)

set(EMBED_HEADER_BODY "#pragma once
#include <string>
namespace duckdb {
namespace us_geocoder {
inline const std::string &LookupTablesSql() {
  static const std::string data(
  ${LOOKUP_TABLES_LITS});
  return data;
}
inline const std::string &CanonMacrosSql() {
  static const std::string data(
  ${CANON_MACROS_LITS});
  return data;
}
inline const std::string &ScoringMacrosSql() {
  static const std::string data(
  ${SCORING_MACROS_LITS});
  return data;
}
inline const std::string &SpatialMacrosSql() {
  static const std::string data(
  ${SPATIAL_MACROS_LITS});
  return data;
}
inline const std::string &TigerSchemaSql() {
  static const std::string data(
  ${TIGER_SCHEMA_LITS});
  return data;
}
inline const std::string &GeocodeLocationSql() {
  static const std::string data(
  ${GEOCODE_LOCATION_LITS});
  return data;
}
inline const std::string &GeocodeAddressSql() {
  static const std::string data(
  ${GEOCODE_ADDRESS_LITS});
  return data;
}
inline const std::string &GeocodeAddressForStateSql() {
  static const std::string data(
  ${GEOCODE_ADDRESS_FOR_STATE_LITS});
  return data;
}
inline const std::string &GeocodeSql() {
  static const std::string data(
  ${GEOCODE_LITS});
  return data;
}
inline const std::string &GeocodeIntersectionSql() {
  static const std::string data(
  ${GEOCODE_INTERSECTION_LITS});
  return data;
}
inline const std::string &ReverseGeocodeSql() {
  static const std::string data(
  ${REVERSE_GEOCODE_LITS});
  return data;
}
inline const std::string &LoaderTemplatesSql() {
  static const std::string data(
  ${LOADER_TEMPLATES_LITS});
  return data;
}
inline const std::string &GeocodeInputTypeSql() {
  static const std::string data(
  ${GEOCODE_INPUT_TYPE_LITS});
  return data;
}
inline const std::string &FromPagcSql() {
  static const std::string data(
  ${FROM_PAGC_LITS});
  return data;
}
inline const std::string &PagcTablesSql() {
  static const std::string data(
  ${PAGC_TABLES_LITS});
  return data;
}
inline const std::string &PprintAdrSql() {
  static const std::string data(
  ${PPRINT_ADR_LITS});
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sql/geocode_address_for_state.sql.in
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
