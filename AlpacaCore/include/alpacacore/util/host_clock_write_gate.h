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

#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace alpacacore::util {

/**
 * The connect-time decision for a mount that keeps its own clock (open-astro#2480): may the host clock go into
 * the mount now. A driver holds one as a member (decision 0013 rule 5: composed, not a base class); the wire write
 * and the lock choreography around it stay in the driver (decision 0013 rule 2).
 *
 * It owns two things the five adopters (iOptron, ZWO AM, SynScan, OnStep, Celestron NexStar) each carried:
 *
 * - The untrusted-connect warning. may_write() asks HostClockTrust::trusted(); untrusted, it logs the one text
 *   every adopter uses, at WARN for the first decision since begin_connect() and at DEBUG after (a redundant
 *   Connected=true, a second connect pass).
 * - The owed write: a host clock change TelescopeDriver::on_host_clock_changed() could not act on yet. owe() records
 *   it before the hook tries the driver's transition lock; whoever holds that lock takes it with take_owed() after
 *   letting go. Everything here is lock-free, so the timer-thread hook never waits on it.
 */
class HostClockWriteGate {
public:
    explicit HostClockWriteGate(std::string_view component) : component_(component) {}

    HostClockWriteGate(const HostClockWriteGate&) = delete;
    HostClockWriteGate& operator=(const HostClockWriteGate&) = delete;

    /// A new connection: the next may_write() that finds the clock untrusted logs at WARN.
    void begin_connect() noexcept { warn_armed_.store(true); }

    /// True when the host clock may go into the mount now (HostClockTrust::trusted()). False logs why, naming
    /// `mount_clock` (the mount's own time) when it is not empty. The first call since begin_connect() logs at WARN,
    /// later ones at DEBUG, trusted or not.
    bool may_write(std::string_view mount_clock = {}) {
        const bool loud = warn_armed_.exchange(false);
        if (HostClockTrust::trusted()) return true;
        std::string text =
            "Host clock is not trusted (not NTP-synchronised, not set or confirmed by a client); leaving the mount "
            "clock";
        if (!mount_clock.empty()) text += " (" + std::string(mount_clock) + ")";
        text += " alone until the host clock is set or a client writes UTCDate";
        if (loud) {
            ALPACA_LOG_WARN(component_, text);
        } else {
            ALPACA_LOG_DEBUG(component_, text);
        }
        return false;
    }

    /// The timer thread: records `event` and marks a write owed. Call before trying the driver's lock.
    void owe(const HostClockEvent& event) noexcept {
        stepped_.store(event.kind == HostClockEvent::Kind::Stepped);
        step_ms_.store(static_cast<std::int64_t>(event.step.count()));
        owed_.store(true);
    }

    /// True while a change is owed and nobody has taken it.
    bool owed() const noexcept { return owed_.load(); }

    /// Takes the owed write: true for exactly one caller per owe().
    bool take_owed() noexcept { return owed_.exchange(false); }

    /// Forgets an owed write (a connect that decides on the clock itself, a disconnect).
    void drop_owed() noexcept { owed_.store(false); }

    /// The last change owe() recorded, as a log reason.
    std::string owed_why() const {
        return describe(
            HostClockEvent{stepped_.load() ? HostClockEvent::Kind::Stepped : HostClockEvent::Kind::BecameTrusted,
                           std::chrono::milliseconds(step_ms_.load())});
    }

    /// A host clock change as a log reason: "Host clock stepped by N ms" or "Host clock became trusted".
    static std::string describe(const HostClockEvent& event) {
        if (event.kind == HostClockEvent::Kind::Stepped) {
            return "Host clock stepped by " + std::to_string(event.step.count()) + " ms";
        }
        return "Host clock became trusted";
    }

private:
    const std::string component_;
    std::atomic<bool> warn_armed_{true};
    std::atomic<bool> owed_{false};
    std::atomic<bool> stepped_{false};
    std::atomic<std::int64_t> step_ms_{0};
};

}  // namespace alpacacore::util
