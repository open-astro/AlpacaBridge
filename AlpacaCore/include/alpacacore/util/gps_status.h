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

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace alpacacore::util {

/// Action names are case-insensitive (ASCOM), so "GPS_Status" selects the action too.
inline bool is_gps_status_action(std::string_view action_name) {
    constexpr std::string_view kName = "gps_status";
    return std::equal(action_name.begin(), action_name.end(), kName.begin(), kName.end(),
                      [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

/**
 * The Action("gps_status") reply of a mount that reports its own GPS receiver:
 * {"fix":<bool>,"latitude":<deg|null>,"longitude":<deg|null>,"elevation":<m|null>}.
 *
 * Latitude and longitude are the receiver's reading and are given only with a
 * fix; they are never the site stored in the bridge config. Elevation is null
 * when the mount does not report one (iOptron and Celestron never do).
 */
inline std::string format_gps_status(bool fix, std::optional<double> latitude_degrees,
                                     std::optional<double> longitude_degrees,
                                     std::optional<double> elevation_metres = std::nullopt) {
    // std::to_chars is locale-independent, so the decimal separator is always '.'.
    const auto number = [](const std::optional<double>& value) -> std::string {
        if (!value.has_value()) return "null";
        char buffer[48];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *value, std::chars_format::fixed, 6);
        return std::string(buffer, result.ptr);
    };
    return std::string("{\"fix\":") + (fix ? "true" : "false") + ",\"latitude\":" + number(latitude_degrees) +
           ",\"longitude\":" + number(longitude_degrees) + ",\"elevation\":" + number(elevation_metres) + "}";
}

}  // namespace alpacacore::util
