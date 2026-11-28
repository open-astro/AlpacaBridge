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

// The remotealpaca Dome proxied device (decision 0023, issue #1753 slice 5, #2705) over the scripted FakeHttpClient
// and a scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision 0005). Fake
// evidence only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the driver's
// contract_sweep.h entry. The rules the Dome shares with the other proxied devices (the snapshot, the loss bound, the
// self-loop check) live in UpstreamProxy (upstream_proxy.h) and are covered at length by
// test_remotealpaca_observingconditions.cpp; the cases here pin what the Dome's own members do with each of them.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/dome_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_dome_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <limits>
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
using alpacacore::DomeDriver;
using alpacacore::test::alpaca_error_message;
using alpacacore::test::AlpacaUpstreamScript;
using alpacacore::test::error_code_of;
using alpacacore::test::require_alpaca_error;

constexpr std::chrono::milliseconds kRendezvous{5000};
constexpr int kUpstreamPort = 11111;
const std::string kPrefix = "127.0.0.1:11111: ";

// IDome ShutterState.
constexpr int kOpen = 0;
constexpr int kClosed = 1;
constexpr int kOpening = 2;
constexpr int kClosing = 3;

// The fake clock, the scripted upstream behind the fake HTTP client, and the driver, destroyed in reverse.
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::Dome};
    std::unique_ptr<DomeDriver> driver;

    explicit Rig(std::chrono::seconds poll = std::chrono::seconds(1), remote::ServerAddressSource self = {},
                 int device_number = 0) {
        upstream.install(http);
        driver = remote::create_remotealpaca_dome(device_number, config(poll), http.client(), std::move(self), clock);
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
    void poll(std::chrono::nanoseconds by = std::chrono::seconds(1)) {
        clock.advance(by);
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

    // The PUT members sent so far, in order.
    std::vector<std::string> puts() const {
        std::vector<std::string> out;
        for (const auto& request : http.requests()) {
            if (request.method == "PUT") out.push_back(request.url.substr(request.url.rfind('/') + 1));
        }
        return out;
    }
};

double nan() { return std::numeric_limits<double>::quiet_NaN(); }

}  // namespace

TEST_CASE("Remote Alpaca Dome Driver - Defaults", "[remotealpaca][dome][unit]") {
    Rig rig;
    auto& d = *rig.driver;
    REQUIRE(d.get_device_type() == alpacacore::DeviceType::Dome);
    REQUIRE(d.get_device_number() == 0);
    REQUIRE_FALSE(d.get_connected());
    CHECK_FALSE(d.get_connecting());
    CHECK(d.get_name() == "Remote Alpaca Dome");
    CHECK(d.get_link_fault().empty());
    CHECK(d.get_device_state().empty());
    // The capabilities answer while disconnected: false before any Connect has read the upstream's.
    CHECK_FALSE(d.get_can_find_home());
    CHECK_FALSE(d.get_can_park());
    CHECK_FALSE(d.get_can_set_altitude());
    CHECK_FALSE(d.get_can_set_azimuth());
    CHECK_FALSE(d.get_can_set_park());
    CHECK_FALSE(d.get_can_set_shutter());
    CHECK_FALSE(d.get_can_slave());
    CHECK_FALSE(d.get_can_sync_azimuth());
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca Dome Driver - Device metadata", "[remotealpaca][dome][unit]") {
    Rig rig(std::chrono::seconds(1), {}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "Dome of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca Dome Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 3);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_DOME_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_DOME_3");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Dome");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator Dome");
}

