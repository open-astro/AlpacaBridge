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

// The remotealpaca ObservingConditions proxied device (decision 0023, issue #1753 slice 3) over the scripted
// FakeHttpClient and a scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision
// 0005). Fake evidence only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the
// driver's contract_sweep.h entry.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_observingconditions_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "alpaca_upstream_script.h"
#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_http_client.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
namespace remote = alpacacore::vendor::remotealpaca;
using alpacacore::AlpacaException;
using alpacacore::ObservingConditionsDriver;
using alpacacore::test::alpaca_error_message;
using alpacacore::test::require_alpaca_error;

constexpr std::chrono::milliseconds kRendezvous{5000};
constexpr int kUpstreamPort = 11111;
const std::string kPrefix = "127.0.0.1:11111: ";

// The fake clock, the scripted upstream behind the fake HTTP client, and the driver, destroyed in reverse.
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    std::unique_ptr<ObservingConditionsDriver> driver;

    explicit Rig(std::chrono::seconds poll = std::chrono::seconds(1), remote::ServerAddressSource self = {},
                 int device_number = 0) {
        upstream.install(http);
        remote::RemoteObservingConditionsConfig config;
        config.host = "127.0.0.1";
        config.port = kUpstreamPort;
        config.upstream_device_number = 0;
        config.poll_period = poll;
        driver = remote::create_remotealpaca_observingconditions(device_number, config, http.client(), std::move(self),
                                                                 clock);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    ~Rig() { driver.reset(); }

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

TEST_CASE("Remote Alpaca ObservingConditions Driver - Defaults", "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::ObservingConditions);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca ObservingConditions");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - Device metadata", "[remotealpaca][observingconditions][unit]") {
    Rig rig(std::chrono::seconds(1), {}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() ==
          "ObservingConditions of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca ObservingConditions Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 2);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_OC_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_OC_3");
    rig.driver->assign_unique_id("8c6d3a0e-4a54-4d7f-9d0c-2b7b0f1e5a11");
    CHECK(rig.driver->get_unique_id() == "8c6d3a0e-4a54-4d7f-9d0c-2b7b0f1e5a11");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Weather");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator ObservingConditions");
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - the connected Name is the brand and the upstream model",
          "[remotealpaca][observingconditions][unit]") {
    {
        Rig rig;
        rig.upstream.set("name", "\"Simulator (ObservingConditions, port 2)\"");
        rig.connect();
        // Decision 0024 rules 1 and 2: no '(' or ',' in Name; the left-out part goes in the Description (rule 4).
        CHECK(rig.driver->get_name() == "Remote Alpaca Simulator");
        CHECK(rig.driver->get_description() ==
              "ASCOM Alpaca Simulator ObservingConditions (upstream Name: Simulator (ObservingConditions, port 2))");
    }
    {
        Rig rig;
        rig.upstream.set("name", "\"Boltwood Cloud Sensor II, rev 3\"");
        rig.connect();
        CHECK(rig.driver->get_name() == "Remote Alpaca Boltwood Cloud Sensor II");
    }
    {
        Rig rig;
        rig.upstream.set("name", "\"Remote Alpaca Weather\"");  // already branded: not prefixed twice
        rig.connect();
        CHECK(rig.driver->get_name() == "Remote Alpaca Weather");
        CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator ObservingConditions");
    }
    {
        Rig rig;
        rig.upstream.set("name", "\"(unnamed)\"");  // nothing left: the default Name stays
        rig.connect();
        CHECK(rig.driver->get_name() == "Remote Alpaca ObservingConditions");
    }
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - getters serve the polled snapshot and never call the upstream",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.connect();
    auto& oc = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(oc.get_cloud_cover() == 10.0);
    CHECK(oc.get_temperature() == 19.0);
    CHECK(oc.get_wind_speed() == 22.0);
    CHECK(oc.get_average_period() == 0.0);
    CHECK(oc.get_time_since_last_update("Temperature") == Catch::Approx(1.5));
    CHECK(oc.get_time_since_last_update("") == Catch::Approx(1.5));
    CHECK(oc.get_sensor_description("temperature") == "Temperature sensor");
    const auto state = oc.get_device_state();
    CHECK(state.size() == 14);  // 13 sensors and TimeStamp
    CHECK(rig.http.calls() == calls_after_connect);

    // The next poll measures the upstream again; until then the frame holds.
    rig.upstream.set_sensor("Temperature", 3.25);
    CHECK(oc.get_temperature() == 19.0);
    rig.poll();
    CHECK(oc.get_temperature() == 3.25);
    // TimeSinceLastUpdate ages with the frame on the task clock.
    rig.clock.advance(std::chrono::milliseconds(500));
    CHECK(oc.get_time_since_last_update("Temperature") == Catch::Approx(2.0));
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - Connect and Disconnect are forwarded to the upstream",
          "[remotealpaca][observingconditions][unit]") {
    SECTION("Platform 7 upstream: Connect and Disconnect") {
        Rig rig;
        rig.connect();
        CHECK(rig.upstream.calls("interfaceversion") == 1);
        CHECK(rig.upstream.calls("connect") == 1);
        CHECK(rig.upstream.upstream_connected());
        rig.driver->set_connected(false);
        CHECK(rig.upstream.calls("disconnect") == 1);
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
    SECTION("an older upstream: Connected") {
        Rig rig;
        rig.upstream.set("interfaceversion", "1");
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

TEST_CASE("Remote Alpaca ObservingConditions Driver - Value range validation",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    auto& oc = *rig.driver;
    // InvalidValue precedes NotConnected.
    require_alpaca_error([&] { oc.set_average_period(-1.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { oc.set_average_period(std::numeric_limits<double>::quiet_NaN()); },
                         AlpacaError::InvalidValue);
    require_alpaca_error([&] { oc.set_average_period(0.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)oc.get_time_since_last_update("Moonlight"); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)oc.get_sensor_description("Moonlight"); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)oc.get_sensor_description(""); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)oc.get_time_since_last_update("temperature"); }, AlpacaError::NotConnected);
    rig.connect();
    require_alpaca_error([&] { oc.set_average_period(-0.5); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)oc.get_sensor_description("Moonlight"); }, AlpacaError::InvalidValue);
    CHECK(rig.upstream.calls("averageperiod") == 1);  // the Connect's sample, nothing for the refused writes

    // The factory refuses a poll period outside 1 to 60 s and a malformed upstream.
    remote::RemoteObservingConditionsConfig config;
    config.host = "127.0.0.1";
    config.port = kUpstreamPort;
    for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
        config.poll_period = bad;
        require_alpaca_error(
            [&] { (void)remote::create_remotealpaca_observingconditions(0, config, rig.http.client(), {}, rig.clock); },
            AlpacaError::InvalidValue);
    }
    config.poll_period = std::chrono::seconds(2);
    config.port = 0;
    require_alpaca_error(
        [&] { (void)remote::create_remotealpaca_observingconditions(0, config, rig.http.client(), {}, rig.clock); },
        AlpacaError::InvalidValue);
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - State machine contracts",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    auto& oc = *rig.driver;
    CHECK_FALSE(oc.get_connected());
    CHECK_FALSE(oc.get_connecting());
    CHECK(oc.get_device_state().empty());
    require_alpaca_error([&] { (void)oc.get_temperature(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { oc.refresh(); }, AlpacaError::NotConnected);

    rig.connect();
    CHECK_FALSE(oc.get_connecting());
    const auto state = oc.get_device_state();
    REQUIRE_FALSE(state.empty());
    CHECK(state.back().name == "TimeStamp");
    rig.driver->set_connected(true);  // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    rig.driver->set_connected(false);
    CHECK_FALSE(oc.get_connected());
    CHECK(oc.get_device_state().empty());
    require_alpaca_error([&] { (void)oc.get_temperature(); }, AlpacaError::NotConnected);
    // The poll ended with the Disconnect: the clock moving calls nothing.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == calls);
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - sensors the upstream lacks are NotImplemented with its text",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.upstream.remove_sensor("CloudCover");
    rig.upstream.remove_sensor("StarFWHM");
    auto& oc = *rig.driver;
    // NotConnected before NotImplemented.
    require_alpaca_error([&] { (void)oc.get_cloud_cover(); }, AlpacaError::NotConnected);
    rig.connect();
    CHECK(alpaca_error_message([&] { (void)oc.get_cloud_cover(); }, AlpacaError::NotImplemented) ==
          kPrefix + "Property CloudCover is not implemented");
    require_alpaca_error([&] { (void)oc.get_star_fwhm(); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)oc.get_time_since_last_update("cloudcover"); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)oc.get_sensor_description("StarFWHM"); }, AlpacaError::NotImplemented);
    CHECK(oc.get_temperature() == 19.0);
    // The poll does not ask for a sensor the upstream lacks, and DeviceState omits it.
    const int probes = rig.upstream.calls("cloudcover");
    rig.poll();
    CHECK(rig.upstream.calls("cloudcover") == probes);
    CHECK(oc.get_device_state().size() == 12);
    // Members this driver does not proxy.
    CHECK(oc.get_supported_actions().empty());
    require_alpaca_error([&] { (void)oc.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)oc.command_blind("x", false); }, AlpacaError::NotImplemented);
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - an upstream error passes through unchanged with its host",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.upstream.set_error("humidity", 0x402, "hygrometer has no reading yet");
    rig.upstream.set_put_error("averageperiod", 0x401, "AveragePeriod 0.5 is not supported");
    rig.connect();
    auto& oc = *rig.driver;
    CHECK(alpaca_error_message([&] { (void)oc.get_humidity(); }, AlpacaError::ValueNotSet) ==
          kPrefix + "hygrometer has no reading yet");
    CHECK(alpaca_error_message([&] { oc.set_average_period(0.5); }, AlpacaError::InvalidValue) ==
          kPrefix + "AveragePeriod 0.5 is not supported");
    // An answered error is not a transport failure: the link stays healthy.
    rig.poll();
    rig.poll();
    rig.poll();
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(oc.get_temperature() == 19.0);
    // A write the upstream takes is read back before the next poll (write-through).
    rig.upstream.set_put_error("averageperiod", 0, "");
    oc.set_average_period(0.25);
    CHECK(oc.get_average_period() == 0.25);
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - a refused connection is a lost link at once",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.connect();
    rig.http.refuse_next(1);
    rig.poll();
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    const std::string text =
        alpaca_error_message([&] { (void)rig.driver->get_temperature(); }, AlpacaError::NotConnected);
    CHECK(text.find("connection refused") != std::string::npos);
    CHECK(rig.driver->get_link_fault().find("connection refused") != std::string::npos);
    // The poll ended: nothing reconnects by itself (decision 0009 rule 4).
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    CHECK_FALSE(rig.driver->get_connected());
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - a stalled upstream latches a fault, then is lost at the bound",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.connect();
    auto& oc = *rig.driver;
    rig.http.fail_next(1000);  // a timeout, not a refusal
    rig.poll();
    rig.poll();
    CHECK(oc.get_temperature() == 19.0);  // two failed polls: the last frame is still served
    rig.poll();
    // The third latches: Connected stays true and the frame is refused.
    CHECK(oc.get_connected());
    CHECK_FALSE(oc.get_link_fault().empty());
    require_alpaca_error([&] { (void)oc.get_temperature(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)oc.get_average_period(); }, AlpacaError::DriverException);
    CHECK(oc.get_sensor_description("Temperature") == "Temperature sensor");  // static metadata still answers
    // Lost only at util::kLinkStalenessBound after the latch.
    rig.poll(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    CHECK(oc.get_connected());
    rig.poll(std::chrono::seconds(1));
    CHECK_FALSE(oc.get_connected());
    require_alpaca_error([&] { (void)oc.get_temperature(); }, AlpacaError::NotConnected);
    CHECK(oc.get_device_state().empty());
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - the staleness bound is three poll periods when that is longer",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig(std::chrono::seconds(20));
    rig.connect();
    rig.http.fail_next(1000);
    for (int i = 0; i < 3; ++i) rig.poll(std::chrono::seconds(20));
    REQUIRE_FALSE(rig.driver->get_link_fault().empty());
    rig.poll(std::chrono::seconds(20));
    rig.poll(std::chrono::seconds(20));
    CHECK(rig.driver->get_connected());  // 40 s after the latch, past 30 s but under 60 s
    rig.poll(std::chrono::seconds(20));
    CHECK_FALSE(rig.driver->get_connected());
}

