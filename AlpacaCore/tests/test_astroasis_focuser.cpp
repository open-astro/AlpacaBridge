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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_hid_transport.h>
#include <alpacacore/version.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_astroasis_hid.h"
#include "fake_task_clock.h"
#include "locked_astroasis_hid_transport.h"

namespace {

using alpacacore::test::connect_and_park;
using alpacacore::test::require_alpaca_error;
using alpacacore::test::tick;

using alpacacore::test::FakeHidState;
using alpacacore::test::FakeHidTransport;

std::unique_ptr<alpacacore::FocuserDriver> make_fake_focuser(const std::shared_ptr<FakeHidState>& state,
                                                             alpacacore::test::FakeTaskClock& clock) {
    return alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw-fake",
                                                                   std::make_unique<FakeHidTransport>(state), clock);
}

// Longer than the poll period (100 ms) so one advance() is one tick.
constexpr auto kTick = std::chrono::milliseconds(150);
constexpr auto kBound = std::chrono::milliseconds(5000);

// A connected driver on a virtual clock. The poll body parks on the clock after
// Connect returns; not a REQUIRE, so a case fails on the behaviour it pins.
std::unique_ptr<alpacacore::FocuserDriver> connected_focuser(const std::shared_ptr<FakeHidState>& state,
                                                             alpacacore::test::FakeTaskClock& clock) {
    auto driver = make_fake_focuser(state, clock);
    (void)connect_and_park(*driver, clock, std::chrono::milliseconds(2000));
    return driver;
}

}  // namespace

// Falsified by: astroasis_focuser_driver.cpp:read_status() getter reads the device again (a poll-less cache refill).
TEST_CASE("Astroasis Focuser Driver - getters between poll ticks cause no device calls",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    CHECK(driver->get_position() == 1234);
    CHECK_FALSE(driver->get_is_moving());
    const int after_connect = state->transactions();

    // A client that polls slower than the old 100 ms cache lifetime: real time
    // passes, the virtual clock does not, so no tick runs.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    for (int i = 0; i < 50; ++i) {
        (void)driver->get_position();
        (void)driver->get_is_moving();
        (void)driver->get_temperature();
    }
    CHECK(state->transactions() == after_connect);

    // The next tick measures the device once and the getters follow.
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->position = 4321;
    }
    CHECK(driver->get_position() == 1234);  // unchanged until the tick
    tick(clock, kTick, kBound);
    CHECK(driver->get_position() == 4321);
    CHECK(state->transactions() == after_connect + 1);

    driver->set_connected(false);
}

// Falsified by: polled_state.h:102 start() delete the first poll_once() at connect.
TEST_CASE("Astroasis Focuser Driver - DeviceState reads the device not at all",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    REQUIRE(driver->get_connected());
    const int after_connect = state->transactions();
    CHECK(state->status_reads >= 1);  // the first frame was taken during connect

    const auto device_state = driver->get_device_state();
    CHECK_FALSE(device_state.empty());
    CHECK(state->transactions() == after_connect);

    driver->set_connected(false);
}

TEST_CASE("Astroasis Focuser Driver - static members never touch the device", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    const int after_connect = state->transactions();

    for (int i = 0; i < 5; ++i) {
        CHECK(driver->get_max_step() == 50000);
        CHECK(driver->get_max_increment() == 50000);
    }
    CHECK(state->transactions() == after_connect);

    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:move():324 delete the state_.write() after move_to.
TEST_CASE("Astroasis Focuser Driver - Move and Halt write through to the frame",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    state->move_reports_moving = false;  // the device's own answer would say "not moving"
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);

    CHECK_FALSE(driver->get_is_moving());
    const int reads_before = state->status_reads;
    driver->move(2000);
    CHECK(driver->get_is_moving());  // before any poll ran
    CHECK(state->status_reads == reads_before);
    driver->halt();
    CHECK_FALSE(driver->get_is_moving());
    driver->move(3000);
    CHECK(driver->get_is_moving());
    driver->halt();
    CHECK_FALSE(driver->get_is_moving());
    CHECK(state->status_reads == reads_before);

    // The poll that follows measures the device again.
    tick(clock, kTick, kBound);
    CHECK(state->status_reads == reads_before + 1);

    driver->set_connected(false);
}

