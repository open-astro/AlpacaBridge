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

// Serial-port ownership cases shared by the request/response mount wrappers
// (Celestron, SynScan, ZWO AM; issue #1202), run over a pty "handset"
// (pty_responder.h). Each vendor's test file names its own cases and passes
// its wrapper, a serial ConnectionInfo and a driver factory. POSIX-only.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <termios.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "pty_responder.h"

namespace alpacacore::test::mount_serial {

inline speed_t line_speed(int fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    return cfgetospeed(&tty);
}

/// Retune the port from outside, as another program would: a new speed and,
/// when @p vmin1, the VMIN=1/VTIME=0 blocking-read shape that parked the
/// #1202 readers.
inline void retune(int fd, speed_t speed, bool vmin1) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    if (vmin1) {
        tty.c_cc[VMIN] = 1;
        tty.c_cc[VTIME] = 0;
    }
    REQUIRE(tcsetattr(fd, TCSANOW, &tty) == 0);
}

/// Another device holds the port: the driver's connect is refused before the
/// port is opened, the refusal names the holder, and the line is untouched.
inline void driver_refuses_held_port(PtyResponder& device,
                                     const std::function<std::unique_ptr<TelescopeDriver>()>& make_driver) {
    retune(device.keepalive_fd(), B19200, false);
    auto held = util::SerialPortClaim::acquire(device.slave_path(), util::SerialClaimKind::Link, "Other device");
    REQUIRE(held);

    auto driver = make_driver();
    std::string message;
    try {
        driver->set_connected(true);
    } catch (const AlpacaException& e) {
        message = e.what();
    }
    INFO("connect refusal: " << message);
    CHECK(message.find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK(device.received().empty());
    CHECK(line_speed(device.keepalive_fd()) == B19200);  // never retuned to the mount's 9600
    driver.reset();
    CHECK(util::is_serial_port_in_use(device.slave_path()));  // still the other device's
}

/// The wrapper holds the port while connected and gives it back on
/// disconnect, so a second connect (and another program) can open it.
template <typename Wrapper, typename Info>
void wrapper_holds_and_releases_port(Wrapper& wrapper, const Info& info, const std::string& path) {
    REQUIRE_FALSE(util::is_serial_port_in_use(path));
    REQUIRE(wrapper.connect(info));
    CHECK(util::is_serial_port_in_use(path));
    wrapper.disconnect();
    CHECK_FALSE(wrapper.is_connected());
    CHECK_FALSE(util::is_serial_port_in_use(path));

    REQUIRE(wrapper.connect(info));  // the exclusive lock went with the first link
    CHECK(util::is_serial_port_in_use(path));
    wrapper.disconnect();
    CHECK_FALSE(util::is_serial_port_in_use(path));
}

/// Another program retunes the live port to VMIN=1/VTIME=0 at another speed
/// and the handset goes quiet: a command times out on schedule instead of
/// parking in read(), the wrapper puts its line settings back, and disconnect
/// returns at once.
template <typename Wrapper, typename Info>
void retuned_port_times_out_and_recovers(Wrapper& wrapper, const Info& info, PtyResponder& device,
                                         const std::function<void()>& command) {
    REQUIRE(wrapper.connect(info));
    REQUIRE(line_speed(device.keepalive_fd()) == B9600);
    device.set_silent(true);
    retune(device.keepalive_fd(), B19200, true);

    auto attempt = std::async(std::launch::async, [&] {
        try {
            command();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    });
    REQUIRE(attempt.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(attempt.get());  // timed out, did not answer
    CHECK(line_speed(device.keepalive_fd()) == B9600);

    auto teardown = std::async(std::launch::async, [&] { wrapper.disconnect(); });
    REQUIRE(teardown.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    teardown.get();
    CHECK_FALSE(util::is_serial_port_in_use(device.slave_path()));
}

}  // namespace alpacacore::test::mount_serial

#endif  // _WIN32
