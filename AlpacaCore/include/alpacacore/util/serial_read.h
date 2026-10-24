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

// The read loop above util::tty_read(): read a reply to its terminator, its
// length or an idle gap, drop the bytes the protocol ignores, cap the length,
// keep one steady_clock deadline and classify a hangup.
//
// read_until() reads a tty fd (SerialLink::fd()) through tty_read();
// read_until_socket() reads a TCP fd through recv(). Both share one private
// template, so the loop exists once. They are two concrete overloads and not a
// transport parameter on purpose (decision 0013 section 7: no ByteChannel).
//
// The helper classifies and reports; it never decides policy. It does not
// latch, close, throw or sleep, and it does not flush or delay: a wrapper
// flushes before its command, waits its command delay and turns the outcome
// into its own exception or latch. It calls no raw read(), so the port
// ownership gate needs no exemption for it (decision 0017 rules 5 and 9).
//
// Bytes are read one at a time, so bytes after the stop condition stay in the
// fd for the next read.
//
// POSIX-only, like serial_link.h; callers include it inside their existing
// `#ifndef _WIN32` branches.

#ifndef _WIN32

#include <alpacacore/util/serial_link.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace alpacacore::util {

struct ReadSpec {
    enum class Stop : std::uint8_t {
        Terminator,  ///< any byte of `terminators` ends the read (kept in the result)
        Length,      ///< `length` kept bytes end the read
        IdleGap,     ///< `idle_gap` of silence after the first kept byte ends the read
    };
    Stop stop = Stop::Terminator;
    std::string terminators = "#";
    std::size_t length = 0;
    std::chrono::milliseconds idle_gap{100};
    /// Bytes dropped on arrival, in every mode (a Length read included, so an
    /// ack read skips a stray CR/LF). Empty for binary data.
    std::string skip = "\r\n";
    /// A reply may hold at most this many kept bytes: reaching it without
    /// stopping is Overflow. A Length or Terminator hit on that same byte wins.
    std::size_t max_len = 256;
    /// Timeout if no kept byte arrives in this window (the deadline still applies).
    std::optional<std::chrono::milliseconds> first_byte_timeout;
};

enum class ReadOutcome : std::uint8_t {
    Terminator,
    Length,
    Idle,
    Timeout,   ///< the deadline or first_byte_timeout passed; `bytes` holds what arrived;
               ///< `err` is non-zero when the read failed with an errno that is not a hangup
    Overflow,  ///< max_len reached without stopping
    LinkDead,  ///< the line is gone; `err` holds the errno
};

struct ReadResult {
    std::string bytes;
    ReadOutcome outcome = ReadOutcome::Timeout;
    int err = 0;
};

namespace detail {

inline bool tty_hangup_errno(int e) { return e == EIO || e == ENXIO || e == ENODEV || e == EBADF; }
inline bool socket_hangup_errno(int e) { return e == ENOTCONN || e == ECONNRESET || e == EPIPE || e == ENOTSOCK; }

/// Whole milliseconds left to @p t, rounded up, so a wait never ends early.
inline int wait_ms_until(std::chrono::steady_clock::time_point t) {
    const auto left = t - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return 0;
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(left).count());
}

/// One byte from a socket, waiting at most @p timeout_ms: 1, 0 on timeout, -1
/// with errno set. An orderly close by the peer is -1 / ECONNRESET.
inline ssize_t socket_read_byte(int fd, char* c, int timeout_ms) {
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    if ((pfd.revents & POLLNVAL) != 0) {
        errno = EBADF;
        return -1;
    }
    const ssize_t n = ::recv(fd, c, 1, MSG_DONTWAIT);
    if (n == 1) return 1;
    if (n == 0) {
        errno = ECONNRESET;
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

/// The loop. @p read_byte is `ssize_t(char*, int timeout_ms)` with tty_read's contract.
template <typename ReadByte, typename IsHangup>
ReadResult read_until_impl(ReadByte read_byte, IsHangup is_hangup, const ReadSpec& spec,
                           std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
    ReadResult res;
    const bool has_first_limit = spec.first_byte_timeout.has_value();
    const Clock::time_point first_limit =
        Clock::now() + (has_first_limit ? *spec.first_byte_timeout : std::chrono::milliseconds::zero());
    Clock::time_point last_kept = Clock::now();
    for (;;) {
        // The next instant something other than a byte ends the read.
        auto limit = deadline;
        if (res.bytes.empty()) {
            if (has_first_limit && first_limit < limit) limit = first_limit;
        } else if (spec.stop == ReadSpec::Stop::IdleGap && last_kept + spec.idle_gap < limit) {
            limit = last_kept + spec.idle_gap;
        }
        char c = 0;
        const ssize_t r = read_byte(&c, wait_ms_until(limit));
        if (r < 0) {
            if (errno == EINTR) continue;
            res.err = errno;
            res.outcome = is_hangup(res.err) ? ReadOutcome::LinkDead : ReadOutcome::Timeout;
            return res;
        }
        if (r == 0) {
            const auto now = Clock::now();
            if (!res.bytes.empty() && spec.stop == ReadSpec::Stop::IdleGap && now >= last_kept + spec.idle_gap) {
                res.outcome = ReadOutcome::Idle;
                return res;
            }
            if (now >= limit) {
                res.outcome = ReadOutcome::Timeout;
                return res;
            }
            continue;
        }
        if (spec.skip.find(c) != std::string::npos) continue;
        res.bytes += c;
        last_kept = Clock::now();
        if (spec.stop == ReadSpec::Stop::Terminator && spec.terminators.find(c) != std::string::npos) {
            res.outcome = ReadOutcome::Terminator;
            return res;
        }
        if (spec.stop == ReadSpec::Stop::Length && res.bytes.size() >= spec.length) {
            res.outcome = ReadOutcome::Length;
            return res;
        }
        if (res.bytes.size() >= spec.max_len) {
            res.outcome = ReadOutcome::Overflow;
            return res;
        }
    }
}

}  // namespace detail

/// Read from the tty @p tty_fd (non-blocking, as SerialLink keeps it) until
/// @p spec says stop or @p deadline passes. Hangup errnos: EIO, ENXIO, ENODEV, EBADF.
inline ReadResult read_until(int tty_fd, const ReadSpec& spec, std::chrono::steady_clock::time_point deadline) {
    return detail::read_until_impl([tty_fd](char* c, int timeout_ms) { return tty_read(tty_fd, c, 1, timeout_ms); },
                                   detail::tty_hangup_errno, spec, deadline);
}

/// The same read over a connected stream socket. Hangup errnos: ENOTCONN,
/// ECONNRESET, EPIPE, ENOTSOCK; the peer closing the stream reports ECONNRESET.
inline ReadResult read_until_socket(int socket_fd, const ReadSpec& spec,
                                    std::chrono::steady_clock::time_point deadline) {
    return detail::read_until_impl(
        [socket_fd](char* c, int timeout_ms) { return detail::socket_read_byte(socket_fd, c, timeout_ms); },
        detail::socket_hangup_errno, spec, deadline);
}

}  // namespace alpacacore::util

#endif  // _WIN32
