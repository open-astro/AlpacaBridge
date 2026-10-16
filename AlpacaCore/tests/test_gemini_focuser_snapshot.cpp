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

// Gemini focuser getters served from a util::StateSnapshot (open-astro#1229,
// decision 0010). The driver runs on a FakeTaskClock, so its state poll runs
// only when a case advance()s the clock, and a getter's wire traffic is
// counted exactly on the pty fake.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_gemini_focuser.h"
#include "fake_task_clock.h"

using alpacacore::test::FakeGeminiFocuser;
using alpacacore::test::FakeTaskClock;
using namespace std::chrono_literals;

namespace {

// Longer than the poll period (250 ms) so one advance() is one tick.
constexpr auto kTick = 300ms;
constexpr auto kBound = 5000ms;

int wire_reads(const FakeGeminiFocuser& fake) { return fake.count(":00#") + fake.count(":01#") + fake.count(":06#"); }

// A blind command is counted when the fake's reader thread has seen it.
bool wire_saw(const FakeGeminiFocuser& fake, const std::string& prefix) {
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (fake.count(prefix) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    return fake.count(prefix) == 1;
}

// Advances one tick and waits for the poll body to be waiting again.
void tick(FakeTaskClock& clock) {
    clock.advance(kTick);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
}

}  // namespace

TEST_CASE("Gemini Focuser Snapshot - getters between poll ticks cause no wire reads",
          "[gemini][focuser][snapshot][unit][fake]") {
    FakeGeminiFocuser fake;
    FakeTaskClock clock;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_temperature() == Catch::Approx(12.5));

    const int reads_before = wire_reads(fake);
    for (int i = 0; i < 50; ++i) {
        (void)driver->get_position();
        (void)driver->get_is_moving();
        (void)driver->get_temperature();
    }
    CHECK(wire_reads(fake) == reads_before);

    // The next tick measures the device again and the getters follow.
    fake.set_position(2500);
    fake.set_moving(true);
    fake.set_temperature(-3.5);
    CHECK(driver->get_position() == 1000);  // unchanged until the tick
    tick(clock);
    CHECK(driver->get_position() == 2500);
    CHECK(driver->get_is_moving() == true);
    CHECK(driver->get_temperature() == Catch::Approx(-3.5));
    CHECK(wire_reads(fake) == reads_before + 3);

    driver->set_connected(false);
}

TEST_CASE("Gemini Focuser Snapshot - IsMoving is true right after a successful Move",
          "[gemini][focuser][snapshot][unit][fake]") {
    FakeGeminiFocuser fake;  // reports "not moving" always
    FakeTaskClock clock;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->move(4000);
    CHECK(wire_saw(fake, ":054000#"));  // Move still reaches the device
    CHECK(driver->get_is_moving() == true);

    // The poll that follows measures the device, which says it is not moving.
    tick(clock);
    CHECK(driver->get_is_moving() == false);

    driver->set_connected(false);
}

TEST_CASE("Gemini Focuser Snapshot - Halt reaches the device and IsMoving reads false",
          "[gemini][focuser][snapshot][unit][fake]") {
    FakeGeminiFocuser fake;
    FakeTaskClock clock;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
    fake.set_moving(true);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    CHECK(driver->get_is_moving() == true);

    driver->halt();
    CHECK(wire_saw(fake, ":27#"));
    CHECK(driver->get_is_moving() == false);

    driver->set_connected(false);
}

TEST_CASE("Gemini Focuser Snapshot - disconnect stops the poll and getters throw NotConnected",
          "[gemini][focuser][snapshot][unit][fake]") {
    FakeGeminiFocuser fake;
    FakeTaskClock clock;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->set_connected(false);
    CHECK(clock.waiter_count() == 0);  // the poll body ended and was joined
    const int reads = wire_reads(fake);
    clock.advance(kTick);
    CHECK(wire_reads(fake) == reads);

    for (auto probe : {+[](alpacacore::FocuserDriver& d) { (void)d.get_position(); },
                       +[](alpacacore::FocuserDriver& d) { (void)d.get_is_moving(); },
                       +[](alpacacore::FocuserDriver& d) { (void)d.get_temperature(); }}) {
        try {
            probe(*driver);
            FAIL("expected NotConnected");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        }
    }
}

TEST_CASE("Gemini Focuser Snapshot - failed polls latch a link fault and a good poll clears it",
          "[gemini][focuser][snapshot][unit][fake]") {
    FakeGeminiFocuser fake;
    FakeTaskClock clock;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    CHECK(driver->get_position() == 1000);

    fake.set_garbage_state_replies(true);
    tick(clock);
    tick(clock);
    CHECK(driver->get_position() == 1000);  // two failures: still below the threshold
    tick(clock);

    // Latched: the getters and Move refuse, Connected stays true.
    CHECK(driver->get_connected());
    const auto expect_compromised = [](const std::function<void()>& call) {
        try {
            call();
            FAIL("expected DriverException");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(e.what()).find("communications compromised") != std::string::npos);
        }
    };
    expect_compromised([&] { (void)driver->get_position(); });
    expect_compromised([&] { (void)driver->get_is_moving(); });
    expect_compromised([&] { (void)driver->get_temperature(); });
    expect_compromised([&] { driver->move(500); });
    CHECK(fake.count(":05500#") == 0);  // nothing commanded while faulted

    // Halt is the safety stop: never refused.
    driver->halt();
    CHECK(wire_saw(fake, ":27#"));

    // The poll keeps asking while faulted; the first good reply restores service.
    fake.set_garbage_state_replies(false);
    tick(clock);
    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_temperature() == Catch::Approx(12.5));

    driver->set_connected(false);
}

TEST_CASE("Gemini Focuser Snapshot - a temperature probe that fails keeps Position and backs off",
          "[gemini][focuser][snapshot][unit][fake]") {
    FakeGeminiFocuser fake;
    FakeTaskClock clock;
    fake.set_garbage_temperature(true);
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_is_moving() == false);
    try {
        (void)driver->get_temperature();
        FAIL("expected DriverException");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(fake.count(":06#") == 1);  // the connect-time frame asked once

    tick(clock);
    tick(clock);
    CHECK(fake.count(":06#") == 1);  // backed off: Position keeps polling, the probe is not asked

    // After the retry delay the probe is asked again, and a recovered probe is served.
    fake.set_garbage_temperature(false);
    clock.advance(31s);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
    CHECK(fake.count(":06#") == 2);
    CHECK(driver->get_temperature() == Catch::Approx(12.5));

    driver->set_connected(false);
}
