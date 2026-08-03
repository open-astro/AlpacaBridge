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

#include <alpacacore/video/video_capture.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace alpacacore::video::testing {

// Deterministic hardware-free frame source for §77.1 bench tests: every
// frame is filled with its 0-based index (mod 251, a prime, so wrap
// boundaries are detectable) and delivery can be throttled to a fixed
// per-frame delay to simulate camera pacing. Thread-safe for the
// one-producer use the recorder makes of it.
class SyntheticVideoCapture final : public VideoCapture {
public:
    // frame_limit 0 = unlimited; delay is per get_frame call.
    SyntheticVideoCapture(std::uint64_t frame_limit, std::chrono::microseconds delay)
        : frame_limit_(frame_limit), delay_(delay) {}

    void start(const VideoRequest& request) override {
        started_ = true;
        frame_size_ = frame_bytes(request);
        produced_ = 0;
    }

    bool get_frame(std::uint8_t* buffer, std::size_t size, int timeout_ms) override {
        (void)timeout_ms;
        if (!started_ || size < frame_size_) {
            return false;
        }
        const std::uint64_t index = produced_.load();
        if (frame_limit_ != 0 && index >= frame_limit_) {
            // Source exhausted: behave like a camera timeout.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return false;
        }
        if (delay_.count() > 0) {
            std::this_thread::sleep_for(delay_);
        }
        std::memset(buffer, static_cast<int>(index % 251), frame_size_);
        produced_.fetch_add(1);
        return true;
    }

    void stop() override { started_ = false; }

    std::uint64_t dropped_frames() override { return sdk_dropped_; }

    void set_sdk_dropped(std::uint64_t value) { sdk_dropped_ = value; }
    std::uint64_t produced() const { return produced_.load(); }

private:
    std::uint64_t frame_limit_;
    std::chrono::microseconds delay_;
    std::size_t frame_size_{};
    std::atomic<std::uint64_t> produced_{0};
    std::atomic<bool> started_{false};
    std::uint64_t sdk_dropped_{0};
};

}  // namespace alpacacore::video::testing
