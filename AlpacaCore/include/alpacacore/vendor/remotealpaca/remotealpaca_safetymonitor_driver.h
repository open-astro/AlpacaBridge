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

// The remotealpaca SafetyMonitor proxied device (decision 0023, issue #1753 slice 4): the SafetyMonitor device of an
// upstream Alpaca server, served as a device of this server with this server's own device number and UniqueID. The
// configuration, self-loop, Connect, snapshot, link-loss and pass-through rules are the ObservingConditions device's
// (remotealpaca_observingconditions_driver.h). One rule is its own: while the link is lost, IsSafe reads false and
// does not throw (#1753 maintainer decision, 2026-11-16), so a dead link never reads safe.

#include <alpacacore/safetymonitor_driver.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_device_config.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>

#include <memory>

namespace alpacacore::vendor::remotealpaca {

// Throws AlpacaException(InvalidValue) for a malformed host, a port outside 1 to 65535, a negative upstream device
// number or a poll period outside 1 to 60 seconds. Touches no network. `self` answers this server's listening
// addresses at each Connect (decision 0023 rule 4); empty means no check.
std::unique_ptr<SafetyMonitorDriver> create_remotealpaca_safetymonitor(int device_number,
                                                                       const RemoteDeviceConfig& config,
                                                                       ServerAddressSource self = {});

/**
 * @brief As above, calling the upstream through `http` and polling on `clock` (test seams, decisions 0015 and 0005).
 *
 * The production overload passes util::make_curl_http_client() and the real task clock. `http` is called from the
 * connect thread and the poll, one call at a time per driver; the clock must outlive the driver.
 */
std::unique_ptr<SafetyMonitorDriver> create_remotealpaca_safetymonitor(int device_number,
                                                                       const RemoteDeviceConfig& config,
                                                                       std::shared_ptr<util::HttpClient> http,
                                                                       ServerAddressSource self,
                                                                       util::TaskClock& clock);

// Test seam: while paused the poll keeps its period and samples nothing, so the frame ages without a failed poll
// (the contract sweep's stale-frame case). No effect on a driver of another type.
void set_poll_paused_for_testing(SafetyMonitorDriver& driver, bool paused);

}  // namespace alpacacore::vendor::remotealpaca
