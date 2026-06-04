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
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
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

TEST_CASE("ZWO CAA Rotator Driver - Defaults", "[zwo][rotator][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Rotator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ZWO CAA");
    CHECK(driver->get_can_reverse() == true);
}

TEST_CASE("ZWO CAA Rotator Driver - Disconnected Behavior", "[zwo][rotator][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_supported_actions().empty());

    const auto state = driver->get_device_state();
    REQUIRE(state.size() == 1);
    REQUIRE(state[0].name == "Connected");
    REQUIRE(std::get<bool>(state[0].value) == false);

    require_alpaca_error([&]() { driver->get_reverse(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_mechanical_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_target_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_target_position(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_absolute(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_mechanical(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->sync(10.0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ZWO CAA Rotator Driver - Device metadata", "[zwo][rotator][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO CAA Rotator Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO CAA Rotator Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_CAA_3");
}

TEST_CASE("ZWO CAA Rotator Driver - Device Number Assignment", "[zwo][rotator][unit]") {
    auto driver0 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0);
    auto driver1 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(1, 0);
    auto driver5 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
}

TEST_CASE("ZWO CAA Rotator Driver - Unique IDs", "[zwo][rotator][unit]") {
    auto driver0 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0);
    auto driver1 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(1, 0);

    CHECK(driver0->get_unique_id() != driver1->get_unique_id());
}
