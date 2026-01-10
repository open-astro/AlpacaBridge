// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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

#include <catch2/catch_all.hpp>

#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/util/error_handling.h>
#include <functional>
#include <variant>

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

TEST_CASE("ZWO EFW Filter Wheel Driver - Defaults", "[zwo][filterwheel][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ZWO EFW");
}

TEST_CASE("ZWO EFW Filter Wheel Driver - Disconnected Behavior", "[zwo][filterwheel][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_supported_actions().empty());

    const auto state = driver->get_device_state();
    REQUIRE(state.empty());

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_focus_offsets(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_focus_offsets({0}); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_names(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_names({"L"}); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}
