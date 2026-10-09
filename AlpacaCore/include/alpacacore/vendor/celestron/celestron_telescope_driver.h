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
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace alpacacore::vendor::celestron {

/// Configured geometry of a fork mount, which the handset classifies as Alt-Az
/// although it can sit on a wedge (#860). Auto keeps the nominal Alt-Az answer;
/// German equatorial models ignore the setting.
enum class CelestronAlignmentSetting : std::uint8_t { Auto, AltAz, Equatorial };

std::unique_ptr<TelescopeDriver> create_celestron_telescope(
    int device_number,
    const ConnectionInfo& connection_info);

std::unique_ptr<TelescopeDriver> create_celestron_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, CelestronAlignmentSetting alignment = CelestronAlignmentSetting::Auto,
    util::TaskClock& clock = util::default_task_clock());

/// Endpoint resolved at connect time by `connection_resolver` (#659); the
/// auto-detect factory below wraps it, tests inject a fake's endpoint.
std::unique_ptr<TelescopeDriver> create_celestron_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg = std::nullopt, std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt, std::optional<bool> sync_time_on_connect = std::nullopt,
    CelestronAlignmentSetting alignment = CelestronAlignmentSetting::Auto,
    util::TaskClock& clock = util::default_task_clock());

/// The serial scan behind create_celestron_telescope_auto(); throws when nothing answers.
ConnectionInfo resolve_celestron_serial_auto(int mount_index);

// Auto-detect: scans serial ports, probes for a NexStar mount, creates the
// driver; mount_index selects which mount if several are found (0 = first).
// The scan runs at connect time, so construction succeeds while the mount is
// absent (#659).
std::unique_ptr<TelescopeDriver> create_celestron_telescope_auto(
    int device_number, int mount_index = 0, std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt, std::optional<double> site_elevation_m = std::nullopt,
    std::optional<bool> sync_time_on_connect = std::nullopt,
    CelestronAlignmentSetting alignment = CelestronAlignmentSetting::Auto,
    util::TaskClock& clock = util::default_task_clock());

} // namespace alpacacore::vendor::celestron
