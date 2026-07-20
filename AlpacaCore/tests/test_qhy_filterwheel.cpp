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
#include <alpacacore/vendor/qhy/qhy_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <string>
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

TEST_CASE("QHY Filter Wheel Driver - Defaults", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);
    CHECK(driver->get_name() == "QHY CFW");
}

TEST_CASE("QHY Filter Wheel Driver - Device metadata", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "QHY integrated color filter wheel driver");
    CHECK(driver->get_driver_info() == "AlpacaCore QHY Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "QHY_CFW_3");
}

TEST_CASE("QHY Filter Wheel Driver - Not connected throws", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("QHY Filter Wheel Driver - Unsupported actions", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("QHY Filter Wheel Driver - Names and focus offsets configurable while disconnected",
          "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(1, 0);

    // Slot count is unknown until connect, so no length validation is imposed yet.
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0, 10}));
    REQUIRE_NOTHROW(driver->set_names({"L", "R"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0, 10});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L", "R"});
}

TEST_CASE("QHY Filter Wheel Driver - Value range validation", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    // ASCOM precedence (per AGENTS.md): range validation precedes the
    // connection check. A negative position is unconditionally invalid, so it
    // is InvalidValue even while disconnected. The upper bound depends on the
    // hardware-reported slot count (unknown until connect), so an
    // in-range-but-unverifiable index reports NotConnected instead;
    // ConformU covers the connected out-of-range InvalidValue path on
    // hardware (matches the ToupTek AFW test pattern).
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(99); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("QHY Filter Wheel Driver - State machine contracts", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: Position throws while disconnected and is
    // omitted, leaving just the TimeStamp; the non-compliant "Connected"
    // entry must not appear.
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

TEST_CASE("QHY Filter Wheel Driver - Unsupported methods", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("QHY Filter Wheel Driver - Create by id and device number assignment", "[qhy][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);
    auto d1 = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(1, 0);
    auto by_id = alpacacore::vendor::qhy::create_qhy_filterwheel(7, "qhy-cfw-test-id");

    CHECK(d0->get_device_number() == 0);
    CHECK(d1->get_device_number() == 1);
    CHECK(d0->get_unique_id() != d1->get_unique_id());

    CHECK(by_id->get_device_number() == 7);
    CHECK(by_id->get_unique_id() == "QHY_CFW_qhy-cfw-test-id");
    CHECK(by_id->get_connected() == false);
    CHECK(by_id->get_device_type() == alpacacore::DeviceType::FilterWheel);
}
