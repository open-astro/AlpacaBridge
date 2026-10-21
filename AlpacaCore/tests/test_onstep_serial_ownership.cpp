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

// OnStep serial-port ownership (issue #1202), over a pty that answers every
// '#'-framed LX200 command with "0#". The other OnStep tests drive the
// wrapper's TCP test seam; this file covers the serial link itself: a port
// another device holds is refused untouched, the connected port is claimed
// and released, and a port another program retunes (its own baud,
// VMIN=1/VTIME=0) can no longer park a read, and gets the driver's settings
// back (the TeSeek HM-17 field report: after Park, the blind
// ":ST...#" command's drain blocked forever and every telescope call hung).

#ifndef _WIN32

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_pty_write.h"

namespace {

namespace onstep = alpacacore::vendor::onstep;

// A minimal OnStep controller on a pty: "0#" for every '#'-terminated
// command, or nothing at all while silent.
class OnStepPty {
public:
    OnStepPty() : pty_("OnStepPty") {
        th_ = std::thread([this] { run(); });
    }
    ~OnStepPty() { stop_thread(); }
    OnStepPty(const OnStepPty&) = delete;
    OnStepPty& operator=(const OnStepPty&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }
    int keepalive_fd() const { return pty_.keepalive_fd(); }
    void set_silent(bool on) { silent_.store(on); }

    // Close both pty ends: a reader parked in the slave's read() returns.
    void sever() {
        stop_thread();
        pty_.sever();
    }

private:
    void stop_thread() {
        stop_.store(true);
        if (th_.joinable()) th_.join();
    }
    void run() {
        std::string acc;
        while (!stop_.load()) {
            pollfd p{pty_.master_fd(), POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            char buf[64];
            const ssize_t n = ::read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            acc.append(buf, static_cast<std::size_t>(n));
            for (auto pos = acc.find('#'); pos != std::string::npos; pos = acc.find('#')) {
                acc.erase(0, pos + 1);
                if (!silent_.load()) {
                    alpacacore::test::pty_write_bounded(pty_.master_fd(), "0#", 2, stop_);
                }
            }
        }
    }

    alpacacore::test::PtyPair pty_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> silent_{false};
};

onstep::ConnectionInfo serial_info(const std::string& path) {
    onstep::ConnectionInfo info;
    info.type = onstep::ConnectionType::Serial;
    info.port_path = path;
    info.baud_rate = 9600;
    info.response_timeout_ms = 300;
    return info;
}

speed_t line_speed(int fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    return cfgetospeed(&tty);
}

}  // namespace

TEST_CASE("OnStep serial - a port another device holds is refused untouched, naming the holder",
          "[onstep][serial][issue1202]") {
    OnStepPty fake;
    struct termios tty {};
    REQUIRE(tcgetattr(fake.keepalive_fd(), &tty) == 0);
    cfsetispeed(&tty, B19200);
    cfsetospeed(&tty, B19200);
    REQUIRE(tcsetattr(fake.keepalive_fd(), TCSANOW, &tty) == 0);

    auto other = alpacacore::util::SerialPortClaim::acquire(fake.slave_path(), alpacacore::util::SerialClaimKind::Link,
                                                            "Other device");
    REQUIRE(other);

    auto driver = onstep::create_onstep_telescope(0, serial_info(fake.slave_path()));
    std::string message;
    try {
        driver->set_connected(true);
    } catch (const alpacacore::AlpacaException& e) {
        message = e.what();
    }
    CHECK(message.find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(onstep::OnStepProtocolWrapper::instance().is_connected());
    // Refused before the open: the 9600 baud the driver would set never reached the line.
    CHECK(line_speed(fake.keepalive_fd()) == B19200);
}

TEST_CASE("OnStep serial - the connected port is claimed, released on disconnect, and reconnects",
          "[onstep][serial][issue1202]") {
    OnStepPty fake;
    auto& proto = onstep::OnStepProtocolWrapper::instance();
    REQUIRE(proto.connect(serial_info(fake.slave_path())));
    CHECK(alpacacore::util::is_serial_port_in_use(fake.slave_path()));
    CHECK(proto.get_product_name() == "0");

    proto.disconnect();
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(fake.slave_path()));

    // A second connect opens the port again: the claim and TIOCEXCL were both released.
    REQUIRE(proto.connect(serial_info(fake.slave_path())));
    CHECK(alpacacore::util::is_serial_port_in_use(fake.slave_path()));
    CHECK(proto.get_product_name() == "0");
    proto.disconnect();
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(fake.slave_path()));
}

TEST_CASE("OnStep serial - a blind command on a port retuned to VMIN=1 returns and the line is restored",
          "[onstep][serial][issue1202]") {
    OnStepPty fake;
    auto& proto = onstep::OnStepProtocolWrapper::instance();
    REQUIRE(proto.connect(serial_info(fake.slave_path())));

    // Another program retunes the live port to its own baud, with a blocking
    // read that waits for a byte forever.
    struct termios tty {};
    REQUIRE(tcgetattr(fake.keepalive_fd(), &tty) == 0);
    cfsetispeed(&tty, B4800);
    cfsetospeed(&tty, B4800);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    REQUIRE(tcsetattr(fake.keepalive_fd(), TCSANOW, &tty) == 0);
    // The mount acks nothing, as after Park: the drain finds no stray byte.
    fake.set_silent(true);

    std::promise<void> done;
    auto done_future = done.get_future();
    std::thread caller([&] {
        try {
            proto.send_command_blind(":ST0#");  // drain_stale() reads the line
        } catch (const std::exception&) {
        }
        proto.disconnect();
        done.set_value();
    });
    const bool returned = done_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    if (!returned) {
        fake.sever();  // release a read parked under VMIN=1 so the thread can finish
    }
    caller.join();
    CHECK(returned);

    // The drain's timed-out read noticed the retune and put the driver's line settings back.
    CHECK(line_speed(fake.keepalive_fd()) == B9600);
    proto.disconnect();
}

#endif  // _WIN32
