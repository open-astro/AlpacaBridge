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

// The ZWO camera runs its exposure in a util::CaptureSession body on its own thread (decision 0020, issue #1981), so
// a case waits for the body's result with a bounded poll instead of reading ImageReady right after StartExposure.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>

#include <chrono>
#include <functional>
#include <future>
#include <thread>

#include "fake_task_clock.h"

namespace alpacacore::test {

/// True once `pred` holds, within `bound` of real time.
inline bool zwo_eventually(const std::function<bool()>& pred,
                           std::chrono::milliseconds bound = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// True when CameraState reads neither Exposing nor Download, or raises (a lost link).
inline bool zwo_exposure_ended(const CameraDriver& camera) {
    try {
        const CameraState state = camera.get_camera_state();
        return state != CameraState::Exposing && state != CameraState::Download;
    } catch (const AlpacaException&) {
        return true;
    }
}

/// Waits, in real time, for the exposure body to end (frame ready, failure kept, or link lost).
inline bool wait_exposure_end(const CameraDriver& camera, std::chrono::milliseconds bound = std::chrono::seconds(5)) {
    return zwo_eventually([&] { return zwo_exposure_ended(camera); }, bound);
}

/// As above on a FakeTaskClock: each time a thread waits on the clock (the status poll, the retry pacing, the USB 2.0
/// settle), advances it by `step` and lets the woken threads settle.
inline bool wait_exposure_end(const CameraDriver& camera, FakeTaskClock& clock,
                              std::chrono::nanoseconds step = std::chrono::milliseconds(50),
                              std::chrono::milliseconds bound = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!zwo_exposure_ended(camera)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (clock.waiter_count() > 0) {
            clock.advance(step);
            static_cast<void>(clock.wait_for_woken_settled(std::chrono::milliseconds(100)));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

/// Runs `fn` on a helper thread and waits up to 2 s of real time for it with the clock held. If it has not returned,
/// advances the clock until it does, so the case can report, and returns false.
inline bool zwo_returns_without_advance(FakeTaskClock& clock, const std::function<void()>& fn) {
    auto work = std::async(std::launch::async, fn);
    if (work.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        work.get();
        return true;
    }
    while (work.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        clock.advance(std::chrono::milliseconds(50));
    }
    work.get();
    return false;
}

}  // namespace alpacacore::test
