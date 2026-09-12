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

#include <alpacacore/util/host_clock.h>
#include <alpacacore/util/logging.h>

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace alpacacore::util {

/**
 * The client-clock disagreement warning for mounts that keep their own clock
 * (open-astro#409).
 *
 * Every Alpaca client writes Telescope.UTCDate on connect. For a mount with
 * an onboard clock (OnStep, Celestron, SynScan, iOptron, ZWO AM) the ASCOM
 * setter is specified as setting the MOUNT's time, so the driver pushes the
 * write to the hardware and then aims by that same instant: the mount's own
 * goto and sidereal-time logic now run on the client's clock, and a driver
 * that pointed by anything else would disagree with the mount it commands.
 * That is deliberately NOT the open-astro#301 split the Sky-Watcher direct
 * driver makes, where nothing on the hardware stores time and the host clock
 * is the only other reference.
 *
 * What #301 does share with these drivers is the observation: a client 30
 * minutes out on a host whose own clock is NTP-disciplined is a
 * misconfiguration the driver can see, and the router has already refused
 * to step the host for it (open-astro#289). So the driver says so, once per
 * connection, with the same threshold the router and Sky-Watcher use.
 *
 * disagreement() is the pure rule; warn_once() wires it to the kernel probe
 * and the log. The probe is a process-wide seam so a unit test can run the
 * warning path on an undisciplined CI runner.
 */
class ClientUtcWarning {
public:
    /// The offset to report, or nullopt when the host is undisciplined (the
    /// client's time is then the best the host will see, open-astro#289) or
    /// the delta is within HostClock::kClientDisagreementWarn.
    static std::optional<std::chrono::milliseconds> disagreement(std::chrono::system_clock::duration client_minus_host,
                                                                 bool host_synchronized) {
        if (!host_synchronized) {
            return std::nullopt;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(client_minus_host);
        if (ms > HostClock::kClientDisagreementWarn || ms < -HostClock::kClientDisagreementWarn) {
            return ms;
        }
        return std::nullopt;
    }

    /// Test seam for the kernel discipline probe. Null restores the real one.
    static void set_host_synchronized_probe(std::function<bool()> probe) {
        std::lock_guard<std::mutex> lock(probe_mutex());
        probe_slot() = probe ? std::move(probe) : std::function<bool()>(&HostClock::kernel_is_synchronized);
    }

    static bool host_synchronized() {
        std::function<bool()> probe;
        {
            std::lock_guard<std::mutex> lock(probe_mutex());
            probe = probe_slot();
        }
        return probe();
    }

    /// Log the disagreement at most once per `warned` flag (reset it on
    /// connect so a reconnect re-arms it). The flag is set only when a line
    /// is logged, so a client whose first write agrees and whose later write
    /// does not is still reported. Returns true when it logged.
    ///
    /// `client_minus_host` is the client's instant minus the host's, SAMPLED
    /// BEFORE the mount write: the write is one to three serial round trips
    /// with a multi-second per-command timeout, so sampling afterwards
    /// against a 2 s threshold would report a perfectly set client clock as
    /// seconds out on a mount that was slow to ack (review note on #471).
    static bool warn_once(const std::string& component, std::chrono::system_clock::duration client_minus_host,
                          bool& warned) {
        if (warned) {
            return false;
        }
        const auto offset = disagreement(client_minus_host, host_synchronized());
        if (!offset) {
            return false;
        }
        warned = true;
        ALPACA_LOG_WARN(component, "Client UTCDate disagrees with an NTP-disciplined host clock by " +
                                       std::to_string(offset->count()) +
                                       " ms; the mount's clock and pointing now follow the client (logged once "
                                       "per connection)");
        return true;
    }

    /// Convenience for a caller with nothing to write first: samples now.
    static bool warn_once(const std::string& component, std::chrono::system_clock::time_point client_utc,
                          bool& warned) {
        return warn_once(component, client_utc - std::chrono::system_clock::now(), warned);
    }

private:
    static std::mutex& probe_mutex() {
        static std::mutex m;
        return m;
    }
    static std::function<bool()>& probe_slot() {
        static std::function<bool()> probe = &HostClock::kernel_is_synchronized;
        return probe;
    }
};

}  // namespace alpacacore::util
