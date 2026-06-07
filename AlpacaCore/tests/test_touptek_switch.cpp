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
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>
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
TEST_CASE("ToupTek StellaVita Switch Driver - Defaults", "[touptek][switch][unit]") {
    auto driver =
        alpacacore::vendor::touptek::create_touptek_switch(0, alpacacore::vendor::touptek::default_stellavita_config());

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "ToupTek StellaVita");

    // Four controllable 12V DC outputs, all writable.
    REQUIRE(driver->get_max_switch() == 4);
    CHECK(driver->get_can_write(0));
    CHECK(driver->get_can_write(1));
    CHECK(driver->get_can_write(2));
    CHECK(driver->get_can_write(3));
}

// 2. Device metadata
TEST_CASE("ToupTek StellaVita Switch Driver - Device metadata", "[touptek][switch][unit]") {
    auto driver =
        alpacacore::vendor::touptek::create_touptek_switch(3, alpacacore::vendor::touptek::default_stellavita_config());

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek StellaVita DC power switch (/dev/gpiochip0)");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek StellaVita Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ToupTek_StellaVita_3");
}

// 3. Not connected throws with correct error code
TEST_CASE("ToupTek StellaVita Switch Driver - Not connected throws", "[touptek][switch][unit]") {
    auto driver =
        alpacacore::vendor::touptek::create_touptek_switch(0, alpacacore::vendor::touptek::default_stellavita_config());

    REQUIRE_FALSE(driver->get_connected());

    // Reads and writes require a live GPIO connection.
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(1); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(2); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(3, 1.0); }, alpacacore::AlpacaError::NotConnected);
}

// 4. Unsupported actions
TEST_CASE("ToupTek StellaVita Switch Driver - Unsupported actions", "[touptek][switch][unit]") {
    auto driver =
        alpacacore::vendor::touptek::create_touptek_switch(0, alpacacore::vendor::touptek::default_stellavita_config());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

// 5. Device-specific behavior — per-port metadata and the boot-high DC ports.
TEST_CASE("ToupTek StellaVita Switch Driver - Per-port metadata", "[touptek][switch][unit]") {
    auto driver =
        alpacacore::vendor::touptek::create_touptek_switch(0, alpacacore::vendor::touptek::default_stellavita_config());

    REQUIRE(driver->get_max_switch() == 4);

    // Every port is boolean (step 1, range [0,1]) and synchronous.
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_switch_step(i) == Catch::Approx(1.0));
        CHECK(driver->get_min_switch_value(i) == Catch::Approx(0.0));
        CHECK(driver->get_max_switch_value(i) == Catch::Approx(1.0));
    }

    // Default names match the StellaVita port labels.
    CHECK(driver->get_switch_name(0) == "Port 1");
    CHECK(driver->get_switch_name(1) == "Port 2");
    CHECK(driver->get_switch_name(2) == "Port 3");
    CHECK(driver->get_switch_name(3) == "Port 4");

    // Descriptions carry the verified BCM GPIO mapping (18/10/17/4) and mode.
    CHECK(driver->get_switch_description(0) == "StellaVita DC power port on GPIO 18 (on/off)");
    CHECK(driver->get_switch_description(1) == "StellaVita DC power port on GPIO 10 (on/off)");
    CHECK(driver->get_switch_description(2) == "StellaVita DC power port on GPIO 17 (on/off)");
    CHECK(driver->get_switch_description(3) == "StellaVita DC power port on GPIO 4 (on/off)");

    // User-renamed switches persist for the lifetime of the driver instance.
    driver->set_switch_name(0, "Mount Power");
    CHECK(driver->get_switch_name(0) == "Mount Power");
}