// Runs `act` (a Move or Halt) with its exchange held open and a poll queued behind it, then releases both.
// The poll took its token before the write landed, so a frame it publishes would overwrite the write.
void act_during_poll(const std::shared_ptr<FakeHidState>& state, alpacacore::test::FakeTaskClock& clock,
                     std::uint8_t cmd, const std::function<void()>& act) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->gate_cmd = cmd;
        state->gate_entered = false;
    }
    std::thread actor(act);
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        REQUIRE(state->cv.wait_for(lock, kBound, [&] { return state->gate_entered; }));
    }
    clock.advance(kTick);
    // The woken poll body blocks on the wrapper, not on the clock, so the clock
    // does not settle until the exchange is released.
    CHECK_FALSE(clock.wait_for_woken_settled(std::chrono::milliseconds(300)));
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->gate_cmd = 0;
    }
    state->cv.notify_all();
    actor.join();
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
}

// The overlapped poll sampled after the failed exchange released the wrapper, so its frame is real data
// (decision 0010, point 4) and may land before or after the catch write: 1234 or 5555 are both right.
// What must hold is that nothing invents a value and the next poll measures the device.
// Falsified by: astroasis_focuser_driver.cpp:move() catch block writes frame.position = position instead
// of an empty frame write (the position reads 2000 after the failed move).
TEST_CASE("Astroasis Focuser Driver - the next poll measures the device after a failed move",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    REQUIRE(driver->get_position() == 1234);

    state->fail_move = true;  // the device moves, the reply is lost
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->position = 5555;
    }
    act_during_poll(state, clock, 0x36, [&] { CHECK_THROWS_AS(driver->move(2000), alpacacore::AlpacaException); });
    const int overlapped = driver->get_position();
    CHECK((overlapped == 1234 || overlapped == 5555));  // dropped, or measured after the failed exchange
    tick(clock, kTick, kBound);
    CHECK(driver->get_position() == 5555);  // the next poll measured the device
    CHECK(driver->get_is_moving());
    driver->set_connected(false);
}

// Same as the move twin: the overlapped poll's frame is real data, so 1234 or 5555 are both right.
// Falsified by: astroasis_focuser_driver.cpp:halt() catch block writes frame.position = 0 instead of
// an empty frame write (the position reads 0 after the failed halt).
TEST_CASE("Astroasis Focuser Driver - the next poll measures the device after a failed halt",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    driver->move(2000);
    REQUIRE(driver->get_is_moving());

    state->fail_halt = true;  // the device stops, the reply is lost
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->position = 5555;
    }
    act_during_poll(state, clock, 0x37, [&] { CHECK_THROWS_AS(driver->halt(), alpacacore::AlpacaException); });
    const int overlapped = driver->get_position();
    CHECK((overlapped == 1234 || overlapped == 5555));  // dropped, or measured after the failed exchange
    tick(clock, kTick, kBound);
    CHECK(driver->get_position() == 5555);
    CHECK_FALSE(driver->get_is_moving());  // the next poll measured the stop
    driver->set_connected(false);
}

// Falsified by: polled_state.h:201 poll_once() take begin_poll() after the device read instead of
// before.
TEST_CASE("Astroasis Focuser Driver - a poll that overlaps a write drops its frame",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    state->move_reports_moving = false;
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    REQUIRE_FALSE(driver->get_is_moving());

    // The Move's write lands before the queued poll reads the device, which
    // still answers "not moving".
    act_during_poll(state, clock, 0x36, [&] { driver->move(2000); });

    // The poll's frame (moving=false) was dropped; the write-through stands.
    CHECK(driver->get_is_moving());
    tick(clock, kTick, kBound);
    CHECK_FALSE(driver->get_is_moving());  // the next poll publishes normally

    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:set_connected() delete protocol_.disconnect() in the MaxStep catch at
// connect.
TEST_CASE("Astroasis Focuser Driver - failed MaxStep read at connect closes the handle", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_fake_focuser(state, clock);
    state->fail_config = true;
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        CHECK(state->closes >= 1);
        state->fail_config = false;
    }
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_max_step() == 50000);
    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:39 kFaultThreshold raised from 3 to 100 (the link never latches).
