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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <mutex>
#include <string>

namespace alpacacore::util {

/**
 * The asynchronous Park flow shared by the Celestron and SynScan telescope
 * drivers (ascom-contract.instructions.md, "Telescope Park / MoveAxis(axis, 0) are asynchronous
 * initiators"): dispatch the park GOTO, poll the hardware until the slew has
 * ended, wait out the settle time, stop tracking, check the mount landed at the park
 * target, then publish AtPark and Slewing in one locked step. The driver keeps what differs between mounts
 * (the target, the wire commands, the state it clears) in the hooks.
 *
 * Every hook is entered and left with the driver mutex held. The flow itself
 * releases the mutex around each wait, so the HTTP getters stay responsive. A
 * hook may release the mutex around a blocking call into the hardware if it
 * takes it again before it returns or throws; the driver then supplies
 * `superseded` (below), because a newer operation can take the state meanwhile.
 */
struct ParkSlewHooks {
    /// True while the park is still wanted: connected and not aborted or
    /// unparked meanwhile. When false the canceller owns the state.
    std::function<bool()> still_parking;
    /// Optional; empty means never superseded. True once a newer operation owns
    /// the slot's state (the body is Superseded). The flow then ends with
    /// `abandon()` and runs neither `fail` nor `complete`, which would write
    /// over that operation; it is checked after each hook that may have
    /// released the mutex.
    std::function<bool()> superseded;
    /// Drops the parking state; run when the body is cancelled or superseded
    /// (a driver whose hook sets `superseded` leaves the state alone then).
    std::function<void()> abandon;
    /// Sends the park GOTO. May throw.
    std::function<void()> dispatch;
    /// Reads the hardware's own slewing state.
    std::function<bool()> poll_slewing;
    /// Stops tracking at the park position. May throw.
    std::function<void()> stop_tracking;
    /// A failed step: stop hardware and record failure while this body still
    /// owns the park. If cancelled or superseded, the newer operation owns state.
    std::function<void(const std::string&)> fail;
    /// Optional; empty means no check. Reads where the mount landed and returns an empty string when it is within
    /// tolerance of the park target, else the failure message. Run after the tracking stop on every completion,
    /// because a mount that acked the GOTO and never moved ends the poll like one that arrived. Like the other hooks
    /// it may throw; it checks `superseded` itself when it releases the mutex.
    std::function<std::string()> verify_at_target;
    /// When set, a miss reported by `verify_at_target` repeats the whole flow once (dispatch to verify) before it
    /// fails: a handset that ends a GOTO far off its aim lands on it when the GOTO is sent again (#1105).
    bool repeat_on_miss = false;
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
    if (hooks.superseded && hooks.superseded()) {
        hooks.abandon();
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
        bool still_slewing = false;
        try {
            still_slewing = hooks.poll_slewing();
        } catch (const std::exception& ex) {
            hooks.fail(std::string("Park slew status polling failed: ") + ex.what());
            return;
        } catch (...) {
            hooks.fail("Park slew status polling failed: unknown exception");
            return;
        }
        if (ctx.stop_reason() != StopReason::None || !hooks.still_parking()) {
            hooks.abandon();
            return;
        }
        if (still_slewing) {
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
    if (hooks.superseded && hooks.superseded()) {
        hooks.abandon();
        return;
    }
    if (hooks.verify_at_target) {
        std::string miss;
        // A check that throws fails the park at once: a repeat GOTO would not fix a position that cannot be read.
        std::string check_error;
        try {
            miss = hooks.verify_at_target();
        } catch (const std::exception& ex) {
            check_error = std::string("Park: position check failed: ") + ex.what();
        } catch (...) {
            check_error = "Park: position check failed with unknown exception";
        }
        if (hooks.superseded && hooks.superseded()) {
            hooks.abandon();
            return;
        }
        if (!check_error.empty()) {
            hooks.fail(check_error);
            return;
        }
        if (!miss.empty()) {
            if (hooks.repeat_on_miss) {
                ParkSlewHooks again = hooks;
                again.repeat_on_miss = false;
                run_park_slew(ctx, lock, clock, settle_seconds, again);
                return;
            }
            hooks.fail(miss);
            return;
        }
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
