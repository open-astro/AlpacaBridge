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

// The remotealpaca Focuser proxied device (decision 0023, issue #1753 slice 5) over the scripted FakeHttpClient and a
// scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision 0005). Fake evidence
// only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the driver's
// contract_sweep.h entry. The rules the Focuser shares with the other proxied devices (the snapshot, the loss bound,
// the self-loop check) live in UpstreamProxy (upstream_proxy.h) and are covered at length by
// test_remotealpaca_observingconditions.cpp; the cases here pin what the Focuser's own members do with each of them.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_focuser_driver.h>
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
using alpacacore::FocuserDriver;
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
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::Focuser};
    std::unique_ptr<FocuserDriver> driver;

    explicit Rig(std::chrono::seconds poll = std::chrono::seconds(1), remote::ServerAddressSource self = {},
                 int device_number = 0) {
        upstream.install(http);
        driver =
            remote::create_remotealpaca_focuser(device_number, config(poll), http.client(), std::move(self), clock);
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

}  // namespace

TEST_CASE("Remote Alpaca Focuser Driver - Defaults", "[remotealpaca][focuser][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca Focuser");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    // Absolute answers while disconnected: true before any Connect has read the upstream's. TempCompAvailable and the
    // other limits are the upstream's, read at Connect, so each is NotConnected.
    CHECK(rig.driver->get_absolute());
    require_alpaca_error([&] { (void)rig.driver->get_temp_comp_available(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)rig.driver->get_max_step(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca Focuser Driver - Device metadata", "[remotealpaca][focuser][unit]") {
    Rig rig(std::chrono::seconds(1), {}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "Focuser of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca Focuser Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 4);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_FOC_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_FOC_3");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Focuser");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator Focuser");
}

TEST_CASE("Remote Alpaca Focuser Driver - the getters serve the snapshot and the Connect's limits, calling nothing",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.connect();
    auto& f = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(f.get_absolute());
    CHECK(f.get_max_step() == 50000);
    CHECK(f.get_max_increment() == 50000);
    CHECK(f.get_step_size() == 20.0);
    CHECK(f.get_temp_comp_available());
    CHECK_FALSE(f.get_temp_comp());
    CHECK(f.get_temperature() == 5.0);
    CHECK(f.get_position() == 25000);
    CHECK_FALSE(f.get_is_moving());
    const auto state = f.get_device_state();
    REQUIRE(state.size() == 4);  // IsMoving, Position, Temperature and TimeStamp
    CHECK(state[0].name == "IsMoving");
    CHECK_FALSE(std::get<bool>(state[0].value));
    CHECK(state[1].name == "Position");
    CHECK(std::get<std::int32_t>(state[1].value) == 25000);
    CHECK(state[2].name == "Temperature");
    CHECK(state.back().name == "TimeStamp");
    CHECK(rig.http.calls() == calls_after_connect);

    // The limits are read once at Connect; the next poll measures the frame members only.
    CHECK(rig.upstream.calls("maxstep") == 1);
    rig.upstream.set("position", "26000");
    rig.upstream.set("maxstep", "40000");
    CHECK(f.get_position() == 25000);  // until the next poll the frame holds
    rig.poll();
    CHECK(f.get_position() == 26000);
    CHECK(f.get_max_step() == 50000);
    CHECK(rig.upstream.calls("maxstep") == 1);
}

// DeviceState's TimeStamp is when the frame was measured (decision 0010), through the focuser base's
// device_state_age(): a frame the poll stopped refreshing carries its age.
TEST_CASE("Remote Alpaca Focuser Driver - DeviceState is stamped with the frame's age",
          "[remotealpaca][focuser][unit]") {
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

TEST_CASE("Remote Alpaca Focuser Driver - Move is forwarded and IsMoving reads true at once",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.connect();
    auto& f = *rig.driver;
    f.move(30000);
    CHECK(rig.upstream.last_move() == "move 30000");
    // Written through: IsMoving is true before any poll; Position stays the measured one.
    CHECK(f.get_is_moving());
    CHECK(f.get_position() == 25000);
    rig.poll();
    CHECK(f.get_is_moving());  // the upstream still moves
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(f.get_is_moving());
    CHECK(f.get_position() == 30000);
    // A Move to where the frame already has it still reads IsMoving true: the frame is no evidence the focuser is
    // there now; the next poll says when the upstream is still.
    f.move(30000);
    CHECK(f.get_is_moving());
    CHECK(rig.upstream.calls("move") == 2);
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(f.get_is_moving());
}

// An absolute Move to the Position a stale frame shows still reads IsMoving true: another client may have moved the
// focuser since the last poll, so the upstream moves even though the frame says the target is reached.
TEST_CASE("Remote Alpaca Focuser Driver - an absolute Move to the frame's Position reads IsMoving true",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.connect();
    auto& f = *rig.driver;
    REQUIRE(f.get_position() == 25000);
    REQUIRE_FALSE(f.get_is_moving());
    rig.upstream.set("position", "26000");  // moved by another client; no poll has seen it
    f.move(25000);
    CHECK(rig.upstream.last_move() == "move 25000");
    CHECK(f.get_is_moving());
    rig.poll();
    CHECK(f.get_is_moving());  // the upstream runs the move back to 25000
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(f.get_is_moving());
    CHECK(f.get_position() == 25000);
}

// A relative step of 0 has nowhere to go and writes nothing through: an earlier move the upstream still runs keeps
// IsMoving true until a poll says otherwise.
TEST_CASE("Remote Alpaca Focuser Driver - a relative step of 0 leaves a running move's IsMoving",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.upstream.set("absolute", "false");
    rig.connect();
    auto& f = *rig.driver;
    f.move(500);
    REQUIRE(f.get_is_moving());
    rig.poll();  // the upstream still moves
    REQUIRE(f.get_is_moving());
    f.move(0);
    CHECK(rig.upstream.last_move() == "move 0");
    CHECK(f.get_is_moving());  // the frame's value stands: the step wrote nothing
    rig.upstream.finish_move();
    rig.poll();
    CHECK_FALSE(f.get_is_moving());
}

TEST_CASE("Remote Alpaca Focuser Driver - Value range validation", "[remotealpaca][focuser][unit]") {
    SECTION("an absolute Move outside 0..MaxStep goes to the limit, not InvalidValue (#2028)") {
        Rig rig;
        rig.connect();
        rig.driver->move(-5);
        CHECK(rig.upstream.last_move() == "move 0");
        rig.driver->move(60000);
        CHECK(rig.upstream.last_move() == "move 50000");
        rig.driver->move(50000);
        CHECK(rig.upstream.last_move() == "move 50000");
    }
    SECTION("a relative upstream's step goes as given: the upstream decides its range") {
        Rig rig;
        rig.upstream.set("absolute", "false");
        rig.connect();
        CHECK_FALSE(rig.driver->get_absolute());
        rig.driver->move(-60000);
        CHECK(rig.upstream.last_move() == "move -60000");
        CHECK(rig.driver->get_is_moving());
        // Disconnected, Absolute is what the last Connect read.
        rig.driver->set_connected(false);
        CHECK_FALSE(rig.driver->get_absolute());
    }
    SECTION("the factory refuses each config field out of range before any network call") {
        alpacacore::test::FakeTaskClock clock;
        alpacacore::test::FakeHttpClient http;
        for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_focuser(0, Rig::config(bad), http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        for (const int port : {0, 65536}) {
            remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
            config.port = port;
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_focuser(0, config, http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.upstream_device_number = -1;
        require_alpaca_error([&] { (void)remote::create_remotealpaca_focuser(0, config, http.client(), {}, clock); },
                             AlpacaError::InvalidValue);
        CHECK(http.calls() == 0);
    }
}

TEST_CASE("Remote Alpaca Focuser Driver - Halt is forwarded while the link is faulted, Move and TempComp are not",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.connect();
    auto& f = *rig.driver;
    f.move(30000);
    REQUIRE(f.get_is_moving());
    rig.fault();
    const int moves = rig.upstream.calls("move");
    const int temp_comps = rig.upstream.calls("tempcomp");  // the polls' GETs so far
    // Faulted: the frame is refused with DriverException, the Connect's limits still answer (static metadata).
    CHECK(alpaca_error_message([&] { (void)f.get_is_moving(); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    require_alpaca_error([&] { (void)f.get_position(); }, AlpacaError::DriverException);
    CHECK(f.get_max_step() == 50000);
    CHECK(f.get_absolute());
    // The safety stop goes out; the commands that need the frame are refused and send nothing.
    f.halt();
    CHECK(rig.upstream.calls("halt") == 1);
    require_alpaca_error([&] { f.move(10000); }, AlpacaError::DriverException);
    require_alpaca_error([&] { f.set_temp_comp(true); }, AlpacaError::DriverException);
    CHECK(rig.upstream.calls("move") == moves);
    CHECK(rig.upstream.calls("tempcomp") == temp_comps);
    // The next good poll clears the fault: the upstream stopped.
    rig.poll();
    CHECK(f.get_link_fault().empty());
    CHECK_FALSE(f.get_is_moving());
}

// The Move's body is held before its PUT; a Halt sent meanwhile waits for it (run_command() keeps caller order), so the
// upstream gets the Move and then the Halt, never the Halt first with the Move after it.
TEST_CASE("Remote Alpaca Focuser Driver - a Halt sent during a move reaches the upstream after the move",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.connect();
    alpacacore::test::Latch move_body;
    std::atomic<bool> held{false};  // only the first body (the Move's) holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) move_body.block();
    });
    std::thread mover([&] { rig.driver->move(30000); });
    CHECK(move_body.wait_entered());  // CHECK, not REQUIRE: the threads are joined below either way
    // The Halt's caller finds the Move's command running and waits for it: the hook marks the moment.
    alpacacore::test::Latch halt_waits;
    remote::set_command_waiting_hook_for_testing(*rig.driver, [&] { halt_waits.block(); });
    std::thread halter([&] { rig.driver->halt(); });
    CHECK(halt_waits.wait_entered());
    CHECK(rig.upstream.calls("halt") == 0);
    halt_waits.release();
    move_body.release();
    mover.join();
    halter.join();
    std::vector<std::string> puts;
    for (const auto& request : rig.http.requests()) {
        if (request.method != "PUT") continue;
        puts.push_back(request.url.substr(request.url.rfind('/') + 1));
    }
    REQUIRE(puts.size() >= 2);
    CHECK(puts[puts.size() - 2] == "move");
    CHECK(puts.back() == "halt");
    CHECK_FALSE(rig.driver->get_is_moving());
}

// The Move writes IsMoving true through and the Halt after it writes false: before any poll, IsMoving reads the Halt's.
TEST_CASE("Remote Alpaca Focuser Driver - IsMoving reads false after a Halt that follows a Move, before any poll",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.connect();
    rig.driver->move(30000);
    REQUIRE(rig.driver->get_is_moving());
    rig.driver->halt();
    CHECK_FALSE(rig.driver->get_is_moving());
    CHECK(rig.upstream.calls("halt") == 1);
    rig.poll();  // the upstream stopped too
    CHECK_FALSE(rig.driver->get_is_moving());
}

TEST_CASE("Remote Alpaca Focuser Driver - TempComp is forwarded and written through", "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.connect();
    auto& f = *rig.driver;
    const int before = rig.upstream.calls("tempcomp");
    f.set_temp_comp(true);
    CHECK(rig.upstream.calls("tempcomp") == before + 1);
    CHECK(f.get_temp_comp());  // before any poll
    rig.poll();
    CHECK(f.get_temp_comp());  // the upstream holds it
    // An upstream without temperature compensation refuses the write with its own error, unchanged.
    rig.upstream.set_put_error("tempcomp", 0x400, "TempComp is not available");
    CHECK(alpaca_error_message([&] { f.set_temp_comp(false); }, AlpacaError::NotImplemented) ==
          kPrefix + "TempComp is not available");
    CHECK(f.get_temp_comp());
}

TEST_CASE("Remote Alpaca Focuser Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][focuser][unit]") {
    SECTION("Platform 7 upstream (IFocuserV4): Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("interfaceversion") == 1);
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an IFocuserV3 upstream has no Connect: Connected") {
        Rig rig;
        rig.upstream.set("interfaceversion", "3");
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 0);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 0);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
}

TEST_CASE("Remote Alpaca Focuser Driver - State machine contracts", "[remotealpaca][focuser][unit]") {
    Rig rig;
    auto& f = *rig.driver;
    CHECK_FALSE(f.get_connected());
    CHECK(f.get_device_state().empty());
    require_alpaca_error([&] { (void)f.get_is_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { f.move(100); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { f.halt(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);

    rig.connect();
    CHECK_FALSE(f.get_is_moving());   // idle after Connect
    rig.driver->set_connected(true);  // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // A refused Move is a lost link at once (decision 0009 point 2(a)): NotConnected, and Halt too.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { f.move(100); }, AlpacaError::NotConnected);
    CHECK_FALSE(f.get_connected());
    CHECK(f.get_device_state().empty());
    require_alpaca_error([&] { f.halt(); }, AlpacaError::NotConnected);
    CHECK(f.get_link_fault().find("connection refused") != std::string::npos);
    // Nothing relinks by itself (decision 0009 rule 4): the poll ended.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    // Only the client's Connect relinks.
    rig.connect();
    CHECK(f.get_link_fault().empty());
    CHECK(f.get_position() == 25000);

    // Disconnected by the client: NotConnected again, and the poll ended.
    f.set_connected(false);
    require_alpaca_error([&] { (void)f.get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)f.get_max_step(); }, AlpacaError::NotConnected);
    const int after = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == after);
}

TEST_CASE("Remote Alpaca Focuser Driver - members it does not proxy or the upstream lacks are not implemented",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    // An upstream with no step size and no temperature probe answers its own NotImplemented for each.
    rig.upstream.set_error("stepsize", 0x400, "StepSize is not implemented");
    rig.upstream.set_error("temperature", 0x400, "Temperature is not implemented");
    rig.connect();  // the errors are the upstream's answers: the Connect stands
    auto& f = *rig.driver;
    CHECK(alpaca_error_message([&] { (void)f.get_step_size(); }, AlpacaError::NotImplemented) ==
          kPrefix + "StepSize is not implemented");
    CHECK(alpaca_error_message([&] { (void)f.get_temperature(); }, AlpacaError::NotImplemented) ==
          kPrefix + "Temperature is not implemented");
    // DeviceState omits the member that throws.
    const auto state = f.get_device_state();
    REQUIRE(state.size() == 3);
    CHECK(state[0].name == "IsMoving");
    CHECK(state[1].name == "Position");
    // Actions and Command* are not forwarded.
    CHECK(f.get_supported_actions().empty());
    CHECK_FALSE(f.can_action("anything"));
    require_alpaca_error([&] { (void)f.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)f.command_blind("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)f.command_bool("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)f.command_string("x", false); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("action") == 0);
    CHECK(rig.upstream.calls("commandblind") == 0);
}

TEST_CASE("Remote Alpaca Focuser Driver - an upstream Move error passes through unchanged with its host",
          "[remotealpaca][focuser][unit]") {
    Rig rig;
    rig.connect();
    rig.upstream.set_put_error("move", 0x40B, "focuser is in temperature compensation mode");
    CHECK(alpaca_error_message([&] { rig.driver->move(30000); }, AlpacaError::InvalidOperation) ==
          kPrefix + "focuser is in temperature compensation mode");
    // A refused Move wrote nothing through, and an answered error is not a transport failure: the link stays up.
    CHECK_FALSE(rig.driver->get_is_moving());
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("Remote Alpaca Focuser Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][focuser][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
}
