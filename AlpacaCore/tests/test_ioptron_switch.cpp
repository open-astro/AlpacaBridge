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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>
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

// 1. Defaults
TEST_CASE("iOptron iMate PowerBox Switch Driver - Defaults", "[ioptron][switch][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "iOptron iMate PowerBox");

    // Three DC outputs: one always-on pass-through plus two controllable.
    REQUIRE(driver->get_max_switch() == 3);
    CHECK_FALSE(driver->get_can_write(0));  // DC3 pass-through is read-only
    CHECK(driver->get_can_write(1));        // DC1
    CHECK(driver->get_can_write(2));        // DC2
}

// 2. Device metadata
TEST_CASE("iOptron iMate PowerBox Switch Driver - Device metadata", "[ioptron][switch][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        3, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "iOptron iMate PowerBox DC power switch (/dev/gpiochip1)");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron iMate PowerBox Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "iOptron_iMate_PowerBox_3");
}

// 3. Not connected throws with correct error code
TEST_CASE("iOptron iMate PowerBox Switch Driver - Not connected throws", "[ioptron][switch][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    REQUIRE_FALSE(driver->get_connected());

    // Reads require a live GPIO connection — even for the always-on port.
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(1); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
    // Writing a *writable* port while disconnected throws NotConnected.
    require_alpaca_error([&]() { driver->set_switch(1, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(2, 1.0); }, alpacacore::AlpacaError::NotConnected);
}

// 4. Unsupported actions
TEST_CASE("iOptron iMate PowerBox Switch Driver - Unsupported actions", "[ioptron][switch][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

// 5. Device-specific behavior — per-port metadata and the always-on pass-through.
TEST_CASE("iOptron iMate PowerBox Switch Driver - Per-port metadata", "[ioptron][switch][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    REQUIRE(driver->get_max_switch() == 3);

    // Every port is boolean (step 1, range [0,1]) and synchronous.
    for (int i = 0; i < 3; ++i) {
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_switch_step(i) == Catch::Approx(1.0));
        CHECK(driver->get_min_switch_value(i) == Catch::Approx(0.0));
        CHECK(driver->get_max_switch_value(i) == Catch::Approx(1.0));
    }

    // Default names match the iMate DC silk-screen labels.
    CHECK(driver->get_switch_name(0) == "DC3 (always on)");
    CHECK(driver->get_switch_name(1) == "DC1");
    CHECK(driver->get_switch_name(2) == "DC2");

    // Descriptions distinguish the pass-through from the GPIO-backed ports.
    CHECK(driver->get_switch_description(0) == "iMate always-on DC pass-through (read-only)");
    CHECK(driver->get_switch_description(1) == "iMate DC power port on GPIO 118 (on/off)");
    CHECK(driver->get_switch_description(2) == "iMate DC power port on GPIO 114 (on/off)");

    // User-renamed switches persist for the lifetime of the driver instance.
    driver->set_switch_name(1, "Mount Power");
    CHECK(driver->get_switch_name(1) == "Mount Power");
}

// 6. Value range validation
TEST_CASE("iOptron iMate PowerBox Switch Driver - Value range validation", "[ioptron][switch][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    // Out-of-range switch IDs are rejected with InvalidValue regardless of
    // connection state.
    require_alpaca_error([&]() { driver->get_switch_name(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(99); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_description(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(3); }, alpacacore::AlpacaError::InvalidValue);
}

// 7. State machine contracts (no hardware required).
TEST_CASE("iOptron iMate PowerBox Switch Driver - State machine", "[ioptron][switch][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    // A disconnected switch reports not connected, not connecting, and an empty
    // device-state bag.
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_connecting());
    CHECK(driver->get_device_state().empty());
}

// 8. Unsupported / read-only method error codes.
TEST_CASE("iOptron iMate PowerBox Switch Driver - Unsupported methods", "[ioptron][switch][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    // The DC3 pass-through is read-only: writes must throw NotImplemented, and
    // (unlike a writable port) this is a static property independent of the
    // connection state.
    require_alpaca_error([&]() { driver->set_switch(0, false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_switch_value(0, 0.0); }, alpacacore::AlpacaError::NotImplemented);

    // Asynchronous mutation is not implemented for any port (CanAsync == false).
    require_alpaca_error([&]() { driver->set_async(1, true); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_async_value(1, 1.0); }, alpacacore::AlpacaError::NotImplemented);

    // Action/Command methods are unsupported on this device.
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); }, alpacacore::AlpacaError::NotImplemented);
}

