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
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/zwo/zwo_mount_protocol_wrapper.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::zwo {

/// Test-only view of the stored time: disconnected public getters must refuse even an offline anchor.
std::optional<std::chrono::system_clock::time_point> utc_anchor_for_testing(const TelescopeDriver& driver);

/// Test-only: replaces the GOTO slot's thread factory (call before the first slew), so a test can refuse a start.
void set_goto_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn);

/// Test-only: replaces the disconnect teardown slot's thread factory (call before the first Connected=false).
void set_teardown_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn);

/// Test-only: called with the frame prefix (":Sr", ":MS", ...) after a GOTO wire call is counted at the send gate and
/// before it is sent (call before the first slew).
void set_goto_send_hook_for_testing(TelescopeDriver& driver, std::function<void(std::string_view)> hook);

std::unique_ptr<TelescopeDriver> create_zwo_telescope(int device_number, const ConnectionInfo& connection_info,
                                                      util::TaskClock& clock = util::default_task_clock());

std::unique_ptr<TelescopeDriver> create_zwo_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, util::TaskClock& clock = util::default_task_clock());

} // namespace alpacacore::vendor::zwo
