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
#include <string>
#include <string_view>

namespace alpacacore::util {

/// The telescope Action that says whether FindHome is a real sensor search: a search whose end point does not depend
/// on where the mount believes it points. Every telescope driver lists it in SupportedActions while connected; the
/// Action answers NotConnected while disconnected.
inline constexpr std::string_view kFindHomeSensorSearchAction = "findhomeissensorsearch";

/// The FindHome method words of the reply.
inline constexpr std::string_view kFindHomeZeroSensorSearch = "zero_sensor_search";  ///< the mount's zero sensors
inline constexpr std::string_view kFindHomeSwitchSearch = "home_switch_search";      ///< hardware home switches
inline constexpr std::string_view kFindHomeIndexSearch = "home_index_search";        ///< home index sensors
inline constexpr std::string_view kFindHomeGotoStoredHome = "goto_stored_home";      ///< slews to a stored position
inline constexpr std::string_view kFindHomeNoSensor = "no_home_sensor";      ///< a sensor search with no sensor found
inline constexpr std::string_view kFindHomeExternal = "external_find_home";  ///< another program's own FindHome
inline constexpr std::string_view kFindHomeNotSupported = "not_supported";   ///< CanFindHome is false

/// Action names are case-insensitive (ASCOM), so "FindHomeIsSensorSearch" selects the action too.
inline bool is_findhome_sensor_search_action(std::string_view action_name) {
    return std::equal(action_name.begin(), action_name.end(), kFindHomeSensorSearchAction.begin(),
                      kFindHomeSensorSearchAction.end(),
                      [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

/**
 * The Action("findhomeissensorsearch") reply: {"sensor_search":<bool>,"method":"<word>","model":"<model>"}.
 *
 * sensor_search is true only when the driver's FindHome is a search on the mount's own home or zero sensors, so a
 * completed FindHome puts the mount at its true home whatever position it believed before. A client may trust
 * AtHome after a FindHome it sent only when this is true. model is the mount model code the answer depends on, or
 * "" when the driver does not know or the answer does not depend on it. method is not escaped: it must be one of the
 * kFindHome* words above.
 */
inline std::string format_findhome_sensor_search(bool sensor_search, std::string_view method, std::string_view model) {
    // The model comes from the mount; escape what JSON requires so a stray byte cannot break the reply.
    std::string escaped;
    for (const char ch : model) {
        const auto byte = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') {
            escaped += '\\';
            escaped += ch;
        } else if (byte < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            escaped += "\\u00";
            escaped += kHex[byte >> 4];
            escaped += kHex[byte & 0x0f];
        } else {
            escaped += ch;
        }
    }
    return std::string("{\"sensor_search\":") + (sensor_search ? "true" : "false") + ",\"method\":\"" +
           std::string(method) + "\",\"model\":\"" + escaped + "\"}";
}

}  // namespace alpacacore::util
