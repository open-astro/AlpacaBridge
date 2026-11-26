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
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::ioptron {

/**
 * @brief Factory function to create iOptron telescope driver.
 *
 * @param device_number Alpaca device number
 * @param connection_info Connection information (serial port or network)
 * @param clock TaskClock the driver's settle/poll sleeps and deadlines run on
 *        (open-astro#1223, decision 0005): real by default, a FakeTaskClock in tests.
 *        Every factory below takes it last.
 * @return Unique pointer to telescope driver
 */
std::unique_ptr<TelescopeDriver> create_ioptron_telescope(int device_number, const ConnectionInfo& connection_info,
                                                          util::TaskClock& clock = util::default_task_clock());

/// Instance-scoped worker seam for deterministic async-slew lifecycle tests.
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_with_slew_worker_hook(
    int device_number, const ConnectionInfo& connection_info, std::function<void()> before_worker_lock,
    std::optional<double> site_latitude_deg = std::nullopt, std::optional<double> site_longitude_deg = std::nullopt,
    std::function<void()> before_slew_reap = {}, std::function<void()> before_handoff_lock = {},
    util::TaskClock& clock = util::default_task_clock());

/**
 * @brief Factory function to create iOptron telescope driver with site defaults.
 *
 * @param device_number Alpaca device number
 * @param connection_info Connection information (serial port or network)
 * @param site_latitude_deg Optional site latitude in degrees
 * @param site_longitude_deg Optional site longitude in degrees
 * @param site_elevation_m Optional site elevation in meters
 * @return Unique pointer to telescope driver
 */
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, util::TaskClock& clock = util::default_task_clock());

/**
 * @brief Create a driver whose endpoint is resolved at connect time.
 *
 * The resolver runs inside set_connected(true) and returns the endpoint to
 * open, or throws the refusal the client should see. The auto-detect
 * factories below are thin wrappers over it (#659); tests inject a resolver
 * that returns a fake's endpoint or throws.
 */
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg = std::nullopt, std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt, std::optional<bool> sync_time_on_connect = std::nullopt,
    util::TaskClock& clock = util::default_task_clock());

/// The serial scan behind create_ioptron_telescope_auto(); throws when nothing answers.
/// With a non-empty @p location_key (`util::location_key` of the unique id and mount index) it first tries the port
/// remembered for that key with one :MountInfo# and scans only when that fails.
ConnectionInfo resolve_ioptron_serial_auto(int mount_index, const std::string& location_key = {});
/// The network sweep behind create_ioptron_telescope_auto_network(); throws when nothing answers.
/// With a non-empty @p location_key it first tries the remembered host:port with one handshake.
ConnectionInfo resolve_ioptron_network_auto(int mount_index, const std::string& location_key = {});

// Auto-detect (serial). The scan runs at connect time, so construction
// succeeds even while the mount is absent (#659).
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_auto(int device_number, int mount_index = 0,
                                                               std::optional<double> site_latitude_deg = std::nullopt,
                                                               std::optional<double> site_longitude_deg = std::nullopt,
                                                               std::optional<double> site_elevation_m = std::nullopt,
                                                               std::optional<bool> sync_time_on_connect = std::nullopt,
                                                               util::TaskClock& clock = util::default_task_clock());

// Auto-detect (Wi-Fi). The subnet sweep runs at connect time (#659).
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_auto_network(
    int device_number, int mount_index = 0, std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt, std::optional<double> site_elevation_m = std::nullopt,
    std::optional<bool> sync_time_on_connect = std::nullopt, util::TaskClock& clock = util::default_task_clock());

/// Test seam: replaces the thread factory of the driver's slew slot, so a case
/// can make a body's start fail. Call only while no start is in flight.
/// `driver` must come from the factories above; anything else is ignored.
void set_slew_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn);

/// Test seam (issue #2432): replaces the thread factory of the zero search watchdog slot, so a case can refuse or
/// hold its body. Call only while no FindHome runs. A driver not from the factories is ignored.
void set_zero_search_watchdog_spawn_for_testing(TelescopeDriver& driver,
                                                std::function<std::thread(std::function<void()>)> spawn);