// 9. Constructor rejects an empty port configuration.
TEST_CASE("iOptron iMate PowerBox Switch Driver - Constructor rejects empty config", "[ioptron][switch][unit]") {
    alpacacore::vendor::ioptron::IoptronSwitchConfig empty_cfg;
    empty_cfg.ports.clear();
    require_alpaca_error([&]() { alpacacore::vendor::ioptron::create_ioptron_switch(0, empty_cfg); },
                         alpacacore::AlpacaError::InvalidValue);
}

// 10. A PWM-enabled port becomes an analog 0-100 channel; siblings stay boolean.
TEST_CASE("iOptron iMate PowerBox Switch Driver - PWM port metadata", "[ioptron][switch][unit]") {
    auto cfg = alpacacore::vendor::ioptron::default_imate_powerbox_config();
    cfg.ports[1].pwm_enabled = true;  // DC1 -> soft-PWM
    cfg.pwm_frequency_hz = 2000;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(0, cfg);

    // DC1 (PWM) reports a 0-100 range with step 1; DC2 stays boolean [0,1].
    CHECK(driver->get_min_switch_value(1) == Catch::Approx(0.0));
    CHECK(driver->get_max_switch_value(1) == Catch::Approx(100.0));
    CHECK(driver->get_switch_step(1) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(2) == Catch::Approx(1.0));

    // Descriptions reflect each port's mode.
    CHECK(driver->get_switch_description(1) == "iMate DC power port on GPIO 118 (PWM 0-100%)");
    CHECK(driver->get_switch_description(2) == "iMate DC power port on GPIO 114 (on/off)");

    // The always-on pass-through is unaffected and remains read-only.
    CHECK_FALSE(driver->get_can_write(0));
}

// 11. An out-of-range PWM frequency is rejected at construction.
TEST_CASE("iOptron iMate PowerBox Switch Driver - Rejects invalid PWM frequency", "[ioptron][switch][unit]") {
    auto cfg = alpacacore::vendor::ioptron::default_imate_powerbox_config();
    cfg.ports[1].pwm_enabled = true;

    cfg.pwm_frequency_hz = 0;
    require_alpaca_error([&]() { alpacacore::vendor::ioptron::create_ioptron_switch(0, cfg); },
                         alpacacore::AlpacaError::InvalidValue);

    cfg.pwm_frequency_hz = 200000;  // above the 100 kHz ceiling
    require_alpaca_error([&]() { alpacacore::vendor::ioptron::create_ioptron_switch(0, cfg); },
                         alpacacore::AlpacaError::InvalidValue);
}

// A GPIO chip path that is not an absolute /dev/ node is rejected at construction.
TEST_CASE("iOptron iMate PowerBox Switch Driver - Rejects invalid GPIO chip path", "[ioptron][switch][unit]") {
    for (const char* bad : {"", "gpiochip1", "/sys/class/gpio", "relative/gpiochip1"}) {
        auto cfg = alpacacore::vendor::ioptron::default_imate_powerbox_config();
        cfg.gpio_chip_path = bad;
        require_alpaca_error([&]() { alpacacore::vendor::ioptron::create_ioptron_switch(0, cfg); },
                             alpacacore::AlpacaError::InvalidValue);
    }
    auto ok = alpacacore::vendor::ioptron::default_imate_powerbox_config();
    ok.gpio_chip_path = "/dev/gpiochip0";
    CHECK_NOTHROW(alpacacore::vendor::ioptron::create_ioptron_switch(0, ok));
}
