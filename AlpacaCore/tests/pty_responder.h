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

// A minimal pty "serial device": every chunk read from the master goes
// through a responder, whose reply (if any) is written straight back. Shared
// by the SynScan handset-probe cases and the mount serial-ownership cases
// (issue #1202), which need a tty a wrapper can open, configure and lock, not
// a protocol model. POSIX-only.
//
// The keep-alive slave fd (PtyPair) lets a case read and change the line
// settings the wrapper applied, the way another program would retune the port.

#ifndef _WIN32

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "fake_pty_write.h"

namespace alpacacore::test {

class PtyResponder {
public:
    using Responder = std::function<std::string(const std::string& chunk)>;

    explicit PtyResponder(Responder responder, const char* who = "PtyResponder")
        : pty_(who), responder_(std::move(responder)) {
        // The pty pair is owned by pty_ (fake_pty_write.h), constructed
        // before this body runs; a setup failure throws from there with
        // nothing left open (issue #387).
        reader_ = std::thread([this] { run(); });
    }

    ~PtyResponder() {
        stop_.store(true);
        if (reader_.joinable()) {
            reader_.join();
        }
    }

    PtyResponder(const PtyResponder&) = delete;
    PtyResponder& operator=(const PtyResponder&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }
    int keepalive_fd() const { return pty_.keepalive_fd(); }

    /// Everything the device has received so far.
    std::string received() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

    /// Stop answering (a handset gone quiet); bytes are still recorded.
    void set_silent(bool silent) { silent_.store(silent); }

    /// Writes bytes to the host unprompted (a mount that keeps talking).
    void push(const std::string& bytes) { pty_write_bounded(pty_.master_fd(), bytes, stop_); }

    /// A USB unplug while the device is idle: stops answering and closes both pty ends, so the host's next write
    /// fails with EIO. Not reversible; slave_path() is empty afterwards.
    void unplug() {
        stop_.store(true);
        if (reader_.joinable()) {
            reader_.join();
        }
        pty_.sever();
    }

    /// A USB unplug in the middle of an exchange: the first chunk `when` accepts is recorded and left unanswered, and
    /// the reader closes both pty ends at once, so the host's read for the reply hangs up. Set it before the exchange;
    /// `when` runs on the reader thread. Not reversible.
    void unplug_on(std::function<bool(const std::string& chunk)> when) {
        std::lock_guard<std::mutex> lock(mutex_);
        unplug_on_ = std::move(when);
    }

    /// True once unplug_on() has fired.
    bool unplugged() const { return unplugged_.load(); }

private:
    void run() {
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 50) <= 0) {  // 50 ms slices so stop_ is honoured promptly
                continue;
            }
            char buf[64];
            const ssize_t n = ::read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN && errno != EINTR && errno != EIO) {
                    return;
                }
                // EIO: no slave open right now; wait for the next open.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            const std::string chunk(buf, static_cast<std::size_t>(n));
            bool pull = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                received_ += chunk;
                pull = unplug_on_ && unplug_on_(chunk);
            }
            if (pull) {
                pty_.sever();
                unplugged_.store(true);
                return;
            }
            if (silent_.load()) {
                continue;
            }
            const std::string reply = responder_(chunk);
            if (!reply.empty()) {
                // Issue #424: bounded, on a non-blocking master, so a reply
                // the wrapper is not draining can never hang the join.
                pty_write_bounded(pty_.master_fd(), reply, stop_);
            }
        }
    }

    // First member: constructed before the reader, destroyed after it has
    // been joined.
    PtyPair pty_;
    Responder responder_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> silent_{false};
    std::atomic<bool> unplugged_{false};
    mutable std::mutex mutex_;
    std::string received_;
    std::function<bool(const std::string&)> unplug_on_;
};

/// Answers the NexStar / SynScan protocol echo ("K" + byte -> byte + "#").
inline std::string answer_handset_echo(const std::string& chunk) {
    if (chunk.size() >= 2 && chunk[0] == 'K') {
        return std::string(1, chunk[1]) + "#";
    }
    return "";
}

}  // namespace alpacacore::test

#endif  // _WIN32
