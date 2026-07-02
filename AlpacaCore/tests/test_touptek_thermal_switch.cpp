// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>
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

TEST_CASE("ToupTek Thermal Switch Driver - Defaults", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ToupTek Thermal Switch");
    // Disconnected, the bound is the potential element count (dew heater + fan +
    // tail LED); the per-model count is probed at connect.
    REQUIRE(driver->get_max_switch() == 3);
}

TEST_CASE("ToupTek Thermal Switch Driver - Device metadata", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek camera dew heater and fan switch");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek Thermal Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    // Without a connected camera the serial number is unknown, so the unique id
    // falls back to the device number.
    CHECK(driver->get_unique_id() == "TOUPTEK_THERMALSW_3");
}

TEST_CASE("ToupTek Thermal Switch Driver - Not connected throws", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    require_alpaca_error([&]() { driver->get_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(0, 0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_name(1); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_write(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_min_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_switch_value(1); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Thermal Switch Driver - Unsupported actions", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("ToupTek Thermal Switch Driver - Invalid Switch ID", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(3, 50.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(3); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ToupTek Thermal Switch Driver - Async not supported", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    // Async members validate the ID, then require a connection. Out-of-range ID
    // still wins.
    require_alpaca_error([&]() { driver->set_async(3, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async_value(3, 50.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async(0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(0, 50.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_async(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Thermal Switch Driver - State machine", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    REQUIRE_FALSE(driver->get_connected());
    REQUIRE_FALSE(driver->get_connecting());
    // DeviceState is empty while disconnected per the DeviceState contract.
    CHECK(driver->get_device_state().empty());
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Thermal Switch Driver - Connect fails on invalid camera index", "[touptek][switch][unit]") {
    // Index 999 can never enumerate: with no camera attached connect throws
    // NotConnected, with cameras attached it throws InvalidValue. Either way it
    // must throw and never silently report connected — and the absurd index
    // keeps this test hardware-independent.
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 999);

    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
}
