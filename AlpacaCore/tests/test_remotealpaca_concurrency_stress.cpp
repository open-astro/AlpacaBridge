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

// Connect/disconnect/operate concurrency stress for the remotealpaca ObservingConditions, SafetyMonitor and Rotator
// proxied devices (issues #101, #1753, #2708). The upstream is the scripted AlpacaUpstreamScript behind FakeHttpClient
// and the test-only LockedHttpClient (decision 0015 rule 2), so every Connect lands and the poll and the command bodies
// run during the storm. The last case of each device races refused-connection losses against the lifecycle and the
// reads.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/safetymonitor_driver.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_observingconditions_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_rotator_driver.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_safetymonitor_driver.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "alpaca_upstream_script.h"
#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_http_client.h"
#include "fake_task_clock.h"
#include "locked_http_client.h"

using alpacacore::AlpacaDriver;

namespace {

namespace remote = alpacacore::vendor::remotealpaca;

remote::RemoteObservingConditionsConfig stress_config() {
    remote::RemoteObservingConditionsConfig config;
    config.host = "127.0.0.1";
    config.port = 11111;
    config.poll_period = std::chrono::seconds(1);
    return config;
}

// The fake behind the locked decorator; the fake and the script must outlive the driver.
std::unique_ptr<alpacacore::ObservingConditionsDriver> make_driver(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_observingconditions(
        0, stress_config(), std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
        alpacacore::util::default_task_clock());
}

// `clock` defaults to the real task clock; the refused-connection case passes a fake one it ticks.
std::unique_ptr<alpacacore::SafetyMonitorDriver> make_safetymonitor(
    alpacacore::test::FakeHttpClient& http,
    alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock()) {
    return remote::create_remotealpaca_safetymonitor(
        0, stress_config(), std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {}, clock);
}

// Every member the SafetyMonitor serves, each in its own guard.
void operate_safetymonitor(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& sm = static_cast<alpacacore::SafetyMonitorDriver&>(d);
    guard([&] { static_cast<void>(sm.get_is_safe()); });
    guard([&] { static_cast<void>(sm.get_device_state()); });
    guard([&] { static_cast<void>(sm.get_name()); });
    guard([&] { static_cast<void>(sm.get_description()); });
    guard([&] { static_cast<void>(sm.get_link_fault()); });
}

std::unique_ptr<alpacacore::RotatorDriver> make_rotator(alpacacore::test::FakeHttpClient& http) {
    return remote::create_remotealpaca_rotator(0, stress_config(),
                                               std::make_shared<alpacacore::test::LockedHttpClient>(http.client()), {},
                                               alpacacore::util::default_task_clock());
}

// Every member the Rotator serves and its six commands, each in its own guard.
void operate_rotator(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& r = static_cast<alpacacore::RotatorDriver&>(d);
    guard([&] { static_cast<void>(r.get_is_moving()); });
    guard([&] { static_cast<void>(r.get_position()); });
    guard([&] { static_cast<void>(r.get_mechanical_position()); });
    guard([&] { static_cast<void>(r.get_target_position()); });
    guard([&] { static_cast<void>(r.get_reverse()); });
    guard([&] { static_cast<void>(r.get_can_reverse()); });
    guard([&] { static_cast<void>(r.get_step_size()); });
    guard([&] { static_cast<void>(r.get_device_state()); });
    guard([&] { static_cast<void>(r.get_name()); });
    guard([&] { static_cast<void>(r.get_link_fault()); });
    guard([&] { r.move_absolute(200.0); });
    guard([&] { r.move(10.0); });
    guard([&] { r.move_mechanical(50.0); });
    guard([&] { r.sync(100.0); });
    guard([&] { r.set_reverse(false); });
    guard([&] { r.halt(); });
}

// Every member the driver serves, each in its own guard so one throw skips no later call.
void operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(d);
    guard([&] { static_cast<void>(oc.get_temperature()); });
    guard([&] { static_cast<void>(oc.get_humidity()); });
    guard([&] { static_cast<void>(oc.get_cloud_cover()); });
    guard([&] { static_cast<void>(oc.get_average_period()); });
    guard([&] { oc.set_average_period(0.0); });
    guard([&] { static_cast<void>(oc.get_time_since_last_update("Temperature")); });
    guard([&] { static_cast<void>(oc.get_sensor_description("Temperature")); });
    guard([&] { static_cast<void>(oc.get_device_state()); });
    guard([&] { static_cast<void>(oc.get_name()); });
    guard([&] { static_cast<void>(oc.get_link_fault()); });
    guard([&] { oc.refresh(); });
}

}  // namespace