TEST_CASE("Remote Alpaca Dome Driver - the getters serve the snapshot and the Connect's capabilities, calling nothing",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(d.get_can_find_home());
    CHECK(d.get_can_park());
    CHECK(d.get_can_set_altitude());
    CHECK(d.get_can_set_azimuth());
    CHECK(d.get_can_set_park());
    CHECK(d.get_can_set_shutter());
    CHECK(d.get_can_slave());
    CHECK(d.get_can_sync_azimuth());
    CHECK_FALSE(d.get_slewing());
    CHECK(d.get_azimuth() == 100.0);
    CHECK(d.get_altitude() == 0.0);
    CHECK_FALSE(d.get_at_home());
    CHECK_FALSE(d.get_at_park());
    CHECK(d.get_shutter_status() == kClosed);
    CHECK_FALSE(d.get_slaved());
    const auto state = d.get_device_state();
    REQUIRE(state.size() == 7);  // Altitude, AtHome, AtPark, Azimuth, ShutterStatus, Slewing and TimeStamp
    CHECK(state[0].name == "Altitude");
    CHECK(state[3].name == "Azimuth");
    CHECK(std::get<double>(state[3].value) == 100.0);
    CHECK(state[4].name == "ShutterStatus");
    CHECK(std::get<std::int32_t>(state[4].value) == kClosed);
    CHECK(state[5].name == "Slewing");
    CHECK_FALSE(std::get<bool>(state[5].value));
    CHECK(state.back().name == "TimeStamp");
    CHECK(rig.http.calls() == calls_after_connect);

    // The capabilities are read once at Connect; a poll measures the seven frame members, one call each.
    rig.upstream.set("azimuth", "120.5");
    rig.upstream.set("slaved", "true");
    rig.upstream.set("canpark", "false");
    CHECK(d.get_azimuth() == 100.0);  // until the next poll the frame holds
    const int before_poll = rig.http.calls();
    rig.poll();
    CHECK(rig.http.calls() - before_poll == 7);
    CHECK(d.get_azimuth() == 120.5);
    CHECK(d.get_slaved());
    CHECK(d.get_can_park());
    CHECK(rig.upstream.calls("canpark") == 1);
    CHECK(rig.upstream.calls("cansyncazimuth") == 1);
}

// DeviceState's TimeStamp is when the frame was measured (decision 0010), through the dome base's
// device_state_age(): a frame the poll stopped refreshing carries its age.
TEST_CASE("Remote Alpaca Dome Driver - DeviceState is stamped with the frame's age", "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    remote::set_poll_paused_for_testing(*rig.driver, true);
    rig.poll(std::chrono::seconds(20));
    const auto stamp = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(*rig.driver));
    const auto age = std::chrono::system_clock::now() - stamp;
    CHECK(age >= std::chrono::seconds(19));
    CHECK(age <= std::chrono::seconds(25));
    remote::set_poll_paused_for_testing(*rig.driver, false);
}

// A member whose capability the Connect read as false is asked once there and never polled; its answer, a value or
// the upstream's own error, is served unchanged. A capability that answered an error keeps its member polled.
TEST_CASE("Remote Alpaca Dome Driver - a member whose capability is false is read once at Connect, not polled",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.upstream.set("cansetaltitude", "false");
    rig.upstream.set_error("altitude", 0x400, "Altitude is not implemented");
    rig.upstream.set("canslave", "false");
    rig.upstream.set_error("canfindhome", 0x400, "CanFindHome is not implemented");
    rig.connect();
    auto& d = *rig.driver;
    CHECK(alpaca_error_message([&] { (void)d.get_altitude(); }, AlpacaError::NotImplemented) ==
          kPrefix + "Altitude is not implemented");
    CHECK_FALSE(d.get_slaved());
    CHECK_FALSE(d.get_can_set_altitude());
    CHECK(alpaca_error_message([&] { (void)d.get_can_find_home(); }, AlpacaError::NotImplemented) ==
          kPrefix + "CanFindHome is not implemented");
    // Five calls a poll: Slewing, Azimuth, AtHome (its capability did not answer), AtPark and ShutterStatus.
    const int before_poll = rig.http.calls();
    const int at_home_reads = rig.upstream.calls("athome");
    rig.poll();
    CHECK(rig.http.calls() - before_poll == 5);
    rig.poll();
    CHECK(rig.upstream.calls("altitude") == 1);
    CHECK(rig.upstream.calls("slaved") == 1);
    CHECK(rig.upstream.calls("athome") == at_home_reads + 2);
    // DeviceState omits the member that throws.
    const auto state = d.get_device_state();
    REQUIRE(state.size() == 6);
    CHECK(state[0].name == "AtHome");
}

