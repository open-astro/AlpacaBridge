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
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <memory>
#include <optional>

namespace alpacacore::vendor::synscan {

enum class SynScanVersion {
    Auto,
    V3,
    V4
};

std::unique_ptr<TelescopeDriver> create_synscan_telescope(
    int device_number,
    const ConnectionInfo& connection_info,
    SynScanVersion version);

std::unique_ptr<TelescopeDriver> create_synscan_telescope_with_site(
    int device_number,
    const ConnectionInfo& connection_info,
    SynScanVersion version,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect);

std::unique_ptr<TelescopeDriver> create_synscan_telescope_auto(
    int device_number,
    int mount_index = 0,
    SynScanVersion version = SynScanVersion::Auto,
    std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt,
    std::optional<bool> sync_time_on_connect = std::nullopt);

} // namespace alpacacore::vendor::synscan
