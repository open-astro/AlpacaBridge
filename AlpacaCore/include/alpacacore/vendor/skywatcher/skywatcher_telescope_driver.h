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

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>
#include <memory>
#include <optional>

namespace alpacacore::vendor::skywatcher {

// Driver for Sky-Watcher mounts spoken to directly at the motor controller
// level (Wave 100i/150i over USB serial or built-in Wi-Fi UDP). Equatorial
// mode only. Unlike the SynScan hand-controller driver, all pointing math
// (RA/Dec <-> axis counts, LST, pier side) lives in this driver; the mount
// stores no site or time information, so the site must be provided via
// configuration or the SiteLatitude/SiteLongitude setters.

std::unique_ptr<TelescopeDriver> create_skywatcher_telescope(
    int device_number,
    const ConnectionInfo& connection_info,
    std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt);

// Auto-detect: scan serial ports first, then Wi-Fi discovery (UDP 11880).
std::unique_ptr<TelescopeDriver> create_skywatcher_telescope_auto(
    int device_number,
    int mount_index = 0,
    std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt);

} // namespace alpacacore::vendor::skywatcher
