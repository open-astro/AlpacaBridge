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

// The remotealpaca SafetyMonitor proxied device (decision 0023, issue #1753 slice 4) over the scripted FakeHttpClient
// and a scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision 0005). Fake
// evidence only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the driver's
// contract_sweep.h entry. The rules the SafetyMonitor shares with the ObservingConditions device (the snapshot, the
// loss bound, the self-loop check) live in UpstreamProxy (upstream_proxy.h) and are covered at length by
// test_remotealpaca_observingconditions.cpp; the cases here pin what IsSafe does with each of them.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/safetymonitor_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_safetymonitor_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <chrono>
#include <memory>
#include <string>
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
using alpacacore::SafetyMonitorDriver;
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
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::SafetyMonitor};
    std::unique_ptr<SafetyMonitorDriver> driver;

    explicit Rig(std::chrono::seconds poll = std::chrono::seconds(1), remote::ServerAddressSource self = {},
                 int device_number = 0) {
        upstream.install(http);
        driver = remote::create_remotealpaca_safetymonitor(device_number, config(poll), http.client(), std::move(self),
                                                           clock);
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
};

}  // namespace

TEST_CASE("Remote Alpaca SafetyMonitor Driver - Defaults", "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::SafetyMonitor);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca SafetyMonitor");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - Device metadata", "[remotealpaca][safetymonitor][unit]") {
    Rig rig(std::chrono::seconds(1), {}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "SafetyMonitor of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca SafetyMonitor Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 3);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_SM_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_SM_3");
    rig.driver->assign_unique_id("5b0f7c2e-1d3a-4e8b-9f60-7a2c4d1e9b33");
    CHECK(rig.driver->get_unique_id() == "5b0f7c2e-1d3a-4e8b-9f60-7a2c4d1e9b33");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Safety Monitor");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator SafetyMonitor");
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - IsSafe serves the polled snapshot and never calls the upstream",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.connect();
    auto& sm = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(sm.get_is_safe());
    const auto state = sm.get_device_state();
    REQUIRE(state.size() == 2);  // IsSafe and TimeStamp
    CHECK(state.front().name == "IsSafe");
    CHECK(std::get<bool>(state.front().value));
    CHECK(rig.http.calls() == calls_after_connect);

    // The next poll measures the upstream again; until then the frame holds.
    rig.upstream.set("issafe", "false");
    CHECK(sm.get_is_safe());
    rig.poll();
    CHECK_FALSE(sm.get_is_safe());
    rig.upstream.set("issafe", "true");
    rig.poll();
    CHECK(sm.get_is_safe());
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][safetymonitor][unit]") {
    SECTION("Platform 7 upstream (ISafetyMonitorV3): Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("interfaceversion") == 1);
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an ISafetyMonitorV2 upstream has no Connect: Connected") {
        Rig rig;
        rig.upstream.set("interfaceversion", "2");
        rig.connect();
        CHECK(rig.upstream.calls("connect") == 0);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 0);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("a Connect the upstream refuses fails with its error and leaves it disconnected") {
        Rig rig;
        rig.upstream.set_error("name", 0x500, "upstream broke");
        const std::string text =
            alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
        CHECK(text == kPrefix + "upstream broke");
        CHECK_FALSE(rig.driver->get_connected());
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
}

