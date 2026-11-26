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
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>

#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace alpacacore::vendor::bisque {

namespace detail {
// open-astro#627: the argument checks the coordinate slew/sync forms and
// MoveAxis run before the connection check and any TheSkyX call. Free
// functions so a test can call them without constructing a driver.
// Throws InvalidValue for a non-finite or out-of-range RA (checked first) or Dec.
inline void validate_ra_dec(double ra, double dec, const char* operation = "SlewToCoordinates") {
    util::validate_ra_dec(ra, dec, operation);
}
// Throws InvalidValue for a non-finite MoveAxis rate.
void validate_move_axis_rate(double rate);
}  // namespace detail

std::unique_ptr<TelescopeDriver> create_bisque_telescope(int device_number, const ConnectionInfo& connection_info,
                                                         util::TaskClock& clock = util::default_task_clock());

std::unique_ptr<TelescopeDriver> create_bisque_telescope_with_site(int device_number,
                                                                   const ConnectionInfo& connection_info,
                                                                   std::optional<double> site_latitude_deg,
                                                                   std::optional<double> site_longitude_deg,
                                                                   std::optional<double> site_elevation_m,
                                                                   util::TaskClock& clock = util::default_task_clock());

/// Test seams of the status poll (decision 0010): stop or resume its ticks, and run one tick on the calling thread
/// (paused or not; false when the connection is gone). A pause holds across a reconnect. `driver` must come from the
/// factories above; anything else is ignored.
void set_status_poll_paused_for_testing(TelescopeDriver& driver, bool paused);
bool poll_status_now_for_testing(TelescopeDriver& driver);
/// Runs `hook` on the SlewToCoordinates thread once IsSlewComplete has answered true and the position is published,
/// before the wait applies Slewing false; no driver lock is held. An empty hook removes it.
void set_slew_complete_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// Runs `hook` once a driver stop has been sent (the synchronous deadline, an unconfirmed slew, a motion bound, or
/// AbortSlew's first stop), before the driver takes its state lock again; only the initiator lock is held. An empty
/// hook removes it.
void set_stop_sent_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// Test seam (issue #2651): replaces the thread factory of both MoveAxis cap slots (one per axis), so a case can make
/// a cap body's start fail. Call only while no start is in flight. A driver not from the factories is ignored.
void set_move_axis_cap_spawn_for_testing(TelescopeDriver& driver,
                                         std::function<std::thread(std::function<void()>)> spawn);
/// Test seam (issue #2651): `hook` runs on the thread of a non-zero MoveAxis, with the initiator lock released and the
/// axis's cap body not yet started, so a case can land a second MoveAxis on the axis in that window. Pass an empty
/// function to clear it, and release anything the hook blocks on first.
void set_move_axis_cap_before_start_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook);
/// Whether the 8 s Slewing hold (decision 0022) is open now, on the driver's task clock. A Slewing read cannot tell
/// once the slew's own flag is down, so the clear sites that lower it too (a disconnect, a landing) are pinned here.
bool slew_hold_active_for_testing(TelescopeDriver& driver);
/// Whether the 8 s hold was cleared rather than left to expire. A lost link is declared only once its fault has
/// stood past util::kLinkStalenessBound (30 s), so the window has always expired by then and active() cannot show
/// the lost-link Disconnect's clear; this reads the deadline itself.
bool slew_hold_cleared_for_testing(TelescopeDriver& driver);

} // namespace alpacacore::vendor::bisque
