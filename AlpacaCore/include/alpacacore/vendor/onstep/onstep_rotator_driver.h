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

#include <alpacacore/rotator_driver.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>

#include <memory>
#include <optional>

namespace alpacacore::vendor::onstep {

// The OnStepX rotator (#2221), over the controller's shared link: the same serial port or TCP channel as the
// telescope and its focusers, joined through OnStepProtocolWrapper::acquire(). `clock` drives the state poll and its
// fault and loss timing (decision 0005); tests inject a FakeTaskClock.
std::unique_ptr<RotatorDriver> create_onstep_rotator(int device_number, const ConnectionInfo& connection_info,
                                                     util::TaskClock& clock = util::default_task_clock());

/// Endpoint resolved at connect time (#659): the auto-detect factory below wraps it, tests inject a fake's endpoint.
std::unique_ptr<RotatorDriver> create_onstep_rotator_deferred(int device_number,
                                                              util::ConnectionResolver<ConnectionInfo> resolver,
                                                              util::TaskClock& clock = util::default_task_clock());

// Auto-detect: the telescope's serial scan, at connect time, with the same mount index, so the rotator finds the
// controller the telescope uses (a port a connected OnStep device holds keeps its place in the scan).
std::unique_ptr<RotatorDriver> create_onstep_rotator_auto(int device_number, int mount_index);

/// Test seams of the state poll. `driver` must come from the factories above; anything else is ignored.
/// While paused the poll waits its ticks but sends nothing.
void set_onstep_rotator_poll_paused_for_testing(RotatorDriver& driver, bool paused);
/// One poll cycle on the calling thread.
void poll_onstep_rotator_now_for_testing(RotatorDriver& driver);

namespace detail {
/// The firmware angle a move to `mechanical` (0-360) goes to: the one of mechanical + 360 k inside the travel limits
/// [min, max] (OnStepX :rI#/:rM#) nearest `current`, so the rotator never leaves its travel and takes the shorter way
/// when the travel holds two. std::nullopt when no such angle is inside the limits.
std::optional<double> rotator_angle_within_travel(double mechanical, double current, double min, double max);
}  // namespace detail

}  // namespace alpacacore::vendor::onstep
