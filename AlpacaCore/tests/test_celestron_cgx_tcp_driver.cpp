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
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "cgx_clock_cleanup.h"
#include "driver_rig.h"
#include "fake_celestron_aux_tcp_adapter.h"
#include "fake_celestron_cgx_mount.h"
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
    CHECK(rig.driver->get_description().find("lost replies recovered: 2") != std::string::npos);
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
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (rig.adapter.serving() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);  // real time: the fake sees the close within a serve slice
    CHECK_FALSE(rig.adapter.serving());
    rig.set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.adapter.connections() == 2);
    CHECK_FALSE(rig.driver->get_at_home());
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); }, AlpacaError::DriverException);
}

TEST_CASE("Celestron CGX TCP driver - a stop not confirmed on a moving axis gets the send-only stop at once",
          "[celestron][cgx][telescope][tcp][motion-confirm]") {
    TcpRig rig;
    // Another client left RA moving; the takeover stop's first RA stop is lost on its way to the bus, so the status
    // cannot confirm it (the axis still moves) and the ACK-checked round fails on RA.
    rig.adapter.set_drift(kRa, 1.0e6);
    rig.adapter.script(kRa, 0x24, 1, Fault::Drop);
    rig.set_connected(true);
    REQUIRE(rig.driver->get_connected());
    const auto frames = rig.adapter.received();
    const auto first = [&](std::uint8_t destination, std::uint8_t command) {
        for (std::size_t i = 0; i < frames.size(); ++i)
            if (frames[i].destination == destination && frames[i].command == command) return i;
        return frames.size();
    };
    // The send-only stop (0x24 then a zero rate) goes to RA before the Dec stop, not after three rounds.
    REQUIRE(first(kRa, 0x06) < frames.size());
    CHECK(first(kRa, 0x06) < first(kDec, 0x24));
    const auto stopped_at = rig.adapter.position(kRa);
    std::this_thread::sleep_for(5ms);  // real time: a drifting axis would move ~5,000 counts
    CHECK(rig.adapter.position(kRa) == stopped_at);
}

TEST_CASE("Celestron CGX TCP driver - the cleanup stop budget fits a TCP status poll",
          "[celestron][cgx][telescope][tcp][motion-confirm]") {
    TcpRig rig;
    SECTION("an axis that reads busy for 800 ms after the stop verifies") {
        // 40 busy SLEW_DONE reads per axis, one per 20 ms poll: past USB's 500 ms, inside TCP's budget.
        rig.adapter.set_stop_settle_reads(40);
        rig.set_connected(true);
        CHECK(rig.driver->get_connected());
    }
    SECTION("an axis that stays busy still fails the stop") {
        rig.adapter.set_stop_settle_reads(200);  // 4 s of busy reads
        try {
            rig.set_connected(true);
            FAIL("a stop the status never confirmed was taken as verified");
        } catch (const alpacacore::AlpacaException& error) {
            CHECK(error.error_code() == AlpacaError::DriverException);
            CHECK(std::string(error.what()).find("did not confirm that both motor axes stopped") != std::string::npos);
        }
        CHECK_FALSE(rig.driver->get_connected());
    }
}

TEST_CASE("Celestron CGX TCP driver - FindHome over a lost LEVEL_START reply",
          "[celestron][cgx][telescope][tcp][motion-confirm]") {
    TcpRig rig;
    rig.adapter.set_home_duration(800ms);
    SECTION("the search started: FindHome completes with the reference") {
        rig.adapter.script(kRa, 0x0b, 1, Fault::DropReply);
        rig.set_connected(true);
        auto task = std::async(std::launch::async, [&] { rig.driver->find_home(); });
        alpacacore::test::drain_cgx_clock_task(task, rig.clock);
        task.get();
        // The home body waits on the fake clock between LEVEL_DONE polls; the search itself takes 800 ms real time.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (rig.driver->get_slewing() && std::chrono::steady_clock::now() < deadline) {
            if (rig.clock.wait_for_waiters(1, 1ms)) rig.clock.advance(20ms);
            std::this_thread::sleep_for(1ms);
        }
        CHECK(rig.driver->get_at_home());
        CHECK(rig.driver->get_declination() > 89.99);
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_description().find("lost replies recovered: 1") != std::string::npos);
    }
    SECTION("the request was lost: FindHome fails and sends no second LEVEL_START") {
        rig.adapter.script(kRa, 0x0b, 1, Fault::Drop);
        rig.set_connected(true);
        auto task = std::async(std::launch::async, [&] { rig.driver->find_home(); });
        alpacacore::test::drain_cgx_clock_task(task, rig.clock);
        require_alpaca_error([&] { task.get(); }, AlpacaError::DriverException);
        CHECK_FALSE(rig.driver->get_at_home());
        CHECK_FALSE(rig.driver->get_slewing());
        CHECK(rig.adapter.count(kDec, 0x0b) == 0);
    }
    CHECK(rig.adapter.count(kRa, 0x0b) == 1);
}

