// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace alpacahttp::util {

// Returns `line` up to (not including) the first '#' that starts a YAML
// comment. A '#' inside a quoted value is data, not a comment (location
// "Obs #2" or 'Obs #2'). A double or single quote opens a quoted value only
// as the first non-space character after the key's colon; later in a plain
// value it is a literal. Backslash escapes apply inside double quotes only.
// Shared by the config loader (config.cpp) and the config rewriter
// (router.cpp) so a quoting fix lands in one place.
inline std::string strip_yaml_comment(const std::string& line) {
    char quote = 0;
    bool seen_colon = false;
    bool at_value_start = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote == '"' && c == '\\') {
            ++i;
        } else if ((c == '"' || c == '\'') && (quote == c || (quote == 0 && at_value_start))) {
            quote = (quote == 0) ? c : static_cast<char>(0);
            at_value_start = false;
        } else if (c == '#' && quote == 0) {
            return line.substr(0, i);
        } else if (c == ':' && !seen_colon) {
            seen_colon = true;
            at_value_start = true;
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            at_value_start = false;
        }
    }
    return line;
}

}  // namespace alpacahttp::util
