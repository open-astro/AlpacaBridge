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

// POSIX serial-I/O helpers shared by the vendor protocol wrappers. These
// centralise two easy-to-get-wrong patterns that were each fixed once in the
// WandererCover wrapper and then needed across every serial driver:
//
//   * write_all()        — POSIX write() may satisfy only part of the request
//                          (0 < n < len) or be interrupted (EINTR). A bare
//                          `write(fd, buf, len)` that treats any non-negative
//                          return as success silently drops trailing bytes,
//                          e.g. a command terminator.
//   * clear_nonblocking() — `fcntl(F_GETFL)` can fail and return -1; feeding
//                          that into `flags & ~O_NONBLOCK` (i.e. -1 & ~flag)
//                          can leave the fd non-blocking, spinning a reader
//                          thread at 100% CPU. The result must be checked.
//
// The whole header is POSIX-only; callers already wrap their serial code in
// `#ifndef _WIN32`, so this is compiled into the same branches.

#ifndef _WIN32

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <thread>

namespace alpacacore::util {

// Absolute upper bound on how long write_all()/send_all() keep retrying a
// partial transfer that hits backpressure (EAGAIN/EWOULDBLOCK under a send
// timeout). It bounds the only retry path that can loop — a partial write
// followed by persistent backpressure — so a dead peer can't wedge the calling
// thread for the multi-minute TCP retransmit window. The deadline starts at the
// first backpressure stall and is NOT extended by subsequent progress, so even a
// trickle-then-stall peer is bounded; the payloads here (short device commands)
// complete in well under this budget once the link drains.
inline constexpr std::chrono::milliseconds kPartialIoRetryBudget{2000};

/**
 * @brief Write the entire buffer to @p fd, looping over partial writes.
 *
 * Retries on EINTR and continues until every byte is written. Returns true
 * only when the whole payload was written; false on the first hard write
 * error (errno is left set by write()).
 */
inline bool write_all(int fd, const char* data, std::size_t len) {
    std::size_t total = 0;
    bool deadline_set = false;
    std::chrono::steady_clock::time_point deadline{};
    while (total < len) {
        const ssize_t written = ::write(fd, data + total, len - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;  // interrupted before any byte was written — retry
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && total > 0) {
                // Transient backpressure (SO_SNDTIMEO) AFTER a partial write:
                // retry so the half-written payload completes rather than corrupt
                // framing — but only within a bounded budget so a dead peer can't
                // stall here for the TCP retransmit window. When total == 0
                // nothing was written, so fail fast.
                if (!deadline_set) {
                    deadline = std::chrono::steady_clock::now() + kPartialIoRetryBudget;
                    deadline_set = true;
                }
                if (std::chrono::steady_clock::now() < deadline) {
                    // Back off so this can't busy-spin if the fd is non-blocking
                    // (no send timeout to pace us); negligible when write/send
                    // already blocked on SO_SNDTIMEO.
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
            return false;
        }
        if (written == 0) {
            // write() made no progress on a non-empty request; treat as a hard
            // error rather than spinning forever waiting for it to advance. Set
            // errno so a caller that reports strerror(errno) gets a real message
            // instead of a stale/zero value.
            errno = EIO;
            return false;
        }
        total += static_cast<std::size_t>(written);
    }
    return true;
}

/**
 * @brief Send the entire buffer over socket @p fd, looping over short sends.
 *
 * The socket analogue of write_all(): retries EINTR (and transient
 * EAGAIN/EWOULDBLOCK only after a partial send), treats a 0 return as a hard
 * error, and forwards @p flags. Callers pass MSG_NOSIGNAL so that a peer
 * disconnect mid-send surfaces as an error return instead of delivering SIGPIPE
 * (whose default disposition would terminate the process). Returns true only
 * when the whole payload was sent; a short send that isn't completed would
 * otherwise leave stray bytes in the kernel buffer and corrupt later framing.
 */
inline bool send_all(int fd, const char* data, std::size_t len, int flags) {
    std::size_t total = 0;
    bool deadline_set = false;
    std::chrono::steady_clock::time_point deadline{};
    while (total < len) {
        const ssize_t sent = ::send(fd, data + total, len - total, flags);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && total > 0) {
                // Transient backpressure under SO_SNDTIMEO AFTER a partial send:
                // retry so the half-sent command completes (dropping it would
                // corrupt framing) — but only within a bounded budget so a dead
                // peer can't stall here for the TCP retransmit window. When
                // total == 0 fail fast.
                if (!deadline_set) {
                    deadline = std::chrono::steady_clock::now() + kPartialIoRetryBudget;
                    deadline_set = true;
                }
                if (std::chrono::steady_clock::now() < deadline) {
                    // Back off so this can't busy-spin if the socket is
                    // non-blocking (no SO_SNDTIMEO to pace us); negligible when
                    // send() already blocked on SO_SNDTIMEO.
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
            return false;
        }
        if (sent == 0) {
            errno = EIO;  // keep strerror(errno) meaningful for callers that report it
            return false;
        }
        total += static_cast<std::size_t>(sent);
    }
    return true;
}

/**
 * @brief Clear O_NONBLOCK on @p fd, checking both fcntl calls.
 *
 * Returns false if F_GETFL fails (so the caller never feeds -1 into F_SETFL)
 * or if F_SETFL fails. On success the fd is left in blocking mode.
 */
inline bool clear_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) >= 0;
}

/**
 * @brief Set O_NONBLOCK on @p fd, checking both fcntl calls.
 *
 * Mirror of clear_nonblocking() for the connect-with-timeout pattern (set
 * non-blocking, connect, poll, restore blocking). Returns false if either
 * F_GETFL or F_SETFL fails, so the caller never connect()s on a socket that
 * was meant to be non-blocking but silently stayed blocking.
 */
inline bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

}  // namespace alpacacore::util

#endif  // _WIN32
