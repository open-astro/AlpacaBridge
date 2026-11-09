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

// Shared link-health helpers for the WandererAstro streamer tests (box switch,
// filter wheel, cover calibrator): one copy instead of three (#2088).

#pragma once

#include <sys/ioctl.h>

#include <chrono>
#include <thread>

#include "fake_serial_streamer.h"
#include "fake_task_clock.h"

namespace alpacacore::test::wanderer {

// Moves the fake clock by @p d, then waits (bounded, real time) for the
// reader thread's next pass: its silence check is the one clock read a muted
// or severed link makes per pass, taken under the lock the driver's getters
// also take, so a getter called after this returns sees that pass's verdict.
// The box reader's pass costs about 1 s, so the bound leaves room under sanitizers.
inline bool advance_one_pass(FakeTaskClock& clock, std::chrono::nanoseconds d,
                             std::chrono::milliseconds bound = std::chrono::milliseconds(5000)) {
    clock.advance(d);
    return clock.wait_for_now_calls(clock.now_calls() + 1, bound);
}

// After set_muted(true): the streamer may have committed one more frame. Wait
// until it is quiet, the reader has drained the tty input queue, and the reader
// has made one more pass, so that last frame was taken at the current virtual
// time and cannot restart the silence window after the clock moves.
inline bool drain_after_mute(const FakeSerialStreamer& streamer, FakeTaskClock& clock,
                             std::chrono::milliseconds bound = std::chrono::milliseconds(5000)) {
    if (!streamer.wait_for_quiet(bound)) return false;
    const auto deadline = std::chrono::steady_clock::now() + bound;
    int queued = 1;
    while (ioctl(streamer.keepalive_fd(), FIONREAD, &queued) == 0 && queued > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return clock.wait_for_now_calls(clock.now_calls() + 1, bound);
}

// Waits (bounded, real time) until @p streamer has written @p n more frames.
inline bool wait_for_frames(const FakeSerialStreamer& streamer, long long n,
                            std::chrono::milliseconds bound = std::chrono::milliseconds(5000)) {
    const long long target = streamer.frames_sent() + n;
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (streamer.frames_sent() < target) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace alpacacore::test::wanderer