// Rig 2026-11-21 (slice 5a): during a home the adapter lost requests in bursts, while the home-switch boards talk.
TEST_CASE("Celestron CGX TCP driver - FindHome through a burst of lost requests after LEVEL_START",
          "[celestron][cgx][telescope][tcp][home-search]") {
    TcpRig rig;
    rig.adapter.set_home_duration(800ms);
    rig.set_connected(true);
    const int ra_stops = rig.adapter.count(kRa, 0x24);
    const int dec_stops = rig.adapter.count(kDec, 0x24);
    const int ra_status = rig.adapter.count(kRa, 0x13);
    const int dec_status = rig.adapter.count(kDec, 0x13);
    const int lost = GENERATE(2, 3);
    INFO("lost=" << lost);
    rig.adapter.set_home_burst_loss(lost);  // the requests right after Dec's LEVEL_START, whatever they are
    rig.driver->find_home();
    std::string failure;
    const auto ended = [&] {
        try {
            return !rig.driver->get_slewing();
        } catch (const alpacacore::AlpacaException& error) {
            failure = error.what();
            // Until the stop's first ACK the link fault raises (#1950); the body's own error comes after it.
            return failure.find("communications compromised") == std::string::npos;
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!ended() && std::chrono::steady_clock::now() < deadline) {
        if (rig.clock.wait_for_waiters(1, 1ms)) rig.clock.advance(20ms);
        std::this_thread::sleep_for(1ms);  // real time: the search and the re-sends run in real time
    }
    INFO("Slewing failure: " << failure);
    CHECK(rig.adapter.count(kRa, 0x0b) == 1);
    CHECK(rig.adapter.count(kDec, 0x0b) == 1);
    if (lost == 2) {
        CHECK(failure.empty());
        CHECK(rig.driver->get_at_home());
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_description().find("lost replies recovered: 1") != std::string::npos);
        CHECK(rig.adapter.count(kRa, 0x24) == ra_stops);
    } else {
        // All three sends lost: the home fails as a lost reply always did, and both axes are stopped and verified
        // (a stop the status did not confirm would replace the error with its own).
        CHECK(failure.find("AUX transaction timed out") != std::string::npos);
        CHECK(failure.find("did not confirm") == std::string::npos);
        CHECK_FALSE(rig.driver->get_at_home());
        CHECK(rig.adapter.count(kRa, 0x24) > ra_stops);
        CHECK(rig.adapter.count(kDec, 0x24) > dec_stops);
        CHECK(rig.adapter.count(kRa, 0x13) > ra_status);
        CHECK(rig.adapter.count(kDec, 0x13) > dec_status);
    }
}

TEST_CASE("Celestron CGX USB driver - a cleanup stop keeps its 500 ms budget",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    cgx::CelestronCgxConnectionInfo info;
    info.port_path = mount.slave_path();
    info.site_latitude_degrees = 45.0;
    info.site_longitude_degrees = -75.0;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    // 40 busy reads per axis, 20 ms apart in real time: past 500 ms, so the Disconnect's stop is not verified.
    mount.set_stop_completion_reads(40);
    require_alpaca_error([&] { driver->set_connected(false); }, AlpacaError::DriverException);
    mount.set_stop_completion_reads(0);
}

