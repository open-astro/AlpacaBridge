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

// The remotealpaca CoverCalibrator proxied device (decision 0023, issue #1753 slice 5, #2704) over the scripted
// FakeHttpClient and a scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision
// 0005). Fake evidence only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the
// driver's contract_sweep.h entry. The rules the CoverCalibrator shares with the other proxied devices (the snapshot,
// the loss bound, the self-loop check) live in UpstreamProxy (upstream_proxy.h) and are covered at length by
// test_remotealpaca_observingconditions.cpp; the cases here pin what the CoverCalibrator's own members do with each of
// them.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_covercalibrator_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "alpaca_upstream_script.h"
#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_http_client.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
namespace remote = alpacacore::vendor::remotealpaca;
using alpacacore::CalibratorState;
using alpacacore::CoverCalibratorDriver;
using alpacacore::CoverState;
using alpacacore::test::alpaca_error_message;
using alpacacore::test::AlpacaUpstreamScript;
using alpacacore::test::require_alpaca_error;

constexpr std::chrono::milliseconds kRendezvous{5000};
constexpr int kUpstreamPort = 11111;
const std::string kPrefix = "127.0.0.1:11111: ";
const std::string kLabel = "Upstream 127.0.0.1:11111 device 0";

// The fake clock, the scripted upstream behind the fake HTTP client, and the driver, destroyed in reverse.
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::CoverCalibrator};
    std::unique_ptr<CoverCalibratorDriver> driver;

    explicit Rig(remote::ServerAddressSource self = {}, int device_number = 0) {
        upstream.install(http);
        driver = remote::create_remotealpaca_covercalibrator(device_number, config(std::chrono::seconds(1)),
                                                             http.client(), std::move(self), clock);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    ~Rig() { driver.reset(); }

    static remote::RemoteDeviceConfig config(std::chrono::seconds poll) {
        remote::RemoteDeviceConfig config;
        config.host = "127.0.0.1";
        config.port = kUpstreamPort;
        config.upstream_device_number = 0;
        config.poll_period = poll;
        return config;
    }

    // Connect, then wait for the poll to park on the clock.
    void connect() {
        REQUIRE(alpacacore::test::connect_and_park(*driver, clock, kRendezvous));
        REQUIRE(driver->get_connected());
    }

    // One poll period on the clock; returns when the woken poll has parked again or ended.
    void poll() {
        clock.advance(std::chrono::seconds(1));
        REQUIRE(clock.wait_for_woken_settled(kRendezvous));
    }

    // Three failed polls in a row latch the fault (decision 0009 rule 1); the upstream answers again after them.
    void fault() {
        http.fail_next(3);
        poll();
        poll();
        poll();
        REQUIRE(driver->get_connected());
        REQUIRE_FALSE(driver->get_link_fault().empty());
    }

    // The PUTs the upstream got, by member, in order.
    std::vector<std::string> puts() const {
        std::vector<std::string> out;
        for (const auto& request : http.requests()) {
            if (request.method == "PUT") out.push_back(request.url.substr(request.url.rfind('/') + 1));
        }
        return out;
    }
};

}  // namespace

