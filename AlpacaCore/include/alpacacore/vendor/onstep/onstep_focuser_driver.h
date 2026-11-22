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

#include <alpacacore/focuser_driver.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>

#include <memory>

namespace alpacacore::vendor::onstep {

// One of the up to six focusers an OnStepX controller drives (#2223), over the controller's shared link: the same
// serial port or TCP channel as the telescope, joined through OnStepProtocolWrapper::acquire(). `focuser_number` is
// 1-6 and addresses every command (:F<n>...#). `clock` drives the state poll and its fault and loss timing
// (decision 0005); tests inject a FakeTaskClock.
std::unique_ptr<FocuserDriver> create_onstep_focuser(int device_number, const ConnectionInfo& connection_info,
                                                     int focuser_number,
                                                     util::TaskClock& clock = util::default_task_clock());

/// Endpoint resolved at connect time (#659): the auto-detect factory below wraps it, tests inject a fake's endpoint.
std::unique_ptr<FocuserDriver> create_onstep_focuser_deferred(int device_number,
                                                              util::ConnectionResolver<ConnectionInfo> resolver,
                                                              int focuser_number,
                                                              util::TaskClock& clock = util::default_task_clock());

// Auto-detect: the telescope's serial scan, at connect time, with the same mount index, so the focuser finds the
// controller the telescope uses (a port a connected OnStep device holds keeps its place in the scan).
std::unique_ptr<FocuserDriver> create_onstep_focuser_auto(int device_number, int mount_index, int focuser_number);

/// Test seams of the state poll. `driver` must come from the factories above; anything else is ignored.
/// While paused the poll waits its ticks but sends nothing.
void set_onstep_focuser_poll_paused_for_testing(FocuserDriver& driver, bool paused);
/// One poll cycle on the calling thread.
void poll_onstep_focuser_now_for_testing(FocuserDriver& driver);

}  // namespace alpacacore::vendor::onstep
