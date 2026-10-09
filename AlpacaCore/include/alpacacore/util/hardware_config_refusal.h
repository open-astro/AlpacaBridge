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

// open-astro#765: a device config may not choose which GPIO chip, GPIO line or
// device node the server opens. The boards have fixed wiring, so the only
// accepted values are the board's own. One home for the refusal text, shared by
// the router (ZWO, iOptron) and the ToupTek catalog schema; configuredevice
// answers HTTP 400 for a refusal that starts with kHardwareConfigRefusal.

#include <string>

namespace alpacacore::util {

inline constexpr const char* kHardwareConfigRefusal = "Hardware config refused: ";

inline std::string hardware_config_refusal(const std::string& field, const std::string& allowed) {
    return std::string(kHardwareConfigRefusal) + "'" + field + "' must be " + allowed +
           " for this board; the server does not open other chip nodes or GPIO lines";
}

}  // namespace alpacacore::util
