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

#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/task_clock.h>

#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <string>

namespace alpacacore::util {

/**
 * The asynchronous Park flow shared by the Celestron and SynScan telescope
 * drivers (AGENTS.md, "Telescope Park / MoveAxis(axis, 0) are asynchronous
 * initiators"): dispatch the park GOTO, poll the hardware until the slew has
 * ended, wait out the settle time, stop tracking, then publish AtPark and
 * Slewing in one locked step. The driver keeps what differs between mounts
 * (the target, the wire commands, the state it clears) in the hooks.
 *
 * Every hook runs with the driver mutex held. The flow itself releases the
 * mutex around each wait, so the HTTP getters stay responsive.
 */
struct ParkSlewHooks {
    /// True while the park is still wanted: connected and not aborted or
    /// unparked meanwhile. When false the canceller owns the state.
    std::function<bool()> still_parking;
    /// Drops the parking state; run when the body is cancelled.
    std::function<void()> abandon;
    /// Sends the park GOTO. May throw.
    std::function<void()> dispatch;
    /// Reads the hardware's own slewing state.
    std::function<bool()> poll_slewing;
    /// Stops tracking at the park position. May throw.
    std::function<void()> stop_tracking;
    /// A failed step: stop the hardware and drop the parking state.
    std::function<void(const std::string&)> fail;
    /// Publishes AtPark true and Slewing false in the same locked step.
    std::function<void()> complete;
};

/**
 * Body of the slot that parks the mount. `lock` is the driver mutex, held on
 * entry and on return. `settle_seconds` is the settle time after the slew ends.
 */
inline void run_park_slew(OperationContext& ctx, std::unique_lock<std::mutex>& lock, TaskClock& clock,
                          int settle_seconds, const ParkSlewHooks& hooks) {
    if (ctx.stop_reason() != StopReason::None || !hooks.still_parking()) {
        hooks.abandon();
        return;
    }
    try {
        hooks.dispatch();
    } catch (const std::exception& ex) {
        hooks.fail(std::string("Park slew dispatch failed: ") + ex.what());
        return;
    } catch (...) {
        hooks.fail("Park slew dispatch failed with unknown exception");
        return;
    }
    // Poll for completion with the mutex released between polls so the HTTP
    // getters (Slewing, RightAscension, ...) stay responsive.
    const auto timeout = std::chrono::seconds(120);
    const auto start = clock.now();
    const auto start_grace = std::chrono::seconds(2);
    bool saw_slewing = false;
    while (true) {
        lock.unlock();
        const bool keep_going = ctx.wait_for(std::chrono::milliseconds(250));
        lock.lock();
        // Cancelled (disconnect / destruction / newer initiator), aborted, or
        // unparked meanwhile: the canceller owns the state.
        if (!keep_going || !hooks.still_parking()) {
            hooks.abandon();
            return;
        }
        if (hooks.poll_slewing()) {
            saw_slewing = true;
        } else {
            if (!saw_slewing && (clock.now() - start) < start_grace) {
                continue;
            }
            break;
        }
        if (clock.now() - start > timeout) {
            hooks.fail("Park slew timed out after 120s");
            return;
        }
    }
    if (settle_seconds > 0) {
        lock.unlock();
        const bool keep_going = ctx.wait_for(std::chrono::seconds(settle_seconds));
        lock.lock();
        if (!keep_going || !hooks.still_parking()) {
            hooks.abandon();
            return;
        }
    }
    try {
        hooks.stop_tracking();
    } catch (const std::exception& ex) {
        hooks.fail(std::string("Park: stopping tracking failed: ") + ex.what());
        return;
    } catch (...) {
        hooks.fail("Park: stopping tracking failed with unknown exception");
        return;
    }
    hooks.complete();
}

/**
 * Stops a park slew: cancels the GOTO, then both axes to rate 0. Each stop is
 * tried on its own so one failure does not skip the others. Returns the first
 * failure's message, or an empty string when every stop was answered (#742).
 * `Protocol` offers cancel_goto() and move_axis_fixed_rate(axis, rate).
 */
template <typename Protocol>
std::string stop_goto_and_axes(Protocol& protocol) {
    std::string first_error;
    const auto try_stop = [&first_error](auto&& stop) {
        try {
            stop();
        } catch (const std::exception& ex) {
            if (first_error.empty()) {
                first_error = ex.what();
            }
        } catch (...) {
            if (first_error.empty()) {
                first_error = "unknown exception";
            }
        }
    };
    try_stop([&protocol] { protocol.cancel_goto(); });
    try_stop([&protocol] { protocol.move_axis_fixed_rate(0, 0); });
    try_stop([&protocol] { protocol.move_axis_fixed_rate(1, 0); });
    return first_error;
}

}  // namespace alpacacore::util
