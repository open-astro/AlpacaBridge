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

// [stress] coverage for the Celestron focus motor (issue #896), the shape of
// the Gemini focuser cases, over fake_celestron_focuser.h.

#ifndef _WIN32

#include <alpacacore/focuser_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/celestron/celestron_focuser_driver.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_celestron_focuser.h"
#include "fake_task_clock.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeCelestronFocuser;

namespace {

void focuser_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
    guard([&] { static_cast<void>(focuser.get_is_moving()); });
    guard([&] { static_cast<void>(focuser.get_position()); });
    guard([&] { static_cast<void>(focuser.get_max_step()); });
    guard([&] { static_cast<void>(focuser.get_max_increment()); });
    guard([&] { focuser.move(1234); });
    guard([&] { static_cast<void>(focuser.action("CalibrationStatus", "")); });
    // Halt resynchronizes before its stop; it must never reopen the port a
    // racing disconnect closed (issue #896).
    guard([&] { focuser.halt(); });
}

}  // namespace

TEST_CASE("Celestron focuser - concurrent connect/disconnect/operate stress", "[celestron][focuser][stress]") {
    FakeCelestronFocuser fake;
    auto driver = alpacacore::vendor::celestron::create_celestron_focuser(0, fake.slave_path());

    // Prove the fake connects before storming it.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK_NOTHROW(driver->set_connected(false));

    // A connected focus motor's only legitimate throw here is NotConnected
    // from a racing disconnect (ensure_connected(), or the transport's own
    // check under its lock in exchange()/resync()). move() clamps and the fake
    // answers every frame, so a DriverException would be a real regression.
    alpacacore::test::StressCallGuard guard({alpacacore::AlpacaError::NotConnected});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { focuser_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Celestron focuser - getters across reconnects never serve a prior session's frame",
          "[celestron][focuser][stress]") {
    // A getter that passed ensure_connected() can still be running while
    // set_connected(false) then set_connected(true) runs. Each
    // session has its own encoder reading; a getter that sees the session
    // current before and after its call must read that session's value.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_focuser(0, fake.slave_path());

    std::atomic<int> epoch{-1};
    std::atomic<bool> done{false};
    std::atomic<int> stale{0};
    std::atomic<int> unexpected{0};
    std::atomic<int> served{0};
    std::mutex first_mutex;
    std::string first_unexpected;
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            auto& focuser = static_cast<alpacacore::FocuserDriver&>(*driver);
            while (!done.load()) {
                const int before = epoch.load();
                try {
                    const int position = focuser.get_position();
                    static_cast<void>(focuser.get_is_moving());
                    static_cast<void>(focuser.get_max_step());
                    static_cast<void>(focuser.get_device_state());
                    if (before >= 0 && epoch.load() == before) {
                        ++served;
                        if (position != 61000 - (30000 + before * 10)) ++stale;
                    }
                } catch (const alpacacore::AlpacaException& ex) {
                    if (ex.error_code() != alpacacore::AlpacaError::NotConnected) {
                        if (unexpected.fetch_add(1) == 0) {
                            std::lock_guard<std::mutex> lock(first_mutex);
                            first_unexpected = ex.what();
                        }
                    }
                }
            }
        });
    }
    for (int cycle = 0; cycle < 25; ++cycle) {
        fake.set_raw_position(static_cast<std::uint32_t>(30000 + cycle * 10));
        driver->set_connected(true);
        epoch.store(cycle);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        epoch.store(-1);
        driver->set_connected(false);
    }
    done.store(true);
    for (auto& reader : readers) reader.join();

    INFO(first_unexpected);
    CHECK(stale.load() == 0);
    CHECK(unexpected.load() == 0);
    CHECK(served.load() > 0);
}

TEST_CASE("Celestron focuser - a getter racing a disconnect logs no link fault", "[celestron][focuser][stress]") {
    // Pins that a disconnect never logs a link fault: the state poll is
    // joined before the port closes and getters run no exchange, so
    // "communications compromised" must not appear for a link closed on purpose.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    std::mutex log_mutex;
    std::vector<std::string> faults;
    const auto previous = alpacacore::logging::get_log_sink();
    alpacacore::logging::set_log_sink([&](alpacacore::logging::LogLevel, std::string_view, std::string_view message) {
        if (message.find("communications compromised") != std::string_view::npos) {
            std::lock_guard<std::mutex> lock(log_mutex);
            faults.emplace_back(message);
        }
    });
    // The state poll waits on a fake clock a ticker thread advances, so polls
    // land throughout the connect/disconnect cycles instead of once per 250 ms.
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::celestron::create_celestron_focuser(0, fake.slave_path(), clock);
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    readers.emplace_back([&] {
        while (!done.load()) {
            clock.advance(std::chrono::milliseconds(300));
            std::this_thread::yield();
        }
    });
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!done.load()) {
                try {
                    static_cast<void>(static_cast<alpacacore::FocuserDriver&>(*driver).get_position());
                } catch (const alpacacore::AlpacaException&) {
                }
            }
        });
    }
    for (int cycle = 0; cycle < 60; ++cycle) {
        driver->set_connected(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        driver->set_connected(false);
    }
    done.store(true);
    for (auto& reader : readers) reader.join();
    alpacacore::logging::set_log_sink(previous);

    std::lock_guard<std::mutex> lock(log_mutex);
    INFO((faults.empty() ? std::string() : faults.front()));
    CHECK(faults.empty());
}

TEST_CASE("Celestron focuser - destruction races an in-flight connect", "[celestron][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<FakeCelestronFocuser>(); },
        [](FakeCelestronFocuser& fake) {
            return alpacacore::vendor::celestron::create_celestron_focuser(0, fake.slave_path());
        },
        25);
}

TEST_CASE("Celestron focuser - racing disconnect is never dropped", "[celestron][focuser][stress]") {
    FakeCelestronFocuser fake;
    auto driver = alpacacore::vendor::celestron::create_celestron_focuser(0, fake.slave_path());
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

#endif  // _WIN32
