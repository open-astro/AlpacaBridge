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

// The CGX driver over the SkyPortal WiFi (AUX over TCP) transport, against the loopback fake adapter. The driver's
// waits run on the fake task clock with the state poll held (cgx_clock_cleanup.h); the wire timing (a dropped reply's
// 1000 ms timeout, the 300 ms re-send) is real, bounded time (decision 0005 rule 6).

#ifndef _WIN32

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "cgx_clock_cleanup.h"
#include "driver_rig.h"
#include "fake_celestron_aux_tcp_adapter.h"
#include "fake_task_clock.h"

namespace {

using namespace std::chrono_literals;
namespace AlpacaError = alpacacore::AlpacaError;
namespace cgx = alpacacore::vendor::celestron;
using alpacacore::test::FakeCelestronAuxTcpAdapter;
using alpacacore::test::require_alpaca_error;
using Fault = FakeCelestronAuxTcpAdapter::Fault;

constexpr std::uint8_t kRa = 0x10;
constexpr std::uint8_t kDec = 0x11;

struct TcpRig {
    FakeCelestronAuxTcpAdapter adapter;
    alpacacore::test::FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};

    TcpRig() {
        REQUIRE(adapter.ok());
        cgx::CelestronCgxConnectionInfo info;
        info.port_path = "/dev/null/not-a-port";  // unused: the host selects TCP
        info.host = "127.0.0.1";
        info.tcp_port = adapter.port();
        info.site_latitude_degrees = 45.0;
        info.site_longitude_degrees = -75.0;
        driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    }

    void set_connected(bool connected) { alpacacore::test::set_cgx_connected(*driver, clock, connected); }

    void find_home() {
        driver->find_home();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        REQUIRE(driver->get_at_home());
    }
};

}  // namespace

TEST_CASE("Celestron CGX TCP driver - connects through the adapter from source 0x20",
          "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.adapter.connections() == 1);
    CHECK(rig.driver->get_name() == "Celestron CGX/CGX-L");
    CHECK(rig.driver->get_description().find("SkyPortal WiFi") != std::string::npos);
    CHECK(rig.driver->get_device_firmware() == "RA 7.18.2214, Dec 7.18.2214");
    const auto frames = rig.adapter.received();
    REQUIRE_FALSE(frames.empty());
    for (const auto& frame : frames) CHECK(frame.source == 0x20);
}

TEST_CASE("Celestron CGX TCP driver - Connect takes the mount over with a verified stop",
          "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.adapter.count(kRa, 0x24) == 1);
    CHECK(rig.adapter.count(kDec, 0x24) == 1);
    CHECK(rig.adapter.count(kRa, 0x13) >= 1);  // the stop was verified on the status, not taken on its ACK
    CHECK(rig.adapter.count(kDec, 0x13) >= 1);
    CHECK_FALSE(rig.driver->get_tracking());
}

TEST_CASE("Celestron CGX TCP driver - a lost identity reply is recovered and counted, not a fault",
          "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.adapter.script(kRa, 0xfe, 1, Fault::Drop);
    rig.adapter.script(kDec, 0x05, 1, Fault::Drop);
    rig.set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.adapter.count(kRa, 0xfe) == 2);
    CHECK(rig.adapter.count(kDec, 0x05) == 2);
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_description().find("lost replies recovered by a re-send: 2") != std::string::npos);
}

TEST_CASE("Celestron CGX TCP driver - Disconnect closes the socket and the reference goes with it",
          "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    rig.find_home();
    REQUIRE(rig.driver->get_declination() > 89.99);
    const int position_reads = rig.adapter.count(kRa, 0x01);

    rig.set_connected(false);
    // No continuity checkpoint: a new socket is a new session, so it could never be used.
    CHECK(rig.adapter.count(kRa, 0x01) == position_reads);
    rig.set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.adapter.connections() == 2);
    CHECK_FALSE(rig.driver->get_at_home());
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); }, AlpacaError::DriverException);
}

TEST_CASE("Celestron CGX TCP driver - a stop nobody acknowledges ends with send-only stops",
          "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    REQUIRE(rig.adapter.count(kRa, 0x24) == 1);  // the connect's takeover stop
    // Every ACK-checked RA stop of the disconnect is lost (three rounds, real 1000 ms each).
    for (int nth = 2; nth <= 4; ++nth) rig.adapter.script(kRa, 0x24, nth, Fault::Drop);
    rig.set_connected(false);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (rig.adapter.count(kDec, 0x06) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);  // real time: the fake reads the last frames
    CHECK(rig.adapter.count(kRa, 0x24) == 5);
    CHECK(rig.adapter.count(kRa, 0x06) == 1);
    CHECK(rig.adapter.count(kDec, 0x06) == 1);
}

TEST_CASE("Celestron CGX TCP driver - a motor fault report costs the reference", "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    rig.find_home();
    REQUIRE(rig.driver->get_declination() > 89.99);

    rig.adapter.inject(FakeCelestronAuxTcpAdapter::frame(kDec, 0x20, 0x51, {0x01}));
    std::this_thread::sleep_for(100ms);  // real time: the fake's serve slice sends it
    cgx::poll_state_now_for_testing(*rig.driver);  // meets the report: a session error
    cgx::poll_state_now_for_testing(*rig.driver);  // resyncs and reads a frame again
    CHECK(rig.driver->get_connected());
    try {
        static_cast<void>(rig.driver->get_declination());
        FAIL("the reference survived a motor fault report");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == AlpacaError::DriverException);
        CHECK(std::string(error.what()).find("FindHome") != std::string::npos);
    }
}

TEST_CASE("Celestron CGX TCP driver - two devices cannot share one adapter", "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    cgx::CelestronCgxConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = rig.adapter.port();
    auto second = alpacacore::test::create_cgx_telescope_poll_held(1, info, rig.clock);
    require_alpaca_error([&] { second->set_connected(true); }, AlpacaError::InvalidOperation);
    CHECK_FALSE(second->get_connected());
    CHECK(rig.adapter.connections() == 1);
}

#endif  // !_WIN32
