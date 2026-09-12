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
//     fail with EIO from then on (USB re-enumeration / unplug).

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeSerialStreamer {
public:
    FakeSerialStreamer(std::string frame, std::chrono::milliseconds interval)
        : interval_ms_(static_cast<int>(interval.count())), frame_(std::move(frame)) {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        // Issue #424: the master goes non-blocking here, so a reply to a
        // driver that has stopped draining can never park this fake's
        // worker inside write() and hang the destructor's join. Folded
        // into the same throw as the other setup failures: a silent
        // fallback to a blocking master would look exactly like the hang
        // this exists to remove. See fake_pty_write.h.
        if (master_fd_ < 0 || grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0 ||
            !make_pty_nonblocking(master_fd_)) {
            throw std::runtime_error("FakeSerialStreamer: cannot open pty");
        }
        const char* name = ptsname(master_fd_);
        if (name == nullptr) {
            throw std::runtime_error("FakeSerialStreamer: ptsname failed");
        }
        slave_path_ = name;
        keepalive_fd_ = open(slave_path_.c_str(), O_RDWR | O_NOCTTY);
        struct termios tty {};
        if (keepalive_fd_ >= 0 && tcgetattr(keepalive_fd_, &tty) == 0) {
            cfmakeraw(&tty);
            tcsetattr(keepalive_fd_, TCSANOW, &tty);
        }
        worker_ = std::thread([this] { run(); });
    }

    ~FakeSerialStreamer() { sever_link(); }

    FakeSerialStreamer(const FakeSerialStreamer&) = delete;
    FakeSerialStreamer& operator=(const FakeSerialStreamer&) = delete;

    const std::string& slave_path() const { return slave_path_; }

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
        if (keepalive_fd_ >= 0) {
            close(keepalive_fd_);
            keepalive_fd_ = -1;
        }
        if (master_fd_ >= 0) {
            close(master_fd_);
            master_fd_ = -1;
        }
    }

private:
    void run() {
        std::string pending;
        char buf[64];
        auto last_stream = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = master_fd_;
            pfd.events = POLLIN;
            const int r = poll(&pfd, 1, 10);
            if (r > 0) {
                const ssize_t n = read(master_fd_, buf, sizeof(buf));
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
            if (!muted_.load() && now - last_stream >= std::chrono::milliseconds(interval_ms_)) {
                last_stream = now;
                std::string frame;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    frame = frame_;
                }
                pty_write_bounded(master_fd_, frame, stop_);
            }
        }
    }

    int master_fd_ = -1;
    int keepalive_fd_ = -1;
    std::string slave_path_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> muted_{false};
    int interval_ms_;

    mutable std::mutex mutex_;
    std::string frame_;
    std::vector<std::string> commands_;
};

}  // namespace alpacacore::test
