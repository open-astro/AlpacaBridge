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

// The remotealpaca Rotator proxied device (decision 0023, issue #1753 slice 5, #2708) over the scripted
// FakeHttpClient and a scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision
// 0005). Fake evidence only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the
// driver's contract_sweep.h entry. The rules the Rotator shares with the other proxied devices (the snapshot, the loss
// bound, the self-loop check) live in UpstreamProxy (upstream_proxy.h) and are covered at length by
// test_remotealpaca_observingconditions.cpp; the cases here pin what the Rotator's own members do with each of them.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_rotator_driver.h>
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
using alpacacore::RotatorDriver;
using alpacacore::test::alpaca_error_message;
using alpacacore::test::AlpacaUpstreamScript;
using alpacacore::test::require_alpaca_error;

constexpr std::chrono::milliseconds kRendezvous{5000};
constexpr int kUpstreamPort = 11111;
const std::string kPrefix = "127.0.0.1:11111: ";

// The fake clock, the scripted upstream behind the fake HTTP client, and the driver, destroyed in reverse.
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::Rotator};
    std::unique_ptr<RotatorDriver> driver;

    explicit Rig(std::chrono::seconds poll = std::chrono::seconds(1), remote::ServerAddressSource self = {},
                 int device_number = 0) {
        upstream.install(http);
        driver =
            remote::create_remotealpaca_rotator(device_number, config(poll), http.client(), std::move(self), clock);
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
};

double nan() { return std::numeric_limits<double>::quiet_NaN(); }

}  // namespace