TEST_CASE(
    "Remote Alpaca Dome Driver - a slew is forwarded and Slewing reads true at once, until a poll says it stopped",
    "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    d.slew_to_azimuth(200.0);
    CHECK(rig.upstream.last_move() == "slewtoazimuth 200");
    // Written through: Slewing before any poll; Azimuth stays the measured one.
    CHECK(d.get_slewing());
    CHECK(d.get_azimuth() == 100.0);
    rig.poll();
    CHECK(d.get_slewing());  // the upstream still slews
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(d.get_slewing());
    CHECK(d.get_azimuth() == 200.0);
    // A slew to the frame's Azimuth still reads Slewing true: that Azimuth can be a poll period old (another client
    // or a hand paddle may have moved the dome), so only the upstream's next answer says it is still.
    d.slew_to_azimuth(200.0);
    CHECK(d.get_slewing());
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(d.get_slewing());
    // The altitude slew, as the azimuth one.
    d.slew_to_altitude(45.0);
    CHECK(rig.upstream.last_move() == "slewtoaltitude 45");
    CHECK(d.get_slewing());
    CHECK(d.get_altitude() == 0.0);
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(d.get_slewing());
    CHECK(d.get_altitude() == 45.0);
}

TEST_CASE("Remote Alpaca Dome Driver - FindHome and Park read Slewing true at once and AtHome or AtPark from the poll",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    d.find_home();
    CHECK(rig.upstream.last_move() == "findhome");
    CHECK(d.get_slewing());
    CHECK_FALSE(d.get_at_home());
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(d.get_slewing());
    CHECK(d.get_at_home());
    CHECK(d.get_azimuth() == 0.0);
    // Park while the frame says AtHome: Slewing is written through, AtHome stays the measured one until the poll.
    d.park();
    CHECK(rig.upstream.last_move() == "park");
    CHECK(d.get_slewing());
    CHECK(d.get_at_home());
    rig.poll();
    CHECK_FALSE(d.get_at_home());
    CHECK_FALSE(d.get_at_park());
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(d.get_slewing());
    CHECK(d.get_at_park());
    CHECK(d.get_azimuth() == 180.0);
}

TEST_CASE("Remote Alpaca Dome Driver - the shutter commands write the transition through, never the end state",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    d.open_shutter();
    CHECK(rig.upstream.calls("openshutter") == 1);
    CHECK(d.get_shutter_status() == kOpening);
    CHECK_FALSE(d.get_slewing());  // the shutter command writes only ShutterStatus
    rig.poll();
    CHECK(d.get_shutter_status() == kOpening);  // the upstream still opens it
    rig.upstream.finish_shutter();
    rig.poll();
    CHECK(d.get_shutter_status() == kOpen);
    // OpenShutter on a shutter the frame has Open still reads Opening: the frame can be a poll period old (another
    // client may have closed it), so only the upstream's next answer says it is open.
    d.open_shutter();
    CHECK(d.get_shutter_status() == kOpening);
    rig.poll();
    CHECK(d.get_shutter_status() == kOpening);
    rig.upstream.finish_shutter();
    rig.poll();
    CHECK(d.get_shutter_status() == kOpen);
    d.close_shutter();
    CHECK(d.get_shutter_status() == kClosing);
    rig.upstream.finish_shutter();
    rig.poll();
    CHECK(d.get_shutter_status() == kClosed);
}