TEST_CASE("Astroasis Focuser Driver - failed polls are not served once the link latches",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    REQUIRE(driver->get_position() == 1234);

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->dead = true;
        state->position = 9999;
    }
    // Below the threshold the last good frame is still served (decision 0010:
    // stale is never a refusal); a failed poll publishes nothing.
    for (int i = 0; i < 2; ++i) {
        tick(clock, kTick, kBound);
        CHECK(driver->get_position() == 1234);
    }
    CHECK(driver->get_link_fault().empty());
    // The third failed poll latches: DriverException naming the link, Connected untouched.
    tick(clock, kTick, kBound);
    try {
        (void)driver->get_position();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(driver->get_connected());
    CHECK(driver->get_max_step() == 50000);  // static metadata keeps answering
    CHECK_FALSE(driver->get_link_fault().empty());

    // The device answers again but no poll has run: Move is refused by the
    // latch (nothing reaches the device), Halt is never refused.
    int moves_before = 0;
    int halts_before = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->dead = false;
        moves_before = state->moves;
        halts_before = state->halts;
    }
    try {
        driver->move(2000);
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK_NOTHROW(driver->halt());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        CHECK(state->moves == moves_before);
        CHECK(state->halts == halts_before + 1);
    }
    tick(clock, kTick, kBound);
    CHECK(driver->get_position() == 9999);  // recovers with no reconnect
    CHECK(driver->get_link_fault().empty());

    driver->set_connected(false);
}

// Falsified by: astroasis_protocol_wrapper.cpp send_command() drop the link_lost_ latch on a failed hid write;
// astroasis_focuser_driver.cpp probe() drop the !link_alive() release;
// astroasis_focuser_driver.cpp get_link_fault() move the HID transport check after state_.fault().
TEST_CASE("Astroasis Focuser Driver - a removed transport drops Connected at once, not after the staleness bound",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    REQUIRE(driver->get_connected());

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->removed = true;
    }
    // One failed poll, far short of the 3-failure fault latch and the 30 s bound.
    tick(clock, kTick, kBound);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault() == "HID transport error");
    require_alpaca_error([&] { (void)driver->get_position(); }, alpacacore::AlpacaError::NotConnected);

    // The next poll releases the transport instead of writing to the removed node again.
    tick(clock, kTick, kBound);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        CHECK(state->closes >= 1);
    }
    // Further failed polls latch the poll fault, which must not replace the transport text (decision 0009, point 5).
    tick(clock, kTick, kBound);
    tick(clock, kTick, kBound);
    tick(clock, kTick, kBound);
    CHECK(driver->get_link_fault() == "HID transport error");

    // Connected=true over the dead link reconnects instead of taking the idempotency return.
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->removed = false;
    }
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_position() == 1234);
    driver->set_connected(false);
}

// Falsified by: polled_state.h:206 latch_lost() (note_failure() calls it, :253) drop the
// lost_.exchange(true) latch past kLinkStalenessBound.
TEST_CASE("Astroasis Focuser Driver - a link silent past the staleness bound drops Connected",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    REQUIRE(driver->get_position() == 1234);

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->dead = true;
    }
    tick(clock, kTick, kBound);
    CHECK(driver->get_position() == 1234);  // stale, still served
    CHECK(driver->get_connected());

    clock.advance(std::chrono::seconds(31));
    REQUIRE(clock.wait_for_woken_settled(kBound));
    CHECK_FALSE(driver->get_connected());
    CHECK_THROWS_AS(driver->get_position(), alpacacore::AlpacaException);
    try {
        (void)driver->get_is_moving();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    CHECK(driver->get_device_state().empty());
    CHECK_FALSE(driver->get_link_fault().empty());
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (clock.waiter_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(clock.waiter_count() == 0);  // the poll ended with the loss
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        CHECK(state->closes >= 1);  // the transport was released
    }

    // No autonomous reconnect, but Connected=true relinks and serves a fresh frame.
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->dead = false;
        state->position = 777;
    }
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_position() == 777);
    CHECK(driver->get_link_fault().empty());

    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:308 move() replace served_frame() with a throw_if_faulted()-only gate
