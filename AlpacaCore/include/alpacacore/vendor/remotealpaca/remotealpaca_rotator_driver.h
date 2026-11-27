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

// The remotealpaca Rotator proxied device (decision 0023, issue #1753 slice 5, #2708): the Rotator device of an
// upstream Alpaca server, served as a device of this server with this server's own device number and UniqueID. The
// configuration, self-loop, Connect, snapshot, link-loss and pass-through rules are the ObservingConditions device's
// (remotealpaca_observingconditions_driver.h). IsMoving, Position, MechanicalPosition, TargetPosition and Reverse are
// served from the polled frame; Move, MoveAbsolute, MoveMechanical, Sync, Halt and the Reverse write are forwarded in
// the command slot, and Halt is forwarded even while the link is faulted.

#include <alpacacore/rotator_driver.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_device_config.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>

#include <functional>
#include <memory>

namespace alpacacore::vendor::remotealpaca {

// Throws AlpacaException(InvalidValue) for a malformed host, a port outside 1 to 65535, a negative upstream device
// number or a poll period outside 1 to 60 seconds. Touches no network. `self` answers this server's listening
// addresses at each Connect (decision 0023 rule 4); empty means no check.
std::unique_ptr<RotatorDriver> create_remotealpaca_rotator(int device_number, const RemoteDeviceConfig& config,
                                                           ServerAddressSource self = {});

/**
 * @brief As above, calling the upstream through `http` and polling on `clock` (test seams, decisions 0015 and 0005).
 *
 * The production overload passes util::make_curl_http_client() and the real task clock. `http` is called from the
 * connect thread, the poll and the command bodies, one call at a time per driver; the clock must outlive the driver.
 */
std::unique_ptr<RotatorDriver> create_remotealpaca_rotator(int device_number, const RemoteDeviceConfig& config,
                                                           std::shared_ptr<util::HttpClient> http,
                                                           ServerAddressSource self, util::TaskClock& clock);

// Test seam: while paused the poll keeps its period and samples nothing, so the frame ages without a failed poll
// (the contract sweep's stale-frame case). No effect on a driver of another type.
void set_poll_paused_for_testing(RotatorDriver& driver, bool paused);

// Runs `hook` first inside each command body (the moves, Sync, Halt and the Reverse write), before its PUT, so a test
// can hold a body before anything reaches the upstream. Set it while no command runs.
void set_command_hook_for_testing(RotatorDriver& driver, std::function<void()> hook);

// Runs `hook` in a command's caller that finds an earlier command still running, before it waits for it (commands
// reach the upstream in caller order), so a test knows the caller is waiting. Set it while no command runs.
void set_command_waiting_hook_for_testing(RotatorDriver& driver, std::function<void()> hook);

}  // namespace alpacacore::vendor::remotealpaca