TEST_CASE(
    "Remote Alpaca ObservingConditions Driver - only the client's Connect relinks; the fault text is kept until then",
    "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.connect();
    rig.http.refuse_next(1);
    rig.poll();
    REQUIRE_FALSE(rig.driver->get_connected());
    const std::string fault = rig.driver->get_link_fault();
    CHECK_FALSE(fault.empty());
    rig.clock.advance(std::chrono::seconds(30));
    CHECK(rig.driver->get_link_fault() == fault);

    SECTION("Connect relinks and clears it") {
        rig.connect();
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_temperature() == 19.0);
    }
    SECTION("Disconnect clears it and does not call the lost upstream") {
        rig.driver->set_connected(false);
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.upstream.calls("disconnect") == 0);
    }
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - Refresh is forwarded and measures a new frame",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.connect();
    rig.upstream.set_sensor("Pressure", 1013.25);
    rig.driver->refresh();
    CHECK(rig.upstream.calls("refresh") == 1);
    CHECK(rig.driver->get_pressure() == 1013.25);  // no poll tick in between

    // An upstream that has no Refresh answers its own NotImplemented.
    rig.upstream.set_put_error("refresh", 0x400, "Refresh is not implemented");
    require_alpaca_error([&] { rig.driver->refresh(); }, AlpacaError::NotImplemented);
    CHECK(rig.driver->get_connected());

    // A refused Refresh is a lost link at once, and the poll ends with it.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { rig.driver->refresh(); }, AlpacaError::NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
    const int calls = rig.http.calls();
    rig.poll();
    rig.poll();
    CHECK(rig.http.calls() == calls);
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - a superseded AveragePeriod write leaves the newer one served",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.connect();
    // The first write's body holds after its PUT, before its write-through, until the second write has returned.
    alpacacore::test::Latch first_body;
    std::atomic<bool> held{false};  // only the first body holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) first_body.block();
    });
    std::thread first([&] { rig.driver->set_average_period(5.0); });
    REQUIRE(first_body.wait_entered());
    rig.driver->set_average_period(0.0);  // supersedes the first body; the upstream now holds 0
    first_body.release();
    first.join();
    // Decision 0006 rule 6: the superseded body writes nothing, so the read matches the upstream before any poll.
    CHECK(rig.driver->get_average_period() == 0.0);
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - a superseded Refresh sends nothing after its PUT",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.connect();
    // The first Refresh body holds after its PUT, before it measures, until a second Refresh has returned.
    alpacacore::test::Latch first_body;
    std::atomic<bool> held{false};  // only the first body holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) first_body.block();
    });
    std::thread first([&] { rig.driver->refresh(); });
    REQUIRE(first_body.wait_entered());
    rig.driver->refresh();  // supersedes the first body and measures a frame of its own
    const int calls = rig.http.calls();
    first_body.release();
    first.join();
    // Decision 0006 rule 6: the superseded body neither measures nor publishes a frame.
    CHECK(rig.http.calls() == calls);
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - a reconnect rebuilds the Description, not appends to it",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig;
    rig.upstream.set("description", "\"\"");  // the upstream has no Description: the default stays
    rig.upstream.set("name", "\"Simulator (ObservingConditions, port 2)\"");
    const std::string expected = "ObservingConditions of the upstream Alpaca server 127.0.0.1:11111 device 0 "
                                 "(upstream Name: Simulator (ObservingConditions, port 2))";
    rig.connect();
    CHECK(rig.driver->get_description() == expected);
    rig.driver->set_connected(false);
    rig.connect();
    CHECK(rig.driver->get_description() == expected);

    // An upstream that drops its Description between two Connects leaves the default, not the old text.
    rig.driver->set_connected(false);
    rig.upstream.set("description", "\"Old station text\"");
    rig.connect();
    rig.driver->set_connected(false);
    rig.upstream.set("description", "\"\"");
    rig.connect();
    CHECK(rig.driver->get_description() == expected);
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][observingconditions][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK(text.find("127.0.0.1:11111") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);

    // Another port of this host is another server.
    Rig other(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"::", 6800}}; });
    other.connect();
}