// SafetyMonitor has no writable member: the range checks are the factory's, which refuses before any network call.
TEST_CASE("Remote Alpaca SafetyMonitor Driver - Value range validation", "[remotealpaca][safetymonitor][unit]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
        require_alpaca_error(
            [&] { (void)remote::create_remotealpaca_safetymonitor(0, Rig::config(bad), http.client(), {}, clock); },
            AlpacaError::InvalidValue);
    }
    for (const int port : {0, 65536}) {
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.port = port;
        require_alpaca_error(
            [&] { (void)remote::create_remotealpaca_safetymonitor(0, config, http.client(), {}, clock); },
            AlpacaError::InvalidValue);
    }
    remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
    config.upstream_device_number = -1;
    require_alpaca_error([&] { (void)remote::create_remotealpaca_safetymonitor(0, config, http.client(), {}, clock); },
                         AlpacaError::InvalidValue);
    config = Rig::config(std::chrono::seconds(2));
    config.host = "http://127.0.0.1";
    require_alpaca_error([&] { (void)remote::create_remotealpaca_safetymonitor(0, config, http.client(), {}, clock); },
                         AlpacaError::InvalidValue);
    CHECK(http.calls() == 0);
    // The bounds themselves are accepted.
    CHECK(remote::create_remotealpaca_safetymonitor(0, Rig::config(std::chrono::seconds(1)), http.client(), {}, clock));
    CHECK(
        remote::create_remotealpaca_safetymonitor(0, Rig::config(std::chrono::seconds(60)), http.client(), {}, clock));
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - State machine contracts", "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    auto& sm = *rig.driver;
    CHECK_FALSE(sm.get_connected());
    CHECK_FALSE(sm.get_connecting());
    CHECK(sm.get_device_state().empty());
    // Never connected: NotConnected, not false (only a lost link reads false).
    require_alpaca_error([&] { (void)sm.get_is_safe(); }, AlpacaError::NotConnected);

    rig.connect();
    CHECK_FALSE(sm.get_connecting());
    CHECK(sm.get_is_safe());
    CHECK(sm.get_device_state().back().name == "TimeStamp");
    rig.driver->set_connected(true);  // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // Disconnected by the client: NotConnected again, and the poll ended (the clock moving calls nothing).
    rig.driver->set_connected(false);
    CHECK_FALSE(sm.get_connected());
    CHECK(sm.get_device_state().empty());
    require_alpaca_error([&] { (void)sm.get_is_safe(); }, AlpacaError::NotConnected);
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == calls);
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - members it does not proxy are not implemented",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.connect();
    auto& sm = *rig.driver;
    CHECK(sm.get_supported_actions().empty());
    CHECK_FALSE(sm.can_action("anything"));
    require_alpaca_error([&] { (void)sm.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)sm.command_blind("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)sm.command_bool("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)sm.command_string("x", false); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("action") == 0);  // none of them reaches the upstream
    CHECK(rig.upstream.calls("commandblind") == 0);
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - an upstream IsSafe error passes through unchanged with its host",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.upstream.set_error("issafe", 0x500, "rain sensor offline");
    rig.connect();  // the error is the upstream's answer, kept in the frame: the Connect stands
    auto& sm = *rig.driver;
    CHECK(alpaca_error_message([&] { (void)sm.get_is_safe(); }, AlpacaError::DriverException) ==
          kPrefix + "rain sensor offline");
    // DeviceState omits a member that throws.
    CHECK(sm.get_device_state().empty());
    // An answered error is not a transport failure: the link stays healthy.
    rig.poll();
    rig.poll();
    rig.poll();
    CHECK(sm.get_connected());
    CHECK(sm.get_link_fault().empty());
    rig.upstream.set("issafe", "true");
    rig.poll();
    CHECK(sm.get_is_safe());
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - a refused connection reads unsafe and does not throw",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.connect();
    auto& sm = *rig.driver;
    REQUIRE(sm.get_is_safe());
    rig.http.refuse_next(1);
    rig.poll();
    // Lost at once (decision 0009 point 2(a)): Connected false, and IsSafe false with no exception (#1753).
    CHECK_FALSE(sm.get_connected());
    CHECK_FALSE(sm.get_is_safe());
    CHECK(sm.get_device_state().empty());
    CHECK(sm.get_link_fault().find("connection refused") != std::string::npos);
    // Still false however long the link stays down: the poll ended and nothing relinks by itself (decision 0009 rule
    // 4), so an upstream that reads safe again is not asked.
    rig.upstream.set("issafe", "true");
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    CHECK_FALSE(sm.get_is_safe());
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - a stalled upstream faults IsSafe, then reads unsafe once lost",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.connect();
    auto& sm = *rig.driver;
    rig.http.fail_next(1000);  // a timeout, not a refusal
    rig.poll();
    rig.poll();
    CHECK(sm.get_is_safe());  // two failed polls: the last frame is still served
    rig.poll();
    // The third latches the fault: Connected stays true and IsSafe refuses the frame (decision 0009 rule 1).
    CHECK(sm.get_connected());
    CHECK_FALSE(sm.get_link_fault().empty());
    CHECK(alpaca_error_message([&] { (void)sm.get_is_safe(); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    CHECK(sm.get_device_state().empty());
    // Lost when no poll has refreshed the frame for util::kLinkStalenessBound, 30 s after the Connect's frame: from
    // then on IsSafe reads false and does not throw.
    rig.poll(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(4));
    CHECK(sm.get_connected());
    require_alpaca_error([&] { (void)sm.get_is_safe(); }, AlpacaError::DriverException);
    rig.poll(std::chrono::seconds(1));
    CHECK_FALSE(sm.get_connected());
    CHECK_FALSE(sm.get_is_safe());
    CHECK(sm.get_device_state().empty());
    CHECK_FALSE(sm.get_link_fault().empty());  // the reason it was lost, kept until the next Connect or Disconnect
}

// The getter, not the poll, finds the loss here: the poll is paused, so the frame ages past the bound with no failed
// poll and no on_lost(). IsSafe's own read latches the loss (PolledState::served()) and answers false, not
// NotConnected.
TEST_CASE("Remote Alpaca SafetyMonitor Driver - a frame past the bound reads unsafe from the getter that finds it",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.connect();
    auto& sm = *rig.driver;
    REQUIRE(sm.get_is_safe());
    remote::set_poll_paused_for_testing(sm, true);
    rig.poll(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    CHECK(sm.get_is_safe());  // stale, still served (decision 0010 rule 6)
    rig.poll(std::chrono::seconds(1));
    CHECK_FALSE(sm.get_is_safe());
    CHECK_FALSE(sm.get_connected());
    CHECK(sm.get_device_state().empty());
    CHECK(sm.get_link_fault().find("no answer for 30 s") != std::string::npos);
    // A Disconnect ends the loss: NotConnected again, never false.
    remote::set_poll_paused_for_testing(sm, false);
    sm.set_connected(false);
    require_alpaca_error([&] { (void)sm.get_is_safe(); }, AlpacaError::NotConnected);
}

// The DeviceState read, not IsSafe, finds the loss here (the poll is paused, as above). The base checks Connected,
// then reads IsSafe, which latches the loss and answers false; a lost link's DeviceState is still the empty list
// (decision 0009 rule 3), not IsSafe false with a TimeStamp.
TEST_CASE("Remote Alpaca SafetyMonitor Driver - a DeviceState read that finds the loss returns the empty list",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.connect();
    auto& sm = *rig.driver;
    remote::set_poll_paused_for_testing(sm, true);
    rig.poll(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    CHECK(sm.get_device_state().size() == 2);  // stale, still served
    rig.poll(std::chrono::seconds(1));
    CHECK(sm.get_device_state().empty());
    CHECK_FALSE(sm.get_connected());
    CHECK_FALSE(sm.get_is_safe());
    remote::set_poll_paused_for_testing(sm, false);
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - only the client's Connect relinks; Disconnect ends the unsafe answer",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.connect();
    rig.http.refuse_next(1);
    rig.poll();
    REQUIRE_FALSE(rig.driver->get_connected());
    REQUIRE_FALSE(rig.driver->get_is_safe());
    const std::string fault = rig.driver->get_link_fault();
    CHECK_FALSE(fault.empty());

    SECTION("Connect relinks: IsSafe is the upstream's again") {
        rig.connect();
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_is_safe());
    }
    SECTION("Disconnect ends the loss: NotConnected, and the lost upstream is not called") {
        rig.driver->set_connected(false);
        CHECK(rig.driver->get_link_fault().empty());
        require_alpaca_error([&] { (void)rig.driver->get_is_safe(); }, AlpacaError::NotConnected);
        CHECK(rig.upstream.calls("disconnect") == 0);
    }
}

// The reading this driver takes of "while the link is lost" (#1753): a Connect after a loss ends the kept reason when
// it starts, so a Connect that then fails (the upstream still refuses) leaves the driver not connected, not lost, and
// IsSafe throws NotConnected as before any Connect. Both answers mean not safe; which one a failed relink gives is
// the maintainer's call on the slice 4 pull request.
TEST_CASE("Remote Alpaca SafetyMonitor Driver - a Connect that fails after a loss leaves IsSafe NotConnected",
          "[remotealpaca][safetymonitor][unit]") {
    Rig rig;
    rig.connect();
    rig.http.refuse_next(1);
    rig.poll();
    REQUIRE_FALSE(rig.driver->get_is_safe());  // lost: false, no exception

    rig.http.refuse_next(1);  // the Connect's first call is refused too
    require_alpaca_error([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    require_alpaca_error([&] { (void)rig.driver->get_is_safe(); }, AlpacaError::NotConnected);
    CHECK(rig.driver->get_device_state().empty());

    // The next Connect that the upstream answers relinks as usual.
    rig.connect();
    CHECK(rig.driver->get_is_safe());
}

TEST_CASE("Remote Alpaca SafetyMonitor Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][safetymonitor][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
    // A refused Connect is not a lost link: IsSafe stays NotConnected.
    require_alpaca_error([&] { (void)rig.driver->get_is_safe(); }, AlpacaError::NotConnected);
}
