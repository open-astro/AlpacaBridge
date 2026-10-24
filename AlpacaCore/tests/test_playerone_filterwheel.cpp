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
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_playerone_pw_sdk.h"
#include "locked_playerone_pw_sdk.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

using alpacacore::vendor::playerone::create_playerone_filterwheel;

}  // namespace

TEST_CASE("Player One PW Filter Wheel Driver - Defaults", "[playerone][filterwheel][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Player One Phoenix Wheel");
}

TEST_CASE("Player One PW Filter Wheel Driver - Disconnected Behavior", "[playerone][filterwheel][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: Position throws while disconnected and is
    // omitted, and TimeStamp itself is withheld too, leaving the
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

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
    // Range validation precedes the connection check (ASCOM precedence): a negative
    // position is InvalidValue even while disconnected (matches the ToupTek AFW).
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0}));
    REQUIRE_NOTHROW(driver->set_names({"L"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L"});

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Player One PW Filter Wheel Driver - Device metadata", "[playerone][filterwheel][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Player One Phoenix Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Player One Phoenix Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "PlayerOne_PW_3");
}

TEST_CASE("Player One PW Filter Wheel Driver - Default names applied", "[playerone][filterwheel][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);

    // Empty entries are replaced with the ASCOM-conventional "Filter N".
    driver->set_names({"Red", "", "Blue"});
    REQUIRE(driver->get_names() == std::vector<std::string>{"Red", "Filter 2", "Blue"});
}

TEST_CASE("Player One PW Filter Wheel Driver - Device Number Assignment", "[playerone][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);
    auto d1 = alpacacore::vendor::playerone::create_playerone_filterwheel(1, 0);
    auto d5 = alpacacore::vendor::playerone::create_playerone_filterwheel(5, 0);

    CHECK(d0->get_device_number() == 0);
    CHECK(d1->get_device_number() == 1);
    CHECK(d5->get_device_number() == 5);
}

TEST_CASE("Player One PW Filter Wheel Driver - Unique IDs", "[playerone][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);
    auto d1 = alpacacore::vendor::playerone::create_playerone_filterwheel(1, 0);

    CHECK(d0->get_unique_id() != d1->get_unique_id());
}

// --- SDK seam: driver over FakePlayerOnePWSDK / LockedPlayerOnePWSDK (issue #1256) ---

TEST_CASE("Player One PW Filter Wheel Driver - connects over an injected SDK",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    fake.set_offset(1, 25);
    fake.set_alias(2, "");
    auto driver = create_playerone_filterwheel(0, 0, fake);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.open_count() == 1);
    CHECK(driver->get_name() == "FakePhoenixWheel");
    CHECK(driver->get_unique_id() == "PlayerOne_PW_SN_fake-pw-41");
    // Aliases and offsets come from the wheel; an empty alias gets the ASCOM default.
    CHECK(driver->get_names() == std::vector<std::string>{"Slot1", "Slot2", "Filter 3", "Slot4", "Slot5"});
    CHECK(driver->get_focus_offsets() == std::vector<int>{0, 25, 0, 0, 0});

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.open_count() == 0);
    CHECK(fake.call_count("close_wheel") == 1);
}

TEST_CASE("Player One PW Filter Wheel Driver - every Phoenix lineup connects with its slot count",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    CHECK_FALSE(fake.set_slot_count(6));
    for (const int slots : {5, 7, 8}) {
        INFO(slots);
        REQUIRE(fake.set_slot_count(slots));
        auto driver = create_playerone_filterwheel(0, 0, fake);
        driver->set_connected(true);
        CHECK(driver->get_names().size() == static_cast<std::size_t>(slots));
        CHECK(driver->get_focus_offsets().size() == static_cast<std::size_t>(slots));
        driver->set_position(slots - 1);
        CHECK(driver->get_position() == slots - 1);
        // The driver refuses the slot itself; the request never reaches the SDK.
        const int moves = fake.call_count("goto_position");
        require_alpaca_error([&] { driver->set_position(slots); }, alpacacore::AlpacaError::InvalidValue);
        CHECK(fake.call_count("goto_position") == moves);
        driver->set_connected(false);
    }
    CHECK(fake.open_count() == 0);
}

TEST_CASE("Player One PW Filter Wheel Driver - Position is -1 while the wheel moves",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    fake.set_moving_reads(2);
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);

    driver->set_position(3);
    // A second move while the first is under way is refused, not queued.
    require_alpaca_error([&] { driver->set_position(1); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == 3);
    // Moving to the slot the wheel is already on never reports motion.
    driver->set_position(3);
    CHECK(driver->get_position() == 3);
    driver->set_connected(false);
}

