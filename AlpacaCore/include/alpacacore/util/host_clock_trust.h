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

#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/host_clock.h>
#include <alpacacore/util/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace alpacacore::util {

/**
 * Whether the host's realtime clock may be written into a mount that keeps
 * its own clock (open-astro#2419).
 *
 * A Raspberry Pi 4 has no RTC: it boots on the time systemd-timesyncd saved
 * at the last shutdown, so a connect that pushes system_clock::now() into the
 * mount (:SUT on iOptron) writes a stale time the mount then points by. The
 * host clock is trusted when the kernel reports it NTP-disciplined (the
 * ClientUtcWarning probe seam, so a test can force either answer), or once
 * this process has seen something set it: a client's UTCDate write that
 * stepped or confirmed it, the web UI's Sync Time, or a realtime step the
 * server's timer observed (an external clock_settime, such as Ara's time
 * push, or NTP's first step). A clock the kernel loaded from a hardware RTC
 * at boot does NOT count: nothing on an NTP-less host verifies the RTC.
 *
 * The latch never clears: a clock that something has set only gets better.
 * It is deliberately separate from HostClock's stepped_ flag, which drives
 * the router's ClockSource readout and connect-time warning. Never call
 * HostClock::mark_stepped() for an observed step: it writes the hardware
 * RTC, and an observed step's time is not verified.
 */
class HostClockTrust {
public:
    /// True when the kernel reports the clock synchronised or the latch is set.
    static bool trusted() { return latched() || ClientUtcWarning::host_synchronized(); }

    /// True when something in this process set or confirmed the clock.
    static bool latched() { return latch().load(std::memory_order_acquire); }

    /// Latch trust. Logs once, naming what set the clock. Never throws.
    static void mark_set(std::string_view why) noexcept {
        if (latch().exchange(true, std::memory_order_acq_rel)) return;
        try {
            ALPACA_LOG_INFO("HostClock", "Host clock is now trusted for mount clock writes (" + std::string(why) + ")");
        } catch (...) {  // NOLINT(bugprone-empty-catch): a log failure must not undo the latch
        }
    }

    /// Test seam: clear the latch (the probe seam is ClientUtcWarning's).
    static void reset_for_testing() { latch().store(false, std::memory_order_release); }

private:
    static std::atomic<bool>& latch() {
        static std::atomic<bool> set{false};
        return set;
    }
};

/**
 * A realtime step seen against the monotonic clock (open-astro#2419). Pure:
 * the caller samples both clocks, so a test feeds it synthetic pairs.
 *
 * The first observation primes it. Each later one returns the realtime
 * elapsed minus the monotonic elapsed since the previous one when
 * HostClock::clock_was_stepped() calls it a step (more than 1 s apart), and
 * nullopt otherwise; NTP's slew (at most 0.5 ms/s) never reaches that
 * between two 1 s samples.
 *
 * steady_clock is CLOCK_MONOTONIC, which stops while the host is suspended,
 * so a resume reads as a step. An SBC does not suspend; on a laptop the step
 * is real enough (the realtime clock moved without this process seeing it)
 * and the only cost is one re-sync.
 */
class HostClockStepDetector {
public:
    std::optional<std::chrono::milliseconds> observe(std::chrono::system_clock::time_point sys,
                                                     std::chrono::steady_clock::time_point mono) {
        if (!primed_) {
            primed_ = true;
            last_sys_ = sys;
            last_mono_ = mono;
            return std::nullopt;
        }
        const auto sys_elapsed = sys - last_sys_;
        const auto mono_elapsed = mono - last_mono_;
        last_sys_ = sys;
        last_mono_ = mono;
        if (!HostClock::clock_was_stepped(sys_elapsed, mono_elapsed)) return std::nullopt;
        return std::chrono::duration_cast<std::chrono::milliseconds>(sys_elapsed) -
               std::chrono::duration_cast<std::chrono::milliseconds>(mono_elapsed);
    }

private:
    bool primed_ = false;
    std::chrono::system_clock::time_point last_sys_{};
    std::chrono::steady_clock::time_point last_mono_{};
};

/// What the server's timer saw happen to the host clock (open-astro#2419),
/// handed to TelescopeDriver::on_host_clock_changed().
/**
 * How a host clock re-sync waits out driver motion (open-astro#2419): it looks
 * again every kHostClockResyncMotionPoll on the driver's task clock, and gives
 * up after kHostClockResyncMotionBound (a MoveAxis held that long, a stuck
 * slewing frame), leaving the mount clock for the next host clock change or a
 * client UTCDate write. One pair for every mount that re-syncs, so they agree.
 */
inline constexpr std::chrono::seconds kHostClockResyncMotionPoll{1};
inline constexpr std::chrono::minutes kHostClockResyncMotionBound{15};

struct HostClockEvent {
    enum class Kind : std::uint8_t {
        Stepped,        // realtime jumped by `step` against monotonic time
        BecameTrusted,  // HostClockTrust::trusted() went from false to true with no step
    };
    Kind kind = Kind::Stepped;
    std::chrono::milliseconds step{0};
};

}  // namespace alpacacore::util
