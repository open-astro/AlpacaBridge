// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace alpacacore::test {

/// The number after `"<key>":` in a gps_status reply, or nullopt when it is `null` or absent.
inline std::optional<double> gps_status_number(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const auto at = json.find(needle);
    if (at == std::string::npos) return std::nullopt;
    const char* text = json.c_str() + at + needle.size();
    if (std::string(text, 4) == "null") return std::nullopt;
    return std::strtod(text, nullptr);
}

}  // namespace alpacacore::test