/// Test seam (issue #2396): replaces the thread factory of the motion bound watchdog slot (the async GOTO bound), so a
/// case can find its thread. A driver not from the factories is ignored.
void set_motion_bound_watchdog_spawn_for_testing(TelescopeDriver& driver,
                                                 std::function<std::thread(std::function<void()>)> spawn);

/// Test seam (issue #2651): replaces the thread factory of both MoveAxis cap slots (one per axis), so a case can find
/// a cap body's thread. A driver not from the factories is ignored.
void set_move_axis_cap_spawn_for_testing(TelescopeDriver& driver,
                                         std::function<std::thread(std::function<void()>)> spawn);

/// Test seam (issue #2396): whether the motion bound watchdog has a body running. False for a driver not from the
/// factories.
bool motion_bound_watchdog_running_for_testing(TelescopeDriver& driver);

/// Test seam (decision 0010, #2491): pauses or resumes the state poll. A paused poll keeps waiting its ticks on the
/// task clock and probes nothing, so a case that scripts the mount's replies, or advances the clock while it counts the
/// commands sent, does not have the poll consume them. A driver not from the factories is ignored.
void set_state_poll_paused_for_testing(TelescopeDriver& driver, bool paused);

/// Test seam (decision 0010, #2491): one full poll tick on the calling thread, paused or not, with the effects it
/// applies. A driver not from the factories is ignored.
void poll_state_now_for_testing(TelescopeDriver& driver);

/// Test seam: `hook` runs on the clock-sync body after its start delay (it may block or throw).
/// Set it before connecting. A driver not from the factories above is ignored.
void set_clock_sync_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam: starts the clock-sync body again, as a connect that released the driver mutex late does.
/// Call with no driver lock held. A driver not from the factories above is ignored.
void start_clock_sync_for_testing(TelescopeDriver& driver);

/// Test hook: called with false before each ":SUT" send attempt of the clock sync, true once its sends are done.
void set_clock_sync_utc_hook_for_testing(TelescopeDriver& driver, std::function<void(bool)> hook);

/// Test seam: the host UTC the clock sync writes with :SUT, and set_utc_date() compares a client's time to
/// (issue #2419). Null restores system_clock. Set it before connecting. A driver not from the factories is ignored.
void set_host_utc_source_for_testing(TelescopeDriver& driver,
                                     std::function<std::chrono::system_clock::time_point()> source);

/// Test seam (issue #2432): `hook` runs on a host clock re-sync's body once its wait found no driver motion,
/// before the body reads the mount clock, with no driver lock held. Set it before the re-sync starts. A driver
/// not from the factories is ignored.
void set_clock_sync_resync_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam (issue #2432): `hook` runs at the start of every set_connected(), with transition_mutex_ held and no
/// other driver lock, so a case can hold that call (it may block). Set it while no set_connected() runs. A driver
/// not from the factories is ignored.
void set_connected_entry_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam (issue #2419): `hook` runs when on_host_clock_changed() has stored a change as pending and found
/// transition_mutex_ held, before it returns, with no driver lock held (it may block). Set it while no host clock
/// change is delivered. A driver not from the factories is ignored.
void set_host_clock_busy_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam (issue #2419): `hook` runs as the last step of every set_connected(), with transition_mutex_ still held
/// and no other driver lock (it may block). Set it while no set_connected() runs. A driver not from the factories is
/// ignored.
void set_connected_exit_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam (issue #2419): `hook` runs before each attempt of the clock sync's UTC write (:SUT), before that write's
/// exchange takes the operation lock, with no driver lock held (it may start a GOTO). Set it while no clock-sync body
/// runs. A driver not from the factories is ignored.
void set_clock_sync_before_utc_exchange_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

/// Test seam: the kept failure of the clock-sync slot, until its next start.
std::optional<std::string> clock_sync_failure_for_testing(const TelescopeDriver& driver);

} // namespace alpacacore::vendor::ioptron
