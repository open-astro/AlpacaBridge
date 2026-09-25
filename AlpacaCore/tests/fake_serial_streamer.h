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

// Pty-backed fake for devices that STREAM a status frame unprompted and take
// fire-and-forget commands (the WandererAstro cover / filter wheel / box
// family). The test supplies the frame bytes; the fake repeats them at a fixed
// interval and records every '\n'-terminated command line it receives. Two
// failure modes for the issue #237 link-health tests:
//   - set_muted(true): the stream stops, the fd stays healthy (hung MCU);
//   - sever_link(): the pty master closes, so the driver's reads and writes
//     fail with EIO from then on (USB re-enumeration / unplug), and
//     slave_path() is empty afterwards: the path named a pty that no longer
//     exists, so copy it before the cut if the test still needs it.

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeSerialStreamer {
public:
    /// @p first_frame_hold is a start-up window, not a one-shot: no frame is sent until that long after the worker
    /// started, and nothing is consumed (later frames stream as normal). A connect that waits for the first streamed
    /// frame stays open until then. It is a constructor argument because the worker starts streaming inside the
    /// constructor: a setter called afterwards races the first frames. Used by the contract sweep.
    FakeSerialStreamer(std::string frame, std::chrono::milliseconds interval,
                       std::chrono::milliseconds first_frame_hold = std::chrono::milliseconds(0))
        : pty_("FakeSerialStreamer"),
          first_frame_hold_ms_(static_cast<int>(first_frame_hold.count())),
          interval_ms_(static_cast<int>(interval.count())),
          frame_(std::move(frame)) {
        // The pty pair is owned by pty_ (fake_pty_write.h), constructed
        // before this body runs; a setup failure throws from there with
        // nothing left open (issue #387).
        worker_ = std::thread([this] { run(); });
    }

    ~FakeSerialStreamer() { sever_link(); }

    FakeSerialStreamer(const FakeSerialStreamer&) = delete;
    FakeSerialStreamer& operator=(const FakeSerialStreamer&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    void set_frame(std::string frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_ = std::move(frame);
    }
    void set_muted(bool muted) { muted_.store(muted); }
    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }
    bool received(const std::string& exact) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& c : commands_) {
            if (c == exact) return true;
        }
        return false;
    }

    // Close the master side underneath the driver (idempotent).
    void sever_link() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        pty_.sever();
    }

private:
    void run() {
        std::string pending;
        char buf[64];
        const auto started = std::chrono::steady_clock::now();
        auto last_stream = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            const int r = poll(&pfd, 1, 10);
            if (r > 0) {
                const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
                for (ssize_t i = 0; i < n; ++i) {
                    const char ch = buf[i];
                    if (ch == '\n' || ch == '\r') {
                        if (!pending.empty()) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            commands_.push_back(pending);
                        }
                        pending.clear();
                    } else {
                        pending += ch;
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (!muted_.load() && now - started >= std::chrono::milliseconds(first_frame_hold_ms_) &&
                now - last_stream >= std::chrono::milliseconds(interval_ms_)) {
                last_stream = now;
                std::string frame;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    frame = frame_;
                }
                pty_write_bounded(pty_.master_fd(), frame, stop_);
            }
        }
    }

    // First member: constructed before the worker, destroyed after it has
    // been joined.
    PtyPair pty_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> muted_{false};
    const int first_frame_hold_ms_;
    int interval_ms_;

    mutable std::mutex mutex_;
    std::string frame_;
    std::vector<std::string> commands_;
};

}  // namespace alpacacore::test