// The home travel guard reads the encoders through the same AUX link over TCP (docs/failures/0016-cgx-home-travel.md).
TEST_CASE("Celestron CGX TCP driver - a runaway home search is stopped by the travel guard",
          "[celestron][cgx][telescope][tcp][home-search][travel-guard]") {
    TcpRig rig;
    rig.set_connected(true);
    // About 172 deg per real second: the sum passes 270 deg in about 1.6 s. The loop below lets at most 20 ms of
    // task-clock time pass per real millisecond, so that is at most 32 s of task-clock time, before the 60 s point.
    rig.adapter.set_home_runaway(kRa, 8'000'000.0);
    const int stops = rig.adapter.count(kRa, 0x24);
    rig.driver->find_home();
    std::string failure;
    const auto ended = [&] {
        try {
            return !rig.driver->get_slewing();
        } catch (const alpacacore::AlpacaException& error) {
            failure = error.what();
            return true;
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!ended() && std::chrono::steady_clock::now() < deadline) {
        if (rig.clock.wait_for_waiters(1, 1ms)) rig.clock.advance(20ms);
        std::this_thread::sleep_for(1ms);  // real time: the fake's runaway drifts in real time
    }
    INFO("Slewing failure: " << failure);
    CHECK(failure.find("Celestron CGX home travel guard") != std::string::npos);
    CHECK(rig.adapter.count(kRa, 0x0b) == 1);
    CHECK(rig.adapter.count(kRa, 0x24) > stops);
    const auto stopped_at = rig.adapter.position(kRa);
    std::this_thread::sleep_for(200ms);  // real time: the fake's runaway drifts in real time
    CHECK(rig.adapter.position(kRa) == stopped_at);
}

TEST_CASE("Celestron CGX TCP driver - a motor fault report costs the reference", "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    rig.find_home();
    REQUIRE(rig.driver->get_declination() > 89.99);

    rig.adapter.inject(FakeCelestronAuxTcpAdapter::frame(kDec, 0x20, 0x51, {0x01}));
    std::this_thread::sleep_for(100ms);            // real time: the fake's serve slice sends it
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

// The required driver cases for the WiFi variant (testing instructions, cases 1, 2 and 5-8; cases 3, 4 and the
// disconnected range probes come from the celestron_telescope_cgx_tcp entry in contract_sweep.h).
namespace {
cgx::CelestronCgxConnectionInfo wifi_info(int port) {
    cgx::CelestronCgxConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.site_latitude_degrees = 45.0;
    info.site_longitude_degrees = -75.0;
    return info;
}
}  // namespace

namespace {

// Slice 5 rig finding (2026-11-21): over WiFi each exchange takes ~21 ms, and an untracked mount falls behind the sky
// between aiming a correction and reading its arrival, so every GOTO used all three corrections and one failed at
// 3.055". The rig: every exchange costs 21 ms on every clock (the task clock, the pointing UTC and the adapter's
// motors), a GOTO takes 150 ms (the rig's 2-3 SLEW_DONE polls), and sidereal tracking drifts the RA axis.
struct TcpArrivalRig {
    FakeCelestronAuxTcpAdapter adapter;
    std::atomic<std::int64_t> epoch_milliseconds{1800000000000};
    alpacacore::test::FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    double target_ra = 0.0;

    TcpArrivalRig() {
        REQUIRE(adapter.ok());
        adapter.use_virtual_time();
        cgx::CelestronCgxConnectionInfo info;
        info.port_path = "/dev/null/not-a-port";
        info.host = "127.0.0.1";
        info.tcp_port = adapter.port();
        info.site_latitude_degrees = 45.0;
        info.site_longitude_degrees = -75.0;
        const auto now = [this] {
            return std::chrono::system_clock::time_point{std::chrono::milliseconds(epoch_milliseconds.load())};
        };
        driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, now);
        alpacacore::test::set_cgx_connected(*driver, clock, true);
        auto home = std::async(std::launch::async, [this] { driver->find_home(); });
        alpacacore::test::drain_cgx_clock_task(home, clock);
        home.get();
        REQUIRE(step([this] { return slew_ended(); }));
        REQUIRE(driver->get_at_home());
        driver->set_tracking(true);
        adapter.set_goto_duration(150ms);
        adapter.set_tracking_rate(16777216.0 / 86164.0905);  // counts per second at the sidereal rate
        adapter.set_before_request([this] { advance(21ms); });
    }
    // The hook goes before the clock and the UTC it advances, which die before the adapter whose thread runs it.
    ~TcpArrivalRig() { adapter.set_before_request(nullptr); }

    void advance(std::chrono::milliseconds elapsed) {
        epoch_milliseconds.fetch_add(elapsed.count());
        adapter.advance(elapsed);
        clock.advance(elapsed);
    }
    bool step(const std::function<bool()>& done) {
        return alpacacore::test::step_cgx_motion(
            clock, [this](std::chrono::milliseconds elapsed) { advance(elapsed); }, done, 20ms);
    }
    bool slew_ended() const {
        try {
            return !driver->get_slewing();
        } catch (const alpacacore::AlpacaException&) {
            return true;  // a failed slew ends too; the case reads the failure
        }
    }
    // A slew to hour angle +1 (Normal), stepped until it ends.
    void slew() {
        target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
        driver->slew_to_coordinates_async(target_ra, 30.0);
        REQUIRE(step([this] { return slew_ended(); }));
    }
    void check_landed() {
        REQUIRE_NOTHROW(driver->get_slewing());
        CHECK_FALSE(driver->get_slewing());
        CHECK(driver->get_tracking());
        advance(500ms);
        cgx::poll_state_now_for_testing(*driver);
        CHECK(std::abs(std::remainder(driver->get_right_ascension() - target_ra, 24.0)) * 15.0 * 3600.0 <= 0.5);
        CHECK(std::abs(driver->get_declination() - 30.0) * 3600.0 <= 3.0);
    }
    // The RA rate commands (0x06) sent so far, in order.
    std::vector<std::vector<std::uint8_t>> ra_rates() const {
        std::vector<std::vector<std::uint8_t>> rates;
        for (const auto& frame : adapter.received())
            if (frame.destination == kRa && frame.command == 0x06) rates.push_back(frame.payload);
        return rates;
    }
    // A failed slew ends with both axes stopped and verified: the stop went out and RA stands still.
    void check_failed_and_stopped(int ra_stops_before) {
        require_alpaca_error([&] { static_cast<void>(driver->get_slewing()); }, AlpacaError::DriverException);
        CHECK_FALSE(driver->get_tracking());
        CHECK(adapter.count(kRa, 0x24) > ra_stops_before);
        CHECK(adapter.count(kDec, 0x24) > 0);
        const auto ra = adapter.position(kRa);
        adapter.advance(1000ms);
        CHECK(adapter.position(kRa) == ra);
    }
};

const std::vector<std::uint8_t> kSidereal{0xff, 0xff};
const std::vector<std::uint8_t> kRateZero{0x00, 0x00, 0x00};

}  // namespace

TEST_CASE("Celestron CGX TCP driver - GOTO arrival over 21 ms exchanges lands within 3 arcsec",
          "[celestron][cgx][telescope][tcp]") {
    TcpArrivalRig rig;
    rig.slew();
    rig.check_landed();
    CHECK(rig.adapter.count(kRa, 0x17) <= 2);
}

// Slice 5c, rig 2026-11-21 23:41 (5b build): the tracking-off write of the first correction lost its reply, and the
// 1000 ms timeout failed the GOTO. An absolute rate written twice is the same rate, so its lost reply is re-sent.
TEST_CASE("Celestron CGX TCP driver - a lost tracking-rate reply during the corrections is re-sent and the GOTO lands",
          "[celestron][cgx][telescope][tcp]") {
    // The RA rate writes of a slew that needs one correction: 1 Tracking on (before the slew), 2 off for the FAST
    // GOTO, 3 on at its landing, 4 off before the correction, 5 on at the correction's landing.
    const auto [nth, payload] = GENERATE(table<int, std::vector<std::uint8_t>>({{3, kSidereal}, {4, kRateZero}}));
    INFO("RA rate write " << nth);
    TcpArrivalRig rig;
    rig.adapter.script(kRa, 0x06, nth, Fault::DropReply);
    rig.slew();
    const auto rates = rig.ra_rates();
    REQUIRE(rates.size() > static_cast<std::size_t>(nth));
    CHECK(rates[static_cast<std::size_t>(nth - 1)] == payload);
    CHECK(rates[static_cast<std::size_t>(nth)] == payload);  // the re-send: the same absolute rate
    rig.check_landed();
    CHECK(rig.driver->get_description().find("lost replies recovered: 1") != std::string::npos);
}

TEST_CASE("Celestron CGX TCP driver - three lost replies to a tracking-rate write fail the GOTO and the stop verifies",
          "[celestron][cgx][telescope][tcp]") {
    TcpArrivalRig rig;
    // The tracking-off write before the first correction and both of its re-sends.
    for (const int nth : {4, 5, 6}) rig.adapter.script(kRa, 0x06, nth, Fault::DropReply);
    const int ra_stops = rig.adapter.count(kRa, 0x24);
    rig.slew();
    const auto rates = rig.ra_rates();
    REQUIRE(rates.size() >= 6);
    for (const std::size_t index : {3U, 4U, 5U}) CHECK(rates[index] == kRateZero);
    CHECK(rig.adapter.count(kRa, 0x17) == 0);
    rig.check_failed_and_stopped(ra_stops);
}

TEST_CASE("Celestron CGX TCP driver - a lost GOTO_SLOW fails the GOTO and is never re-sent",
          "[celestron][cgx][telescope][tcp]") {
    TcpArrivalRig rig;
    rig.adapter.script(kRa, 0x17, 1, Fault::Drop);  // lost on its way to the bus: the status cannot confirm it
    const int ra_stops = rig.adapter.count(kRa, 0x24);
    rig.slew();
    CHECK(rig.adapter.count(kRa, 0x17) == 1);
    rig.check_failed_and_stopped(ra_stops);
}

// #2541 item 1: a GOTO whose reply was lost is confirmed when the axis is closer to its target than just before it.
// An axis tracking toward the target would read closer with no GOTO at all (sidereal is ~58 counts per 300 ms against
// the 2-count threshold), so the lost GOTO must still read "not confirmed" with tracking on before the slew: the slew
// turns tracking off before its first leg, and every correction before its GOTO_SLOW.
TEST_CASE("Celestron CGX TCP driver - a GOTO lost while the mount tracked before the slew is not confirmed",
          "[celestron][cgx][telescope][tcp]") {
    TcpArrivalRig rig;
    // Tracking on again now that the fake tracks: the axis drifts at the sidereal rate until the slew stops it.
    rig.driver->set_tracking(false);
    rig.driver->set_tracking(true);
    rig.adapter.script(kRa, 0x02, 1, Fault::Drop);  // lost on its way to the bus: it never moved the axis
    const auto ra_reads = rig.adapter.count(kRa, 0x01);
    // Hour angle +11 (Normal): from home, tracking carries the RA axis toward this target.
    rig.target_ra = std::fmod(rig.driver->get_sidereal_time() - 11.0 + 24.0, 24.0);
    require_alpaca_error([&] { rig.driver->slew_to_coordinates_async(rig.target_ra, 30.0); },
                         AlpacaError::DriverException);
    CHECK(rig.adapter.count(kRa, 0x02) == 1);        // never re-sent
    CHECK(rig.adapter.count(kRa, 0x01) > ra_reads);  // the status was read to settle it
    CHECK(rig.driver->get_description().find("lost replies recovered: 0") != std::string::npos);
}

// #2541 item 3: each correction after the first is aimed at the sky the previous correction's aim-to-tracking time
// ahead. A long first correction (2 s of travel, 30" of sky) then aimed the short second one 2 s ahead, ~28" past the
// target, and left the landing to the third and last correction. The lead is capped, so the second lands.
TEST_CASE("Celestron CGX TCP driver - a long correction does not aim the next short one past the target",
          "[celestron][cgx][telescope][tcp]") {
    TcpArrivalRig rig;
    // The FAST GOTO and the first correction travel 2 s each, every later correction the rig's 150 ms.
    rig.adapter.set_before_request([&rig] {
        rig.advance(21ms);
        rig.adapter.set_goto_duration(rig.adapter.count(kRa, 0x17) == 0 ? 2000ms : 150ms);
    });
    rig.slew();
    rig.check_landed();
    CHECK(rig.adapter.count(kRa, 0x17) == 2);
}

TEST_CASE("Celestron CGX WiFi Telescope Driver - Defaults", "[celestron][cgx][telescope][tcp][unit]") {
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, wifi_info(1), clock);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == alpacacore::DeviceType::Telescope);
    CHECK(driver->get_device_number() == 0);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "Celestron CGX/CGX-L");
    CHECK_FALSE(driver->get_can_slew());
    CHECK(driver->get_can_slew_async());
    CHECK(driver->get_can_pulse_guide());
    CHECK(driver->get_can_set_guide_rates());
    CHECK(driver->get_can_find_home());
    CHECK(driver->get_can_park());
    CHECK_FALSE(driver->get_can_move_axis(0));
    CHECK_FALSE(driver->get_can_move_axis(1));
}

