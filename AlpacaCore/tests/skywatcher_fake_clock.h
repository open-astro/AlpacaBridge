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

// Goto-jump pumping for Sky-Watcher cases whose fake board (the UDP mount or
// the serial one) runs on a FakeTaskClock (open-astro#715; one home since open-astro#1221).
//
// The fake integrates motion against the FakeTaskClock; the driver stays on the
// wall clock, so nothing here waits on the clock and
// FakeTaskClock::wait_for_waiters() has nothing to rendezvous with. The
// rendezvous is the board traffic instead: each step waits for the driver's
// next frame, then decides how virtual time moves:
//   - while an axis runs a goto, jump far enough that it lands at once (a
//     goto's duration is what these suites spent most of their real time on);
//   - otherwise let virtual time follow wall time, so a tracking RA axis and
//     the driver's LST agree (decision 0001: LST is host time, never this
//     clock) and the driver's sampled post-slew rate check sees a sidereal
//     axis.
// A jump never moves a tracking axis: only a goto axis is jumped, and the
// driver restarts tracking only after both axes have landed. Nothing here
// sleeps for a driver timer.
//
// A mount on a FakeTaskClock moves ONLY while one of these helpers runs, so
// every wait in such a case goes through pump_until() or follow_wall_clock(),
// never a plain real-time poll: a pulse or a MoveAxis would otherwise stand
// still while the driver's wall-clock timer runs out. That includes a teardown
// stop on a mount with a stop ramp.
//
// These helpers assume the driver is on the wall clock. A case that also puts
// the driver on the clock steps time after the driver's waiters park on it
// instead (step_until() in test_skywatcher_backlash.cpp, run_clock_until() in
// test_skywatcher_async.cpp).

#include <chrono>
#include <functional>
#include <thread>

#include "fake_skywatcher_board.h"
#include "fake_task_clock.h"

namespace alpacacore::test::skywatcher_clock {

// Under util::kLinkStalenessBound (30 s): a longer jump would age the status
// frame into a lost link before the poll could publish. Three or four jumps
// cover any goto here (180 deg at 800x sidereal = 55 s).
inline constexpr auto kGotoJump = std::chrono::seconds(20);
inline constexpr auto kFrameRendezvous = std::chrono::milliseconds(50);

// Pumps virtual time (goto jumps, otherwise wall time) until pred() holds;
// false when `real_bound` of wall time passes first.
inline bool pump_until(FakeTaskClock& clock, FakeSkyWatcherBoard& mount, const std::function<bool()>& pred,
                       std::chrono::milliseconds real_bound) {
    const auto deadline = std::chrono::steady_clock::now() + real_bound;
    auto last = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        if (mount.axis_in_goto(1) || mount.axis_in_goto(2)) {
            clock.advance(kGotoJump);
        }
        mount.wait_for_frames(0, mount.frames_seen() + 1, kFrameRendezvous);
        const auto t = std::chrono::steady_clock::now();
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(t - last));
        last = t;
    }
    return true;
}

// Lets the fake's virtual time follow wall time for `d`, or until `until()`
// holds (tracking, a pulse, a jog: anything that is not a goto).
inline void follow_wall_clock(FakeTaskClock& clock, std::chrono::milliseconds d,
                              const std::function<bool()>& until = nullptr) {
    const auto start = std::chrono::steady_clock::now();
    auto last = start;
    while (std::chrono::steady_clock::now() - start < d && !(until && until())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto t = std::chrono::steady_clock::now();
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(t - last));
        last = t;
    }
}

// A wait for motion to START: virtual time follows wall time and never jumps,
// so the goto or park the case needs in flight is still in flight afterwards.
// pump_until() would jump a goto that started between two of its looks to its
// target before the case could act on it.
inline bool follow_until(FakeTaskClock& clock, const std::function<bool()>& pred,
                         std::chrono::milliseconds real_bound) {
    follow_wall_clock(clock, real_bound, pred);
    return pred();
}

}  // namespace alpacacore::test::skywatcher_clock
