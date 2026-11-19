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

// The remotealpaca ObservingConditions proxied device (decision 0023, issue #1753 slice 3): one ObservingConditions
// device of an upstream Alpaca server, served as a device of this server with this server's own device number and
// UniqueID. The getters answer from a polled snapshot of the upstream (decision 0010) and never wait on it (decision
// 0016); the poll's link health follows decision 0009 through util::PolledLinkHealth, and a refused connection is a
// lost link at once. The upstream's ErrorNumber and ErrorMessage pass through unchanged, after its host (decision 0023
// rule 5); an upstream that is this server is refused at configure and at Connect (rule 4).

#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace alpacacore::vendor::remotealpaca {

// The poll period: 1 to 60 seconds, 2 by default (#1753 maintainer decision, 2026-11-16).
inline constexpr std::chrono::seconds kDefaultPollPeriod{2};
inline constexpr std::chrono::seconds kMinPollPeriod{1};
inline constexpr std::chrono::seconds kMaxPollPeriod{60};

struct RemoteObservingConditionsConfig {
    std::string host;                // the upstream server: a name or an IP address, no scheme, no brackets
    int port = 0;                    // 1 to 65535
    int upstream_device_number = 0;  // the upstream's own ObservingConditions device number
    std::chrono::seconds poll_period{kDefaultPollPeriod};
};

// Throws AlpacaException(InvalidValue) for a malformed host, a port outside 1 to 65535, a negative upstream device
// number or a poll period outside 1 to 60 seconds. Touches no network. `self` answers this server's listening
// addresses at each Connect (decision 0023 rule 4); empty means no check.
std::unique_ptr<ObservingConditionsDriver> create_remotealpaca_observingconditions(
    int device_number, const RemoteObservingConditionsConfig& config, ServerAddressSource self = {});

/**
 * @brief As above, calling the upstream through `http` and polling on `clock` (test seams, decisions 0015 and 0005).
 *
 * The production overload passes util::make_curl_http_client() and the real task clock. `http` is called from the
 * connect thread, the poll and the command bodies, one call at a time per driver; the clock must outlive the driver.
 */
std::unique_ptr<ObservingConditionsDriver> create_remotealpaca_observingconditions(
    int device_number, const RemoteObservingConditionsConfig& config, std::shared_ptr<util::HttpClient> http,
    ServerAddressSource self, util::TaskClock& clock);

// Test seam: while paused the poll keeps its period and samples nothing, so the frame ages without a failed poll
// (the contract sweep's stale-frame case). No effect on a driver of another type.
void set_poll_paused_for_testing(ObservingConditionsDriver& driver, bool paused);

// Runs `hook` inside each AveragePeriod body, after its PUT and before its write-through, so a test can hold a body
// there. Set it while no command runs.
void set_command_hook_for_testing(ObservingConditionsDriver& driver, std::function<void()> hook);

}  // namespace alpacacore::vendor::remotealpaca
