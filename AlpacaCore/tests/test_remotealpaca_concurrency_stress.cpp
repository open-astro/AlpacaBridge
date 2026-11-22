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

// Connect/disconnect/operate concurrency stress for the remotealpaca ObservingConditions proxied device (issues
// #101, #1753). The upstream is the scripted AlpacaUpstreamScript behind FakeHttpClient and the test-only
// LockedHttpClient (decision 0015 rule 2), so every Connect lands and the poll and the command bodies run during the
// storm. The last case races refused-connection losses against the lifecycle and the commands.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_observingconditions_driver.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "alpaca_upstream_script.h"
#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_http_client.h"
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
