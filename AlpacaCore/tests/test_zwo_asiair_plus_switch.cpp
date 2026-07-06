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
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
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

} // namespace

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Defaults", "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ZWO ASIAIR Plus Switch (RK3568)");
    REQUIRE(driver->get_max_switch() == 4);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Device metadata",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        3, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() ==
          "ZWO ASIAIR Plus (RK3568) 12V power switch (/dev/pwm-gpio-misc)");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO ASIAIR Plus Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_ASIAIR_PLUS_RK3568_3");
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Not connected throws",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE_FALSE(driver->get_connected());

    require_alpaca_error([&]() { driver->get_switch(0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, true); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(2); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(2, 1.0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); },
                         alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Unsupported actions",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Per-port metadata",
          "[zwo][switch][asiair-plus][unit]") {
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true; // mark Port 4 (DC port 4) as a PWM channel
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);

    REQUIRE(driver->get_max_switch() == 4);

    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_can_write(i));
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_switch_step(i) == Catch::Approx(1.0));
        CHECK(driver->get_min_switch_value(i) == Catch::Approx(0.0));
    }

    CHECK(driver->get_switch_name(0) == "Port 1");
    CHECK(driver->get_switch_name(1) == "Port 2");
    CHECK(driver->get_switch_name(2) == "Port 3");
    CHECK(driver->get_switch_name(3) == "Port 4");

    // Boolean channels have range [0, 1]; the PWM channel exposes [0, 100].
    CHECK(driver->get_max_switch_value(0) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(1) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(2) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(3) == Catch::Approx(100.0));

    CHECK(driver->get_switch_description(0) == "ASIAIR Plus DC port 1 (on/off)");
    CHECK(driver->get_switch_description(1) == "ASIAIR Plus DC port 2 (on/off)");
    CHECK(driver->get_switch_description(2) == "ASIAIR Plus DC port 3 (on/off)");
    CHECK(driver->get_switch_description(3) == "ASIAIR Plus DC port 4 (PWM 0-100%)");

    // User-renamed switches persist for the lifetime of the driver instance.
    driver->set_switch_name(0, "Mount Power");
    CHECK(driver->get_switch_name(0) == "Mount Power");
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Switch ID range validation",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    require_alpaca_error([&]() { driver->get_switch_name(-1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(99); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_description(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(4); },
                         alpacacore::AlpacaError::InvalidValue);

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(4); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - State machine when disconnected",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    // ASCOM contract: a disconnected switch reports get_connected() = false,
    // get_connecting() = false, and get_device_state() returns only a TimeStamp.
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_connecting());
    {
        // Only the TimeStamp survives while disconnected: the SwitchDriver base
        // builds DeviceState from the public getters, which throw NotConnected
        // and are omitted per the DeviceState contract.
        const auto state = driver->get_device_state();
        REQUIRE(state.size() == 1);
        CHECK(state[0].name == "TimeStamp");
    }
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Unsupported method error codes",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    // Async mutation isn't implemented; the disconnected-state check fires first
    // because ASCOM contract requires NotConnected before any other validation.
    require_alpaca_error([&]() { driver->set_async(0, true); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(0, 1.0); },
                         alpacacore::AlpacaError::NotConnected);

    // Action/Command surfaces are unsupported on this device.
    require_alpaca_error([&]() { driver->action("test", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Constructor rejects invalid configs",
          "[zwo][switch][asiair-plus][unit]") {
    // Empty ports list rejected at construction.
    alpacacore::vendor::zwo::AsiairPlusSwitchConfig empty_cfg;
    empty_cfg.ports.clear();
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, empty_cfg); },
        alpacacore::AlpacaError::InvalidValue);

    // More than four ports rejected (the hardware only has four DC outputs).
    auto too_many = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    too_many.ports.push_back({"Port 5", false});
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, too_many); },
        alpacacore::AlpacaError::InvalidValue);

    // PWM frequency of 0 Hz is rejected (would divide by zero in the period calc).
    auto zero_freq = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    zero_freq.pwm_frequency_hz = 0;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, zero_freq); },
        alpacacore::AlpacaError::InvalidValue);

    // Absurdly high PWM frequency is rejected (above the empirically validated range).
    auto huge_freq = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    huge_freq.pwm_frequency_hz = 1000001;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, huge_freq); },
        alpacacore::AlpacaError::InvalidValue);
}
