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
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <variant>

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

TEST_CASE("Gemini Focuser Driver - Defaults", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Gemini Automatic Astro Focuser Pro");
}

TEST_CASE("Gemini Focuser Driver - Metadata", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    CHECK(driver->get_description() == "Gemini Automatic Astro Focuser Pro Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Gemini Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "GEMINI_FOCUSER_0");
}

TEST_CASE("Gemini Focuser Driver - Disconnected Behavior", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB0");

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    REQUIRE(driver->get_temp_comp_available() == true);
    REQUIRE(driver->get_temp_comp() == false);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, leaving just the TimeStamp; the old non-compliant
    // "Connected" entry is gone.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Gemini Focuser Driver - Connecting State", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);
}

TEST_CASE("Gemini Focuser Driver - Device Number Assignment", "[gemini][focuser][unit]") {
    auto driver0 = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");
    auto driver1 = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB1");
    auto driver5 = alpacacore::vendor::gemini::create_gemini_focuser(5, "/dev/ttyUSB2");

    REQUIRE(driver0->get_device_number() == 0);
    REQUIRE(driver1->get_device_number() == 1);
    REQUIRE(driver5->get_device_number() == 5);
}

TEST_CASE("Gemini Focuser Driver - Unique IDs", "[gemini][focuser][unit]") {
    auto driver0 = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");
    auto driver1 = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB1");

    REQUIRE(driver0->get_unique_id() != driver1->get_unique_id());
    CHECK(driver0->get_unique_id() == "GEMINI_FOCUSER_0");
    CHECK(driver1->get_unique_id() == "GEMINI_FOCUSER_1");
}