TEST_CASE("Remote Alpaca CoverCalibrator Driver - Defaults", "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::CoverCalibrator);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca CoverCalibrator");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    // Whether the cover and the calibrator are there, and MaxBrightness, are the upstream's, read at Connect.
    require_alpaca_error([&] { (void)rig.driver->get_cover_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)rig.driver->get_calibrator_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)rig.driver->get_max_brightness(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - Device metadata", "[remotealpaca][covercalibrator][unit]") {
    Rig rig({}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "CoverCalibrator of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca CoverCalibrator Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 2);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_COVERCAL_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_COVERCAL_3");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Cover Calibrator");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator CoverCalibrator");
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - the getters serve the snapshot and MaxBrightness, calling nothing",
          "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(c.get_cover_state() == CoverState::Closed);
    CHECK_FALSE(c.get_cover_moving());
    CHECK(c.get_calibrator_state() == CalibratorState::Off);
    CHECK_FALSE(c.get_calibrator_changing());
    CHECK(c.get_brightness() == 0);
    CHECK(c.get_max_brightness() == 100);
    const auto state = c.get_device_state();
    REQUIRE(state.size() == 6);  // Brightness, CalibratorState, CoverState, CalibratorChanging, CoverMoving, TimeStamp
    CHECK(state[0].name == "Brightness");
    CHECK(std::get<std::int32_t>(state[0].value) == 0);
    CHECK(state[1].name == "CalibratorState");
    CHECK(std::get<std::int32_t>(state[1].value) == static_cast<std::int32_t>(CalibratorState::Off));
    CHECK(state[2].name == "CoverState");
    CHECK(std::get<std::int32_t>(state[2].value) == static_cast<std::int32_t>(CoverState::Closed));
    CHECK(state[3].name == "CalibratorChanging");
    CHECK(state[4].name == "CoverMoving");
    CHECK(state.back().name == "TimeStamp");
    CHECK(rig.http.calls() == calls_after_connect);

    // MaxBrightness is read once at Connect; the next poll measures the frame members only.
    rig.upstream.set("coverstate", "3");
    rig.upstream.set("brightness", "40");
    rig.upstream.set("maxbrightness", "255");
    CHECK(c.get_cover_state() == CoverState::Closed);  // until the next poll the frame holds
    rig.poll();
    CHECK(c.get_cover_state() == CoverState::Open);
    CHECK(c.get_brightness() == 40);
    CHECK(c.get_max_brightness() == 100);
    CHECK(rig.upstream.calls("maxbrightness") == 1);
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - OpenCover and CloseCover are forwarded and read Moving at once",
          "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    c.open_cover();
    CHECK(rig.upstream.calls("opencover") == 1);
    // Written through: Moving before any poll.
    CHECK(c.get_cover_state() == CoverState::Moving);
    CHECK(c.get_cover_moving());
    rig.poll();
    CHECK(c.get_cover_moving());  // the upstream still moves
    rig.upstream.finish_cover();
    rig.poll();
    CHECK(c.get_cover_state() == CoverState::Open);
    CHECK_FALSE(c.get_cover_moving());
    // An OpenCover of a cover the frame has Open still reads Moving: that state can be a poll period old (another
    // client may have closed the cover), so only the upstream's next answer says it is there.
    c.open_cover();
    CHECK(c.get_cover_state() == CoverState::Moving);
    CHECK(c.get_cover_moving());
    rig.upstream.finish_cover();
    rig.poll();
    CHECK(c.get_cover_state() == CoverState::Open);
    c.close_cover();
    CHECK(rig.upstream.calls("closecover") == 1);
    CHECK(c.get_cover_state() == CoverState::Moving);
    rig.upstream.finish_cover();
    rig.poll();
    CHECK(c.get_cover_state() == CoverState::Closed);
    CHECK_FALSE(c.get_cover_moving());
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - CalibratorOn and CalibratorOff read NotReady at once",
          "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    c.calibrator_on(60);
    CHECK(rig.upstream.calls("calibratoron") == 1);
    CHECK(rig.http.requests().back().form_body.find("Brightness=60") != std::string::npos);
    // Written through: NotReady and Brightness before any poll; only the poll has the upstream Ready.
    CHECK(c.get_calibrator_state() == CalibratorState::NotReady);
    CHECK(c.get_calibrator_changing());
    CHECK(c.get_brightness() == 60);
    rig.poll();
    CHECK(c.get_calibrator_state() == CalibratorState::NotReady);  // the upstream still warms up
    rig.upstream.finish_calibrator();
    rig.poll();
    CHECK(c.get_calibrator_state() == CalibratorState::Ready);
    CHECK_FALSE(c.get_calibrator_changing());
    CHECK(c.get_brightness() == 60);
    // CalibratorOn(0) is on at brightness 0, not off (ICoverCalibratorV2).
    c.calibrator_on(0);
    CHECK(c.get_calibrator_state() == CalibratorState::NotReady);
    CHECK(c.get_brightness() == 0);
    rig.upstream.finish_calibrator();
    rig.poll();
    CHECK(c.get_calibrator_state() == CalibratorState::Ready);
    // CalibratorOff: NotReady at once; Brightness stays the polled one until the next poll has the upstream's.
    c.calibrator_on(60);
    rig.upstream.finish_calibrator();
    rig.poll();
    c.calibrator_off();
    CHECK(rig.upstream.calls("calibratoroff") == 1);
    CHECK(c.get_calibrator_state() == CalibratorState::NotReady);
    CHECK(c.get_calibrator_changing());
    CHECK(c.get_brightness() == 60);
    rig.upstream.finish_calibrator();
    rig.poll();
    CHECK(c.get_calibrator_state() == CalibratorState::Off);
    CHECK_FALSE(c.get_calibrator_changing());
    CHECK(c.get_brightness() == 0);
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - Value range validation", "[remotealpaca][covercalibrator][unit]") {
    SECTION("a negative brightness is InvalidValue before NotConnected, and sends nothing") {
        Rig rig;
        require_alpaca_error([&] { rig.driver->calibrator_on(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { rig.driver->set_brightness(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { rig.driver->calibrator_on(0); }, AlpacaError::NotConnected);
        rig.connect();
        require_alpaca_error([&] { rig.driver->calibrator_on(-1); }, AlpacaError::InvalidValue);
        CHECK(rig.upstream.calls("calibratoron") == 0);
    }
    SECTION("above the MaxBrightness the Connect read is InvalidValue, and sends nothing") {
        Rig rig;
        rig.connect();
        CHECK(alpaca_error_message([&] { rig.driver->calibrator_on(101); }, AlpacaError::InvalidValue) ==
              "Brightness 101 is above MaxBrightness 100");
        CHECK(rig.upstream.calls("calibratoron") == 0);
        rig.driver->calibrator_on(100);
        CHECK(rig.upstream.calls("calibratoron") == 1);
    }
    SECTION("an upstream that answered MaxBrightness with an error judges the range, and its InvalidValue passes") {
        Rig rig;
        rig.upstream.set_error("maxbrightness", 0x500, "MaxBrightness unavailable");
        rig.connect();
        CHECK(alpaca_error_message([&] { (void)rig.driver->get_max_brightness(); }, AlpacaError::DriverException) ==
              kPrefix + "MaxBrightness unavailable");
        rig.upstream.set_put_error("calibratoron", 0x401, "Brightness 500 is outside 0 to 255");
        CHECK(alpaca_error_message([&] { rig.driver->calibrator_on(500); }, AlpacaError::InvalidValue) ==
              kPrefix + "Brightness 500 is outside 0 to 255");
        CHECK(rig.upstream.calls("calibratoron") == 1);
        CHECK(rig.driver->get_calibrator_state() == CalibratorState::Off);  // a refused command writes nothing through
    }
    SECTION("the factory refuses each config field out of range before any network call") {
        alpacacore::test::FakeTaskClock clock;
        alpacacore::test::FakeHttpClient http;
        for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
            require_alpaca_error(
                [&] {
                    (void)remote::create_remotealpaca_covercalibrator(0, Rig::config(bad), http.client(), {}, clock);
                },
                AlpacaError::InvalidValue);
        }
        for (const int port : {0, 65536}) {
            remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
            config.port = port;
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_covercalibrator(0, config, http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.upstream_device_number = -1;
        require_alpaca_error(
            [&] { (void)remote::create_remotealpaca_covercalibrator(0, config, http.client(), {}, clock); },
            AlpacaError::InvalidValue);
        CHECK(http.calls() == 0);
    }
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - HaltCover is forwarded while the link is faulted, the rest is not",
          "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    rig.connect();
    auto& c = *rig.driver;
    c.open_cover();
    REQUIRE(c.get_cover_moving());
    rig.fault();
    // Faulted: the states read Unknown, the other frame members are refused with DriverException, and MaxBrightness
    // (read at Connect) still answers.
    CHECK(c.get_cover_state() == CoverState::Unknown);
    CHECK(c.get_calibrator_state() == CalibratorState::Unknown);
    CHECK(alpaca_error_message([&] { (void)c.get_cover_moving(); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    require_alpaca_error([&] { (void)c.get_calibrator_changing(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)c.get_brightness(); }, AlpacaError::DriverException);
    CHECK(c.get_max_brightness() == 100);
    const auto state = c.get_device_state();
    REQUIRE(state.size() == 3);  // CalibratorState, CoverState (Unknown both) and TimeStamp
    CHECK(state[0].name == "CalibratorState");
    CHECK(state[1].name == "CoverState");
    // The safety stop goes out; the commands that need the frame are refused and send nothing.
    c.halt_cover();
    CHECK(rig.upstream.calls("haltcover") == 1);
    require_alpaca_error([&] { c.open_cover(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.close_cover(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.calibrator_on(10); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.calibrator_off(); }, AlpacaError::DriverException);
    CHECK(rig.upstream.calls("opencover") == 1);
    CHECK(rig.upstream.calls("closecover") == 0);
    CHECK(rig.upstream.calls("calibratoron") == 0);
    CHECK(rig.upstream.calls("calibratoroff") == 0);
    // The next good poll clears the fault: the upstream stopped.
    rig.poll();
    CHECK(c.get_link_fault().empty());
    CHECK_FALSE(c.get_cover_moving());
    CHECK(c.get_cover_state() == CoverState::Unknown);  // the scripted upstream stops between the two
}

// CoverState after a HaltCover is the upstream's, read after the PUT: only it knows where the cover stopped. A read
// that gets no reply does not fail the Halt: CoverState is left to the next poll.
TEST_CASE("Remote Alpaca CoverCalibrator Driver - HaltCover reads the upstream's CoverState after the PUT",
          "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    std::atomic<bool> state_silent{false};  // the CoverState read gets HTTP 500, no usable reply
    rig.http.set_responder([&](const alpacacore::test::FakeHttpClient::Request& request) {
        if (state_silent.load() && request.method == "GET" && request.url.find("/coverstate") != std::string::npos) {
            return alpacacore::util::HttpResponse{500, ""};
        }
        return rig.upstream.answer(request);
    });
    rig.connect();
    auto& c = *rig.driver;
    c.open_cover();
    c.halt_cover();
    CHECK_FALSE(c.get_cover_moving());
    CHECK(c.get_cover_state() == CoverState::Unknown);  // the upstream's answer, before any poll
    // The read after the PUT gets no reply: the Halt stands, the link stays up, and the next poll has the state.
    c.open_cover();
    state_silent.store(true);
    CHECK(alpacacore::test::error_code_of([&] { c.halt_cover(); }) == 0);
    CHECK(rig.upstream.calls("haltcover") == 2);
    CHECK_FALSE(c.get_cover_moving());
    CHECK(c.get_cover_state() == CoverState::Moving);
    CHECK(c.get_link_fault().empty());
    state_silent.store(false);
    rig.poll();
    CHECK(c.get_cover_state() == CoverState::Unknown);
}

// Commands reach the upstream in caller order (UpstreamProxy's command order mutex): a HaltCover sent while an
// OpenCover's body is held before its PUT waits for it, so the cover never moves after the Halt.
TEST_CASE("Remote Alpaca CoverCalibrator Driver - a HaltCover sent during an OpenCover reaches the upstream after it",
          "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    rig.connect();
    alpacacore::test::Latch open_body;
    std::atomic<bool> held{false};  // only the first body (the OpenCover's) holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) open_body.block();
    });
    std::thread opener([&] { rig.driver->open_cover(); });
    REQUIRE(open_body.wait_entered());
    // The Halt's caller finds the OpenCover's command running and waits for it: the hook marks the moment.
    alpacacore::test::Latch halt_waits;
    remote::set_command_waiting_hook_for_testing(*rig.driver, [&] { halt_waits.block(); });
    std::thread halter([&] { rig.driver->halt_cover(); });
    CHECK(halt_waits.wait_entered());  // CHECK, not REQUIRE: the threads are joined below either way
    CHECK(rig.upstream.calls("haltcover") == 0);
    halt_waits.release();
    open_body.release();
    opener.join();
    halter.join();
    const auto puts = rig.puts();
    REQUIRE(puts.size() >= 2);
    CHECK(puts[puts.size() - 2] == "opencover");
    CHECK(puts.back() == "haltcover");
    CHECK_FALSE(rig.driver->get_cover_moving());
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - a part the upstream has NotPresent is never asked",
          "[remotealpaca][covercalibrator][unit]") {
    SECTION("no cover: CoverState NotPresent, CoverMoving false, the cover commands not implemented") {
        Rig rig;
        rig.upstream.set("coverstate", "0");
        rig.connect();
        auto& c = *rig.driver;
        CHECK(c.get_cover_state() == CoverState::NotPresent);
        CHECK_FALSE(c.get_cover_moving());
        const std::string no_cover = kLabel + " has no cover (its CoverState is NotPresent): ";
        CHECK(alpaca_error_message([&] { c.open_cover(); }, AlpacaError::MethodNotImplemented) ==
              no_cover + "OpenCover is not implemented");
        require_alpaca_error([&] { c.close_cover(); }, AlpacaError::MethodNotImplemented);
        require_alpaca_error([&] { c.halt_cover(); }, AlpacaError::MethodNotImplemented);
        rig.poll();
        CHECK(rig.upstream.calls("coverstate") == 1);  // the Connect's read only
        CHECK(rig.upstream.calls("covermoving") == 0);
        CHECK(rig.puts() == std::vector<std::string>{"connect"});
        // Static: NotPresent answers while the link is faulted too.
        rig.fault();
        CHECK(c.get_cover_state() == CoverState::NotPresent);
        CHECK_FALSE(c.get_cover_moving());
        CHECK(c.get_calibrator_state() == CalibratorState::Unknown);
    }
    SECTION("no calibrator: CalibratorState NotPresent, Brightness and the calibrator commands not implemented") {
        Rig rig;
        rig.upstream.set("calibratorstate", "0");
        rig.upstream.set_error("maxbrightness", 0x400, "MaxBrightness is not implemented");
        rig.connect();
        auto& c = *rig.driver;
        CHECK(c.get_calibrator_state() == CalibratorState::NotPresent);
        CHECK_FALSE(c.get_calibrator_changing());
        CHECK(alpaca_error_message([&] { (void)c.get_brightness(); }, AlpacaError::PropertyNotImplemented) ==
              kLabel + " has no calibrator (its CalibratorState is NotPresent): Brightness is not implemented");
        // MaxBrightness is the upstream's own answer, unchanged.
        CHECK(alpaca_error_message([&] { (void)c.get_max_brightness(); }, AlpacaError::PropertyNotImplemented) ==
              kPrefix + "MaxBrightness is not implemented");
        require_alpaca_error([&] { c.calibrator_on(10); }, AlpacaError::MethodNotImplemented);
        require_alpaca_error([&] { c.calibrator_off(); }, AlpacaError::MethodNotImplemented);
        rig.poll();
        CHECK(rig.upstream.calls("calibratorstate") == 1);  // the Connect's read only
        CHECK(rig.upstream.calls("calibratorchanging") == 0);
        CHECK(rig.upstream.calls("brightness") == 0);
        CHECK(rig.upstream.calls("calibratoron") == 0);
        // DeviceState omits Brightness; the cover is served.
        const auto state = c.get_device_state();
        REQUIRE(state.size() == 5);
        CHECK(state[0].name == "CalibratorState");
        CHECK(std::get<std::int32_t>(state[0].value) == static_cast<std::int32_t>(CalibratorState::NotPresent));
    }
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][covercalibrator][unit]") {
    SECTION("Platform 7 upstream (ICoverCalibratorV2): Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an ICoverCalibratorV1 upstream has no Connect: Connected") {
        Rig rig;
        rig.upstream.set("interfaceversion", "1");
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 0);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 0);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - State machine contracts", "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    auto& c = *rig.driver;
    CHECK_FALSE(c.get_connected());
    CHECK(c.get_device_state().empty());
    require_alpaca_error([&] { (void)c.get_cover_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)c.get_calibrator_changing(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)c.get_brightness(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { c.open_cover(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { c.halt_cover(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { c.calibrator_off(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);

    rig.connect();
    CHECK_FALSE(c.get_cover_moving());  // idle after Connect
    rig.driver->set_connected(true);    // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // A refused command is a lost link at once (decision 0009 point 2(a)): NotConnected, and HaltCover too.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { c.open_cover(); }, AlpacaError::NotConnected);
    CHECK_FALSE(c.get_connected());
    CHECK(c.get_device_state().empty());
    require_alpaca_error([&] { (void)c.get_cover_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { c.halt_cover(); }, AlpacaError::NotConnected);
    CHECK(c.get_link_fault().find("connection refused") != std::string::npos);
    // Nothing relinks by itself (decision 0009 rule 4): the poll ended.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    // Only the client's Connect relinks.
    rig.connect();
    CHECK(c.get_link_fault().empty());
    CHECK(c.get_cover_state() == CoverState::Closed);

    // Disconnected by the client: NotConnected again, and the poll ended.
    c.set_connected(false);
    require_alpaca_error([&] { (void)c.get_cover_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)c.get_max_brightness(); }, AlpacaError::NotConnected);
    const int after = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == after);
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - members it does not proxy or the upstream lacks are not implemented",
          "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    // An ICoverCalibratorV1 upstream has no CoverMoving or CalibratorChanging.
    rig.upstream.set("interfaceversion", "1");
    rig.connect();
    auto& c = *rig.driver;
    const std::string lacks = kLabel + " is an ICoverCalibratorV1 and has no ";
    CHECK(alpaca_error_message([&] { (void)c.get_cover_moving(); }, AlpacaError::NotImplemented) ==
          lacks + "CoverMoving");
    CHECK(alpaca_error_message([&] { (void)c.get_calibrator_changing(); }, AlpacaError::NotImplemented) ==
          lacks + "CalibratorChanging");
    // The commands still write the state through.
    c.open_cover();
    CHECK(c.get_cover_state() == CoverState::Moving);
    c.calibrator_on(20);
    CHECK(c.get_calibrator_state() == CalibratorState::NotReady);
    rig.poll();
    // Never asked of the upstream: an older server may answer an unknown member with HTTP 400, a failed poll.
    CHECK(rig.upstream.calls("covermoving") == 0);
    CHECK(rig.upstream.calls("calibratorchanging") == 0);
    CHECK(c.get_link_fault().empty());
    // DeviceState omits the members that throw.
    const auto state = c.get_device_state();
    REQUIRE(state.size() == 4);
    CHECK(state[0].name == "Brightness");
    CHECK(state[2].name == "CoverState");
    // Actions and Command* are not forwarded.
    CHECK(c.get_supported_actions().empty());
    CHECK_FALSE(c.can_action("anything"));
    require_alpaca_error([&] { (void)c.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)c.command_blind("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)c.command_bool("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)c.command_string("x", false); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("action") == 0);
    CHECK(rig.upstream.calls("commandblind") == 0);
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - an upstream command error passes through unchanged with its host",
          "[remotealpaca][covercalibrator][unit]") {
    Rig rig;
    rig.connect();
    rig.upstream.set_put_error("opencover", 0x40B, "the cover is blocked");
    CHECK(alpaca_error_message([&] { rig.driver->open_cover(); }, AlpacaError::InvalidOperation) ==
          kPrefix + "the cover is blocked");
    // A refused command wrote nothing through, and an answered error is not a transport failure: the link stays up.
    CHECK(rig.driver->get_cover_state() == CoverState::Closed);
    CHECK_FALSE(rig.driver->get_cover_moving());
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    // An upstream state error is kept in the frame and served unchanged; a number outside the enumeration is the
    // upstream's error too, not a state.
    rig.upstream.set_error("calibratorstate", 0x500, "calibrator offline");
    rig.upstream.set("coverstate", "7");
    rig.poll();
    CHECK(alpaca_error_message([&] { (void)rig.driver->get_calibrator_state(); }, AlpacaError::DriverException) ==
          kPrefix + "calibrator offline");
    CHECK(alpaca_error_message([&] { (void)rig.driver->get_cover_state(); }, AlpacaError::DriverException) ==
          kLabel + " answered coverstate 7, which is not an ASCOM state");
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("Remote Alpaca CoverCalibrator Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][covercalibrator][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig([] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
}
