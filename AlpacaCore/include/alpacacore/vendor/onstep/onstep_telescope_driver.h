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
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace alpacacore::vendor::onstep {

// `clock` drives every wait, sleep and deadline of the driver (decision 0005),
// and the wrapper's link-loss stamps while it is connected. Tests inject a
// FakeTaskClock.
std::unique_ptr<TelescopeDriver> create_onstep_telescope(int device_number, const ConnectionInfo& connection_info,
                                                         util::TaskClock& clock = util::default_task_clock());

std::unique_ptr<TelescopeDriver> create_onstep_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, util::TaskClock& clock = util::default_task_clock());

/// Endpoint resolved at connect time by `connection_resolver` (#659); the
/// auto-detect factory below wraps it, tests inject a fake's endpoint.
std::unique_ptr<TelescopeDriver> create_onstep_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg = std::nullopt, std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt, std::optional<bool> sync_time_on_connect = std::nullopt);

/// The serial scan behind create_onstep_telescope_auto(); throws when nothing answers.
ConnectionInfo resolve_onstep_serial_auto(int mount_index);

// Auto-detect: the scan runs at connect time, so construction succeeds while
// the mount is absent (#659).
std::unique_ptr<TelescopeDriver> create_onstep_telescope_auto(int device_number, int mount_index = 0,
                                                              std::optional<double> site_latitude_deg = std::nullopt,
                                                              std::optional<double> site_longitude_deg = std::nullopt,
                                                              std::optional<double> site_elevation_m = std::nullopt,
                                                              std::optional<bool> sync_time_on_connect = std::nullopt);

/// Test seam: runs `hook` in a Park or FindHome between releasing the driver
/// lock and starting its background part (#1430), so a case can land a newer
/// command in that gap. Set only while no Park or FindHome is in flight.
/// `driver` must come from the factories above; anything else is ignored.
void set_park_home_before_start_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam: replaces the thread factory of the driver's MoveAxis re-send
/// slot, so a case can make a body's start fail (#1458). Call only while no
/// start is in flight. `driver` must come from the factories above; anything
/// else is ignored.
void set_move_axis_resend_spawn_for_testing(TelescopeDriver& driver,
                                            std::function<std::thread(std::function<void()>)> spawn);

/// Test seam: replaces the thread factory of the driver's Park/FindHome
/// slot, so a case can hold a body's thread before it runs (#1430). Call only
/// while no start is in flight. `driver` must come from the factories above;
/// anything else is ignored.
void set_park_home_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn);

/// Test seam: `hook` runs on the thread of a clean set_connected(false), with
/// the driver's state lock released and the close not yet sent, so a case can
/// read a getter while the close is in flight (decision 0016, #1449). Pass an
/// empty function to clear it. `driver` must come from the factories above;
/// anything else is ignored.
void set_disconnect_before_close_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam: `hook` runs once, on the next getter thread that reads the mount
/// because its cache is stale, after it has released the driver's state lock and
/// before it takes the operation lock (decision 0016, #1449), so a case can land
/// a command, a disconnect or a reconnect in that gap. Pass an empty function to
/// clear it. `driver` must come from the factories above; anything else is ignored.
void set_single_read_before_io_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam: `hook` runs once, on the next getter thread that reads the mount
/// because its cache is stale, with the operation lock taken and just before the
/// read itself, so a case can see what a nested call inside the read sees (#1728
/// review). Pass an empty function to clear it. `driver` must come from the
/// factories above; anything else is ignored.
void set_single_read_in_io_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam: true when the calling thread holds the driver's operation lock, as
/// the driver's own nested calls see it. False for anything the factories above
/// did not make.
bool holds_operation_lock_for_testing(const TelescopeDriver& driver);

/// Test seam: `hook` runs in a MoveAxis re-send tick after its status and
/// altitude reads and before the re-send, with the driver locks held; when it
/// returns true the tick ends the move there, as a cancel that landed between
/// those reads and the re-send would (decision 0006 rule 3, #1728). The hook
/// must not call the driver. Pass an empty function to clear it.
void set_move_axis_resend_before_send_hook_for_testing(TelescopeDriver& driver, std::function<bool()> hook);

namespace detail {
// open-astro#521 / #1241: how long the serial link may be gone before motion
// that outlived the outage is stopped at relink rather than left running
// (default util::kRelinkMotionPreserveWindow, 5 s; a test shortens it).
// Process-wide; tests restore the default afterwards.
void set_relink_motion_preserve_window(std::chrono::milliseconds window);
std::chrono::milliseconds relink_motion_preserve_window();
}  // namespace detail

}  // namespace alpacacore::vendor::onstep