TEST_CASE("Remote Alpaca Rotator Driver - Defaults", "[remotealpaca][rotator][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::Rotator);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca Rotator");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    // CanReverse answers while disconnected: true before any Connect has read the upstream's (IRotatorV3 and later
    // must answer true). StepSize is the upstream's, read at Connect, so it is NotConnected.
    CHECK(rig.driver->get_can_reverse());
    require_alpaca_error([&] { (void)rig.driver->get_step_size(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca Rotator Driver - Device metadata", "[remotealpaca][rotator][unit]") {
    Rig rig(std::chrono::seconds(1), {}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "Rotator of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca Rotator Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 4);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_ROT_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_ROT_3");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Rotator");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator Rotator");
}

TEST_CASE("Remote Alpaca Rotator Driver - the getters serve the snapshot and the Connect's limits, calling nothing",
          "[remotealpaca][rotator][unit]") {
    Rig rig;
    rig.connect();
    auto& r = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(r.get_can_reverse());
    CHECK(r.get_step_size() == 0.5);
    CHECK_FALSE(r.get_reverse());
    CHECK_FALSE(r.get_is_moving());
    CHECK(r.get_position() == 100.0);
    CHECK(r.get_mechanical_position() == 90.0);
    CHECK(r.get_target_position() == 100.0);
    const auto state = r.get_device_state();
    REQUIRE(state.size() == 4);  // IsMoving, MechanicalPosition, Position and TimeStamp
    CHECK(state[0].name == "IsMoving");
    CHECK_FALSE(std::get<bool>(state[0].value));
    CHECK(state[1].name == "MechanicalPosition");
    CHECK(std::get<double>(state[1].value) == 90.0);
    CHECK(state[2].name == "Position");
    CHECK(std::get<double>(state[2].value) == 100.0);
    CHECK(state.back().name == "TimeStamp");
    CHECK(rig.http.calls() == calls_after_connect);

    // CanReverse and StepSize are read once at Connect; the next poll measures the frame members only.
    CHECK(rig.upstream.calls("stepsize") == 1);
    rig.upstream.set("position", "120");
    rig.upstream.set("reverse", "true");
    rig.upstream.set("stepsize", "1.0");
    CHECK(r.get_position() == 100.0);  // until the next poll the frame holds
    rig.poll();
    CHECK(r.get_position() == 120.0);
    CHECK(r.get_reverse());
    CHECK(r.get_step_size() == 0.5);
    CHECK(rig.upstream.calls("stepsize") == 1);
    CHECK(rig.upstream.calls("canreverse") == 1);
}

// DeviceState's TimeStamp is when the frame was measured (decision 0010), through the rotator base's
// device_state_age(): a frame the poll stopped refreshing carries its age.
TEST_CASE("Remote Alpaca Rotator Driver - DeviceState is stamped with the frame's age",
          "[remotealpaca][rotator][unit]") {
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

TEST_CASE("Remote Alpaca Rotator Driver - MoveAbsolute is forwarded and IsMoving and TargetPosition read at once",
          "[remotealpaca][rotator][unit]") {
    Rig rig;
    rig.connect();
    auto& r = *rig.driver;
    r.move_absolute(200.0);
    CHECK(rig.upstream.last_move() == "moveabsolute 200");
    // Written through: IsMoving and TargetPosition before any poll; Position stays the measured one.
    CHECK(r.get_is_moving());
    CHECK(r.get_target_position() == 200.0);
    CHECK(r.get_position() == 100.0);
    rig.poll();
    CHECK(r.get_is_moving());  // the upstream still moves
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(r.get_is_moving());
    CHECK(r.get_position() == 200.0);
    CHECK(r.get_mechanical_position() == 190.0);
    // A MoveAbsolute to where the frame already has it has nowhere to go.
    r.move_absolute(200.0);
    CHECK_FALSE(r.get_is_moving());
    CHECK(rig.upstream.calls("moveabsolute") == 2);
}

TEST_CASE("Remote Alpaca Rotator Driver - Move and MoveMechanical forward the angle and write the target through",
          "[remotealpaca][rotator][unit]") {
    Rig rig;
    rig.connect();
    auto& r = *rig.driver;
    // Relative to Position 100: the target is 100 + 300, wrapped.
    r.move(300.0);
    CHECK(rig.upstream.last_move() == "move 300");
    CHECK(r.get_is_moving());
    CHECK(r.get_target_position() == 40.0);
    rig.upstream.finish_move();
    rig.poll();
    CHECK(r.get_position() == 40.0);
    CHECK(r.get_mechanical_position() == 30.0);
    // A raw mechanical angle: the target is it plus the Sync offset (Position 40 less MechanicalPosition 30).
    r.move_mechanical(50.0);
    CHECK(rig.upstream.last_move() == "movemechanical 50");
    CHECK(r.get_is_moving());
    CHECK(r.get_target_position() == 60.0);
    rig.upstream.finish_move();
    rig.poll();
    CHECK(r.get_mechanical_position() == 50.0);
    CHECK(r.get_position() == 60.0);
    // A relative Move of 0 has nowhere to go.
    r.move(0.0);
    CHECK_FALSE(r.get_is_moving());
}

TEST_CASE("Remote Alpaca Rotator Driver - Value range validation", "[remotealpaca][rotator][unit]") {
    SECTION("a non-finite angle is InvalidValue before NotConnected, and sends nothing") {
        Rig rig;
        auto& r = *rig.driver;
        const double inf = std::numeric_limits<double>::infinity();
        require_alpaca_error([&] { r.move(nan()); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { r.move_absolute(inf); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { r.move_mechanical(-inf); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { r.sync(nan()); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { r.set_target_position(nan()); }, AlpacaError::InvalidValue);
        rig.connect();
        require_alpaca_error([&] { r.move_absolute(nan()); }, AlpacaError::InvalidValue);
        CHECK(rig.upstream.calls("moveabsolute") == 0);
    }
    SECTION("a finite angle goes to the upstream as given, and its InvalidValue passes through unchanged") {
        Rig rig;
        rig.connect();
        rig.upstream.set_put_error("moveabsolute", 0x401, "MoveAbsolute 400 is outside 0 to 360");
        CHECK(alpaca_error_message([&] { rig.driver->move_absolute(400.0); }, AlpacaError::InvalidValue) ==
              kPrefix + "MoveAbsolute 400 is outside 0 to 360");
        CHECK(rig.upstream.calls("moveabsolute") == 1);
        CHECK_FALSE(rig.driver->get_is_moving());  // a refused move writes nothing through
    }
    SECTION("the factory refuses each config field out of range before any network call") {
        alpacacore::test::FakeTaskClock clock;
        alpacacore::test::FakeHttpClient http;
        for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_rotator(0, Rig::config(bad), http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        for (const int port : {0, 65536}) {
            remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
            config.port = port;
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_rotator(0, config, http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.upstream_device_number = -1;
        require_alpaca_error([&] { (void)remote::create_remotealpaca_rotator(0, config, http.client(), {}, clock); },
                             AlpacaError::InvalidValue);
        CHECK(http.calls() == 0);
    }
}

TEST_CASE("Remote Alpaca Rotator Driver - Halt is forwarded while the link is faulted, the other commands are not",
          "[remotealpaca][rotator][unit]") {
    Rig rig;
    rig.connect();
    auto& r = *rig.driver;
    r.move_absolute(200.0);
    REQUIRE(r.get_is_moving());
    rig.fault();
    // Faulted: the frame is refused with DriverException, the Connect's reads still answer (static metadata).
    CHECK(alpaca_error_message([&] { (void)r.get_is_moving(); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    require_alpaca_error([&] { (void)r.get_position(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)r.get_mechanical_position(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)r.get_target_position(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)r.get_reverse(); }, AlpacaError::DriverException);
    CHECK(r.get_step_size() == 0.5);
    CHECK(r.get_can_reverse());
    // The safety stop goes out; the commands that need the frame are refused and send nothing.
    const int puts_before = rig.upstream.calls("move") + rig.upstream.calls("moveabsolute") +
                            rig.upstream.calls("movemechanical") + rig.upstream.calls("sync");
    const int reverses = rig.upstream.calls("reverse");  // the polls' GETs so far
    r.halt();
    CHECK(rig.upstream.calls("halt") == 1);
    require_alpaca_error([&] { r.move(10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { r.move_absolute(10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { r.move_mechanical(10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { r.sync(10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { r.set_reverse(true); }, AlpacaError::DriverException);
    CHECK(rig.upstream.calls("move") + rig.upstream.calls("moveabsolute") + rig.upstream.calls("movemechanical") +
              rig.upstream.calls("sync") ==
          puts_before);
    CHECK(rig.upstream.calls("reverse") == reverses);
    // The next good poll clears the fault: the upstream stopped.
    rig.poll();
    CHECK(r.get_link_fault().empty());
    CHECK_FALSE(r.get_is_moving());
}

// Decision 0006 rule 6: a Halt supersedes a move whose body has not yet written IsMoving through, so the move's late
// write-through cannot make IsMoving read true after the Halt.
TEST_CASE("Remote Alpaca Rotator Driver - a Halt supersedes a move's write-through", "[remotealpaca][rotator][unit]") {
    Rig rig;
    rig.connect();
    alpacacore::test::Latch move_body;
    std::atomic<bool> held{false};  // only the first body (the move's) holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) move_body.block();
    });
    std::thread mover([&] { rig.driver->move_absolute(200.0); });
    REQUIRE(move_body.wait_entered());
    rig.driver->halt();
    move_body.release();
    mover.join();
    CHECK(rig.upstream.calls("moveabsolute") == 1);
    CHECK(rig.upstream.calls("halt") == 1);
    CHECK_FALSE(rig.driver->get_is_moving());
    CHECK(rig.driver->get_target_position() == 100.0);  // the superseded body wrote no target either
}

TEST_CASE("Remote Alpaca Rotator Driver - Sync and Reverse are forwarded and written through",
          "[remotealpaca][rotator][unit]") {
    Rig rig;
    rig.connect();
    auto& r = *rig.driver;
    // Sync: the upstream keeps the new offset; Position reads the synced angle before any poll.
    r.sync(-30.0);
    CHECK(rig.upstream.calls("sync") == 1);
    CHECK(r.get_position() == 330.0);
    CHECK(r.get_mechanical_position() == 90.0);
    rig.poll();
    CHECK(r.get_position() == 330.0);  // the upstream holds it
    CHECK(r.get_mechanical_position() == 90.0);
    // Reverse: written through, and the upstream holds it.
    const int before = rig.upstream.calls("reverse");
    r.set_reverse(true);
    CHECK(rig.upstream.calls("reverse") == before + 1);
    CHECK(r.get_reverse());
    rig.poll();
    CHECK(r.get_reverse());
    // An upstream that cannot reverse refuses the write with its own error, unchanged.
    rig.upstream.set_put_error("reverse", 0x400, "Reverse is not implemented");
    CHECK(alpaca_error_message([&] { r.set_reverse(false); }, AlpacaError::NotImplemented) ==
          kPrefix + "Reverse is not implemented");
    CHECK(r.get_reverse());
}

TEST_CASE("Remote Alpaca Rotator Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][rotator][unit]") {
    SECTION("Platform 7 upstream (IRotatorV4): Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an IRotatorV3 upstream has no Connect: Connected, and it keeps MechanicalPosition") {
        Rig rig;
        rig.upstream.set("interfaceversion", "3");
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 0);
        CHECK(rig.upstream.upstream_connected());
        CHECK(rig.driver->get_mechanical_position() == 90.0);
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 0);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
}

TEST_CASE("Remote Alpaca Rotator Driver - State machine contracts", "[remotealpaca][rotator][unit]") {
    Rig rig;
    auto& r = *rig.driver;
    CHECK_FALSE(r.get_connected());
    CHECK(r.get_device_state().empty());
    require_alpaca_error([&] { (void)r.get_is_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)r.get_mechanical_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { r.move_absolute(10.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { r.move_mechanical(10.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { r.sync(10.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { r.halt(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);

    rig.connect();
    CHECK_FALSE(r.get_is_moving());   // idle after Connect
    rig.driver->set_connected(true);  // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // A refused move is a lost link at once (decision 0009 point 2(a)): NotConnected, and Halt too.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { r.move_absolute(10.0); }, AlpacaError::NotConnected);
    CHECK_FALSE(r.get_connected());
    CHECK(r.get_device_state().empty());
    require_alpaca_error([&] { r.halt(); }, AlpacaError::NotConnected);
    CHECK(r.get_link_fault().find("connection refused") != std::string::npos);
    // Nothing relinks by itself (decision 0009 rule 4): the poll ended.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    // Only the client's Connect relinks.
    rig.connect();
    CHECK(r.get_link_fault().empty());
    CHECK(r.get_position() == 100.0);

    // Disconnected by the client: NotConnected again, and the poll ended.
    r.set_connected(false);
    require_alpaca_error([&] { (void)r.get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)r.get_step_size(); }, AlpacaError::NotConnected);
    const int after = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == after);
}

TEST_CASE("Remote Alpaca Rotator Driver - members it does not proxy or the upstream lacks are not implemented",
          "[remotealpaca][rotator][unit]") {
    Rig rig;
    // An IRotatorV2 upstream has no MechanicalPosition, MoveMechanical or Sync, and this one no StepSize either.
    rig.upstream.set("interfaceversion", "2");
    rig.upstream.set_error("stepsize", 0x400, "StepSize is not implemented");
    rig.connect();  // the errors are the upstream's answers: the Connect stands
    auto& r = *rig.driver;
    CHECK(alpaca_error_message([&] { (void)r.get_step_size(); }, AlpacaError::NotImplemented) ==
          kPrefix + "StepSize is not implemented");
    const std::string lacks = "Upstream 127.0.0.1:11111 device 0 is an IRotatorV2 and has no ";
    CHECK(alpaca_error_message([&] { (void)r.get_mechanical_position(); }, AlpacaError::NotImplemented) ==
          lacks + "MechanicalPosition");
    CHECK(alpaca_error_message([&] { r.move_mechanical(10.0); }, AlpacaError::NotImplemented) ==
          lacks + "MoveMechanical");
    CHECK(alpaca_error_message([&] { r.sync(10.0); }, AlpacaError::NotImplemented) == lacks + "Sync");
    rig.poll();
    // Never asked of the upstream: an older server may answer an unknown member with HTTP 400, a failed poll.
    CHECK(rig.upstream.calls("mechanicalposition") == 0);
    CHECK(rig.upstream.calls("movemechanical") == 0);
    CHECK(rig.upstream.calls("sync") == 0);
    CHECK(r.get_link_fault().empty());
    // DeviceState omits the member that throws.
    const auto state = r.get_device_state();
    REQUIRE(state.size() == 3);
    CHECK(state[0].name == "IsMoving");
    CHECK(state[1].name == "Position");
    // TargetPosition is read-only in IRotatorV4; this server's PUT extension is not forwarded.
    require_alpaca_error([&] { r.set_target_position(10.0); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("targetposition") > 0);  // the polls' GETs only
    // Actions and Command* are not forwarded.
    CHECK(r.get_supported_actions().empty());
    CHECK_FALSE(r.can_action("anything"));
    require_alpaca_error([&] { (void)r.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)r.command_blind("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)r.command_bool("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)r.command_string("x", false); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("action") == 0);
    CHECK(rig.upstream.calls("commandblind") == 0);
}

TEST_CASE("Remote Alpaca Rotator Driver - an upstream move error passes through unchanged with its host",
          "[remotealpaca][rotator][unit]") {
    Rig rig;
    rig.connect();
    rig.upstream.set_put_error("move", 0x40B, "rotator is already moving");
    CHECK(alpaca_error_message([&] { rig.driver->move(30.0); }, AlpacaError::InvalidOperation) ==
          kPrefix + "rotator is already moving");
    // A refused move wrote nothing through, and an answered error is not a transport failure: the link stays up.
    CHECK_FALSE(rig.driver->get_is_moving());
    CHECK(rig.driver->get_target_position() == 100.0);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("Remote Alpaca Rotator Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][rotator][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
}
