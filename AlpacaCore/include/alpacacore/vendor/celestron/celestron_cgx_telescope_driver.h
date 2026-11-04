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

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::celestron {

struct CelestronCgxConnectionInfo {
    std::string port_path;
    std::optional<double> site_latitude_degrees;
    std::optional<double> site_longitude_degrees;
    std::optional<double> site_elevation_meters;
    double aperture_diameter_meters = 0.0;
    double focal_length_meters = 0.0;
};

using CgxSystemNow = std::function<std::chrono::system_clock::time_point()>;

/// `system_now` is a deterministic wall-clock seam for UTCDate/pointing tests.
std::unique_ptr<TelescopeDriver> create_celestron_cgx_telescope(int device_number,
                                                                const CelestronCgxConnectionInfo& info,
                                                                util::TaskClock& clock = util::default_task_clock(),
                                                                CgxSystemNow system_now = {});

/// Test seams of the state poll (`util::PolledState`, decision 0010). The spawn replaces the poll slot's
/// thread factory (set it before Connect); while paused the poll waits its ticks but probes nothing; one poll
/// cycle runs on the calling thread, the loss handling included. A driver that is not the CGX is ignored.
void set_state_poll_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn);
void set_state_poll_paused_for_testing(TelescopeDriver& driver, bool paused);
void poll_state_now_for_testing(TelescopeDriver& driver);
/// Reads and clears the link-loss stamp (decision 0009 point 3); nullopt when none is held.
std::optional<util::TaskClock::clock::time_point> consume_link_lost_at_for_testing(TelescopeDriver& driver);

}  // namespace alpacacore::vendor::celestron
