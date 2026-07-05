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

#include <functional>

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
        // Only the TimeStamp survives while disconnected: the SwitchDriver base
        // builds DeviceState from the public getters, which throw NotConnected
        // and are omitted per the DeviceState contract.
        const auto state = driver->get_device_state();
        REQUIRE(state.size() == 1);
        CHECK(state[0].name == "TimeStamp");
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
