#pragma once

#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace us_geocoder {

// Replace tokens (e.g. "@TIGER@") with their runtime-resolved values.
// Each entry in `pairs` is {token, replacement}; tokens should already
// include the surrounding @…@ markers.
inline std::string ApplySubstitutions(const std::string &sql,
                                      const std::vector<std::pair<std::string, std::string>> &pairs) {
	std::string out = sql;
	for (const auto &pair : pairs) {
		const std::string &token = pair.first;
		const std::string &repl = pair.second;
		if (token.empty()) {
			continue;
		}
		size_t pos = 0;
		while ((pos = out.find(token, pos)) != std::string::npos) {
			out.replace(pos, token.size(), repl);
			pos += repl.size();
		}
	}
	return out;
}

} // namespace us_geocoder
} // namespace duckdb