TEST_CASE("Player One PW Filter Wheel Driver - no wheel detected refuses the connect",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    fake.set_no_wheels(true);
    auto driver = create_playerone_filterwheel(0, 0, fake);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK(fake.call_count("open_wheel") == 0);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Player One PW Filter Wheel Driver - wheel index out of range is InvalidValue",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 4, fake);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(fake.call_count("open_wheel") == 0);
}

TEST_CASE("Player One PW Filter Wheel Driver - a wheel with no positions closes what the connect opened",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    fake.set_raw_slot_count(0);
    auto driver = create_playerone_filterwheel(0, 0, fake);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.call_count("open_wheel") == 1);
    CHECK(fake.call_count("close_wheel") == 1);
    CHECK(fake.open_count() == 0);
}

TEST_CASE("Player One PW Filter Wheel Driver - an unreadable alias falls back to the default name",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    fake.fail_next("get_filter_alias");
    auto driver = create_playerone_filterwheel(0, 0, fake);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_names()[0] == "Filter 1");
    CHECK(driver->get_names()[1] == "Slot2");
    driver->set_connected(false);
}

TEST_CASE("Player One PW Filter Wheel Driver - configured names beat the wheel's aliases",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_names({"L", "R", "G", "B", "Ha"});

    driver->set_connected(true);
    CHECK(driver->get_names() == std::vector<std::string>{"L", "R", "G", "B", "Ha"});
    driver->set_connected(false);
}

TEST_CASE("Player One PW Filter Wheel Driver - the default factory binds the real SDK, not an injected one",
          "[playerone][filterwheel][unit][fake-sdk]") {
    // The two-argument factory is the production entry point (the catalog calls
    // it); it must keep working without an SDK argument and reach the real
    // wrapper only on connect.
    auto driver = create_playerone_filterwheel(2, 0);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_number() == 2);
    CHECK_FALSE(driver->get_connected());
    // With no wheel attached the real SDK enumerates none and the connect is
    // refused as NotConnected; with one attached it connects. Either way no fake
    // is involved, and a refusal must not leave the driver connected.
    try {
        driver->set_connected(true);
        CHECK(driver->get_connected());
        driver->set_connected(false);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotConnected);
        CHECK_FALSE(driver->get_connected());
    }
}

TEST_CASE("Player One PW Filter Wheel Driver - moves a FakePlayerOnePWSDK wheel behind LockedPlayerOnePWSDK",
          "[playerone][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    alpacacore::test::LockedPlayerOnePWSDK sdk(fake);
    auto driver = create_playerone_filterwheel(0, 0, sdk);

    driver->set_connected(true);
    driver->set_position(4);
    CHECK(driver->get_position() == 4);
    CHECK(fake.call_count("goto_position") == 1);
    driver->set_connected(false);
    CHECK(fake.open_count() == 0);
}

TEST_CASE("FakePlayerOnePWSDK behind LockedPlayerOnePWSDK - forwards every entry point",
          "[playerone][filterwheel][unit][fake-sdk]") {
    using alpacacore::vendor::playerone::PlayerOnePWInfo;
    alpacacore::test::FakePlayerOnePWSDK fake;
    alpacacore::test::LockedPlayerOnePWSDK sdk(fake);
    fake.set_offset(2, 7);
    fake.set_moving_reads(1);
    const int handle = fake.handle();
    PlayerOnePWInfo info;

    CHECK(sdk.get_sdk_version() == "fake-playerone-pw-sdk");
    CHECK(sdk.enumerate_wheels().size() == 1);
    CHECK(sdk.get_wheel_info_by_index(0, info));
    CHECK(info.position_count == 5);
    CHECK_FALSE(sdk.get_wheel_info_by_index(3, info));
    CHECK(sdk.get_wheel_info_by_handle(handle, info));
    CHECK(info.serial_number == "fake-pw-41");
    CHECK_FALSE(sdk.get_wheel_info_by_handle(handle + 1, info));
    sdk.open_wheel(handle);
    CHECK(fake.open_count() == 1);
    sdk.goto_position(handle, 2);
    CHECK(sdk.is_moving(handle));
    CHECK(sdk.get_position(handle) == -1);
    CHECK(sdk.get_position(handle) == 2);
    sdk.set_one_way(handle, true);
    CHECK(sdk.get_one_way(handle));
    CHECK(sdk.get_filter_alias(handle, 1) == "Slot2");
    CHECK(sdk.get_focus_offset(handle, 2) == 7);
    sdk.reset_wheel(handle);
    CHECK(sdk.get_position(handle) == 0);
    sdk.close_wheel(handle);
    CHECK(fake.open_count() == 0);

    for (const char* name : {"enumerate_wheels", "get_wheel_info_by_index", "get_wheel_info_by_handle", "open_wheel",
                             "close_wheel", "get_position", "goto_position", "is_moving", "get_one_way", "set_one_way",
                             "get_filter_alias", "get_focus_offset", "reset_wheel", "get_sdk_version"}) {
        INFO(name);
        CHECK(fake.call_count(name) >= 1);
    }
}

