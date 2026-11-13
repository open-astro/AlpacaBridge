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
#include <string_view>

namespace alpacacore::util {

/// Action names are case-insensitive (ASCOM), so "setzero" selects "SetZero".
inline bool same_action_name(std::string_view requested, std::string_view supported) {
    return std::equal(requested.begin(), requested.end(), supported.begin(), supported.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

/// The focuser action that makes the current position 0 without moving (ZWO EAF, ToupTek AAF, iOptron iEAF).
inline constexpr std::string_view kFocuserSetZeroAction = "SetZero";

}  // namespace alpacacore::util
