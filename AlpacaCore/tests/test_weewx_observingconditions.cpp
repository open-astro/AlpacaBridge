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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>
#include <alpacacore/version.h>

#include <cmath>

#include "catch2_compat.h"

TEST_CASE("WeeWX current parsing", "[weewx]") {
    const std::string payload = R"json(
{
    "lcd_datasheet": {
        "current": {
            "outTemp": {"value": 50.0, "units": "\u00b0F"},
            "outHumidity": {"value": 40.0, "units": "%"},
            "dewpoint": {"value": 32.0, "units": "\u00b0F"},
            "wind_speed": {"value": 10.0, "units": "mph"},
            "barometer": {"value": 30.0, "units": "inHg"},
            "sqm": {"value": 21.3},
            "sqmTemp": {"value": 41.0}
        },
        "daily_captures": {
            "rows": [
                [1, 2, , 4]
            ]
        }
    }
}
)json";

    auto values = alpacacore::vendor::weewx::parse_weewx_current(payload);
    REQUIRE(values.has_value());

    const auto& v = values.value();
    REQUIRE(std::abs(v.temperature_c - 10.0) < 1e-6);
    REQUIRE(std::abs(v.humidity - 40.0) < 1e-6);
    REQUIRE(std::abs(v.dewpoint_c - 0.0) < 1e-6);
    REQUIRE(std::abs(v.wind_speed_ms - 4.4704) < 1e-4);
    REQUIRE(std::abs(v.pressure_hpa - 1015.9166) < 1e-3);
    REQUIRE(std::abs(v.sky_quality - 21.3) < 1e-6);
    REQUIRE(std::abs(v.sky_temperature_c - 5.0) < 1e-6);
}

TEST_CASE("WeeWX ObservingConditions Driver - Defaults", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(7, config);
    REQUIRE(driver);

    CHECK(driver->get_device_type() == alpacacore::DeviceType::ObservingConditions);
    CHECK(driver->get_device_number() == 7);
    CHECK(driver->get_connected() == false);
    CHECK(driver->get_name() == "WeeWX ObservingConditions");
}

TEST_CASE("WeeWX ObservingConditions Driver - Device metadata", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(3, config);
    REQUIRE(driver);

    CHECK(driver->get_description() == "WeeWX ObservingConditions from HTTP JSON");
    CHECK(driver->get_driver_info() == "AlpacaCore WeeWX ObservingConditions Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 2);
    CHECK(driver->get_unique_id() == "WEEWX_OC_3");
}

TEST_CASE("WeeWX ObservingConditions Driver - Unsupported actions", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    REQUIRE(driver);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("foo", "bar"), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("foo", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("foo", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("foo", false), alpacacore::AlpacaException);
}

TEST_CASE("WeeWX ObservingConditions Driver - ASCOM Error Codes", "[weewx]") {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://localhost:9999/dummy";

    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, config);
    REQUIRE(driver);

    auto require_alpaca_error = [](const std::function<void()>& fn, int expected_code) {
        try {
            fn();
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& ex) {
            REQUIRE(ex.error_code() == expected_code);
        }
    };

    require_alpaca_error([&]() { (void)driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_humidity(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_dew_point(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_pressure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_wind_speed(); }, alpacacore::AlpacaError::NotConnected);
}
