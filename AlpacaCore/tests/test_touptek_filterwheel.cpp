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

#include "catch2_compat.h"

#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/util/error_handling.h>
#include <functional>
#include <vector>

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

TEST_CASE("ToupTek Filter Wheel Driver - Defaults", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ToupTek FilterWheel");
}

TEST_CASE("ToupTek Filter Wheel Driver - Disconnected Behavior", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_supported_actions().empty());

    const auto state = driver->get_device_state();
    REQUIRE(state.empty());

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0}));
    REQUIRE_NOTHROW(driver->set_names({"L"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L"});

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ToupTek Filter Wheel Driver - Device metadata", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek Filter Wheel Driver");
    CHECK(driver->get_driver_version() == "1.0.0");
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "TOUPTEK_FW_3");
}

TEST_CASE("ToupTek Filter Wheel Driver - Device Number Assignment", "[touptek][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::touptek::create_touptek_filterwheel(0, 0);
    auto d1 = alpacacore::vendor::touptek::create_touptek_filterwheel(1, 0);
    auto d5 = alpacacore::vendor::touptek::create_touptek_filterwheel(5, 0);

    CHECK(d0->get_device_number() == 0);
    CHECK(d1->get_device_number() == 1);
    CHECK(d5->get_device_number() == 5);
}

TEST_CASE("ToupTek Filter Wheel Driver - Unique IDs", "[touptek][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::touptek::create_touptek_filterwheel(0, 0);
    auto d1 = alpacacore::vendor::touptek::create_touptek_filterwheel(1, 0);

    CHECK(d0->get_unique_id() != d1->get_unique_id());
}

TEST_CASE("ToupTek Filter Wheel Driver - Names and Focus Offsets (disconnected)", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel(2, 0);

    // Disconnected: can set/retrieve names/offsets even without slot count validation.
    REQUIRE_NOTHROW(driver->set_names({"R", "G", "B", "L"}));
    auto names = driver->get_names();
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "R");
    CHECK(names[1] == "G");
    CHECK(names[2] == "B");
    CHECK(names[3] == "L");

    REQUIRE_NOTHROW(driver->set_focus_offsets({0, 10, -5, 3}));
    auto offsets = driver->get_focus_offsets();
    REQUIRE(offsets.size() == 4);
    CHECK(offsets[0] == 0);
    CHECK(offsets[1] == 10);
    CHECK(offsets[2] == -5);
    CHECK(offsets[3] == 3);
}

TEST_CASE("ToupTek Filter Wheel Driver - Reject invalid actions while disconnected", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel(0, 0);

    REQUIRE_FALSE(driver->can_action("something"));
    require_alpaca_error([&]() { driver->action("something", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
}

TEST_CASE("ToupTek Filter Wheel Driver - State machine", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel(0, 0);

    // Default state: disconnected, not connecting
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);

    // Device state is empty when not connected
    const auto state = driver->get_device_state();
    REQUIRE(state.empty());

    // Interface version is always available
    REQUIRE(driver->get_interface_version() == 3);
}

TEST_CASE("ToupTek Filter Wheel Driver - Value range validation", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel(0, 0);

    // Connected-only operations: verify NotConnected is thrown before value check
    // (set_position validates range only after connection check)
    require_alpaca_error([&]() { driver->set_position(-1); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(100); },
                         alpacacore::AlpacaError::NotConnected);

    // Unsupported methods: verify correct error codes
    require_alpaca_error([&]() { driver->action("test", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}
