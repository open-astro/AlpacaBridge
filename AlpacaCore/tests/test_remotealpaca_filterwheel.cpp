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

// The remotealpaca FilterWheel proxied device (decision 0023, issue #1753 slice 5, #2706) over the scripted
// FakeHttpClient and a scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision
// 0005). Fake evidence only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the
// driver's contract_sweep.h entry, and the slot-data rules (one entry per slot while connected, the shorthand, any
// length after a Disconnect) from the sweep's tier-2 filter wheel case. The rules the FilterWheel shares with the other
// proxied devices (the snapshot, the loss bound, the self-loop check) live in UpstreamProxy (upstream_proxy.h) and are
// covered at length by test_remotealpaca_observingconditions.cpp; the cases here pin what the FilterWheel's own
// members do with each of them.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_filterwheel_driver.h>
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
using alpacacore::FilterWheelDriver;
using alpacacore::test::alpaca_error_message;
using alpacacore::test::AlpacaUpstreamScript;
using alpacacore::test::require_alpaca_error;

constexpr std::chrono::milliseconds kRendezvous{5000};
constexpr int kUpstreamPort = 11111;
const std::string kPrefix = "127.0.0.1:11111: ";
const std::vector<std::string> kUpstreamNames{"Red", "Green", "Blue", "Clear"};
const std::vector<int> kUpstreamOffsets{100, 200, 300, 0};

// The fake clock, the scripted upstream behind the fake HTTP client, and the driver, destroyed in reverse.
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::FilterWheel};
    std::unique_ptr<FilterWheelDriver> driver;

    explicit Rig(std::chrono::seconds poll = std::chrono::seconds(1), remote::ServerAddressSource self = {},
                 int device_number = 0) {
        upstream.install(http);
        driver =
            remote::create_remotealpaca_filterwheel(device_number, config(poll), http.client(), std::move(self), clock);
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

TEST_CASE("Remote Alpaca FilterWheel Driver - Defaults", "[remotealpaca][filterwheel][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca FilterWheel");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    // No Connect has read the upstream's slots: no names and no offsets, and no error (readable while disconnected).
    CHECK(rig.driver->get_names().empty());
    CHECK(rig.driver->get_focus_offsets().empty());
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca FilterWheel Driver - Device metadata", "[remotealpaca][filterwheel][unit]") {
    Rig rig(std::chrono::seconds(1), {}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "FilterWheel of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca FilterWheel Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 3);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_FW_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_FW_3");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Filter Wheel");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator FilterWheel");
}

TEST_CASE("Remote Alpaca FilterWheel Driver - Position is the snapshot, Names and FocusOffsets the Connect's",
          "[remotealpaca][filterwheel][unit]") {
    Rig rig;
    rig.connect();
    auto& f = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(f.get_names() == kUpstreamNames);
    CHECK(f.get_focus_offsets() == kUpstreamOffsets);
    CHECK(f.get_position() == 0);
    const auto state = f.get_device_state();
    REQUIRE(state.size() == 2);  // Position and TimeStamp
    CHECK(state[0].name == "Position");
    CHECK(std::get<std::int32_t>(state[0].value) == 0);
    CHECK(state[1].name == "TimeStamp");
    CHECK(rig.http.calls() == calls_after_connect);  // the getters call nothing

    // Names and offsets are read once at Connect; the next poll measures Position only.
    CHECK(rig.upstream.calls("names") == 1);
    CHECK(rig.upstream.calls("focusoffsets") == 1);
    rig.upstream.set("position", "2");
    rig.upstream.set("names", R"(["A", "B", "C", "D"])");
    CHECK(f.get_position() == 0);  // until the next poll the frame holds
    rig.poll();
    CHECK(f.get_position() == 2);
    CHECK(f.get_names() == kUpstreamNames);
    CHECK(rig.upstream.calls("names") == 1);
    // The next Connect reads the upstream's lineup again: it is the wheel's, and replaces what this server held.
    f.set_connected(false);
    CHECK(f.get_names() == kUpstreamNames);  // kept while disconnected
    rig.connect();
    CHECK(f.get_names() == std::vector<std::string>{"A", "B", "C", "D"});
    CHECK(rig.upstream.calls("names") == 2);
}

