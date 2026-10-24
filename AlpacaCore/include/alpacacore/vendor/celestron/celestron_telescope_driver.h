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
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

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

/// Test seam: replaces the thread factory of the driver's slew slot, so a case
/// can make a body's start fail. Call only while no start is in flight.
/// `driver` must come from the factories above; anything else is ignored.
void set_slew_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn);

/// Same for both pulse-guide slots (one per axis), so a case can make a chain start fail.
void set_pulse_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn);

/// Test seams for the status poll (decision 0010). `driver` must come from the factories above; anything else is
/// ignored (poll_status_now_for_testing returns false).
/// Pauses (true) or resumes the poll body's own ticks; a direct tick below still runs.
void set_status_poll_paused_for_testing(TelescopeDriver& driver, bool paused);
/// One poll tick on the calling thread (a full frame). False when the connection is gone or lost.
bool poll_status_now_for_testing(TelescopeDriver& driver);
/// Runs between the poll's sample and its publish, without any driver lock held; null clears it.
void set_status_poll_after_sample_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// Runs after a slewing read (the slew-complete wait, the park poll), before its result is applied, without any
/// driver lock held; null clears it.
void set_slewing_read_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// Runs when the poll or the slew-complete wait has found a failure, before its stops are sent, without any driver
/// lock held; null clears it.
void set_failure_stop_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// What a slew-slot body passes its test hook: reads the body's own stop reason (None, Cancelled or Superseded).
using SlewBodyStopReason = std::function<util::StopReason()>;
using SlewBodyHook = std::function<void(const SlewBodyStopReason&)>;
/// Runs in the async slew body and in the Park body right after the GOTO exchange returned, with mutex_ released and
/// before the body takes it again; a hook that blocks holds the body there. Null clears it.
void set_slew_body_hook_for_testing(TelescopeDriver& driver, SlewBodyHook hook);
/// Runs in the park's and the blocking slew's dispatch, after the flip decision and before the GOTO, with mutex_
/// released (a blocking slew still holds its initiator lock); null clears it.
void set_slew_dispatch_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);

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
