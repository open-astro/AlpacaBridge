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
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace alpacacore::vendor::synscan {

enum class SynScanVersion {
    Auto,
    V3,
    V4
};

/// Configured geometry of an AZ-EQ mount (model IDs 5/6), whose handset does
/// not report whether it is set up Alt-Az or equatorial (#860). Auto keeps the
/// driver's refusal to guess; other models ignore the setting.
enum class SynScanAlignmentSetting : std::uint8_t { Auto, AltAz, Equatorial };

std::unique_ptr<TelescopeDriver> create_synscan_telescope(
    int device_number,
    const ConnectionInfo& connection_info,
    SynScanVersion version);

std::unique_ptr<TelescopeDriver> create_synscan_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, SynScanVersion version,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
    SynScanAlignmentSetting alignment = SynScanAlignmentSetting::Auto,
    util::TaskClock& clock = util::default_task_clock());

/// Test seam: replaces the thread factory of the driver's slew slot, so a case
/// can make a body's start fail. Call only while no start is in flight.
/// `driver` must come from the factories above; anything else is ignored.
void set_slew_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn);

/// Endpoint resolved at connect time by `connection_resolver` (#659); the
/// auto-detect factory below wraps it, tests inject a fake's endpoint.
std::unique_ptr<TelescopeDriver> create_synscan_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    SynScanVersion version = SynScanVersion::Auto, std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt, std::optional<double> site_elevation_m = std::nullopt,
    std::optional<bool> sync_time_on_connect = std::nullopt,
    SynScanAlignmentSetting alignment = SynScanAlignmentSetting::Auto,
    util::TaskClock& clock = util::default_task_clock());

/// The serial scan behind create_synscan_telescope_auto(); throws when nothing answers.
ConnectionInfo resolve_synscan_serial_auto(int mount_index);

// Auto-detect: the scan runs at connect time, so construction succeeds while
// the mount is absent (#659).
std::unique_ptr<TelescopeDriver> create_synscan_telescope_auto(
    int device_number, int mount_index = 0, SynScanVersion version = SynScanVersion::Auto,
    std::optional<double> site_latitude_deg = std::nullopt, std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt, std::optional<bool> sync_time_on_connect = std::nullopt,
    SynScanAlignmentSetting alignment = SynScanAlignmentSetting::Auto,
    util::TaskClock& clock = util::default_task_clock());

} // namespace alpacacore::vendor::synscan