TEST_CASE("Celestron CGX WiFi Telescope Driver - Device metadata", "[celestron][cgx][telescope][tcp][unit]") {
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(3, wifi_info(1), clock);
    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_name() == "Celestron CGX/CGX-L");  // decision 0024: the transport is not in the name
    CHECK(driver->get_description() ==
          "Celestron CGX/CGX-L mount over SkyPortal WiFi (TCP); lost replies recovered: 0");
    CHECK(driver->get_driver_info() == "AlpacaCore Celestron CGX AUX Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "Celestron_CGX_3");  // the fallback; the router assigns the stored ID
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("Celestron CGX WiFi Telescope Driver - a refused adapter connection fails the connect",
          "[celestron][cgx][telescope][tcp]") {
    alpacacore::test::FakeTaskClock clock;
    // Port 1 on loopback: nothing listens, so the connect is refused at once.
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, wifi_info(1), clock);
    try {
        driver->set_connected(true);
        FAIL("a refused adapter connection must fail the connect");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(std::string(error.what()).find("Celestron CGX TCP") != std::string::npos);
    }
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Celestron CGX WiFi Telescope Driver - state machine before and after FindHome",
          "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK_FALSE(rig.driver->get_at_home());
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_tracking());
    // No reference over a fresh socket: tracking and slews wait for FindHome.
    require_alpaca_error([&] { rig.driver->set_tracking(true); }, AlpacaError::InvalidOperation);
    rig.find_home();
    CHECK(rig.driver->get_at_home());
    CHECK_FALSE(rig.driver->get_slewing());
    rig.driver->set_tracking(true);
    CHECK(rig.driver->get_tracking());
    CHECK_FALSE(rig.driver->get_at_home());
    rig.driver->set_tracking(false);
    rig.set_connected(false);
    CHECK_FALSE(rig.driver->get_connected());
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_slewing()); }, AlpacaError::NotConnected);
}

