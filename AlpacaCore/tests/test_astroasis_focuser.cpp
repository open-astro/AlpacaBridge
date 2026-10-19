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
#include "fake_task_clock.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// Scripted HID device (open-astro#294). Answers the wire protocol the wrapper
// speaks and counts transactions per command. The state is shared so a test
// keeps a handle after handing the transport to the driver.
struct FakeHidState {
    std::mutex mutex;
    int status_reads = 0;  // cmd 0x32
    int config_reads = 0;  // cmd 0x30 / 0x3a
    int moves = 0;         // cmd 0x36
    int halts = 0;         // cmd 0x37
    int position = 1234;
    bool moving = false;
    bool dead = false;         // reads time out, as a pulled cable does
    bool fail_config = false;  // MaxStep (0x30) goes unanswered
    bool fail_move = false;    // the device moves (0x36) but the reply is lost
    bool fail_halt = false;    // the device stops (0x37) but the reply is lost
    int closes = 0;
    bool move_reports_moving = true;  // the device reports "moving" after a Move (0x36)
    std::uint8_t gate_cmd = 0;        // hold this command's exchange open until released
    bool gate_entered = false;        // the gated exchange reached the gate
    std::condition_variable cv;
    std::vector<std::uint8_t> pending;

    int transactions() {
        std::lock_guard<std::mutex> lock(mutex);
        return status_reads + config_reads + moves + halts;
    }
};

class FakeHidTransport final : public alpacacore::vendor::astroasis::AstroasisHidTransport {
public:
    explicit FakeHidTransport(std::shared_ptr<FakeHidState> state) : state_(std::move(state)) {}

    bool open(const std::string&) override { return true; }
    void close() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->closes;
    }

    int write(const std::uint8_t* data, std::size_t length) override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        const std::uint8_t cmd = data[1];
        std::vector<std::uint8_t> resp;
        auto be32 = [&resp](std::uint32_t v) {
            for (int shift = 24; shift >= 0; shift -= 8) resp.push_back(static_cast<std::uint8_t>(v >> shift));
        };
        resp.push_back(cmd);
        switch (cmd) {
            case 0x11:
                resp.insert(resp.end(), {1, 0});
                break;
            case 0x10:
                resp.push_back(4);
                be32(0);
                break;
            case 0x32:
                ++state_->status_reads;
                resp.push_back(14);
                be32(2048);        // internal ADC
                be32(0x80000000);  // no external probe
                resp.push_back(0);
                resp.push_back(state_->moving ? 1 : 0);
                be32(static_cast<std::uint32_t>(state_->position));
                break;
            case 0x30:
                ++state_->config_reads;
                if (state_->fail_config) break;
                resp.push_back(18);
                be32(0xFFFFFFFF);
                be32(50000);
                resp.insert(resp.end(), 10, 0);
                break;
            case 0x36:
                ++state_->moves;
                state_->moving = state_->move_reports_moving;
                hold_gate(lock, cmd);
                if (state_->fail_move) break;
                resp.insert(resp.end(), {1, 0});
                break;
            case 0x37:
                ++state_->halts;
                state_->moving = false;
                hold_gate(lock, cmd);
                if (state_->fail_halt) break;
                resp.insert(resp.end(), {1, 0});
                break;
            default:
                break;
        }
        state_->pending = std::move(resp);
        return static_cast<int>(length);
    }

    int read(std::uint8_t* data, std::size_t length, int) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->dead || state_->pending.empty()) {
            state_->pending.clear();
            return 0;
        }
        std::memset(data, 0, length);
        std::memcpy(data, state_->pending.data(), state_->pending.size());
        state_->pending.clear();
        return static_cast<int>(length);
    }

private:
    // Blocks the exchange of the gated command until the test clears gate_cmd.
    void hold_gate(std::unique_lock<std::mutex>& lock, std::uint8_t cmd) {
        if (state_->gate_cmd != cmd) return;
        state_->gate_entered = true;
        state_->cv.notify_all();
        state_->cv.wait(lock, [this] { return state_->gate_cmd == 0; });
    }

    std::shared_ptr<FakeHidState> state_;
};

std::unique_ptr<alpacacore::FocuserDriver> make_fake_focuser(const std::shared_ptr<FakeHidState>& state,
                                                             alpacacore::test::FakeTaskClock& clock) {
    return alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw-fake",
                                                                   std::make_unique<FakeHidTransport>(state), clock);
}

// Longer than the poll period (100 ms) so one advance() is one tick.
constexpr auto kTick = std::chrono::milliseconds(150);
constexpr auto kBound = std::chrono::milliseconds(5000);

// Advances one tick and waits for the poll body to be waiting again.
void tick(alpacacore::test::FakeTaskClock& clock) {
    clock.advance(kTick);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
}

// A connected driver on a virtual clock. The poll body parks on the clock after
// Connect returns; not a REQUIRE, so a case fails on the behaviour it pins.
std::unique_ptr<alpacacore::FocuserDriver> connected_focuser(const std::shared_ptr<FakeHidState>& state,
                                                             alpacacore::test::FakeTaskClock& clock) {
    auto driver = make_fake_focuser(state, clock);
    driver->set_connected(true);
    (void)clock.wait_for_waiters(1, std::chrono::milliseconds(2000));
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
    tick(clock);
    CHECK(driver->get_position() == 4321);
    CHECK(state->transactions() == after_connect + 1);

    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:set_connected() delete the first poll_state_once() at connect.
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

// Falsified by: astroasis_focuser_driver.cpp:move() delete the snapshot_.write() after move_to.
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
    tick(clock);
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

// Falsified by: astroasis_focuser_driver.cpp:move() delete the snapshot_.write() in move()'s catch block.
TEST_CASE("Astroasis Focuser Driver - a failed move drops the poll that overlaps it",
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
    CHECK(driver->get_position() == 1234);  // the overlapped poll's frame was dropped
    tick(clock);
    CHECK(driver->get_position() == 5555);  // the next poll measured the device
    CHECK(driver->get_is_moving());
    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:halt() delete the snapshot_.write() in halt()'s catch block.
TEST_CASE("Astroasis Focuser Driver - a failed halt drops the poll that overlaps it",
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
    CHECK(driver->get_position() == 1234);  // the overlapped poll's frame was dropped
    tick(clock);
    CHECK(driver->get_position() == 5555);
    CHECK_FALSE(driver->get_is_moving());  // the next poll measured the stop
    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:poll_state_once() take begin_poll() after the device read instead of
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
    tick(clock);
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

// Falsified by: astroasis_focuser_driver.cpp kFaultThreshold raised from 3 to 100 (the link never latches).
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
        tick(clock);
        CHECK(driver->get_position() == 1234);
    }
    CHECK(driver->get_link_fault().empty());
    // The third failed poll latches: DriverException naming the link, Connected untouched.
    tick(clock);
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
    tick(clock);
    CHECK(driver->get_position() == 9999);  // recovers with no reconnect
    CHECK(driver->get_link_fault().empty());

    driver->set_connected(false);
}

// Falsified by: astroasis_focuser_driver.cpp:note_poll_failure() delete the link_lost_ latch past kLinkStalenessBound.
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
    tick(clock);
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

// Falsified by: astroasis_focuser_driver.cpp:set_connected() delete state_poll_.cancel_all_and_join() on disconnect.
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