// --- Position served through util::TtlStatusCache (issue #1072) ---
// The cache TTL is 100 ms; kPastTtl clears it. fake.call_count("get_position") counts the SDK reads.

namespace {

using namespace std::chrono_literals;
constexpr auto kPastTtl = 150ms;

}  // namespace

// Falsified by: playerone_filterwheel_driver.cpp get_position() calling the SDK on every read (two SDK reads
// inside the TTL, not one), or a cache that never expires (no second SDK read after the TTL).
TEST_CASE("Player One PW Filter Wheel Driver - two Position reads inside the TTL cost one SDK call",
          "[playerone][filterwheel][unit][fake-sdk][position-cache]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);
    driver->set_position(3);
    const int before = fake.call_count("get_position");

    CHECK(driver->get_position() == 3);
    CHECK(driver->get_position() == 3);
    CHECK(fake.call_count("get_position") == before + 1);

    std::this_thread::sleep_for(kPastTtl);
    CHECK(driver->get_position() == 3);
    CHECK(fake.call_count("get_position") == before + 2);
    driver->set_connected(false);
}

// Falsified by: playerone_filterwheel_driver.cpp set_position() without status_cache_.invalidate() after
// goto_position (the read after the move is served from the pre-move frame: position 0, one SDK read).
TEST_CASE("Player One PW Filter Wheel Driver - a Position read after set_position goes to the SDK",
          "[playerone][filterwheel][unit][fake-sdk][position-cache]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);
    const int before = fake.call_count("get_position");
    CHECK(driver->get_position() == 0);
    CHECK(driver->get_position() == 0);

    driver->set_position(4);
    CHECK(driver->get_position() == 4);
    CHECK(fake.call_count("get_position") == before + 2);
    driver->set_connected(false);
}

// Falsified by: playerone_filterwheel_driver.cpp connect or disconnect without status_cache_.reset() (the first
// read after the reconnect is served from the previous session's frame: one SDK read, not two).
TEST_CASE("Player One PW Filter Wheel Driver - a reconnect drops the cached Position",
          "[playerone][filterwheel][unit][fake-sdk][position-cache]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);
    const int before = fake.call_count("get_position");
    CHECK(driver->get_position() == 0);
    CHECK(driver->get_position() == 0);

    driver->set_connected(false);
    driver->set_connected(true);
    CHECK(driver->get_position() == 0);
    CHECK(fake.call_count("get_position") == before + 2);
    driver->set_connected(false);
}

// Falsified by: playerone_filterwheel_driver.cpp reading Position without the cache's fault threshold (the
// third failed read throws the raw scripted failure, not "Player One PW communications compromised"), or serving
// the last good frame after a failed refill (the first failed read returns 0).
TEST_CASE("Player One PW Filter Wheel Driver - failed Position reads are not served and latch a link fault",
          "[playerone][filterwheel][unit][fake-sdk][position-cache]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);
    CHECK(driver->get_position() == 0);
    std::this_thread::sleep_for(kPastTtl);

    // fail_next is one-shot: arm it before each read.
    for (int i = 0; i < 2; ++i) {  // below the threshold the raw failure comes through, not the latch
        fake.fail_next("get_position");
        try {
            driver->get_position();
            FAIL("a failed read must throw");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(std::string(ex.what()).find("communications compromised") == std::string::npos);
        }
    }
    fake.fail_next("get_position");
    try {
        driver->get_position();
        FAIL("third consecutive failure must throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
    }

    // A good refill clears the latch with no reconnect, and its frame is served again.
    const int before = fake.call_count("get_position");
    CHECK(driver->get_position() == 0);
    CHECK(driver->get_position() == 0);
    CHECK(fake.call_count("get_position") == before + 1);
    driver->set_connected(false);
}

// Falsified by: playerone_filterwheel_driver.cpp get_position() refill without the catch (const std::exception&)
// branch (a std::runtime_error from the SDK escapes as-is instead of an AlpacaException with DriverException).
TEST_CASE("Player One PW Filter Wheel Driver - a non-Alpaca SDK failure in Position becomes DriverException",
          "[playerone][filterwheel][unit][fake-sdk][position-cache]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);
    fake.fail_next_non_alpaca("get_position");
    try {
        driver->get_position();
        FAIL("a failed read must throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("non-Alpaca failure") != std::string::npos);
    }
    driver->set_connected(false);
}

// Falsified by: playerone_filterwheel_driver.cpp caching a -1 frame (the second poll is served -1 from the
// cache: one SDK read for two polls), or mapping -1 to a slot (Position reads 0, not the ASCOM moving sentinel).
TEST_CASE("Player One PW Filter Wheel Driver - Position reads -1 while moving and the move end is seen at once",
          "[playerone][filterwheel][unit][fake-sdk][position-cache]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    fake.set_moving_reads(2);
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);
    driver->set_position(3);
    const int before = fake.call_count("get_position");

    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == 3);  // no sleep: a moving frame is never held
    CHECK(fake.call_count("get_position") == before + 3);
    driver->set_connected(false);
}

