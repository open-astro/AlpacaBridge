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
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <string>
#include <variant>
#include <vector>

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

TEST_CASE("ToupTek AFW Filter Wheel Driver - Defaults", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);
    CHECK(driver->get_name() == "ToupTek AFW");
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Device metadata", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek AFW Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek AFW Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "TOUPTEK_AFW_3");
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Disconnected behavior", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_supported_actions().empty());

    // Position read/write require a connection.
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);

    // Names/offsets are configurable while disconnected (the slot count is not
    // yet known, so no length validation is imposed).
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0, 10}));
    REQUIRE_NOTHROW(driver->set_names({"L", "R"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0, 10});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L", "R"});
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Platform 7 DeviceState", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);

    // While disconnected, Position throws and is omitted, leaving just the
    // TimeStamp; the non-compliant "Connected" entry must not appear.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Unsupported actions", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Device number assignment", "[touptek][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);
    auto d1 = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(1, 0);
    auto d5 = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(5, 0);

    CHECK(d0->get_device_number() == 0);
    CHECK(d1->get_device_number() == 1);
    CHECK(d5->get_device_number() == 5);
    CHECK(d0->get_unique_id() != d1->get_unique_id());
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Create by id", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(7, "tp-afw-test-id");

    CHECK(driver->get_device_number() == 7);
    CHECK(driver->get_unique_id() == "TOUPTEK_AFW_tp-afw-test-id");
    CHECK(driver->get_connected() == false);
    CHECK(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
}

TEST_CASE("ToupTek AFW Filter Wheel Driver - Set position validation path", "[touptek][filterwheel][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_index(0, 0);

    // set_position() validates the connection first. Without hardware we
    // exercise the disconnected path (NotConnected); ConformU covers the
    // connected out-of-range InvalidValue path against real hardware.
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(99); }, alpacacore::AlpacaError::NotConnected);
}
