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
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
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

TEST_CASE("ZWO EAF Focuser Driver - Defaults", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ZWO EAF");
}

TEST_CASE("ZWO EAF Focuser Driver - Disconnected Behavior", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    REQUIRE(driver->get_temp_comp_available() == false);
    REQUIRE(driver->get_temp_comp() == false);
    REQUIRE(driver->get_supported_actions().empty());

    const auto state = driver->get_device_state();
    REQUIRE(state.size() == 1);
    REQUIRE(state[0].name == "Connected");
    REQUIRE(std::get<bool>(state[0].value) == false);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ZWO EAF Focuser Driver - Device metadata", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO EAF Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO EAF Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_EAF_3");
}

TEST_CASE("ZWO EAF Focuser Driver - Device Number Assignment", "[zwo][focuser][unit]") {
    auto driver0 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);
    auto driver1 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);
    auto driver5 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
}

TEST_CASE("ZWO EAF Focuser Driver - Unique IDs", "[zwo][focuser][unit]") {
    auto driver_a = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);
    auto driver_b = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);

    REQUIRE(!driver_a->get_unique_id().empty());
    REQUIRE(!driver_b->get_unique_id().empty());
    CHECK(driver_a->get_unique_id() != driver_b->get_unique_id());
}
