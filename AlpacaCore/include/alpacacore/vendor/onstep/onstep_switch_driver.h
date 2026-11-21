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

#include <alpacacore/switch_driver.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>

#include <memory>
#include <optional>

namespace alpacacore::vendor::onstep {

// The OnStepX auxiliary features (#2222) as one Switch device, over the controller's shared link: the same serial
// port or TCP channel as the telescope, joined through OnStepProtocolWrapper::acquire(). `clock` drives the state poll
// and its fault and loss timing (decision 0005); tests inject a FakeTaskClock.
std::unique_ptr<SwitchDriver> create_onstep_switch(int device_number, const ConnectionInfo& connection_info,
                                                   util::TaskClock& clock = util::default_task_clock());

/// Endpoint resolved at connect time (#659): the auto-detect factory below wraps it, tests inject a fake's endpoint.
std::unique_ptr<SwitchDriver> create_onstep_switch_deferred(int device_number,
                                                            util::ConnectionResolver<ConnectionInfo> resolver,
                                                            util::TaskClock& clock = util::default_task_clock());

// Auto-detect: the telescope's serial scan, at connect time, with the same mount index, so the switch finds the
// controller the telescope uses (a port a connected OnStep device holds keeps its place in the scan).
std::unique_ptr<SwitchDriver> create_onstep_switch_auto(int device_number, int mount_index);

/// Test seams of the state poll. `driver` must come from the factories above; anything else is ignored.
/// While paused the poll waits its ticks but sends nothing.
void set_onstep_switch_poll_paused_for_testing(SwitchDriver& driver, bool paused);
/// One poll cycle on the calling thread.
void poll_onstep_switch_now_for_testing(SwitchDriver& driver);

namespace detail {
/// How a Switch serves an OnStepX auxiliary feature purpose code (Constants.h at af1d26bf): 1 switch and 3 dew heater
/// (its enable) as 0/1, 2 analog output as 0-255, all in steps of 1. std::nullopt for a purpose the Switch leaves
/// out: 4 intervalometer, 6 hidden switch, and any code it does not know.
struct AuxSwitchKind {
    double max = 1.0;
    const char* description = "";
};
std::optional<AuxSwitchKind> aux_switch_kind(int purpose);
}  // namespace detail

}  // namespace alpacacore::vendor::onstep