TEST_CASE("Remote Alpaca observing conditions - concurrent connect/disconnect/operate stress",
          "[remotealpaca][observingconditions][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    upstream.install(http);
    auto driver = make_driver(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca observing conditions - destruction races an in-flight connect",
          "[remotealpaca][observingconditions][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_driver(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca observing conditions - racing disconnect is never dropped",
          "[remotealpaca][observingconditions][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_driver(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 150 is refused (a Connect makes some sixty, so most land), so the poll and
// Refresh lose the link at once while lifecycle threads connect and disconnect it and readers race the getters. TSan
// checks the loss path's writes (connected_, the kept reason, the dropped frame) against the getters, the commands and
// set_connected().
TEST_CASE("Remote Alpaca observing conditions - refused-connection losses race connects, disconnects and reads",
          "[remotealpaca][observingconditions][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream;
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 150 == 149) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_driver(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(upstream.calls("refresh") > 0);     // commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca safety monitor - concurrent connect/disconnect/operate stress",
          "[remotealpaca][safetymonitor][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::SafetyMonitor};
    upstream.install(http);
    auto driver = make_safetymonitor(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down; nothing else (no fault, no command).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_safetymonitor(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca safety monitor - destruction races an in-flight connect",
          "[remotealpaca][safetymonitor][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::SafetyMonitor};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_safetymonitor(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca safety monitor - racing disconnect is never dropped",
          "[remotealpaca][safetymonitor][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::SafetyMonitor};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_safetymonitor(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// One call in 20 is refused, so a poll loses the link at once while lifecycle threads connect and disconnect it and
// readers race IsSafe. The storm is shorter than one real poll period, so the driver polls on a fake task clock that a
// ticker thread advances a period at a time; a poll the ticker outruns lets the frame age past the loss bound, the
// other loss path (the module's, found by the poll or by IsSafe's own read). A lost link reads IsSafe false with no
// exception, so the guard allows NotConnected only, from a never-connected, connecting or client-disconnected driver;
// the scripted upstream always reads safe, so a false read is a lost link's answer, and the case requires one. TSan
// checks the loss path's writes (connected_, the kept reason, the module's loss latch) against IsSafe's lost() read.
TEST_CASE("Remote Alpaca safety monitor - refused-connection losses race connects, disconnects and IsSafe",
          "[remotealpaca][safetymonitor][stress]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::SafetyMonitor};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 20 == 19) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_safetymonitor(http, clock);
    std::atomic<bool> storm{true};
    std::thread ticker([&clock, &storm] {
        while (storm.load()) {
            clock.advance(std::chrono::seconds(1));  // one poll period: every parked poll samples once
            std::this_thread::yield();
        }
    });

    std::atomic<int> unsafe_reads{0};
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard, &unsafe_reads](AlpacaDriver& d) {
        auto& sm = static_cast<alpacacore::SafetyMonitorDriver&>(d);
        guard([&] {
            if (!sm.get_is_safe()) unsafe_reads.fetch_add(1);
        });
        operate_safetymonitor(guard, d);
    });

    storm.store(false);
    ticker.join();
    // The last connect() may still own its connection task, so a bare set_connected(false) can be recorded against
    // it and return before the task has run (open-astro#326): settle instead, then read the kept reason.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(unsafe_reads.load() > 0);           // the loss path ran during the storm
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca rotator - concurrent connect/disconnect/operate stress", "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    upstream.install(http);
    auto driver = make_rotator(http);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // NotConnected while a racing disconnect has the driver down. InvalidOperation: a command started while a
    // Disconnect joins the command slot is refused ("try again", issue #1713).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_rotator(guard, d); });

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(upstream.calls("moveabsolute") > 0);  // the commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Remote Alpaca rotator - destruction races an in-flight connect", "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));
    alpacacore::test::run_destruction_during_connect_stress([&http]() { return make_rotator(http); });
    CHECK(http.in_flight() == 0);
}

TEST_CASE("Remote Alpaca rotator - racing disconnect is never dropped", "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    upstream.install(http);
    http.set_delay(std::chrono::milliseconds(1));  // keeps each Connect in flight while the disconnect lands
    auto driver = make_rotator(http);
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK(http.in_flight() == 0);
}

// Decision 0009 point 2(a): one call in 40 is refused (a Connect makes some sixteen, so most land), so the poll and
// the commands lose the link at once while lifecycle threads connect and disconnect it and readers race the getters.
// TSan checks the loss path's writes (connected_, the kept reason, the dropped frame) against the getters, the six
// command bodies and their write-throughs, the InterfaceVersion the Connect stores for the poll, and set_connected().
TEST_CASE("Remote Alpaca rotator - refused-connection losses race connects, disconnects, commands and reads",
          "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    std::atomic<int> calls{0};
    http.set_responder([&upstream, &calls](const alpacacore::test::FakeHttpClient::Request& request) {
        if (calls.fetch_add(1) % 40 == 39) {
            throw alpacacore::util::HttpConnectRefused("scripted refusal (connection refused)");
        }
        return upstream.answer(request);
    });
    auto driver = make_rotator(http);

    // A refused command is NotConnected (the loss is its answer); InvalidOperation as above.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { operate_rotator(guard, d); });

    // The last connect() may still own its connection task (open-astro#326): settle instead of a bare Disconnect.
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the Disconnect ended any kept reason
    CHECK(upstream.calls("halt") > 0);        // commands ran on a connected driver, not only NotConnected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

// CanReverse answers while disconnected and while connected, so no read of it throws, whatever Disconnect or Connect
// races it: the driver picks the answer under one state-lock hold. InvalidOperation is never CanReverse's answer; it
// only replaces the guard's default set, so a NotConnected is unexpected here.
TEST_CASE("Remote Alpaca rotator - CanReverse never throws while connects and disconnects race it",
          "[remotealpaca][rotator][stress]") {
    alpacacore::test::FakeHttpClient http;
    alpacacore::test::AlpacaUpstreamScript upstream{alpacacore::test::AlpacaUpstreamScript::Kind::Rotator};
    upstream.install(http);
    auto driver = make_rotator(http);
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::InvalidOperation};
    std::atomic<int> false_reads{0};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard, &false_reads](AlpacaDriver& d) {
        auto& r = static_cast<alpacacore::RotatorDriver&>(d);
        for (int i = 0; i < 50; ++i) {
            guard([&] {
                if (!r.get_can_reverse()) false_reads.fetch_add(1);
            });
        }
    });
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(false_reads.load() == 0);  // the upstream answers true, and so does a driver never connected
    CHECK(http.in_flight() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}
