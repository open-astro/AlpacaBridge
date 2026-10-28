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
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_touptek_sdk.h"

namespace {

using alpacacore::test::require_alpaca_error;

}  // namespace

TEST_CASE("ToupTek AFW Filter Wheel Driver - Defaults", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);
    CHECK(driver->get_name() == "ToupTek AFW");
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Device metadata", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek AFW Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek AFW Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "TOUPTEK_AFW_3");
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Disconnected behavior", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_supported_actions().empty());

    // Position read/write require a connection.
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);

    // Names/offsets are configurable while disconnected (the slot count is not
    // yet known, so no length validation is imposed).
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0, 10}));
    REQUIRE_NOTHROW(driver->set_names({"L", "R"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0, 10});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L", "R"});
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Platform 7 DeviceState", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);

    // While disconnected, Position throws and is omitted, and TimeStamp itself
    // is withheld too, leaving the ASCOM-required empty list; the
    // non-compliant "Connected" entry must not appear.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Unsupported actions", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Device number assignment", "[touptek][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);
    auto d1 = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(1, 0);
    auto d5 = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(5, 0);

    CHECK(d0->get_device_number() == 0);
    CHECK(d1->get_device_number() == 1);
    CHECK(d5->get_device_number() == 5);
    CHECK(d0->get_unique_id() != d1->get_unique_id());
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Create by id", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(7, "tp-afw-test-id");

    CHECK(driver->get_device_number() == 7);
    CHECK(driver->get_unique_id() == "TOUPTEK_AFW_tp-afw-test-id");
    CHECK(driver->get_connected() == false);
    CHECK(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Set position validation path", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);

    // ASCOM precedence (per driver-lifecycle.instructions.md): range validation precedes the connection
    // check. A negative position is unconditionally invalid, so it is InvalidValue
    // even while disconnected. The upper bound depends on slot_count_ (unknown
    // until connect), so an in-range-but-unverifiable index reports NotConnected;
    // ConformU covers the connected out-of-range InvalidValue path on hardware.
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(99); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - homing poll sleeps on the task clock",
          "[touptek][filterwheel][unit][taskclock]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeToupTekSDK fake;
    alpacacore::test::FakeToupTekSDK::ToupFilterWheelInfo wheel;
    wheel.id = "fake-afw-0";
    wheel.name = "FakeAFW";
    wheel.model_name = "AFW-M";
    fake.wheels.push_back(wheel);
    fake.wheel_position_script = {-1, 0, 0, 0};  // moving, then three settled reads

    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(0, "fake-afw-0", fake, clock);
    auto connect = std::async(std::launch::async, [&] { driver->set_connected(true); });

    // Four polls of 100 ms: -1 resets, then three consecutive real slots.
    for (int poll = 1; poll <= 4; ++poll) {
        REQUIRE(clock.wait_for_wait_count(static_cast<std::uint64_t>(poll), 5s));
        CHECK(fake.call_count("get_filter_wheel_position") == poll - 1);
        clock.advance(99ms);
        CHECK(fake.call_count("get_filter_wheel_position") == poll - 1);
        clock.advance(1ms);
        REQUIRE(clock.wait_for_woken_settled(5s));
    }
    REQUIRE(connect.wait_for(5s) == std::future_status::ready);
    connect.get();
    CHECK(driver->get_connected());
    CHECK(fake.call_count("get_filter_wheel_position") == 4);
}

// --- Position served through util::TtlStatusCache (issue #1072) ---
// The cache TTL is 100 ms and reads the driver's task clock (decision 0005): kPastTtl is a FakeTaskClock
// advance(), never a sleep. The connect homes the wheel on that clock, so connect() steps it until the connect
// returns, and every case counts get_filter_wheel_position from after the connect.

namespace {

using namespace std::chrono_literals;
constexpr auto kPastTtl = 150ms;
constexpr const char* kPositionCall = "get_filter_wheel_position";

struct ConnectedAfw {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeToupTekSDK fake;
    std::unique_ptr<alpacacore::FilterWheelDriver> driver;
    int base = 0;

    ConnectedAfw() {
        alpacacore::test::FakeToupTekSDK::ToupFilterWheelInfo wheel;
        wheel.id = "fake-afw-0";
        wheel.name = "FakeAFW";
        wheel.model_name = "AFW-M";
        fake.wheels.push_back(wheel);
        driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(0, "fake-afw-0", fake, clock);
        connect();
    }
    // Connect, stepping the homing polls (100 ms each) on the fake clock until the connect returns.
    void connect() {
        auto done = std::async(std::launch::async, [this] { driver->set_connected(true); });
        while (done.wait_for(1ms) != std::future_status::ready) clock.advance(100ms);
        done.get();
        base = fake.call_count(kPositionCall);
    }
    ~ConnectedAfw() {
        try {
            driver->set_connected(false);
        } catch (...) {
        }
    }
    int reads() const { return fake.call_count(kPositionCall) - base; }
};

}  // namespace

// Falsified by: touptek_filterwheel_driver.cpp get_position() calling the SDK on every read (two SDK reads
// inside the TTL, not one), or a cache that never expires (no second SDK read after the TTL).
TEST_CASE("ToupTek AFW Filter Wheel Driver - two Position reads inside the TTL cost one SDK call",
          "[touptek][filterwheel][unit][position-cache]") {
    ConnectedAfw wheel;
    wheel.driver->set_position(3);
    wheel.base = wheel.fake.call_count(kPositionCall);

    CHECK(wheel.driver->get_position() == 3);
    CHECK(wheel.driver->get_position() == 3);
    CHECK(wheel.reads() == 1);

    wheel.clock.advance(kPastTtl);
    CHECK(wheel.driver->get_position() == 3);
    CHECK(wheel.reads() == 2);
}

// Falsified by: touptek_filterwheel_driver.cpp building status_cache_ without the driver's task clock (the frame
// ages on the real clock: the read after advance(100ms) is served from the cache, one SDK read, not two).
TEST_CASE("ToupTek AFW Filter Wheel Driver - the Position TTL follows the injected task clock",
          "[touptek][filterwheel][unit][position-cache][taskclock]") {
    ConnectedAfw wheel;
    CHECK(wheel.driver->get_position() == 0);
    wheel.clock.advance(99ms);
    CHECK(wheel.driver->get_position() == 0);
    CHECK(wheel.reads() == 1);
    wheel.clock.advance(1ms);
    CHECK(wheel.driver->get_position() == 0);
    CHECK(wheel.reads() == 2);
}

// Falsified by: touptek_filterwheel_driver.cpp set_position() without status_cache_.invalidate() after the SDK
// move (the read after the move is served from the pre-move frame: position 0, one SDK read).
TEST_CASE("ToupTek AFW Filter Wheel Driver - a Position read after set_position goes to the SDK",
          "[touptek][filterwheel][unit][position-cache]") {
    ConnectedAfw wheel;
    CHECK(wheel.driver->get_position() == 0);
    CHECK(wheel.driver->get_position() == 0);

    wheel.driver->set_position(4);
    CHECK(wheel.driver->get_position() == 4);
    CHECK(wheel.reads() == 2);
}

// The connect homes the wheel for about 300 ms, longer than the TTL, so a stale frame cannot outlive a
// reconnect here; the link-health record can. Two failures before the reconnect and two after must not add up to
// the three that latch the fault. The connect's homing poll reads the SDK directly, never through
// status_cache_: a good homing read through the cache would clear the count and hide a missing reset().
// Falsified by: touptek_filterwheel_driver.cpp connect or disconnect without status_cache_.reset() (the fourth
// failed read in all, the second after the reconnect, throws "communications compromised").
// Passes before the cache exists: it guards the reset once the cache does.
TEST_CASE("ToupTek AFW Filter Wheel Driver - a reconnect clears the Position link-health count",
          "[touptek][filterwheel][unit][position-cache]") {
    ConnectedAfw wheel;
    wheel.fake.throw_from.insert(kPositionCall);
    CHECK_THROWS(wheel.driver->get_position());
    CHECK_THROWS(wheel.driver->get_position());
    wheel.fake.throw_from.erase(kPositionCall);

    wheel.driver->set_connected(false);
    wheel.connect();
    wheel.fake.throw_from.insert(kPositionCall);
    CHECK_THROWS(wheel.driver->get_position());
    try {
        wheel.driver->get_position();
        FAIL("a failed read must throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") == std::string::npos);
    }
    wheel.fake.throw_from.erase(kPositionCall);
}