// Decision 0016: a member that needs no device access never waits behind a Position refill.
// Falsified by: playerone_filterwheel_driver.cpp get_position() holding mutex_ across the SDK read (get_names()
// waits the whole 400 ms delay).
TEST_CASE("Player One PW Filter Wheel Driver - get_names does not wait behind a slow Position refill",
          "[playerone][filterwheel][unit][fake-sdk][position-cache][timing]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);
    const int before = fake.call_count("get_position");
    fake.set_position_delay(400ms);
    auto refill = std::async(std::launch::async, [&] { return driver->get_position(); });
    for (int i = 0; i < 500 && fake.call_count("get_position") == before; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(fake.call_count("get_position") == before + 1);

    const auto start = std::chrono::steady_clock::now();
    CHECK(driver->get_names().size() == 5);
    CHECK(driver->get_name() == "FakePhoenixWheel");
    CHECK(std::chrono::steady_clock::now() - start < 150ms);
    refill.get();
    driver->set_connected(false);
}

// A move command that throws leaves the cached frame untrusted.
// Falsified by: playerone_filterwheel_driver.cpp set_position() skipping status_cache_.invalidate() when
// goto_position throws (the read after the failed move is served from the old frame: no second SDK read).
TEST_CASE("Player One PW Filter Wheel Driver - a failed set_position invalidates the cached Position",
          "[playerone][filterwheel][unit][fake-sdk][position-cache]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    driver->set_connected(true);
    CHECK(driver->get_position() == 0);
    CHECK(driver->get_position() == 0);
    const int before = fake.call_count("get_position");

    fake.fail_next("goto_position");
    CHECK_THROWS_AS(driver->set_position(3), alpacacore::AlpacaException);
    CHECK(driver->get_position() == 0);
    CHECK(fake.call_count("get_position") == before + 1);
    driver->set_connected(false);
}

// Decision 0016 rules 1 and 4: the connect handshake runs off the state lock.
// Falsified by: playerone_filterwheel_driver.cpp set_connected(true) holding mutex_ across open_wheel
// (get_names() and get_name() wait the whole 400 ms open delay).
TEST_CASE("Player One PW Filter Wheel Driver - get_names does not wait behind a held connect",
          "[playerone][filterwheel][unit][fake-sdk][position-cache][timing]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    fake.set_open_delay(400ms);
    auto connect = std::async(std::launch::async, [&] { driver->set_connected(true); });
    for (int i = 0; i < 500 && fake.call_count("open_wheel") == 0; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(fake.call_count("open_wheel") == 1);

    const auto start = std::chrono::steady_clock::now();
    CHECK_NOTHROW(driver->get_names());
    CHECK_NOTHROW(driver->get_name());
    CHECK(std::chrono::steady_clock::now() - start < 150ms);
    connect.get();
    driver->set_connected(false);
}

// Decision 0016 rule 3: identity and limits publish at the end of connect, so a connect that throws after the
// enumeration leaves the pre-connect values.
// Falsified by: playerone_filterwheel_driver.cpp resolve_handle() storing wheel_info_/wheel_info_valid_ again after
// the enumeration (get_name() reports "FakePhoenixWheel" and set_names() with 3 names is rejected for 5 positions).
TEST_CASE("Player One PW Filter Wheel Driver - a failed connect leaves the pre-connect name and slot limits",
          "[playerone][filterwheel][unit][fake-sdk][position-cache]") {
    alpacacore::test::FakePlayerOnePWSDK fake;
    auto driver = create_playerone_filterwheel(0, 0, fake);
    fake.fail_next("open_wheel");
    REQUIRE_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "Player One Phoenix Wheel");
    CHECK_NOTHROW(driver->set_names({"A", "B", "C"}));
    CHECK(driver->get_names().size() == 3);
}
