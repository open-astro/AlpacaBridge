// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

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
#include <unistd.h>

#include <cerrno>
#include <cstddef>

namespace alpacacore::util {

/**
 * @brief Write the entire buffer to @p fd, looping over partial writes.
 *
 * Retries on EINTR and continues until every byte is written. Returns true
 * only when the whole payload was written; false on the first hard write
 * error (errno is left set by write()).
 */
inline bool write_all(int fd, const char* data, std::size_t len) {
    std::size_t total = 0;
    while (total < len) {
        const ssize_t written = ::write(fd, data + total, len - total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;  // interrupted before any byte was written — retry
            }
            return false;
        }
        if (written == 0) {
            // write() made no progress on a non-empty request; treat as a hard
            // error rather than spinning forever waiting for it to advance.
            return false;
        }
        total += static_cast<std::size_t>(written);
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