TEST_CASE("Remote Alpaca Dome Driver - AbortSlew writes nothing: Slewing reads true until a poll after the stop",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    d.set_slaved(true);
    d.slew_to_azimuth(300.0);
    REQUIRE(d.get_slewing());
    d.abort_slew();
    CHECK(rig.upstream.calls("abortslew") == 1);
    // The upstream has stopped (and ended Slaved), but a dome coasts: only a poll after the stop reads it settled.
    CHECK(d.get_slewing());
    CHECK(d.get_slaved());
    rig.poll();
    CHECK_FALSE(d.get_slewing());
    CHECK_FALSE(d.get_slaved());
}

TEST_CASE("Remote Alpaca Dome Driver - AbortSlew is forwarded while the link is faulted, the other commands are not",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    d.slew_to_azimuth(200.0);
    REQUIRE(d.get_slewing());
    rig.fault();
    // Faulted: the frame is refused with DriverException, never a stale "not slewing"; the capabilities still answer.
    CHECK(alpaca_error_message([&] { (void)d.get_slewing(); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    require_alpaca_error([&] { (void)d.get_azimuth(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)d.get_altitude(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)d.get_at_home(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)d.get_at_park(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)d.get_shutter_status(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)d.get_slaved(); }, AlpacaError::DriverException);
    CHECK(d.get_device_state().empty());
    CHECK(d.get_can_set_azimuth());
    CHECK(d.get_can_park());
    // The safety stop goes out; the commands that need the frame are refused and send nothing.
    const std::size_t puts_before = rig.puts().size();
    d.abort_slew();
    CHECK(rig.upstream.calls("abortslew") == 1);
    REQUIRE(rig.puts().size() == puts_before + 1);
    require_alpaca_error([&] { d.slew_to_azimuth(10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { d.slew_to_altitude(10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { d.sync_to_azimuth(10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { d.find_home(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { d.park(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { d.set_park(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { d.open_shutter(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { d.close_shutter(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { d.set_slaved(true); }, AlpacaError::DriverException);
    CHECK(rig.puts().size() == puts_before + 1);
    // The next good poll clears the fault: the upstream stopped.
    rig.poll();
    CHECK(d.get_link_fault().empty());
    CHECK_FALSE(d.get_slewing());
}

// Commands reach the upstream in caller order (UpstreamProxy's command order mutex): an AbortSlew sent while a slew's
// body is held before its PUT waits for that slew, so the dome never starts the slew after the stop.
TEST_CASE("Remote Alpaca Dome Driver - an AbortSlew sent during a slew reaches the upstream after the slew",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    alpacacore::test::Latch slew_body;
    std::atomic<bool> held{false};  // only the first body (the slew's) holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) slew_body.block();
    });
    std::thread slewer([&] { rig.driver->slew_to_azimuth(200.0); });
    REQUIRE(slew_body.wait_entered());
    // The AbortSlew's caller finds the slew's command running and waits for it: the hook marks the moment.
    alpacacore::test::Latch abort_waits;
    remote::set_command_waiting_hook_for_testing(*rig.driver, [&] { abort_waits.block(); });
    std::thread aborter([&] { rig.driver->abort_slew(); });
    CHECK(abort_waits.wait_entered());  // CHECK, not REQUIRE: the threads are joined below either way
    CHECK(rig.upstream.calls("abortslew") == 0);
    abort_waits.release();
    slew_body.release();
    slewer.join();
    aborter.join();
    const auto puts = rig.puts();
    REQUIRE(puts.size() >= 2);
    CHECK(puts[puts.size() - 2] == "slewtoazimuth");
    CHECK(puts.back() == "abortslew");
    rig.poll();
    CHECK_FALSE(rig.driver->get_slewing());  // the upstream took the stop last
}

TEST_CASE("Remote Alpaca Dome Driver - a command that races a Disconnect never reaches the upstream; AbortSlew does",
          "[remotealpaca][dome][unit]") {
    SECTION("a slew whose body runs, and a shutter command queued behind it, are refused once the Disconnect began") {
        Rig rig;
        rig.connect();
        alpacacore::test::Latch slew_body;
        std::atomic<bool> held{false};  // only the first body (the slew's) holds
        remote::set_command_hook_for_testing(*rig.driver, [&] {
            if (!held.exchange(true)) slew_body.block();
        });
        std::atomic<int> slew_code{-2};
        std::thread slewer(
            [&] { slew_code = alpacacore::test::worker_code_of([&] { rig.driver->slew_to_azimuth(200.0); }); });
        CHECK(slew_body.wait_entered());  // CHECK, not REQUIRE: the threads are joined below either way
        alpacacore::test::Latch open_waits;
        remote::set_command_waiting_hook_for_testing(*rig.driver, [&] { open_waits.block(); });
        std::atomic<int> open_code{-2};
        std::thread opener([&] { open_code = alpacacore::test::worker_code_of([&] { rig.driver->open_shutter(); }); });
        CHECK(open_waits.wait_entered());
        open_waits.release();
        alpacacore::test::Latch disconnect_began;
        remote::set_disconnect_hook_for_testing(*rig.driver, [&] { disconnect_began.block(); });
        std::thread disconnecter([&] { rig.driver->set_connected(false); });
        CHECK(disconnect_began.wait_entered());
        disconnect_began.release();
        slew_body.release();
        slewer.join();
        opener.join();
        disconnecter.join();
        CHECK(slew_code.load() == AlpacaError::NotConnected);
        // Refused in its body, or by the slot the Disconnect is joining (InvalidOperation, "try again").
        CHECK((open_code.load() == AlpacaError::NotConnected || open_code.load() == AlpacaError::InvalidOperation));
        CHECK(rig.upstream.calls("slewtoazimuth") == 0);
        CHECK(rig.upstream.calls("openshutter") == 0);
        CHECK_FALSE(rig.driver->get_connected());
    }
    SECTION("an AbortSlew sent while the Disconnect runs is still forwarded") {
        Rig rig;
        rig.connect();
        alpacacore::test::Latch disconnect_began;
        remote::set_disconnect_hook_for_testing(*rig.driver, [&] { disconnect_began.block(); });
        std::thread disconnecter([&] { rig.driver->set_connected(false); });
        CHECK(disconnect_began.wait_entered());
        const int code = error_code_of([&] { rig.driver->abort_slew(); });
        disconnect_began.release();
        disconnecter.join();
        CHECK(code == 0);
        CHECK(rig.upstream.calls("abortslew") == 1);
    }
}

TEST_CASE("Remote Alpaca Dome Driver - a slew or shutter command with no reply reads its in-progress state",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    SECTION("a slew") {
        rig.http.fail_next(1);  // the PUT gets no usable reply: the upstream may have started the slew
        CHECK(error_code_of([&] { d.slew_to_azimuth(200.0); }) == AlpacaError::DriverException);
        CHECK(d.get_slewing());
        rig.poll();
        CHECK_FALSE(d.get_slewing());  // the upstream never took it
    }
    SECTION("a shutter command") {
        rig.http.fail_next(1);
        CHECK(error_code_of([&] { d.open_shutter(); }) == AlpacaError::DriverException);
        CHECK(d.get_shutter_status() == kOpening);
        rig.poll();
        CHECK(d.get_shutter_status() == kClosed);
    }
    SECTION("an upstream error answer writes nothing") {
        rig.upstream.set_put_error("slewtoazimuth", 0x401, "Azimuth out of range");
        CHECK(error_code_of([&] { d.slew_to_azimuth(200.0); }) == AlpacaError::InvalidValue);
        CHECK_FALSE(d.get_slewing());
    }
}

TEST_CASE("Remote Alpaca Dome Driver - SyncToAzimuth reads the synced Azimuth back from the upstream",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    std::atomic<bool> fail_read{false};
    rig.http.set_responder([&](const alpacacore::test::FakeHttpClient::Request& request) {
        auto reply = rig.upstream.answer(request);
        if (request.method == "PUT" && request.url.find("/synctoazimuth") != std::string::npos) {
            rig.upstream.set("azimuth", "329.5");  // the upstream's own reading of the synced angle
            if (fail_read.load()) rig.http.fail_next(1);
        }
        return reply;
    });
    rig.connect();
    auto& d = *rig.driver;
    SECTION("the upstream's answer is written through, not this server's own wrap of the angle") {
        d.sync_to_azimuth(-30.0);
        CHECK(d.get_azimuth() == 329.5);
    }
    SECTION("a read-back with no reply leaves the frame to the next poll and does not fail the sync") {
        fail_read.store(true);
        CHECK(error_code_of([&] { d.sync_to_azimuth(-30.0); }) == 0);
        CHECK(d.get_azimuth() == 100.0);
        rig.poll();
        CHECK(d.get_azimuth() == 329.5);
    }
}

TEST_CASE("Remote Alpaca Dome Driver - SyncToAzimuth, SetPark and Slaved are forwarded and written through",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    // SyncToAzimuth: the upstream takes the new azimuth; Azimuth reads the synced angle before any poll.
    d.sync_to_azimuth(-30.0);
    CHECK(rig.upstream.calls("synctoazimuth") == 1);
    CHECK(d.get_azimuth() == 330.0);
    CHECK_FALSE(d.get_slewing());
    rig.poll();
    CHECK(d.get_azimuth() == 330.0);  // the upstream holds it
    // SetPark: forwarded; the next Park goes there.
    d.set_park();
    CHECK(rig.upstream.calls("setpark") == 1);
    d.park();
    rig.upstream.finish_move();
    rig.poll();
    CHECK(d.get_at_park());
    CHECK(d.get_azimuth() == 330.0);
    // Slaved: written through, and the upstream holds it.
    d.set_slaved(true);
    CHECK(d.get_slaved());
    rig.poll();
    CHECK(d.get_slaved());
    d.set_slaved(false);
    CHECK_FALSE(d.get_slaved());
    // An upstream that cannot slave refuses the write with its own error, unchanged, and nothing is written.
    rig.upstream.set_put_error("slaved", 0x400, "Slaved cannot be set");
    CHECK(alpaca_error_message([&] { d.set_slaved(true); }, AlpacaError::NotImplemented) ==
          kPrefix + "Slaved cannot be set");
    CHECK_FALSE(d.get_slaved());
}

TEST_CASE("Remote Alpaca Dome Driver - Value range validation", "[remotealpaca][dome][unit]") {
    SECTION("a non-finite angle is InvalidValue before NotConnected, and sends nothing") {
        Rig rig;
        auto& d = *rig.driver;
        const double inf = std::numeric_limits<double>::infinity();
        require_alpaca_error([&] { d.slew_to_azimuth(nan()); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { d.slew_to_azimuth(inf); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { d.slew_to_altitude(-inf); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { d.sync_to_azimuth(nan()); }, AlpacaError::InvalidValue);
        rig.connect();
        require_alpaca_error([&] { d.slew_to_altitude(nan()); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { d.sync_to_azimuth(inf); }, AlpacaError::InvalidValue);
        CHECK(rig.puts().size() == 1);  // the Connect's own PUT only
    }
    SECTION("a finite angle goes to the upstream as given, and its InvalidValue passes through unchanged") {
        Rig rig;
        rig.connect();
        rig.upstream.set_put_error("slewtoazimuth", 0x401, "Azimuth 400 is outside 0 to 360");
        CHECK(alpaca_error_message([&] { rig.driver->slew_to_azimuth(400.0); }, AlpacaError::InvalidValue) ==
              kPrefix + "Azimuth 400 is outside 0 to 360");
        CHECK(rig.upstream.calls("slewtoazimuth") == 1);
        CHECK_FALSE(rig.driver->get_slewing());  // a refused slew writes nothing through
        rig.upstream.set_put_error("slewtoaltitude", 0x401, "Altitude 91 is outside 0 to 90");
        CHECK(alpaca_error_message([&] { rig.driver->slew_to_altitude(91.0); }, AlpacaError::InvalidValue) ==
              kPrefix + "Altitude 91 is outside 0 to 90");
        CHECK_FALSE(rig.driver->get_slewing());
    }
    SECTION("the factory refuses each config field out of range before any network call") {
        alpacacore::test::FakeTaskClock clock;
        alpacacore::test::FakeHttpClient http;
        for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_dome(0, Rig::config(bad), http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        for (const int port : {0, 65536}) {
            remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
            config.port = port;
            require_alpaca_error([&] { (void)remote::create_remotealpaca_dome(0, config, http.client(), {}, clock); },
                                 AlpacaError::InvalidValue);
        }
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.upstream_device_number = -1;
        require_alpaca_error([&] { (void)remote::create_remotealpaca_dome(0, config, http.client(), {}, clock); },
                             AlpacaError::InvalidValue);
        CHECK(http.calls() == 0);
    }
}

TEST_CASE("Remote Alpaca Dome Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][dome][unit]") {
    SECTION("Platform 7 upstream (IDomeV3): Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an IDomeV2 upstream has no Connect: Connected") {
        Rig rig;
        rig.upstream.set("interfaceversion", "2");
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 0);
        CHECK(rig.upstream.upstream_connected());
        CHECK(rig.driver->get_azimuth() == 100.0);
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 0);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
}

TEST_CASE("Remote Alpaca Dome Driver - State machine contracts", "[remotealpaca][dome][unit]") {
    Rig rig;
    auto& d = *rig.driver;
    CHECK_FALSE(d.get_connected());
    CHECK(d.get_device_state().empty());
    require_alpaca_error([&] { (void)d.get_slewing(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)d.get_shutter_status(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)d.get_slaved(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { d.slew_to_azimuth(10.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { d.open_shutter(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { d.set_park(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { d.abort_slew(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);

    rig.connect();
    CHECK_FALSE(d.get_slewing());     // idle after Connect
    rig.driver->set_connected(true);  // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // A refused slew is a lost link at once (decision 0009 point 2(a)): NotConnected, and AbortSlew too.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { d.slew_to_azimuth(10.0); }, AlpacaError::NotConnected);
    CHECK_FALSE(d.get_connected());
    CHECK(d.get_device_state().empty());
    require_alpaca_error([&] { (void)d.get_slewing(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { d.abort_slew(); }, AlpacaError::NotConnected);
    CHECK(d.get_link_fault().find("connection refused") != std::string::npos);
    // Nothing relinks by itself (decision 0009 rule 4): the poll ended.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    // Only the client's Connect relinks.
    rig.connect();
    CHECK(d.get_link_fault().empty());
    CHECK(d.get_azimuth() == 100.0);

    // Disconnected by the client: NotConnected again, the capabilities keep the last Connect's, and the poll ended.
    d.set_connected(false);
    require_alpaca_error([&] { (void)d.get_azimuth(); }, AlpacaError::NotConnected);
    CHECK(d.get_can_set_shutter());
    const int after = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == after);
}

// A dome moves hardware: across a link that fails mid-slew, Slewing never reads false before an upstream answer
// measured after the failure says so. Faulted, it throws DriverException; lost, NotConnected; the relink's Connect
// measures a fresh frame, which still has the upstream slewing, and nothing is stopped on the relink.
TEST_CASE("Remote Alpaca Dome Driver - a link lost mid-slew never reads Slewing false until the upstream says so",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    auto& d = *rig.driver;
    d.slew_to_azimuth(250.0);
    REQUIRE(d.get_slewing());
    // Every call fails until the loss bound: max(30 s, 3 x 1 s) of a standing fault.
    rig.http.fail_next(1000);
    bool lost = false;
    for (int second = 0; second < 40 && !lost; ++second) {
        rig.poll();
        bool slewing = true;
        const int code = alpacacore::test::error_code_of([&] { slewing = d.get_slewing(); });
        INFO("second " << second << ": code " << code);
        // Before the latch the written-through true is served; then DriverException; then NotConnected.
        CHECK((code == 0 || code == AlpacaError::DriverException || code == AlpacaError::NotConnected));
        CHECK(slewing);
        lost = code == AlpacaError::NotConnected;
    }
    REQUIRE(lost);
    CHECK_FALSE(d.get_connected());
    CHECK(d.get_device_state().empty());
    // The upstream answers again and still slews: the relink serves its answer, and sends no stop.
    rig.http.fail_next(0);
    rig.connect();
    CHECK(d.get_slewing());
    CHECK(rig.upstream.calls("abortslew") == 0);
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(d.get_slewing());
    CHECK(d.get_azimuth() == 250.0);
}

TEST_CASE("Remote Alpaca Dome Driver - members it does not proxy or the upstream lacks are not implemented",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    // A roll-off roof: no azimuth or altitude control, no slaving, no home; its upstream answers those with 0x400.
    for (const char* can : {"cansetazimuth", "cansetaltitude", "canslave", "canfindhome", "cansyncazimuth"}) {
        rig.upstream.set(can, "false");
    }
    rig.upstream.set_error("azimuth", 0x400, "Azimuth is not implemented");
    rig.upstream.set_error("athome", 0x400, "AtHome is not implemented");
    rig.upstream.set_put_error("slewtoazimuth", 0x400, "SlewToAzimuth is not implemented");
    rig.upstream.set_put_error("findhome", 0x400, "FindHome is not implemented");
    rig.connect();  // the errors are the upstream's answers: the Connect stands
    auto& d = *rig.driver;
    CHECK(alpaca_error_message([&] { (void)d.get_azimuth(); }, AlpacaError::NotImplemented) ==
          kPrefix + "Azimuth is not implemented");
    CHECK(alpaca_error_message([&] { (void)d.get_at_home(); }, AlpacaError::NotImplemented) ==
          kPrefix + "AtHome is not implemented");
    // The upstream judges each command: its NotImplemented passes through unchanged, and nothing is written.
    CHECK(alpaca_error_message([&] { d.slew_to_azimuth(10.0); }, AlpacaError::NotImplemented) ==
          kPrefix + "SlewToAzimuth is not implemented");
    CHECK(alpaca_error_message([&] { d.find_home(); }, AlpacaError::NotImplemented) ==
          kPrefix + "FindHome is not implemented");
    CHECK_FALSE(d.get_slewing());
    CHECK(d.get_link_fault().empty());
    // The roof still opens.
    d.open_shutter();
    CHECK(d.get_shutter_status() == kOpening);
    // Actions and Command* are not forwarded.
    CHECK(d.get_supported_actions().empty());
    CHECK_FALSE(d.can_action("anything"));
    require_alpaca_error([&] { (void)d.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)d.command_blind("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)d.command_bool("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)d.command_string("x", false); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("action") == 0);
    CHECK(rig.upstream.calls("commandblind") == 0);
}

TEST_CASE("Remote Alpaca Dome Driver - an upstream command error passes through unchanged with its host",
          "[remotealpaca][dome][unit]") {
    Rig rig;
    rig.connect();
    rig.upstream.set_put_error("openshutter", 0x40B, "the dome is parked");
    CHECK(alpaca_error_message([&] { rig.driver->open_shutter(); }, AlpacaError::InvalidOperation) ==
          kPrefix + "the dome is parked");
    // A refused command wrote nothing through, and an answered error is not a transport failure: the link stays up.
    CHECK(rig.driver->get_shutter_status() == kClosed);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("Remote Alpaca Dome Driver - Connect refuses an upstream that is this server", "[remotealpaca][dome][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
}
