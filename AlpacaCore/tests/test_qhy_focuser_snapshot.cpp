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
#include "driver_rig.h"
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

using alpacacore::test::require_alpaca_error;
using alpacacore::test::tick;

// The driver on a pty fake and a fake clock, not yet connected.
struct QhyRig : alpacacore::test::PolledRig<FakeQhyQFocuser, alpacacore::FocuserDriver> {
    QhyRig()
        : PolledRig({kTick, kBound}, [](FakeQhyQFocuser& fake, FakeTaskClock& clock) {
              return alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path(), {}, clock);
          }) {}
};

}  // namespace

TEST_CASE("QHY Q-Focuser Snapshot - getters between poll ticks cause no wire exchange",
          "[qhy][focuser][snapshot][unit][fake]") {
    QhyRig rig;
    (void)rig.connect();

    CHECK(rig.driver().get_position() == 1000);
    CHECK(rig.driver().get_is_moving() == false);
    CHECK(rig.driver().get_temperature() == Catch::Approx(23.881));

    // Longer than the old 100 ms position TTL, with the task clock unmoved:
    // the old cache refilled here, the snapshot does not.
    const int reads_before = wire_reads(rig.fake());
    std::this_thread::sleep_for(150ms);
    for (int i = 0; i < 50; ++i) {
        (void)rig.driver().get_position();
        (void)rig.driver().get_is_moving();
        (void)rig.driver().get_temperature();
    }
    CHECK(wire_reads(rig.fake()) == reads_before);

    // The next tick measures the device again and the getters follow.
    rig.fake().set_position(2500);
    CHECK(rig.driver().get_position() == 1000);  // unchanged until the tick
    rig.tick();
    CHECK(rig.driver().get_position() == 2500);
    CHECK(wire_reads(rig.fake()) == reads_before + 2);
}

TEST_CASE("QHY Q-Focuser Snapshot - IsMoving is true right after a Move and false right after a Halt",
          "[qhy][focuser][snapshot][unit][fake]") {
    QhyRig rig;
    rig.fake().set_steps_per_poll(1);  // the motor takes many polls to arrive
    (void)rig.connect();
    REQUIRE(rig.driver().get_is_moving() == false);

    const int reads_before = wire_reads(rig.fake());
    rig.driver().move(4000);
    CHECK(rig.fake().count("\"cmd_id\":6,\"tar\":4000}") == 1);
    CHECK(rig.driver().get_is_moving() == true);  // before any poll ran
    CHECK(rig.driver().get_position() == 1000);   // the measured position until the next poll
    CHECK(wire_reads(rig.fake()) == reads_before);

    rig.tick();
    CHECK(rig.driver().get_is_moving() == true);
    CHECK(rig.driver().get_position() == 1001);

    rig.driver().halt();
    CHECK(rig.fake().count("\"cmd_id\":3}") == 1);
    CHECK(rig.driver().get_is_moving() == false);  // before the next poll
}