TEST_CASE("Remote Alpaca self-loop check - loopback, local and wildcard addresses count as this server",
          "[remotealpaca][observingconditions][unit]") {
    const std::vector<remote::ServerAddress> wildcard{{"::", 6800}};
    CHECK(remote::self_loop_refusal("127.0.0.1", 6800, wildcard).has_value());
    CHECK(remote::self_loop_refusal("127.0.0.2", 6800, wildcard).has_value());
    CHECK(remote::self_loop_refusal("::1", 6800, wildcard).has_value());
    CHECK(remote::self_loop_refusal("localhost", 6800, wildcard).has_value());
    CHECK(remote::self_loop_refusal("0.0.0.0", 6800, wildcard).has_value());
    CHECK_FALSE(remote::self_loop_refusal("127.0.0.1", 6801, wildcard).has_value());
    CHECK_FALSE(remote::self_loop_refusal("127.0.0.1", 6800, {}).has_value());
    // A server bound to one address is only that address.
    const std::vector<remote::ServerAddress> bound{{"127.0.0.1", 6800}};
    CHECK(remote::self_loop_refusal("127.0.0.1", 6800, bound).has_value());
    CHECK_FALSE(remote::self_loop_refusal("127.0.0.2", 6800, bound).has_value());
}

TEST_CASE("Remote Alpaca ObservingConditions Driver - the poll waits on the task clock and a disconnect ends it",
          "[remotealpaca][observingconditions][unit]") {
    Rig rig(std::chrono::seconds(2));
    rig.connect();
    const int after_connect = rig.upstream.calls("temperature");
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(rig.clock.wait_for_woken_settled(kRendezvous));
    CHECK(rig.upstream.calls("temperature") == after_connect);  // half a period: no poll yet
    rig.poll(std::chrono::seconds(1));
    CHECK(rig.upstream.calls("temperature") == after_connect + 1);
    rig.driver->set_connected(false);
    CHECK(rig.clock.waiter_count() == 0);
}
