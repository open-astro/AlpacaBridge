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
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
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

// Issue #1984: the staleness loss under contention. The poll thread drops Connected and ends its body while
// lifecycle threads connect and disconnect and readers race the getters; a clock thread pushes the fake task clock a
// whole bound per step, so a latched fault is stale at the next poll. All but one fetch in 25 fail, so a Connect still
// lands now and then, and its poll latches and loses the feed again before a good Refresh clears the fault. TSan checks
// the loss path's writes against the getters, Refresh and set_connected(); the counts check that losses happened and
// the driver is coherent after. Falsified by: weewx_observingconditions_driver.cpp lose_feed_locked() not storing
// connected_ false (the undisturbed loss leaves the driver connected).
TEST_CASE("WeeWX observing conditions - staleness losses race connects, disconnects and reads",
          "[weewx][observingconditions][stress]") {
    alpacacore::test::FakeTaskClock clock;  // first: it outlives the driver
    std::atomic<int> mode{0};               // 0 answers, 1 fails every fetch, 2 answers one in 25
    std::atomic<int> fetches{0};
    alpacacore::vendor::weewx::WeeWxFetcher fetcher = [&](const std::string&, std::chrono::milliseconds) {
        const int n = fetches.fetch_add(1);
        const int m = mode.load();
        if (m == 1 || (m == 2 && n % 25 != 0)) {
            throw alpacacore::AlpacaException("scripted WeeWX fetch failure", alpacacore::AlpacaError::DriverException);
        }
        return alpacacore::test::FakeWeeWxFetcher::canned_payload();
    };
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, fake_config(), fetcher, clock);

    std::atomic<int> losses{0};
    // Restores the previous sink on every exit, a failed REQUIRE included, so no later log line reaches `losses`
    // after it is gone.
    struct LossCounter {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        explicit LossCounter(std::atomic<int>& count) {
            alpacacore::logging::set_log_sink(
                [&count](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                    if (level == alpacacore::logging::LogLevel::Error &&
                        message.find("WeeWX feed lost") != std::string_view::npos) {
                        count.fetch_add(1);
                    }
                });
        }
        ~LossCounter() { alpacacore::logging::set_log_sink(previous); }
        LossCounter(const LossCounter&) = delete;
        LossCounter& operator=(const LossCounter&) = delete;
    };
    std::optional<LossCounter> counter(std::in_place, losses);

    // One undisturbed loss first, so the loss coverage check does not depend on the storm's scheduling (#2099).
    REQUIRE(alpacacore::test::connect_and_park(*driver, clock, std::chrono::seconds(5)));
    mode.store(1);
    for (int i = 0; i < 4; ++i) {
        clock.advance(alpacacore::util::kLinkStalenessBound);
        REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    }
    REQUIRE(losses.load() == 1);
    REQUIRE_FALSE(driver->get_connected());

    mode.store(2);
    std::atomic<bool> stop_clock{false};
    std::thread ager([&] {
        // A yield, not a sleep: a sleep's timer slack outlasts most sessions of the storm, and none was lost.
        while (!stop_clock.load()) {
            clock.advance(alpacacore::util::kLinkStalenessBound);
            std::this_thread::yield();
        }
    });

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::NotImplemented,
                                            alpacacore::AlpacaError::DriverException};
    alpacacore::test::run_lifecycle_stress(
        *driver,
        [&guard](AlpacaDriver& d) {
            auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(d);
            guard([&] { static_cast<void>(oc.get_temperature()); });
            guard([&] { static_cast<void>(oc.get_humidity()); });
            guard([&] { static_cast<void>(oc.get_time_since_last_update("temperature")); });
            guard([&] { static_cast<void>(oc.get_time_since_last_update("")); });
            guard([&] { static_cast<void>(oc.get_sensor_description("temperature")); });
            guard([&] { static_cast<void>(oc.get_link_fault()); });
            guard([&] { static_cast<void>(oc.get_device_state()); });
            guard([&] { oc.refresh(); });
        },
        alpacacore::test::StressOptions{1, 4, std::chrono::milliseconds{750}});

    stop_clock.store(true);
    ager.join();
    counter.reset();  // the counted storm is over: restore the previous sink

    CHECK(alpacacore::test::settle_connected(*driver, false));
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    CHECK(losses.load() > 0);

    // Still coherent afterwards: a healthy feed connects and serves a fresh reading.
    mode.store(0);
    REQUIRE(alpacacore::test::connect_and_park(*driver, clock, std::chrono::seconds(5)));
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(static_cast<alpacacore::ObservingConditionsDriver&>(*driver).get_temperature() - 10.0) < 1e-6);
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}