// Falsified by: touptek_filterwheel_driver.cpp reading Position without the cache's fault threshold (the third
// failed read throws the raw injected failure, not "ToupTek AFW communications compromised"), or serving the
// last good frame after a failed refill (the first failed read returns 0).
TEST_CASE("ToupTek AFW Filter Wheel Driver - failed Position reads are not served and latch a link fault",
          "[touptek][filterwheel][unit][position-cache]") {
    ConnectedAfw wheel;
    CHECK(wheel.driver->get_position() == 0);
    wheel.clock.advance(kPastTtl);

    wheel.fake.throw_from.insert(kPositionCall);
    for (int i = 0; i < 2; ++i) {  // below the threshold the raw failure comes through, not the latch
        try {
            wheel.driver->get_position();
            FAIL("a failed read must throw");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(std::string(ex.what()).find("communications compromised") == std::string::npos);
        }
    }
    try {
        wheel.driver->get_position();
        FAIL("third consecutive failure must throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
    }

    // A good refill clears the latch with no reconnect, and its frame is served again.
    wheel.fake.throw_from.erase(kPositionCall);
    const int before = wheel.reads();
    CHECK(wheel.driver->get_position() == 0);
    CHECK(wheel.driver->get_position() == 0);
    CHECK(wheel.reads() == before + 1);
}

// Falsified by: touptek_filterwheel_driver.cpp caching a -1 frame (the second poll is served -1 from the cache:
// one SDK read for two polls), or mapping -1 to a slot (Position reads 0, not the ASCOM moving sentinel).
TEST_CASE("ToupTek AFW Filter Wheel Driver - Position reads -1 while moving and the move end is seen at once",
          "[touptek][filterwheel][unit][position-cache]") {
    ConnectedAfw wheel;
    wheel.driver->set_position(4);
    wheel.fake.wheel_position_script = {-1, -1, 4};  // after the move command, which clears any script
    wheel.base = wheel.fake.call_count(kPositionCall);

    CHECK(wheel.driver->get_position() == -1);
    CHECK(wheel.driver->get_position() == -1);
    CHECK(wheel.driver->get_position() == 4);  // no sleep: a moving frame is never held
    CHECK(wheel.reads() == 3);
}

// Decision 0016: a member that needs no device access never waits behind a Position refill.
// Falsified by: touptek_filterwheel_driver.cpp get_position() holding mutex_ across the SDK read (get_names()
// waits the whole 400 ms delay).
TEST_CASE("ToupTek AFW Filter Wheel Driver - get_names does not wait behind a slow Position refill",
          "[touptek][filterwheel][unit][position-cache][timing]") {
    ConnectedAfw wheel;
    auto slow = std::make_shared<std::atomic<bool>>(false);
    wheel.fake.before_call = [slow](const std::string& fn) {
        if (slow->load() && fn == kPositionCall) std::this_thread::sleep_for(400ms);
    };
    slow->store(true);
    auto refill = std::async(std::launch::async, [&] { return wheel.driver->get_position(); });
    for (int i = 0; i < 500 && wheel.reads() == 0; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(wheel.reads() == 1);

    const auto start = std::chrono::steady_clock::now();
    CHECK(wheel.driver->get_names().size() == 5);
    CHECK(wheel.driver->get_name() == "FakeAFW");
    CHECK(std::chrono::steady_clock::now() - start < 150ms);
    refill.get();
    slow->store(false);
}

// A move command that throws leaves the cached frame untrusted.
// Falsified by: touptek_filterwheel_driver.cpp set_position() skipping status_cache_.invalidate() when the SDK
// move throws (the read after the failed move is served from the old frame: no second SDK read).
TEST_CASE("ToupTek AFW Filter Wheel Driver - a failed set_position invalidates the cached Position",
          "[touptek][filterwheel][unit][position-cache]") {
    ConnectedAfw wheel;
    CHECK(wheel.driver->get_position() == 0);
    CHECK(wheel.driver->get_position() == 0);
    REQUIRE(wheel.reads() == 1);

    wheel.fake.throw_from.insert("set_filter_wheel_position");
    CHECK_THROWS_AS(wheel.driver->set_position(3), alpacacore::AlpacaException);
    wheel.fake.throw_from.erase("set_filter_wheel_position");
    CHECK(wheel.driver->get_position() == 0);
    CHECK(wheel.reads() == 2);
}

// Decision 0016 rules 1 and 4: the connect (open and handshake) runs off the state lock.
// Falsified by: touptek_filterwheel_driver.cpp set_connected(true) holding mutex_ across open_filter_wheel_by_id
// (get_names() and get_name() wait the whole 400 ms open).
TEST_CASE("ToupTek AFW Filter Wheel Driver - get_names does not wait behind a held connect",
          "[touptek][filterwheel][unit][position-cache][timing]") {
    alpacacore::test::FakeToupTekSDK fake;
    alpacacore::test::FakeToupTekSDK::ToupFilterWheelInfo info;
    info.id = "fake-afw-0";
    info.name = "FakeAFW";
    info.model_name = "AFW-M";
    fake.wheels.push_back(info);
    fake.before_call = [](const std::string& fn) {
        if (fn == "open_filter_wheel_by_id") std::this_thread::sleep_for(400ms);
    };
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(0, "fake-afw-0", fake);
    auto connect = std::async(std::launch::async, [&] { driver->set_connected(true); });
    for (int i = 0; i < 500 && fake.call_count("open_filter_wheel_by_id") == 0; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(fake.call_count("open_filter_wheel_by_id") == 1);

    const auto start = std::chrono::steady_clock::now();
    CHECK_NOTHROW(driver->get_names());
    CHECK_NOTHROW(driver->get_name());
    CHECK(std::chrono::steady_clock::now() - start < 150ms);
    connect.get();
    driver->set_connected(false);
}