// (a Move against a frame older than the staleness bound reaches the wire).
TEST_CASE("Astroasis Focuser Driver - a Move against a frame older than the staleness bound is refused",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    REQUIRE(driver->get_position() == 1234);

    // Park the poll inside its status exchange, so it cannot latch the loss
    // itself, and let the held frame age past util::kLinkStalenessBound (30 s).
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->gate_cmd = 0x32;
        state->gate_entered = false;
    }
    clock.advance(kTick);
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        REQUIRE(state->cv.wait_for(lock, kBound, [&] { return state->gate_entered; }));
    }
    clock.advance(std::chrono::seconds(31));

    // The Move runs on its own thread: with the gate removed it would reach
    // the wire and wait behind the held exchange.
    std::atomic<bool> finished{false};
    int code = 0;
    std::thread mover([&] {
        try {
            driver->move(2000);
        } catch (const alpacacore::AlpacaException& ex) {
            code = ex.error_code();
        }
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->gate_cmd = 0;
    }
    state->cv.notify_all();
    mover.join();
    CHECK(finished.load());
    CHECK(code == alpacacore::AlpacaError::NotConnected);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        CHECK(state->moves == 0);  // no move command was sent
    }
    CHECK_FALSE(driver->get_connected());  // the refused Move latched the loss

    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:device_state_age() delete the override (the stamp is the request time).
TEST_CASE("Astroasis Focuser Driver - DeviceState TimeStamp is when the frame was measured",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);

    clock.advance(std::chrono::milliseconds(90));  // under the 100 ms period: no poll, the frame ages
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
    CHECK(behind >= std::chrono::milliseconds(60));  // the 90 ms frame age; a request-time stamp is ~0
    CHECK(behind < std::chrono::milliseconds(5000));

    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:set_connected():210 delete state_.stop_and_join() on disconnect.
TEST_CASE("Astroasis Focuser Driver - disconnect stops the poll", "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    driver->set_connected(false);
    CHECK(clock.waiter_count() == 0);
    const int reads = state->status_reads;
    clock.advance(kTick);
    CHECK(state->status_reads == reads);
    CHECK_THROWS_AS(driver->get_position(), alpacacore::AlpacaException);
}

TEST_CASE("Astroasis Focuser Driver - Defaults", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Astroasis Oasis Focuser");
}