TEST_CASE("QHY Q-Focuser Snapshot - a stale frame is served until the link-loss bound, then Connected drops",
          "[qhy][focuser][snapshot][unit][fake]") {
    QhyRig rig;
    (void)rig.connect();
    REQUIRE(rig.driver().get_position() == 1000);

    // The device goes quiet behind a healthy port. One poll fails (3 s on the
    // wire) and the frame is now older than the 500 ms freshness bound: it is
    // still served, and Connected is still true.
    rig.fake().set_muted(true);
    tick(rig.clock(), 1s, kBound);
    CHECK(rig.driver().get_position() == 1000);
    CHECK(rig.driver().get_connected() == true);

    // Past util::kLinkStalenessBound (30 s) with no good frame the link is
    // lost (decision 0009): Connected false, operations NotConnected, the
    // DeviceState list empty, and the poll body ends.
    rig.clock().advance(31s);
    REQUIRE(rig.clock().wait_for_woken_settled(kBound));
    CHECK(rig.driver().get_connected() == false);
    require_alpaca_error([&] { (void)rig.driver().get_position(); },
                         static_cast<int>(alpacacore::AlpacaError::NotConnected));
    require_alpaca_error([&] { (void)rig.driver().get_temperature(); },
                         static_cast<int>(alpacacore::AlpacaError::NotConnected));
    CHECK(rig.driver().get_device_state().empty());
    // The poll in flight ends with the loss; no tick is left waiting.
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (rig.clock().waiter_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(rig.clock().waiter_count() == 0);
    CHECK(rig.driver().get_link_fault() == "no answer to the state poll");

    // No autonomous reconnect, but Connected=true relinks and serves a fresh frame.
    rig.fake().set_muted(false);
    rig.fake().set_position(1700);
    rig.driver().set_connected(true);
    CHECK(rig.driver().get_connected() == true);
    CHECK(rig.driver().get_position() == 1700);
    CHECK(rig.driver().get_link_fault().empty());
}

TEST_CASE("QHY Q-Focuser Snapshot - three failed polls latch a fault that the next good poll clears",
          "[qhy][focuser][snapshot][unit][fake]") {
    QhyRig rig;
    (void)rig.connect();

    rig.fake().set_muted(true);
    for (int i = 0; i < 3; ++i) {
        rig.tick();
    }
    // Faulted, not lost: Connected stays true, the cache is refused, static
    // metadata answers.
    CHECK(rig.driver().get_connected() == true);
    require_alpaca_error([&] { (void)rig.driver().get_position(); },
                         static_cast<int>(alpacacore::AlpacaError::DriverException));
    require_alpaca_error([&] { (void)rig.driver().get_is_moving(); },
                         static_cast<int>(alpacacore::AlpacaError::DriverException));
    CHECK(rig.driver().get_max_step() > 0);
    CHECK(rig.driver().get_link_fault().find("no valid reply to the state poll") == 0);

    // Move is refused by the latch, not by a wire timeout: the fake answers
    // again and still sees no move. Halt is never refused.
    rig.fake().set_muted(false);
    require_alpaca_error([&] { rig.driver().move(1200); }, static_cast<int>(alpacacore::AlpacaError::DriverException));
    CHECK(rig.fake().count("\"cmd_id\":6") == 0);
    CHECK_NOTHROW(rig.driver().halt());
    CHECK(rig.fake().count("\"cmd_id\":3}") == 1);

    rig.fake().set_position(1800);
    rig.tick();
    CHECK(rig.driver().get_position() == 1800);
}

TEST_CASE("QHY Q-Focuser Snapshot - DeviceState TimeStamp is when the frame was measured",
          "[qhy][focuser][snapshot][unit][fake]") {
    QhyRig rig;
    (void)rig.connect();

    rig.clock().advance(200ms);  // under the 250 ms period: no poll, the frame ages
    std::string stamp;
    for (const auto& entry : rig.driver().get_device_state()) {
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
}

// Falsified by: qhy_focuser_driver.cpp:330 carry the target in a driver member that probe() resets before
// PolledState decides to publish (a frame a Move overtook is dropped, its arrival is not undone).
TEST_CASE("QHY Q-Focuser Snapshot - a frame a Move overtook does not clear the target",
          "[qhy][focuser][snapshot][unit][fake]") {
    QhyRig rig;
    rig.fake().set_steps_per_poll(1);
    (void)rig.connect();
    rig.driver().move(4000);
    REQUIRE(rig.driver().get_is_moving() == true);

    // The tick's poll reads position 1001, then its telemetry exchange is held.
    const int telemetry_before = rig.fake().count("\"cmd_id\":4}");
    rig.fake().hold_next_reply_to(4, 600ms);
    rig.clock().advance(kTick);
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (rig.fake().count("\"cmd_id\":4}") == telemetry_before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(rig.fake().count("\"cmd_id\":4}") == telemetry_before + 1);

    // A client Move to the position the poll just read lands while the poll is in flight.
    std::thread mover([&] { rig.driver().move(1001); });
    std::this_thread::sleep_for(150ms);
    mover.join();
    REQUIRE(rig.clock().wait_for_woken_settled(kBound));
    REQUIRE(rig.clock().wait_for_waiters(1, kBound));

    // The motor is knocked off that target: the next poll must still see the Move's target.
    rig.fake().set_position(1500);
    rig.tick();
    CHECK(rig.driver().get_is_moving() == true);
}

// Falsified by: qhy_focuser_driver.cpp:308 move() replace served_frame() with a throw_if_faulted()-only gate
// (a Move against a frame older than the staleness bound reaches the wire).
TEST_CASE("QHY Q-Focuser Snapshot - a Move against a frame older than the staleness bound is refused",
          "[qhy][focuser][snapshot][unit][fake]") {
    QhyRig rig;
    (void)rig.connect();
    REQUIRE(rig.driver().get_position() == 1000);

    // Park the poll inside its telemetry exchange, so it cannot latch the loss
    // itself, and let the held frame age past util::kLinkStalenessBound (30 s).
    const int telemetry_before = rig.fake().count("\"cmd_id\":4}");
    rig.fake().hold_next_reply_to(4, 1500ms);
    rig.clock().advance(kTick);
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (rig.fake().count("\"cmd_id\":4}") == telemetry_before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(rig.fake().count("\"cmd_id\":4}") == telemetry_before + 1);
    rig.clock().advance(31s);

    require_alpaca_error([&] { rig.driver().move(1200); }, static_cast<int>(alpacacore::AlpacaError::NotConnected));
    CHECK(rig.fake().count("\"cmd_id\":6") == 0);  // no move command was sent
    CHECK(rig.driver().get_connected() == false);  // the refused Move latched the loss
}

// Falsified by: qhy_focuser_driver.cpp: set_connected() drop the connect's first-frame check (a connect whose first
// state poll was lost succeeds with no frame).
TEST_CASE("QHY Q-Focuser Driver - a getter after a successful connect never throws DriverException",
          "[qhy][focuser][snapshot][unit]") {
    QhyRig rig;
    rig.fake().set_state_muted(true);
    // Both first polls are lost: the connect is refused, so no getter can run on a
    // session without a frame, and the refused connect leaves nothing behind.
    CHECK_THROWS_AS(rig.driver().set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(rig.driver().get_connected());
    CHECK_FALSE(rig.driver().get_device_firmware().has_value());
    rig.fake().set_state_muted(false);
    // The port was released: a connect with replies back succeeds and serves a frame.
    REQUIRE_NOTHROW(rig.driver().set_connected(true));
    CHECK_NOTHROW(rig.driver().get_position());
    CHECK_NOTHROW(rig.driver().get_is_moving());
}
