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

// The shared pty plumbing for the pty-backed fakes: PtyPair, which owns the
// pseudo-terminal pair a fake answers a driver over (issue #387), and
// pty_write_bounded(), the bounded write every reply goes through (issue
// #424, the shape #364 describes). All six fakes -- the five fake_*.h doubles
// plus FakeSerialHandset in test_synscan_handset_probe.cpp -- hold one PtyPair
// and write only through the bounded helper; the hand-rolled posix_openpt()
// block that PtyPair replaced leaked the master on every setup-failure path
// and ignored a failed keep-alive open.
//
// Why the write is bounded: every one of these fakes wrote its reply with a bare
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
// was caught (driver-lifecycle.instructions.md: a template bug found in one place is fixed
// everywhere):
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
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

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
    /// The keep-alive slave handle. It shares one termios with every other
    /// fd open on the slave, so a fake can read back the line speed the
    /// driver configured (FakeSkyWatcherSerialBoard::line_baud()).
    int keepalive_fd() const { return keepalive_fd_; }
    /// Empty after sever(): the path named a pty that no longer exists. By
    /// value, so a copy taken before the cut is not emptied underneath the
    /// caller by sever() clearing the member.
    std::string slave_path() const { return slave_path_; }

    /// Close both ends now: the driver's writes on the slave fail with EIO
    /// and its reads return 0 at once from here on, which is what a USB
    /// unplug looks like (issues #237, #772). Not reversible, and slave_path() is cleared here (not
    /// in close_all(), which the destructor also runs) so a test that tried
    /// to reopen the severed pty fails on an empty path rather than on
    /// ENOENT, or on a recycled /dev/pts/N belonging to someone else.
    void sever() {
        close_all();
        slave_path_.clear();
    }

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

/// The fds of this process open on @p path (a pty slave), read from /proc/self/fd. A test takes this set before
/// and after a wrapper's connect to find the wrapper's own fd, which it cannot otherwise see (issue #2262).
inline std::set<int> fds_open_on(const std::string& path) {
    std::set<int> fds;
    std::error_code ec;
    const auto target = std::filesystem::canonical(path, ec);
    if (ec) return fds;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        std::error_code link_ec;
        const auto points_at = std::filesystem::read_symlink(entry.path(), link_ec);
        if (link_ec || points_at != target) continue;
        try {
            fds.insert(std::stoi(entry.path().filename().string()));
        } catch (const std::exception&) {
        }
    }
    return fds;
}

/// The one fd in @p after that is not in @p before, or -1 when there is not exactly one.
inline int new_fd(const std::set<int>& before, const std::set<int>& after) {
    int found = -1;
    for (const int fd : after) {
        if (before.count(fd) != 0) continue;
        if (found >= 0) return -1;
        found = fd;
    }
    return found;
}

/// Make @p fd unusable for writing while keeping its number valid: a read-only /dev/null takes its place, so the
/// owner's next write fails with EBADF (the fd can no longer be written) and its later close() closes that
/// stand-in, never a number another thread has reused. The EBADF shape of issue #2262, without a double close.
/// TIOCEXCL is released first: the owner's close() now reaches the stand-in, so it could not release it, and the
/// keep-alive fd would hold the tty exclusive against the next open.
inline bool make_fd_unwritable(int fd) {
    (void)::ioctl(fd, TIOCNXCL);
    const int nul = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nul < 0) return false;
    const bool ok = ::dup2(nul, fd) == fd;
    ::close(nul);
    return ok;
}

/// Put a second fd on the tty at @p path, opened with @p flags, in @p fd's place: the shape of make_fd_unwritable() for
/// a wrapper that flushes the tty's input before every command (tcflush() on /dev/null fails with ENOTTY before the
/// write). A stand-in on the same tty keeps the flush working, so O_RDONLY makes the command's write meet EBADF and
/// O_WRONLY its reply read (issue #2262; the ZWO mount and OnStep cases). TIOCEXCL is released first, so the stand-in
/// can open; the owner's close() later closes the stand-in.
inline bool replace_fd_on_tty(int fd, const std::string& path, int flags) {
    (void)::ioctl(fd, TIOCNXCL);
    const int stand_in = ::open(path.c_str(), flags | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (stand_in < 0) return false;
    const bool ok = ::dup2(stand_in, fd) == fd;
    ::close(stand_in);
    return ok;
}

/// Make @p fd unusable for reads and writes alike while keeping its number valid: an O_PATH descriptor takes its
/// place, so the owner's next read() and write() both fail with EBADF (a poll() reports it ready, then the call
/// fails). For an owner with a reader thread, which make_fd_unwritable() would hand an end-of-file (an EIO) before
/// its own write met the EBADF (issue #2262). TIOCEXCL is released first, as there.
inline bool make_fd_unusable(int fd) {
    (void)::ioctl(fd, TIOCNXCL);
    const int path_only = ::open("/dev/null", O_PATH | O_CLOEXEC);
    if (path_only < 0) return false;
    const bool ok = ::dup2(path_only, fd) == fd;
    ::close(path_only);
    return ok;
}

inline std::size_t pty_write_bounded(int fd, const std::string& data, const std::atomic<bool>& stop,
                                     std::chrono::milliseconds budget = std::chrono::milliseconds(250)) {
    return pty_write_bounded(fd, data.data(), data.size(), stop, budget);
}

}  // namespace alpacacore::test

#endif  // _WIN32
