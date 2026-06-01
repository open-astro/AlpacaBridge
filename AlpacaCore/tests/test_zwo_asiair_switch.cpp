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
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include "catch2_compat.h"

#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>
#include <alpacacore/util/error_handling.h>
#include <functional>

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

} // namespace

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Defaults", "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ZWO ASIAIR Pro Switch");
    REQUIRE(driver->get_max_switch() == 4);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Device metadata", "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        3, alpacacore::vendor::zwo::default_asiair_pro_config());

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO ASIAIR Pro 12V power switch (/dev/gpiochip0)");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO ASIAIR Pro Switch");
    CHECK(driver->get_driver_version() == "1.0.0");
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_ASIAIR_3");
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Not connected throws", "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    REQUIRE_FALSE(driver->get_connected());

    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(2); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(2, 1.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Unsupported actions", "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Per-port metadata", "[zwo][switch][asiair][unit]") {
    auto cfg = alpacacore::vendor::zwo::default_asiair_pro_config();
    cfg.ports[3].pwm_enabled = true; // mark Port 4 (GPIO 18) as a PWM channel
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(0, cfg);

    REQUIRE(driver->get_max_switch() == 4);

    // All four switches are writable, none are async.
    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_can_write(i));
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_switch_step(i) == Catch::Approx(1.0));
        CHECK(driver->get_min_switch_value(i) == Catch::Approx(0.0));
    }

    // Default port names match the ASIAIR physical labels.
    CHECK(driver->get_switch_name(0) == "Port 1");
    CHECK(driver->get_switch_name(1) == "Port 2");
    CHECK(driver->get_switch_name(2) == "Port 3");
    CHECK(driver->get_switch_name(3) == "Port 4");

    // Boolean channels have range [0, 1]; the PWM channel exposes [0, 100].
    CHECK(driver->get_max_switch_value(0) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(1) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(2) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(3) == Catch::Approx(100.0));

    // Descriptions surface the underlying GPIO line and mode.
    CHECK(driver->get_switch_description(0) == "ASIAIR power port on GPIO 12 (on/off)");
    CHECK(driver->get_switch_description(1) == "ASIAIR power port on GPIO 13 (on/off)");
    CHECK(driver->get_switch_description(2) == "ASIAIR power port on GPIO 26 (on/off)");
    CHECK(driver->get_switch_description(3) == "ASIAIR power port on GPIO 18 (PWM 0-100%)");

    // User-renamed switches persist for the lifetime of the driver instance.
    driver->set_switch_name(0, "Mount Power");
    CHECK(driver->get_switch_name(0) == "Mount Power");
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Switch ID range validation",
          "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    // Out-of-range IDs are rejected with InvalidValue regardless of connection state.
    require_alpaca_error([&]() { driver->get_switch_name(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(99); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_description(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(4); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - State machine when disconnected",
          "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    // ASCOM contract: a disconnected switch reports get_connected() = false,
    // get_connecting() = false, and get_device_state() returns an empty bag.
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_connecting());
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Unsupported method error codes",
          "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    // Asynchronous mutation is not implemented; ASCOM expects NotImplemented,
    // not a generic DriverException. Connection check fires first because the
    // ASCOM contract requires NotConnected before any other validation.
    require_alpaca_error([&]() { driver->set_async(0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(0, 1.0); }, alpacacore::AlpacaError::NotConnected);

    // Action/Command methods are unsupported on this device.
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); }, alpacacore::AlpacaError::NotImplemented);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Constructor rejects invalid configs",
          "[zwo][switch][asiair][unit]") {
    // Empty ports list must fail at construction (the wrapper has nothing to manage).
    alpacacore::vendor::zwo::AsiairSwitchConfig empty_cfg;
    empty_cfg.ports.clear();
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_switch(0, empty_cfg); },
        alpacacore::AlpacaError::InvalidValue);

    // PWM frequency of 0 Hz is rejected (would divide by zero in the period calc).
    auto zero_freq = alpacacore::vendor::zwo::default_asiair_pro_config();
    zero_freq.pwm_frequency_hz = 0;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_switch(0, zero_freq); },
        alpacacore::AlpacaError::InvalidValue);

    // Absurdly high PWM frequency is rejected.
    auto huge_freq = alpacacore::vendor::zwo::default_asiair_pro_config();
    huge_freq.pwm_frequency_hz = 1000001;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_switch(0, huge_freq); },
        alpacacore::AlpacaError::InvalidValue);
}
