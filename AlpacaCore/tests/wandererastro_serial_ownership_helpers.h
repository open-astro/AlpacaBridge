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

// Helpers for the WandererAstro serial-port ownership cases (issue #1202):
// retune a live port the way another program would, through a pty fake's
// keep-alive fd (it shares the driver's termios), and bound a disconnect.

#include <termios.h>

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <string>
#include <thread>

#include "catch2_compat.h"

namespace alpacacore::test::wanderer {

/// Retune the port to a blocking read (VMIN=1, VTIME=0): a reader that relies
/// on VTIME to come back and check its stop flag parks in read() for good.
inline void retune_to_blocking_read(int keepalive_fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(keepalive_fd, &tty) == 0);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    REQUIRE(tcsetattr(keepalive_fd, TCSANOW, &tty) == 0);
}

inline speed_t line_speed(int keepalive_fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(keepalive_fd, &tty) == 0);
    return cfgetospeed(&tty);
}

inline void set_line_speed(int keepalive_fd, speed_t speed) {
    struct termios tty {};
    REQUIRE(tcgetattr(keepalive_fd, &tty) == 0);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    REQUIRE(tcsetattr(keepalive_fd, TCSANOW, &tty) == 0);
}

/// The message @p connect throws, or "" when it does not throw.
inline std::string connect_error(const std::function<void()>& connect) {
    try {
        connect();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

/**
 * @brief Run @p disconnect on its own thread and report whether it returned
 *        within @p limit.
 *
 * When it did not, @p release is called (unpark the reader, e.g. by letting
 * the fake send bytes again) so the thread can still be joined and the test
 * ends with a failed CHECK rather than a hung binary.
 */
inline bool returns_within(const std::function<void()>& disconnect, std::chrono::milliseconds limit,
                           const std::function<void()>& release) {
    std::promise<void> done;
    auto done_future = done.get_future();
    std::thread runner([&] {
        try {
            disconnect();
        } catch (...) {  // NOLINT(bugprone-empty-catch): only the timing is under test
        }
        done.set_value();
    });
    const bool returned = done_future.wait_for(limit) == std::future_status::ready;
    if (!returned) {
        release();
    }
    runner.join();
    return returned;
}

}  // namespace alpacacore::test::wanderer