TEST_CASE("Remote Alpaca FilterWheel Driver - a Position write is forwarded and reads -1 until the upstream lands",
          "[remotealpaca][filterwheel][unit]") {
    Rig rig;
    rig.connect();
    auto& f = *rig.driver;
    f.set_position(2);
    CHECK(rig.upstream.last_position_put() == "2");
    // Written through: -1 (moving) before any poll.
    CHECK(f.get_position() == -1);
    rig.poll();
    CHECK(f.get_position() == -1);  // the upstream's wheel still turns: its own -1
    const auto state = f.get_device_state();
    REQUIRE_FALSE(state.empty());
    CHECK(std::get<std::int32_t>(state[0].value) == -1);
    rig.upstream.finish_wheel_move();
    rig.poll();
    CHECK(f.get_position() == 2);
    // A write of the slot the frame already has is a move too: another client may have turned the wheel since the
    // last poll, so it reads -1 until the next poll says where the wheel is.
    f.set_position(2);
    CHECK(rig.upstream.last_position_put() == "2");
    CHECK(f.get_position() == -1);
    rig.upstream.finish_wheel_move();
    rig.poll();
    CHECK(f.get_position() == 2);
}

TEST_CASE("Remote Alpaca FilterWheel Driver - Value range validation", "[remotealpaca][filterwheel][unit]") {
    SECTION("a slot past the upstream's count is InvalidValue and sends nothing") {
        Rig rig;
        rig.connect();
        require_alpaca_error([&] { rig.driver->set_position(4); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { rig.driver->set_position(-1); }, AlpacaError::InvalidValue);
        CHECK(rig.upstream.last_position_put().empty());
        rig.driver->set_position(3);  // the last slot
        CHECK(rig.upstream.last_position_put() == "3");
    }
    SECTION("a negative slot is InvalidValue before the connection check, a slot past any count NotConnected") {
        Rig rig;
        require_alpaca_error([&] { rig.driver->set_position(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { rig.driver->set_position(99); }, AlpacaError::NotConnected);
        CHECK(rig.http.calls() == 0);
    }
    SECTION("while connected, names and offsets take one entry per upstream slot") {
        Rig rig;
        rig.connect();
        require_alpaca_error([&] { rig.driver->set_names({"L", "R", "G"}); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { rig.driver->set_focus_offsets({1, 2, 3, 4, 5}); }, AlpacaError::InvalidValue);
        CHECK(rig.driver->get_names() == kUpstreamNames);
        rig.driver->set_names({"L", "R", "G", "B"});
        CHECK(rig.driver->get_names() == std::vector<std::string>{"L", "R", "G", "B"});
        CHECK(rig.upstream.calls("names") == 1);  // a local write the upstream never sees
    }
    SECTION("the factory refuses each config field out of range before any network call") {
        alpacacore::test::FakeTaskClock clock;
        alpacacore::test::FakeHttpClient http;
        for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_filterwheel(0, Rig::config(bad), http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        for (const int port : {0, 65536}) {
            remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
            config.port = port;
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_filterwheel(0, config, http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.upstream_device_number = -1;
        require_alpaca_error(
            [&] { (void)remote::create_remotealpaca_filterwheel(0, config, http.client(), {}, clock); },
            AlpacaError::InvalidValue);
        CHECK(http.calls() == 0);
    }
}

TEST_CASE("Remote Alpaca FilterWheel Driver - while the link is faulted Position and its write are refused",
          "[remotealpaca][filterwheel][unit]") {
    Rig rig;
    rig.connect();
    auto& f = *rig.driver;
    rig.fault();
    // Faulted: the frame is refused with DriverException; the Connect's slot data still answers (static metadata).
    CHECK(alpaca_error_message([&] { (void)f.get_position(); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    CHECK(f.get_names() == kUpstreamNames);
    CHECK(f.get_device_state().empty());  // Position omitted, so no TimeStamp either
    require_alpaca_error([&] { f.set_position(1); }, AlpacaError::DriverException);
    CHECK(rig.upstream.last_position_put().empty());
    // The next good poll clears the fault.
    rig.poll();
    CHECK(f.get_link_fault().empty());
    CHECK(f.get_position() == 0);
}

// Position writes reach the upstream in caller order (UpstreamProxy's command order mutex): a second write sent while
// the first one's body is held before its PUT waits for it, so the wheel ends on the slot written last.
TEST_CASE("Remote Alpaca FilterWheel Driver - two Position writes reach the upstream in caller order",
          "[remotealpaca][filterwheel][unit]") {
    Rig rig;
    rig.connect();
    alpacacore::test::Latch first_body;
    std::atomic<bool> held{false};  // only the first body holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) first_body.block();
    });
    std::thread first([&] { rig.driver->set_position(2); });
    REQUIRE(first_body.wait_entered());
    // The second write's caller finds the first write's command running and waits for it: the hook marks the moment.
    alpacacore::test::Latch second_waits;
    remote::set_command_waiting_hook_for_testing(*rig.driver, [&] { second_waits.block(); });
    std::thread second([&] { rig.driver->set_position(1); });
    CHECK(second_waits.wait_entered());  // CHECK, not REQUIRE: the threads are joined below either way
    CHECK(rig.upstream.last_position_put().empty());
    second_waits.release();
    first_body.release();
    first.join();
    second.join();
    std::vector<std::string> puts;
    for (const auto& request : rig.http.requests()) {
        if (request.method == "PUT" && request.url.substr(request.url.rfind('/') + 1) == "position") {
            puts.push_back(request.form_body.substr(0, request.form_body.find('&')));
        }
    }
    CHECK(puts == std::vector<std::string>{"Position=2", "Position=1"});
    CHECK(rig.upstream.last_position_put() == "1");
    CHECK(rig.driver->get_position() == -1);
    rig.upstream.finish_wheel_move();
    rig.poll();
    CHECK(rig.driver->get_position() == 1);
}

TEST_CASE("Remote Alpaca FilterWheel Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][filterwheel][unit]") {
    SECTION("Platform 7 upstream (IFilterWheelV3): Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("interfaceversion") == 1);
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an IFilterWheelV2 upstream has no Connect: Connected") {
        Rig rig;
        rig.upstream.set("interfaceversion", "2");
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 0);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 0);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
}

TEST_CASE("Remote Alpaca FilterWheel Driver - State machine contracts", "[remotealpaca][filterwheel][unit]") {
    Rig rig;
    auto& f = *rig.driver;
    CHECK_FALSE(f.get_connected());
    CHECK(f.get_device_state().empty());
    require_alpaca_error([&] { (void)f.get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { f.set_position(0); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);

    rig.connect();
    CHECK(f.get_position() == 0);     // at rest after Connect
    rig.driver->set_connected(true);  // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // A refused Position write is a lost link at once (decision 0009 point 2(a)): NotConnected.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { f.set_position(1); }, AlpacaError::NotConnected);
    CHECK_FALSE(f.get_connected());
    CHECK(f.get_device_state().empty());
    require_alpaca_error([&] { (void)f.get_position(); }, AlpacaError::NotConnected);
    CHECK(f.get_link_fault().find("connection refused") != std::string::npos);
    // A lost link keeps the slot count until the Disconnect (ascom-contract.instructions.md).
    require_alpaca_error([&] { f.set_names({"A"}); }, AlpacaError::InvalidValue);
    // Nothing relinks by itself (decision 0009 rule 4): the poll ended.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    // Only the client's Connect relinks, and it reads the upstream's lineup again: a wheel that now has five slots
    // replaces the four the lost link kept, count and all.
    rig.upstream.set("names", R"(["L", "R", "G", "B", "Ha"])");
    rig.upstream.set("focusoffsets", "[0, 1, 2, 3, 4]");
    rig.connect();
    CHECK(f.get_link_fault().empty());
    CHECK(f.get_position() == 0);
    CHECK(f.get_names() == std::vector<std::string>{"L", "R", "G", "B", "Ha"});
    CHECK(f.get_focus_offsets() == std::vector<int>{0, 1, 2, 3, 4});
    f.set_position(4);  // inside the new count
    CHECK(rig.upstream.last_position_put() == "4");

    // Disconnected by the client: NotConnected again, the poll ended, and the slot count unknown.
    f.set_connected(false);
    require_alpaca_error([&] { (void)f.get_position(); }, AlpacaError::NotConnected);
    const int after = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == after);
    f.set_names({"A", "B"});
    CHECK(f.get_names() == std::vector<std::string>{"A", "B"});
}

TEST_CASE("Remote Alpaca FilterWheel Driver - members it does not proxy or the upstream lacks are not implemented",
          "[remotealpaca][filterwheel][unit]") {
    SECTION("an upstream Position error is served unchanged with its host, and DeviceState omits it") {
        Rig rig;
        rig.upstream.set_error("position", 0x400, "Position is not implemented");
        rig.connect();  // the error is the upstream's answer: the Connect stands
        CHECK(alpaca_error_message([&] { (void)rig.driver->get_position(); }, AlpacaError::NotImplemented) ==
              kPrefix + "Position is not implemented");
        CHECK(rig.driver->get_device_state().empty());
    }
    SECTION("an upstream with no Names fails the Connect with its error, and forwards Disconnect") {
        Rig rig;
        rig.upstream.set_error("names", 0x400, "Names is not implemented");
        CHECK(alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::NotImplemented) ==
              kPrefix + "Names is not implemented");
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("a wheel with no slot fails the Connect") {
        Rig rig;
        rig.upstream.set("names", "[]");
        CHECK(alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException)
                  .find("no slots") != std::string::npos);
        CHECK_FALSE(rig.driver->get_connected());
    }
    SECTION("a Connect that fails after it read the slots leaves no slot count behind") {
        Rig rig;
        rig.upstream.set("position", "\"jammed\"");  // no usable first frame: the Connect fails after the store
        require_alpaca_error([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.driver->get_names() == kUpstreamNames);  // what the Connect read stays
        rig.driver->set_names({"A", "B"});                 // but a disconnected wheel takes any length
        CHECK(rig.driver->get_names() == std::vector<std::string>{"A", "B"});
    }
    SECTION("Actions and Command* are not forwarded") {
        Rig rig;
        rig.connect();
        auto& f = *rig.driver;
        CHECK(f.get_supported_actions().empty());
        CHECK_FALSE(f.can_action("anything"));
        require_alpaca_error([&] { (void)f.action("anything", ""); }, AlpacaError::ActionNotImplemented);
        require_alpaca_error([&] { (void)f.command_blind("x", false); }, AlpacaError::NotImplemented);
        require_alpaca_error([&] { (void)f.command_bool("x", false); }, AlpacaError::NotImplemented);
        require_alpaca_error([&] { (void)f.command_string("x", false); }, AlpacaError::NotImplemented);
        CHECK(rig.upstream.calls("action") == 0);
        CHECK(rig.upstream.calls("commandblind") == 0);
    }
}

TEST_CASE("Remote Alpaca FilterWheel Driver - an upstream Position write error passes through unchanged with its host",
          "[remotealpaca][filterwheel][unit]") {
    Rig rig;
    rig.connect();
    rig.upstream.set_put_error("position", 0x40B, "the wheel is jammed");
    CHECK(alpaca_error_message([&] { rig.driver->set_position(1); }, AlpacaError::InvalidOperation) ==
          kPrefix + "the wheel is jammed");
    // A refused write wrote nothing through, and an answered error is not a transport failure: the link stays up.
    CHECK(rig.driver->get_position() == 0);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("Remote Alpaca FilterWheel Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][filterwheel][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
}
