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

// Connect/disconnect/operate concurrency stress for the WeeWX
// ObservingConditions driver (issue #101). The HTTP fetch is injected through
// FakeWeeWxFetcher (issue #1070), so connect succeeds and the poll thread that
// fills last_snapshot_ runs during the storm instead of the connect failing in
// fetch_snapshot().

#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_weewx_fetcher.h"

using alpacacore::AlpacaDriver;

namespace {

// Poll interval is the 1 s minimum of its unit; the poll thread fetches once at
// start, so every successful connect exercises it.
alpacacore::vendor::weewx::WeeWxHttpConfig fake_config() {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://weewx.invalid/";
    config.timeout = std::chrono::milliseconds(100);
    config.poll_interval = std::chrono::seconds(1);
    return config;
}

}  // namespace

TEST_CASE("WeeWX observing conditions - concurrent connect/disconnect/operate stress",
          "[weewx][observingconditions][stress]") {
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, fake_config(), fake.fetcher());

    // The poll thread is only reachable once a connect lands: assert it does,
    // and that a snapshot is served, before the storm starts.
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);
    // connect fetched once; the poll thread's immediate first fetch is the second.
    for (int i = 0; i < 200 && fake.calls() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(fake.calls() >= 2);
    {
        auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(*driver);
        CHECK(std::abs(oc.get_temperature() - 10.0) < 1e-6);
    }

    // open-astro#326: StressCallGuard replaces the local call() lambda. It
    // catches std::exception as well, like the lambda did, but records rather
    // than discards -- so anything escaping teardown now fails the case
    // instead of vanishing.
    // The set REPLACES the default {NotConnected}. NotImplemented is here
    // because a WeeWX station legitimately does not carry every sensor this
    // callback reads, so those getters throw "Sensor not implemented" on every
    // pass -- the ASCOM contract working, not a defect. The old call() lambda
    // discarded it; the guard counts it, so it has to be named. (The three
    // NotImplemented variants share one numeric code, so this covers all.)
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::NotImplemented};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(d);
        // The sensor getters throw NotConnected while a racing disconnect has
        // the driver down, and the harness only swallows the exception from
        // the callback as a whole -- so without per-call handling the first
        // throw would skip every call below it.
        guard([&] { static_cast<void>(oc.get_temperature()); });
        guard([&] { static_cast<void>(oc.get_humidity()); });
        guard([&] { static_cast<void>(oc.get_dew_point()); });
        guard([&] { static_cast<void>(oc.get_pressure()); });
        guard([&] { static_cast<void>(oc.get_wind_speed()); });
        guard([&] { static_cast<void>(oc.get_sky_quality()); });
        guard([&] { static_cast<void>(oc.get_sky_temperature()); });
        guard([&] { static_cast<void>(oc.get_average_period()); });
        guard([&] { oc.set_average_period(0.0); });
        guard([&] { static_cast<void>(oc.get_time_since_last_update("temperature")); });
        guard([&] { static_cast<void>(oc.get_sensor_description("temperature")); });
        guard([&] { static_cast<void>(oc.get_device_state()); });
        guard([&] { oc.refresh(); });
    });

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("WeeWX observing conditions - destruction races an in-flight connect",
          "[weewx][observingconditions][stress]") {
    alpacacore::test::FakeWeeWxFetcher fake;
    fake.set_delay(std::chrono::milliseconds(2));
    alpacacore::test::run_destruction_during_connect_stress([&fake]() {
        return alpacacore::vendor::weewx::create_weewx_observingconditions(0, fake_config(), fake.fetcher());
    });
}

TEST_CASE("WeeWX observing conditions - disconnect races a slow in-flight fetch",
          "[weewx][observingconditions][stress]") {
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, fake_config(), fake.fetcher());
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    // The poll thread's first fetch is instant; slow every later one, then
    // disconnect while the poll thread is inside it. Disconnect must join it.
    fake.set_delay(std::chrono::milliseconds(50));
    for (int i = 0; i < 20; ++i) {
        driver->set_connected(false);
        CHECK(driver->get_connected() == false);
        // Disconnect joins the poll thread, so no fetch may outlive it.
        CHECK(fake.in_flight() == 0);
        driver->set_connected(true);  // starts a poll thread that fetches slowly at once
        REQUIRE(driver->get_connected() == true);
        // Wait until the poll thread is inside the fetch, so the next
        // disconnect really races an in-flight one.
        for (int w = 0; w < 200 && fake.in_flight() == 0; ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(fake.in_flight() > 0);
    }
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
    CHECK(fake.in_flight() == 0);
}

TEST_CASE("WeeWX observing conditions - racing disconnect is never dropped", "[weewx][observingconditions][stress]") {
    alpacacore::test::FakeWeeWxFetcher fake;
    // A slow fetch keeps each connect in flight while the disconnect lands.
    fake.set_delay(std::chrono::milliseconds(20));
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, fake_config(), fake.fetcher());

    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
    CHECK(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false) == false);
    CHECK(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true) == false);
    // Teardown joined the poll thread: nothing is still inside the fetcher.
    CHECK(fake.in_flight() == 0);
}

TEST_CASE("WeeWX observing conditions - poll survives scripted fetch failures",
          "[weewx][observingconditions][stress]") {
    alpacacore::test::FakeWeeWxFetcher fake;
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, fake_config(), fake.fetcher());
    driver->set_connected(true);
    REQUIRE(driver->get_connected() == true);

    // The poll thread's immediate first fetch is the second call; wait for it
    // so the next poll fetch is about 1 s away and cannot take the scripted
    // failure meant for refresh().
    for (int i = 0; i < 200 && fake.calls() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(fake.calls() >= 2);

    // refresh() fetches inline, so the scripted failure reaches the caller.
    fake.fail_next(1);
    CHECK_THROWS_AS(static_cast<alpacacore::ObservingConditionsDriver&>(*driver).refresh(), std::runtime_error);

    // The next poll tick (1 s) fails and is caught; the tick after answers
    // again. The thread must still be alive to make that last call.
    const int before = fake.calls();
    fake.fail_next(1);
    for (int i = 0; i < 400 && fake.calls() < before + 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(fake.calls() >= before + 2);
    CHECK(driver->get_connected() == true);
    auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(*driver);
    CHECK(std::abs(oc.get_temperature() - 10.0) < 1e-6);
    driver->set_connected(false);
    CHECK(fake.in_flight() == 0);
}
