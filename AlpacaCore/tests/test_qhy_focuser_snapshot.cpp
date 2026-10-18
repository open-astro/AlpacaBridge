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

// QHY Q-Focuser getters served from a util::StateSnapshot (open-astro#1310,
// decision 0010). The driver runs on a FakeTaskClock, so its state poll runs
// only when a case advance()s the clock, and a getter's wire traffic is
// counted exactly on the pty fake.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_qhy_qfocuser.h"
#include "fake_task_clock.h"

using alpacacore::test::FakeQhyQFocuser;
using alpacacore::test::FakeTaskClock;
using namespace std::chrono_literals;

namespace {

// Longer than the poll period (250 ms) so one advance() is one tick.
constexpr auto kTick = 300ms;
// A muted poll costs the wrapper's 3 s serial timeout, so allow several.
constexpr auto kBound = 15000ms;

int wire_reads(const FakeQhyQFocuser& fake) { return fake.count("\"cmd_id\":5}") + fake.count("\"cmd_id\":4}"); }

// Advances one tick and waits for the poll body to be waiting again.
void tick(FakeTaskClock& clock) {
    clock.advance(kTick);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
}

void expect_code(const std::function<void()>& fn, int code) {
    try {
        fn();
        FAIL("expected an AlpacaException");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == code);
    }
}

std::unique_ptr<alpacacore::FocuserDriver> connected_driver(FakeQhyQFocuser& fake, FakeTaskClock& clock) {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path(), {}, clock);
    driver->set_connected(true);
    // The poll body parks on the clock after Connect returns. Not a REQUIRE:
    // a case then fails on the behaviour it pins, not on this rendezvous.
    (void)clock.wait_for_waiters(1, 2000ms);
    return driver;
}

}  // namespace

TEST_CASE("QHY Q-Focuser Snapshot - getters between poll ticks cause no wire exchange",
          "[qhy][focuser][snapshot][unit][fake]") {
    FakeQhyQFocuser fake;
    FakeTaskClock clock;
    auto driver = connected_driver(fake, clock);

    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_temperature() == Catch::Approx(23.881));

    // Longer than the old 100 ms position TTL, with the task clock unmoved:
    // the old cache refilled here, the snapshot does not.
    const int reads_before = wire_reads(fake);
    std::this_thread::sleep_for(150ms);
    for (int i = 0; i < 50; ++i) {
        (void)driver->get_position();
        (void)driver->get_is_moving();
        (void)driver->get_temperature();
    }
    CHECK(wire_reads(fake) == reads_before);

    // The next tick measures the device again and the getters follow.
    fake.set_position(2500);
    CHECK(driver->get_position() == 1000);  // unchanged until the tick
    tick(clock);
    CHECK(driver->get_position() == 2500);
    CHECK(wire_reads(fake) == reads_before + 2);

    driver->set_connected(false);
}

TEST_CASE("QHY Q-Focuser Snapshot - IsMoving is true right after a Move and false right after a Halt",
          "[qhy][focuser][snapshot][unit][fake]") {
    FakeQhyQFocuser fake;
    fake.set_steps_per_poll(1);  // the motor takes many polls to arrive
    FakeTaskClock clock;
    auto driver = connected_driver(fake, clock);
    REQUIRE(driver->get_is_moving() == false);

    const int reads_before = wire_reads(fake);
    driver->move(4000);
    CHECK(fake.count("\"cmd_id\":6,\"tar\":4000}") == 1);
    CHECK(driver->get_is_moving() == true);  // before any poll ran
    CHECK(driver->get_position() == 1000);   // the measured position until the next poll
    CHECK(wire_reads(fake) == reads_before);

    tick(clock);
    CHECK(driver->get_is_moving() == true);
    CHECK(driver->get_position() == 1001);

    driver->halt();
    CHECK(fake.count("\"cmd_id\":3}") == 1);
    CHECK(driver->get_is_moving() == false);  // before the next poll

    driver->set_connected(false);
}

