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

// Bounded writes for the pty-backed fakes (issue #424, the shape #364
// describes).
//
// Every one of these fakes wrote its reply with a bare
//
//     (void)!write(master_fd_, reply.data(), reply.size());
//
// on a master obtained from posix_openpt(O_RDWR | O_NOCTTY) -- no O_NONBLOCK.
// Once the pty's buffer fills, which is exactly what happens as a concurrency
// test winds down and the driver side stops draining, that write() parks the
// worker thread indefinitely. The destructor then sets stop_ and joins, and
// the join never returns, because the thread is not at the top of its loop to
// observe stop_: it is asleep inside write(). The process hangs rather than
// failing, and in CI that burns the job's whole time budget before reporting
// anything.
//
// It is a race, not a reproducible red -- the same tree passes a full ctest
// run most of the time -- so it surfaces as an occasional CI hang.
//
// Two rules, applied to all six pty-backed fakes rather than to the one that
// was caught (AGENTS.md: a template bug found in one place is fixed
// everywhere) -- the five fake_*.h doubles plus FakeSerialHandset in
// test_synscan_handset_probe.cpp:
//
//   1. The master is opened non-blocking, so a write can never park.
//   2. A reply that cannot be written within a short bound, or while the fake
//      is being torn down, is DROPPED. That is the right answer for a test
//      double: a reply the driver is not draining is a reply it was never
//      going to read, and a fake whose destructor can hang is worse than one
//      that drops a frame.

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace alpacacore::test {

/// Put a pty master into non-blocking mode. Call once, right after
/// posix_openpt(), before any worker thread can write to it.
inline bool make_pty_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// A pseudo-terminal pair for a serial-device fake: the non-blocking master
// this fake reads and writes, the slave path the driver opens, and a
// keep-alive slave handle so the master never sees EIO between the driver's
// disconnect (close) and reconnect (open). Owns both descriptors, so a
// constructor that throws after opening the master no longer leaks it for
// the life of the test binary (issue #387: no destructor runs for a
// partially constructed fake, so the fake's own destructor could not close
// what its constructor had opened). A keep-alive open that fails throws
// too, rather than surfacing later as a confusing EIO on the first read.
class PtyPair {
public:
    explicit PtyPair(const char* who) {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        // Issue #424: the master goes non-blocking here, so a reply to a
        // driver that has stopped draining can never park a fake's worker
        // inside write() and hang the destructor's join. Folded into the
        // same throw as the other setup failures: a silent fallback to a
        // blocking master would look exactly like the hang this removes.
        if (master_fd_ < 0 || grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0 ||
            !make_pty_nonblocking(master_fd_)) {
            close_all();
            throw std::runtime_error(std::string(who) + ": cannot open pty");
        }
        const char* name = ptsname(master_fd_);
        if (name == nullptr) {
            close_all();
            throw std::runtime_error(std::string(who) + ": ptsname failed");
        }
        slave_path_ = name;
        keepalive_fd_ = ::open(slave_path_.c_str(), O_RDWR | O_NOCTTY);
        if (keepalive_fd_ < 0) {
            close_all();
            throw std::runtime_error(std::string(who) + ": cannot open the keep-alive slave");
        }
        struct termios tty {};
        if (tcgetattr(keepalive_fd_, &tty) == 0) {
            cfmakeraw(&tty);
            tcsetattr(keepalive_fd_, TCSANOW, &tty);
        }
    }
    ~PtyPair() { close_all(); }
    PtyPair(const PtyPair&) = delete;
    PtyPair& operator=(const PtyPair&) = delete;

    int master_fd() const { return master_fd_; }
    const std::string& slave_path() const { return slave_path_; }

    /// Close both ends now: the driver's reads and writes on the slave fail
    /// with EIO from here on, which is what a USB unplug looks like
    /// (issue #237). Not reversible.
    void sever() { close_all(); }

private:
    void close_all() {
        if (keepalive_fd_ >= 0) {
            ::close(keepalive_fd_);
            keepalive_fd_ = -1;
        }
        if (master_fd_ >= 0) {
            ::close(master_fd_);
            master_fd_ = -1;
        }
    }
    int master_fd_ = -1;
    int keepalive_fd_ = -1;
    std::string slave_path_;
};

/// Write `data` to a non-blocking pty master, giving up after `budget` or as
/// soon as `stop` is set. Returns the number of bytes actually written; the
/// remainder is deliberately dropped.
///
/// `stop` is the fake's own teardown flag, so a destructor that sets it and
/// joins is never waiting on a thread parked in here for the full budget.
inline std::size_t pty_write_bounded(int fd, const char* data, std::size_t len, const std::atomic<bool>& stop,
                                     std::chrono::milliseconds budget = std::chrono::milliseconds(250)) {
    if (fd < 0) {
        return 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::size_t written = 0;
    while (written < len && !stop.load()) {
        const ssize_t n = ::write(fd, data + written, len - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;  // EIO after a sever_link(), or the slave went away
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        // Wait for room, but never past the deadline, and in slices short
        // enough that a stop_ set mid-write is noticed promptly.
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int slice = static_cast<int>(remaining.count() < 20 ? remaining.count() : 20);
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        const int ready = ::poll(&pfd, 1, slice);
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
    }
    return written;
}

inline std::size_t pty_write_bounded(int fd, const std::string& data, const std::atomic<bool>& stop,
                                     std::chrono::milliseconds budget = std::chrono::milliseconds(250)) {
    return pty_write_bounded(fd, data.data(), data.size(), stop, budget);
}

}  // namespace alpacacore::test

#endif  // _WIN32
