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

#include <alpacacore/camera_driver.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/task_clock.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace alpacacore::util {

// INTERFACE STUB (issue #1605): declarations only, so that
// tests/test_capture_session.cpp compiles and fails. No member does what its
// comment says yet; the implementation replaces every body below.

/**
 * Handed to a camera's exposure body; valid only inside that body. The body
 * owns the SDK calls (start, the readiness wait, the download); the session
 * owns the state, the time and the failure.
 */
class CaptureControl {
public:
    CaptureControl(const CaptureControl&) = delete;
    CaptureControl& operator=(const CaptureControl&) = delete;

    /// Waits `d` on the session's TaskClock. True when the full time passed;
    /// false when the body must stop (abort, disconnect, the watchdog).
    bool wait_for(std::chrono::nanoseconds /*d*/) { return true; }
    StopReason stop_reason() const { return StopReason::None; }
    /// The SDK exposure has begun: stamps LastExposureStartTime (a
    /// system_clock value) and the PercentCompleted base (the TaskClock).
    void exposure_started() {}
    /// The frame is being downloaded: CameraState Download.
    void reading_out() {}
    /// Publishes the frame. False, and the frame dropped, when this body no
    /// longer owns the session (aborted, timed out, disconnected).
    // NOLINTNEXTLINE(performance-unnecessary-value-param): stub; the implementation moves it.
    bool image_ready(std::shared_ptr<const ImageArray> /*image*/) { return true; }

private:
    friend class CaptureSession;
    CaptureControl() = default;
};

/**
 * One camera's exposure: its state, timestamps, watchdog and kept failure,
 * with the vendor body run in one util::AsyncOperation slot (decision 0006)
 * on the injected TaskClock (decision 0005).
 */
class CaptureSession {
public:
    using Body = std::function<void(CaptureControl&)>;
    /// The longest start() waits for the previous body to return.
    static constexpr std::chrono::milliseconds kStartTimeout{2000};

    // NOLINTNEXTLINE(performance-unnecessary-value-param): stub; the implementation moves it.
    CaptureSession(std::string /*name*/, TaskClock& /*clock*/, std::chrono::nanoseconds /*watchdog_margin*/) {}
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    /// Starts `body` for an exposure of `duration_seconds`. Throws
    /// InvalidOperation while exposing, or when the previous body has not
    /// returned within kStartTimeout; a refused start changes nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): stub; the implementation moves it.
    void start(double /*duration_seconds*/, Body /*body*/) {}
    /// Cancels the exposure in flight and joins its body; the frame is
    /// discarded. A no-op when nothing is exposing.
    void abort() {}
    /// Cancels and joins every body, then clears the image, the failure and
    /// the last exposure.
    void disconnect_and_join() {}
    /// Drops a ready frame (a geometry change); keeps the last exposure.
    void discard_image() {}
    /// The driver fails the exposure in flight (an SDK stop that failed):
    /// kept like a body's throw, and the body is cancelled. A no-op when
    /// nothing is exposing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): stub; the implementation moves it.
    void fail(std::string /*message*/) {}

    /// Applies the watchdog: an exposure past start + duration + margin fails.
    CameraState camera_state() const { return CameraState::Idle; }
    /// True while a body has not returned: the exposure settings stay locked.
    bool busy() const { return false; }
    /// Raises the kept failure as DriverException.
    bool image_ready() const { return false; }
    /// The frame; InvalidOperation before one is ready, DriverException after a failure.
    std::shared_ptr<const ImageArray> image() const { return nullptr; }
    double percent_completed() const { return 0.0; }
    /// InvalidOperation until an image has been acquired (util::LastExposure).
    double last_exposure_duration() const { return 0.0; }
    std::chrono::system_clock::time_point last_exposure_start_time() const { return {}; }
    std::optional<std::string> failure() const { return std::nullopt; }
};

}  // namespace alpacacore::util
