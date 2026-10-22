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

// QHY CFW3 and Q-Focuser serial-port ownership (issue #1202), over their pty
// fakes: a port another device holds is refused untouched and the refusal
// names the holder; the connected port is claimed and released (the CFW3
// keeps its claim across logical disconnects, with its fd, so a reconnect
// skips the wheel's ~17 s reset); and a port another program retunes (its
// own baud, VMIN=1/VTIME=0) under the CFW3's move wait is restored and cannot
// hang Disconnect.

#ifndef _WIN32

#include <alpacacore/catalog/builtin_catalog.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_protocol_wrapper.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>
#include <termios.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_qhy_cfw3.h"
#include "fake_qhy_qfocuser.h"

namespace {

namespace qhy = alpacacore::vendor::qhy;
using alpacacore::test::FakeQhyCfw3;
using alpacacore::test::FakeQhyQFocuser;
using alpacacore::util::is_serial_port_in_use;

// A pty never DTR-resets the fake, so the real 25 s boot wait is dead time here.
qhy::Cfw3Settings fast_settings() {
    qhy::Cfw3Settings s;
    s.boot_timeout_ms = 150;
    s.reply_timeout_ms = 300;
    s.move_timeout_ms = 3000;
    return s;
}

void set_line_speed(int fd, speed_t speed) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    REQUIRE(tcsetattr(fd, TCSANOW, &tty) == 0);
}

speed_t line_speed(int fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    return cfgetospeed(&tty);
}

std::string connect_refusal(alpacacore::AlpacaDriver& driver) {
    try {
        driver.set_connected(true);
    } catch (const alpacacore::AlpacaException& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST_CASE("QHY CFW3 serial - a port another device holds is refused untouched, naming the holder",
          "[qhy][serial][issue1202]") {
    FakeQhyCfw3 fake(7);
    set_line_speed(fake.keepalive_fd(), B19200);
    auto other = alpacacore::util::SerialPortClaim::acquire(fake.slave_path(), alpacacore::util::SerialClaimKind::Link,
                                                            "Other device");
    REQUIRE(other);

    auto driver = qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), fast_settings());
    CHECK(connect_refusal(*driver).find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.connects() == 0);                       // nothing on the wire
    CHECK(line_speed(fake.keepalive_fd()) == B19200);  // and the line was never retuned
}

TEST_CASE("QHY CFW3 serial - the port stays claimed across a logical disconnect and is released with the wrapper",
          "[qhy][serial][issue1202]") {
    FakeQhyCfw3 fake(7);
    qhy::Cfw3ConnectionConfig config;
    config.serial_port = fake.slave_path();
    config.boot_timeout_ms = 150;
    config.reply_timeout_ms = 300;
    {
        qhy::Cfw3ProtocolWrapper wheel;
        REQUIRE(wheel.connect(config).slot_count == 7);
        CHECK(is_serial_port_in_use(fake.slave_path()));

        // Logical disconnect: the link (fd + claim + TIOCEXCL) is held so the
        // next connect does not reset the wheel, and nobody else can take it.
        wheel.disconnect();
        CHECK_FALSE(wheel.is_connected());
        CHECK(is_serial_port_in_use(fake.slave_path()));
        auto other = alpacacore::util::SerialPortClaim::acquire(
            fake.slave_path(), alpacacore::util::SerialClaimKind::Link, "Other device");
        CHECK_FALSE(other);

        REQUIRE(wheel.connect(config).slot_count == 7);
        wheel.disconnect();
    }
    // The wrapper is gone: the port is released for real.
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));

    // And a fresh wrapper can open it again (TIOCNXCL was released too).
    qhy::Cfw3ProtocolWrapper again;
    REQUIRE(again.connect(config).slot_count == 7);
    CHECK(is_serial_port_in_use(fake.slave_path()));
}

TEST_CASE("QHY CFW3 serial - a port retuned to VMIN=1 during a move wait is restored and Disconnect returns",
          "[qhy][serial][issue1202]") {
    FakeQhyCfw3 fake(7);
    auto settings = fast_settings();
    settings.move_timeout_ms = 30000;  // the wait's own deadline cannot end it
    auto driver = qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), settings);
    driver->set_connected(true);

    fake.set_muted(true);  // the arrival byte never comes: the worker stays in its read
    driver->set_position(3);
    REQUIRE(driver->get_position() == -1);

    // Another program retunes the live port to its own baud, with a blocking
    // read that waits for a byte forever.
    struct termios tty {};
    REQUIRE(tcgetattr(fake.keepalive_fd(), &tty) == 0);
    cfsetispeed(&tty, B4800);
    cfsetospeed(&tty, B4800);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    REQUIRE(tcsetattr(fake.keepalive_fd(), TCSANOW, &tty) == 0);
    // Outlast a few of the move wait's 100 ms read slices: a timed-out slice
    // notices the retune and puts the driver's line settings back.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(line_speed(fake.keepalive_fd()) == B9600);

    auto done = std::async(std::launch::async, [&] { driver->set_connected(false); });
    CHECK(done.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    done.wait();
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("QHY Q-Focuser serial - a port another device holds is refused untouched, naming the holder",
          "[qhy][serial][issue1202]") {
    FakeQhyQFocuser fake;
    set_line_speed(fake.keepalive_fd(), B19200);
    auto other = alpacacore::util::SerialPortClaim::acquire(fake.slave_path(), alpacacore::util::SerialClaimKind::Link,
                                                            "Other device");
    REQUIRE(other);

    auto driver = qhy::create_qhy_focuser(0, fake.slave_path());
    CHECK(connect_refusal(*driver).find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.connects() == 0);
    CHECK(line_speed(fake.keepalive_fd()) == B19200);
}

TEST_CASE("QHY Q-Focuser serial - the connected port is claimed, released on disconnect, and reconnects",
          "[qhy][serial][issue1202]") {
    FakeQhyQFocuser fake;
    auto driver = qhy::create_qhy_focuser(0, fake.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(is_serial_port_in_use(fake.slave_path()));

    driver->set_connected(false);
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));

    // TIOCEXCL was released with the claim: the same process can open it again.
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(is_serial_port_in_use(fake.slave_path()));
    CHECK(fake.connects() == 2);
    driver->set_connected(false);
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
}

TEST_CASE("QHY Q-Focuser catalog - the configured maxStep reaches the connected driver and bounds Move",
          "[qhy][serial][catalog]") {
    FakeQhyQFocuser fake;
    alpacacore::catalog::DeviceCatalog catalog;
    alpacacore::catalog::register_builtin_schemas(catalog);
    alpacacore::catalog::register_builtin_factories(catalog);

    alpacacore::catalog::DeviceConfig config;
    config.set("connectionType", std::string{"serial"});
    config.set("portPath", fake.slave_path());
    config.set("maxStep", std::int64_t{30000});
    auto driver = catalog.create(alpacacore::catalog::DeviceKey{"qhy", alpacacore::DeviceType::Focuser}, config, 0);
    REQUIRE(driver != nullptr);
    auto& focuser = dynamic_cast<alpacacore::FocuserDriver&>(*driver);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(focuser.get_max_step() == 30000);
    CHECK(focuser.get_max_increment() == 30000);
    try {
        focuser.move(30001);
        FAIL("Move beyond the configured maxStep must be refused");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::InvalidValue);
    }
    driver->set_connected(false);
}

#endif  // _WIN32
