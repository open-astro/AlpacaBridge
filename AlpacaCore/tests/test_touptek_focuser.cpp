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
#include <alpacacore/vendor/touptek/touptek_focuser_driver.h>
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

TEST_CASE("ToupTek AAF Focuser Driver - Defaults", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ToupTek AAF");
    CHECK(driver->get_absolute() == true);
    CHECK(driver->get_temp_comp_available() == false);
    CHECK(driver->get_temp_comp() == false);
}

TEST_CASE("ToupTek AAF Focuser Driver - Device metadata", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek AAF Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek AAF Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "TOUPTEK_AAF_3");
}

TEST_CASE("ToupTek AAF Focuser Driver - Not connected throws", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    REQUIRE(driver->get_connected() == false);

    require_alpaca_error([&]() { driver->get_is_moving(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); },
                         alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AAF Focuser Driver - Unsupported actions", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);

    require_alpaca_error([&]() { driver->action("noop", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ToupTek AAF Focuser Driver - Absolute focuser semantics",
          "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(2, 0);

    // Absolute focuser, no temperature compensation, step size unsupported.
    CHECK(driver->get_absolute() == true);
    CHECK(driver->get_temp_comp_available() == false);
    CHECK(driver->get_temp_comp() == false);
    require_alpaca_error([&]() { driver->get_step_size(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
}

TEST_CASE("ToupTek AAF Focuser Driver - Value range validation",
          "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    // Move() validates the connection first. The contract is: when not
    // connected, NotConnected is reported; when connected, out-of-range
    // positions yield InvalidValue. Without hardware we exercise the
    // disconnected path here and rely on ConformU for the connected path.
    require_alpaca_error([&]() { driver->move(-1); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(100000000); },
                         alpacacore::AlpacaError::NotConnected);

    // Set temp comp is unsupported regardless of connection state.
    require_alpaca_error([&]() { driver->set_temp_comp(true); },
                         alpacacore::AlpacaError::NotImplemented);
}

TEST_CASE("ToupTek AAF Focuser Driver - State machine", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);

    const auto state = driver->get_device_state();
    REQUIRE(state.size() == 1);
    REQUIRE(state[0].name == "Connected");
    REQUIRE(std::get<bool>(state[0].value) == false);

    // Disconnected reads return NotConnected, never a generic driver error.
    require_alpaca_error([&]() { driver->get_is_moving(); },
                         alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AAF Focuser Driver - Unsupported methods", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    // Step size is not exposed because the AAF firmware does not report a
    // mechanically-valid microns-per-step value for arbitrary focuser setups.
    require_alpaca_error([&]() { driver->get_step_size(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);

    // Temperature compensation is not implemented (no AAF action exists for
    // it) and must report NotImplemented rather than DriverException.
    require_alpaca_error([&]() { driver->set_temp_comp(true); },
                         alpacacore::AlpacaError::NotImplemented);
}

TEST_CASE("ToupTek AAF Focuser Driver - Device number assignment",
          "[touptek][focuser][unit]") {
    auto driver0 = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);
    auto driver1 = alpacacore::vendor::touptek::create_touptek_focuser_by_index(1, 0);
    auto driver5 = alpacacore::vendor::touptek::create_touptek_focuser_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
    CHECK(driver0->get_unique_id() != driver1->get_unique_id());
}

TEST_CASE("ToupTek AAF Focuser Driver - Create by id", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_id(7,
                                                                              "tp-aaf-test-id");

    CHECK(driver->get_device_number() == 7);
    CHECK(driver->get_unique_id() == "TOUPTEK_AAF_tp-aaf-test-id");
    CHECK(driver->get_connected() == false);
}