TEST_CASE("QHY Q-Focuser Snapshot - a stale frame is served until the link-loss bound, then Connected drops",
          "[qhy][focuser][snapshot][unit][fake]") {
    FakeQhyQFocuser fake;
    FakeTaskClock clock;
    auto driver = connected_driver(fake, clock);
    REQUIRE(driver->get_position() == 1000);

    // The device goes quiet behind a healthy port. One poll fails (3 s on the
    // wire) and the frame is now older than the 500 ms freshness bound: it is
    // still served, and Connected is still true.
    fake.set_muted(true);
    clock.advance(1s);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_connected() == true);

    // Past util::kLinkStalenessBound (30 s) with no good frame the link is
    // lost (decision 0009): Connected false, operations NotConnected, the
    // DeviceState list empty, and the poll body ends.
    clock.advance(31s);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    CHECK(driver->get_connected() == false);
    expect_code([&] { (void)driver->get_position(); }, static_cast<int>(alpacacore::AlpacaError::NotConnected));
    expect_code([&] { (void)driver->get_temperature(); }, static_cast<int>(alpacacore::AlpacaError::NotConnected));
    CHECK(driver->get_device_state().empty());
    // The poll in flight ends with the loss; no tick is left waiting.
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (clock.waiter_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(clock.waiter_count() == 0);
    CHECK(driver->get_link_fault() == "no answer to the state poll");

    // No autonomous reconnect, but Connected=true relinks and serves a fresh frame.
    fake.set_muted(false);
    fake.set_position(1700);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    CHECK(driver->get_position() == 1700);
    CHECK(driver->get_link_fault().empty());

    driver->set_connected(false);
}

TEST_CASE("QHY Q-Focuser Snapshot - three failed polls latch a fault that the next good poll clears",
          "[qhy][focuser][snapshot][unit][fake]") {
    FakeQhyQFocuser fake;
    FakeTaskClock clock;
    auto driver = connected_driver(fake, clock);

    fake.set_muted(true);
    for (int i = 0; i < 3; ++i) {
        tick(clock);
    }
    // Faulted, not lost: Connected stays true, the cache is refused, static
    // metadata answers.
    CHECK(driver->get_connected() == true);
    expect_code([&] { (void)driver->get_position(); }, static_cast<int>(alpacacore::AlpacaError::DriverException));
    expect_code([&] { (void)driver->get_is_moving(); }, static_cast<int>(alpacacore::AlpacaError::DriverException));
    CHECK(driver->get_max_step() > 0);
    CHECK(driver->get_link_fault().find("no valid reply to the state poll") == 0);

    // Move is refused by the latch, not by a wire timeout: the fake answers
    // again and still sees no move. Halt is never refused.
    fake.set_muted(false);
    expect_code([&] { driver->move(1200); }, static_cast<int>(alpacacore::AlpacaError::DriverException));
    CHECK(fake.count("\"cmd_id\":6") == 0);
    CHECK_NOTHROW(driver->halt());
    CHECK(fake.count("\"cmd_id\":3}") == 1);

    fake.set_position(1800);
    tick(clock);
    CHECK(driver->get_position() == 1800);

    driver->set_connected(false);
}

TEST_CASE("QHY Q-Focuser Snapshot - DeviceState TimeStamp is when the frame was measured",
          "[qhy][focuser][snapshot][unit][fake]") {
    FakeQhyQFocuser fake;
    FakeTaskClock clock;
    auto driver = connected_driver(fake, clock);

    clock.advance(200ms);  // under the 250 ms period: no poll, the frame ages
    std::string stamp;
    for (const auto& entry : driver->get_device_state()) {
        if (entry.name == "TimeStamp") stamp = std::get<std::string>(entry.value);
    }
    const auto taken = std::chrono::system_clock::now();
    REQUIRE_FALSE(stamp.empty());

    std::tm tm{};
    int ms = 0;
    REQUIRE(std::sscanf(stamp.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                        &tm.tm_min, &tm.tm_sec, &ms) == 7);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    const auto stamped = std::chrono::system_clock::from_time_t(timegm(&tm)) + std::chrono::milliseconds(ms);
    const auto behind = std::chrono::duration_cast<std::chrono::milliseconds>(taken - stamped);
    INFO("TimeStamp is " << behind.count() << " ms behind now");
    CHECK(behind >= 150ms);  // the 200 ms frame age; a request-time stamp is ~0
    CHECK(behind < 5000ms);

    driver->set_connected(false);
}