// 6. Value range validation
TEST_CASE("ToupTek StellaVita Switch Driver - Value range validation", "[touptek][switch][unit]") {
    auto driver =
        alpacacore::vendor::touptek::create_touptek_switch(0, alpacacore::vendor::touptek::default_stellavita_config());

    // Out-of-range switch IDs are rejected with InvalidValue regardless of
    // connection state.
    require_alpaca_error([&]() { driver->get_switch_name(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(99); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_description(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(4); }, alpacacore::AlpacaError::InvalidValue);
}

// 7. State machine contracts (no hardware required).
TEST_CASE("ToupTek StellaVita Switch Driver - State machine", "[touptek][switch][unit]") {
    auto driver =
        alpacacore::vendor::touptek::create_touptek_switch(0, alpacacore::vendor::touptek::default_stellavita_config());

    // A disconnected switch reports not connected, not connecting, and an empty
    // device-state bag.
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_connecting());
    CHECK(driver->get_device_state().empty());
}

// 8. Unsupported method error codes.
TEST_CASE("ToupTek StellaVita Switch Driver - Unsupported methods", "[touptek][switch][unit]") {
    auto driver =
        alpacacore::vendor::touptek::create_touptek_switch(0, alpacacore::vendor::touptek::default_stellavita_config());

    // Asynchronous mutation is not implemented for any port (CanAsync == false).
    require_alpaca_error([&]() { driver->set_async(0, true); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_async_value(0, 1.0); }, alpacacore::AlpacaError::NotImplemented);

    // Action/Command methods are unsupported on this device.
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); }, alpacacore::AlpacaError::NotImplemented);
}

// 9. Constructor rejects an empty port configuration.
TEST_CASE("ToupTek StellaVita Switch Driver - Constructor rejects empty config", "[touptek][switch][unit]") {
    alpacacore::vendor::touptek::TouptekSwitchConfig empty_cfg;
    empty_cfg.ports.clear();
    require_alpaca_error([&]() { alpacacore::vendor::touptek::create_touptek_switch(0, empty_cfg); },
                         alpacacore::AlpacaError::InvalidValue);
}

// 10. A PWM-enabled port becomes an analog 0-100 channel; siblings stay boolean.
TEST_CASE("ToupTek StellaVita Switch Driver - PWM port metadata", "[touptek][switch][unit]") {
    auto cfg = alpacacore::vendor::touptek::default_stellavita_config();
    cfg.ports[0].pwm_enabled = true;  // Port 1 -> soft-PWM
    cfg.pwm_frequency_hz = 2000;
    auto driver = alpacacore::vendor::touptek::create_touptek_switch(0, cfg);

    // Port 1 (PWM) reports a 0-100 range with step 1; Port 2 stays boolean [0,1].
    CHECK(driver->get_min_switch_value(0) == Catch::Approx(0.0));
    CHECK(driver->get_max_switch_value(0) == Catch::Approx(100.0));
    CHECK(driver->get_switch_step(0) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(1) == Catch::Approx(1.0));

    // Descriptions reflect each port's mode.
    CHECK(driver->get_switch_description(0) == "StellaVita DC power port on GPIO 18 (PWM 0-100%)");
    CHECK(driver->get_switch_description(1) == "StellaVita DC power port on GPIO 10 (on/off)");
}

// 11. An out-of-range PWM frequency is rejected at construction.
TEST_CASE("ToupTek StellaVita Switch Driver - Rejects invalid PWM frequency", "[touptek][switch][unit]") {
    auto cfg = alpacacore::vendor::touptek::default_stellavita_config();
    cfg.ports[0].pwm_enabled = true;

    cfg.pwm_frequency_hz = 0;
    require_alpaca_error([&]() { alpacacore::vendor::touptek::create_touptek_switch(0, cfg); },
                         alpacacore::AlpacaError::InvalidValue);

    cfg.pwm_frequency_hz = 200000;  // above the 100 kHz ceiling
    require_alpaca_error([&]() { alpacacore::vendor::touptek::create_touptek_switch(0, cfg); },
                         alpacacore::AlpacaError::InvalidValue);
}

// 12. A GPIO chip path that is not an absolute /dev/ node is rejected at construction.
TEST_CASE("ToupTek StellaVita Switch Driver - Rejects invalid GPIO chip path", "[touptek][switch][unit]") {
    for (const char* bad : {"", "gpiochip0", "/sys/class/gpio", "relative/gpiochip0", "dev/gpiochip0"}) {
        auto cfg = alpacacore::vendor::touptek::default_stellavita_config();
        cfg.gpio_chip_path = bad;
        require_alpaca_error([&]() { alpacacore::vendor::touptek::create_touptek_switch(0, cfg); },
                             alpacacore::AlpacaError::InvalidValue);
    }

    // A well-formed /dev/ path is accepted (construction does not open the chip).
    auto ok = alpacacore::vendor::touptek::default_stellavita_config();
    ok.gpio_chip_path = "/dev/gpiochip3";
    CHECK_NOTHROW(alpacacore::vendor::touptek::create_touptek_switch(0, ok));
}