TEST_CASE("Astroasis Focuser Driver - Metadata", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    CHECK(driver->get_description() == "Astroasis Oasis Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Astroasis Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "ASTROASIS_FOCUSER_0");
}

TEST_CASE("Astroasis Focuser Driver - Disconnected Behavior", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw0");

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    // open-astro#309: TempCompAvailable and TempComp are properties, so a
    // disconnected read refuses rather than answering. These used to assert
    // the value, which is what let the missing connection check survive.
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Astroasis Focuser Driver - Connecting State", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);
}

TEST_CASE("Astroasis Focuser Driver - Device Number Assignment", "[astroasis][focuser][unit]") {
    auto driver0 = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");
    auto driver1 = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw1");
    auto driver5 = alpacacore::vendor::astroasis::create_astroasis_focuser(5, "/dev/hidraw2");

    REQUIRE(driver0->get_device_number() == 0);
    REQUIRE(driver1->get_device_number() == 1);
    REQUIRE(driver5->get_device_number() == 5);
}

TEST_CASE("Astroasis Focuser Driver - Unique IDs", "[astroasis][focuser][unit]") {
    auto driver0 = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");
    auto driver1 = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw1");

    REQUIRE(driver0->get_unique_id() != driver1->get_unique_id());
    CHECK(driver0->get_unique_id() == "ASTROASIS_FOCUSER_0");
    CHECK(driver1->get_unique_id() == "ASTROASIS_FOCUSER_1");
}

namespace {

// Inner transport that reports the most calls ever inside it at once. Each call dwells long enough for a
// second thread to enter if nothing serialises them.
struct OverlapProbe {
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> calls{0};
};

class OverlapProbeTransport final : public alpacacore::vendor::astroasis::AstroasisHidTransport {
public:
    explicit OverlapProbeTransport(std::shared_ptr<OverlapProbe> probe) : probe_(std::move(probe)) {}
    bool open(const std::string&) override { return dwell(), true; }
    void close() override { dwell(); }
    int write(const std::uint8_t*, std::size_t length) override { return dwell(), static_cast<int>(length); }
    int read(std::uint8_t*, std::size_t, int) override { return dwell(), 0; }

private:
    void dwell() {
        const int now = probe_->inside.fetch_add(1) + 1;
        int seen = probe_->max_inside.load();
        while (now > seen && !probe_->max_inside.compare_exchange_weak(seen, now)) {
        }
        probe_->calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        probe_->inside.fetch_sub(1);
    }

    std::shared_ptr<OverlapProbe> probe_;
};

}  // namespace

// Falsified by: locked_astroasis_hid_transport.h: write() and read() forward without the lock_guard.
TEST_CASE("Astroasis FakeHid Locked decorator - concurrent write and read never overlap in the inner transport",
          "[astroasis][focuser][unit]") {
    auto probe = std::make_shared<OverlapProbe>();
    alpacacore::test::LockedAstroasisHidTransport locked(std::make_unique<OverlapProbeTransport>(probe));
    constexpr int kIterations = 200;
    std::array<std::uint8_t, 8> buffer{};
    std::thread writer([&] {
        for (int i = 0; i < kIterations; ++i) locked.write(buffer.data(), buffer.size());
    });
    std::thread reader([&] {
        std::array<std::uint8_t, 8> in{};
        for (int i = 0; i < kIterations; ++i) locked.read(in.data(), in.size(), 0);
    });
    writer.join();
    reader.join();
    locked.open("/dev/hidraw-fake");
    locked.close();
    CHECK(probe->calls.load() == 2 * kIterations + 2);
    CHECK(probe->max_inside.load() == 1);
}

// Falsified by: astroasis_protocol_wrapper.cpp:136: HidapiTransport::open() returns true without a hidapi open.
TEST_CASE("Astroasis Focuser Driver - by-path factory builds the hidapi transport", "[astroasis][focuser][unit]") {
    // The sentinel is not a hidraw node, so only a real hidapi open fails on it with this message; a fake or
    // mute transport would connect or fail later, in the handshake.
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw-alpacabridge-absent");
    REQUIRE_FALSE(driver->get_connected());
    try {
        driver->set_connected(true);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("Failed to open Astroasis focuser HID device") != std::string::npos);
    }
    CHECK_FALSE(driver->get_connected());
}

// Falsified by: astroasis_focuser_driver.cpp: set_connected() drop the connect's first-frame check (a connect whose
// first state poll was lost succeeds with no frame).
TEST_CASE("Astroasis Focuser Driver - a getter after a successful connect never throws DriverException",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    state->fail_status = true;
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_fake_focuser(state, clock);
    // Both first polls are lost: the connect is refused, so no getter can run on a
    // session without a frame, and the refused connect leaves nothing behind.
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
    CHECK(state->closes == 1);  // the refused connect released the transport
    state->fail_status = false;
    // The port was released: a connect with replies back succeeds and serves a frame.
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK_NOTHROW(driver->get_position());
    CHECK_NOTHROW(driver->get_is_moving());
    driver->set_connected(false);
}

// Falsified by: astroasis_protocol_wrapper.cpp:send_command() drop the link_lost_ latch on a failed hid read.
TEST_CASE("Astroasis Focuser Driver - a failed HID read drops Connected at once while writes succeed",
          "[astroasis][focuser][snapshot][unit]") {
    auto state = std::make_shared<FakeHidState>();
    alpacacore::test::FakeTaskClock clock;
    auto driver = connected_focuser(state, clock);
    REQUIRE(driver->get_connected());

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->read_fails = true;
    }
    tick(clock, kTick, kBound);
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_position(); }, alpacacore::AlpacaError::NotConnected);

    driver->set_connected(false);
}
