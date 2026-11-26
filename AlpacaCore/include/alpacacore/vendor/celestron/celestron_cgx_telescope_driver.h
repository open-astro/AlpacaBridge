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
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::celestron {

struct CelestronCgxConnectionInfo {
    std::string port_path;
    /// SkyPortal WiFi: a numeric IPv4 or IPv6 adapter address selects AUX over TCP, and port_path is then unused.
    std::string host;
    int tcp_port = 2000;
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
/// Runs `hook` on the body's thread at the top of every motion body's checked stop, before that stop takes the
/// motion-start lock; a case blocks there to land a newer command between a body's two supersede checks. Pass an
/// empty function to clear it.
void set_stop_worker_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// Runs `hook` on the initiator's thread inside every motion transition's guide handoff (the join of the pulse bodies,
/// or a slew, Park or FindHome's wait on the mount), while the motion-start lock is released and before the new
/// command claims; a case blocks there to let a running body reach a stage. Pass an empty function to clear it.
void set_drain_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// Runs `hook` on the pulse body's thread at the top of every guide poll, before the body reads its stop reason, with
/// no driver lock held; a case blocks there to hold a pulse body while another command runs. Pass an empty function
/// to clear it.
void set_pulse_poll_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// Where set_pulse_pre_lock_hook_for_testing()'s hook runs: before a guide poll's MC_AUX_GUIDE_ACTIVE read, or before
/// the end-of-pulse position read (publish_guide_end_frame()).
enum class CgxPulsePreLockPoint : std::uint8_t { GuidePoll, GuideEndFrame };
/// Runs `hook` on the pulse body's thread just before the body takes the motion-start lock to re-check its stop
/// reason ahead of a guide poll's read or the end-of-pulse read, with no driver lock held; a case blocks there to land
/// a supersede between the body's unlocked check and that read. Pass an empty function to clear it.
void set_pulse_pre_lock_hook_for_testing(TelescopeDriver& driver, std::function<void(CgxPulsePreLockPoint)> hook);
/// Runs `hook` on the initiator's thread just before each MC_AUX_GUIDE_ACTIVE read in a slew, Park or FindHome's wait
/// for a superseded pulse's packet, passed the timeout that read gets (nullopt: the transport's); a case blocks there
/// to model a slow read on the task clock. Pass an empty function to clear it.
void set_guide_end_read_hook_for_testing(TelescopeDriver& driver,
                                         std::function<void(std::optional<std::chrono::milliseconds>)> hook);

}  // namespace alpacacore::vendor::celestron