// #2517 changed read_sky_position() to read Dec before RA: while tracking, RA moves and Dec does not, so the RA count
// read last is the one closest to the UTC the position is computed at (~0.3" at 21 ms an exchange over WiFi, #2540).
// PulseGuide's polarity read is one of its callers; the state poll is held, so its reads are the only 0x01s.
TEST_CASE("Celestron CGX WiFi Telescope Driver - a control read of the sky position reads Dec before RA",
          "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    rig.find_home();
    rig.driver->set_tracking(true);
    const auto before = rig.adapter.received().size();
    rig.driver->pulse_guide(0, 10);
    const auto frames = rig.adapter.received();
    std::vector<std::uint8_t> position_reads;
    for (std::size_t i = before; i < frames.size() && frames[i].command != 0x26; ++i)
        if (frames[i].command == 0x01) position_reads.push_back(frames[i].destination);
    CHECK(position_reads == std::vector<std::uint8_t>{kDec, kRa});
}

TEST_CASE("Celestron CGX WiFi Telescope Driver - value ranges and unsupported members while connected",
          "[celestron][cgx][telescope][tcp]") {
    TcpRig rig;
    rig.set_connected(true);
    REQUIRE(rig.driver->get_connected());
    require_alpaca_error([&] { rig.driver->pulse_guide(4, 100); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.driver->pulse_guide(0, -1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.driver->set_target_declination(90.1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.driver->set_target_right_ascension(24.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.driver->set_tracking_rate(3); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.driver->set_side_of_pier(2); }, AlpacaError::InvalidValue);
    // CGX motor rates are not calibrated (contract_sweep.h): no MoveAxis, no Alt/Az, no rate offsets.
    require_alpaca_error([&] { rig.driver->move_axis(0, 1.0); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { rig.driver->slew_to_alt_az_async(45.0, 180.0); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { rig.driver->set_right_ascension_rate(0.0); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { rig.driver->set_side_of_pier(1); }, AlpacaError::NotImplemented);
    CHECK(rig.driver->get_supported_actions().empty());
    require_alpaca_error([&] { static_cast<void>(rig.driver->action("gps_status", "")); },
                         AlpacaError::ActionNotImplemented);
    // Nothing above reached the wire as motion.
    CHECK(rig.adapter.count(kRa, 0x02) == 0);
    CHECK(rig.adapter.count(kRa, 0x26) == 0);
}

#endif  // !_WIN32
