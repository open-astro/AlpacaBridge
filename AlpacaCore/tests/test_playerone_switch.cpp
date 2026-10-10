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
#include <alpacacore/vendor/playerone/playerone_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

}  // namespace

TEST_CASE("Player One Switch Driver - Defaults", "[playerone][switch][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "Player One Thermal Switch");
    // Disconnected, the bound is the potential element count (dew heater +
    // fan); the per-model count is probed at connect.
    REQUIRE(driver->get_max_switch() == 2);
}

TEST_CASE("Player One Switch Driver - Device metadata", "[playerone][switch][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Player One camera dew heater and fan switch");
    CHECK(driver->get_driver_info() == "AlpacaCore Player One Thermal Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    // Without a connected camera the serial number is unknown, so the unique
    // id falls back to the device number.
    CHECK(driver->get_unique_id() == "PLAYERONE_SW_3");
}

TEST_CASE("Player One Switch Driver - Not connected throws", "[playerone][switch][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0);

    require_alpaca_error([&]() { driver->get_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(0, 0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_name(1); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_write(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_min_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_switch_value(1); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Player One Switch Driver - Unsupported actions", "[playerone][switch][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Player One Switch Driver - Invalid Switch ID", "[playerone][switch][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0);

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(2); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(2); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(2, 50.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(2); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(2); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(2); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(2); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("Player One Switch Driver - Async not supported", "[playerone][switch][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0);

    // Async members validate the ID, then require a connection. Out-of-range
    // ID still wins.
    require_alpaca_error([&]() { driver->set_async(2, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async_value(2, 50.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async(0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(0, 50.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_async(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Player One Switch Driver - State machine", "[playerone][switch][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0);

    REQUIRE_FALSE(driver->get_connected());
    REQUIRE_FALSE(driver->get_connecting());
    // DeviceState is empty while disconnected per the DeviceState contract.
    {
        // DeviceState is the empty list while disconnected: the SwitchDriver
        // base builds it from the public getters, which throw NotConnected and
        // are omitted, and TimeStamp itself is withheld too (ASCOM read-all FAQ).
        const auto state = driver->get_device_state();
        REQUIRE(state.empty());
    }
    // StateChangeComplete requires a connection (per-element state lives on
    // the camera).
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Player One Switch Driver - Connect fails on invalid camera index", "[playerone][switch][unit]") {
    // Index 999 can never enumerate: with no camera attached connect throws
    // NotConnected, with cameras attached it throws InvalidValue. Either way
    // it must throw and never silently report connected — and the absurd
    // index keeps this test hardware-independent (a dev machine with a real
    // camera at index 0 must not have the test open the live device).
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 999);

    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
}

// ---- open-astro#294: one status burst per DeviceState, static members free ----

namespace {

struct FakeThermalSdk final : alpacacore::vendor::playerone::PlayerOneThermalSdk {
    std::atomic<int> reads{0};
    std::atomic<bool> fail_reads{false};
    int heater{40};
    int fan{70};

    std::vector<alpacacore::vendor::playerone::PlayerOneCameraInfo> enumerate_cameras() override {
        alpacacore::vendor::playerone::PlayerOneCameraInfo info;
        info.camera_id = 7;
        info.name = "Fake Poseidon";
        info.serial_number = "FAKE1";
        return {info};
    }
    void open_camera(int) override {}
    void init_camera(int) override {}
    void close_camera(int) override {}
    alpacacore::vendor::playerone::PlayerOneConfigCaps probe_config_caps(int) override {
        alpacacore::vendor::playerone::PlayerOneConfigCaps caps;
        caps.has_heater_power = true;
        caps.heater_power_writable = true;
        caps.heater_power_max = 100;
        caps.has_fan_power = true;
        caps.fan_power_writable = true;
        caps.fan_power_max = 100;
        return caps;
    }
    int get_heater_power_percent(int) override {
        ++reads;
        if (fail_reads) {
            throw alpacacore::AlpacaException("usb gone", alpacacore::AlpacaError::DriverException);
        }
        return heater;
    }
    int get_fan_power_percent(int) override {
        ++reads;
        if (fail_reads) {
            throw alpacacore::AlpacaException("usb gone", alpacacore::AlpacaError::DriverException);
        }
        return fan;
    }
    void set_heater_power_percent(int, int percent) override { heater = percent; }
    void set_fan_power_percent(int, int percent) override { fan = percent; }
};

void wait_past_status_ttl() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

}  // namespace

TEST_CASE("Player One Switch Driver - DeviceState reads the camera once per status frame",
          "[playerone][switch][unit]") {
    FakeThermalSdk sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_switch_value(0) == 40.0);
    CHECK(driver->get_switch_value(1) == 70.0);
    CHECK(driver->get_switch(0));
    const int after_values = sdk.reads;
    // One refill reads each element once; before the cache this was 2 reads per switch per getter.
    CHECK(after_values == 2);

    const auto state = driver->get_device_state();
    CHECK_FALSE(state.empty());
    CHECK(sdk.reads == after_values);

    wait_past_status_ttl();
    (void)driver->get_device_state();
    CHECK(sdk.reads == after_values + 2);
}

TEST_CASE("Player One Switch Driver - static members cost no camera reads", "[playerone][switch][unit]") {
    FakeThermalSdk sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0, sdk);
    driver->set_connected(true);

    for (int i = 0; i < 3; ++i) {
        CHECK(driver->get_max_switch() == 2);
        CHECK(driver->get_switch_name(0) == "DewHeater");
        CHECK_FALSE(driver->get_switch_description(1).empty());
        CHECK(driver->get_min_switch_value(0) == 0.0);
        CHECK(driver->get_max_switch_value(0) == 100.0);
        CHECK(driver->get_switch_step(0) == 1.0);
        CHECK(driver->get_can_write(1));
    }
    CHECK(sdk.reads == 0);
}

TEST_CASE("Player One Switch Driver - a write and a reconnect drop the cached frame", "[playerone][switch][unit]") {
    FakeThermalSdk sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0, sdk);
    driver->set_connected(true);
    CHECK(driver->get_switch_value(0) == 40.0);

    driver->set_switch_value(0, 10.0);
    CHECK(driver->get_switch_value(0) == 10.0);

    sdk.heater = 55;
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK(driver->get_switch_value(0) == 55.0);
}

TEST_CASE("Player One Switch Driver - a dead link is not served from the cache", "[playerone][switch][unit]") {
    FakeThermalSdk sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0, sdk);
    driver->set_connected(true);
    CHECK(driver->get_switch_value(0) == 40.0);

    sdk.fail_reads = true;
    wait_past_status_ttl();
    for (int i = 0; i < 3; ++i) {
        CHECK_THROWS_AS(driver->get_switch_value(0), alpacacore::AlpacaException);
    }
    // Latched: the error names the compromised link, and the old frame is gone.
    try {
        (void)driver->get_switch_value(0);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
    }
    CHECK(driver->get_connected());

    sdk.fail_reads = false;
    CHECK(driver->get_switch_value(0) == 40.0);
}
