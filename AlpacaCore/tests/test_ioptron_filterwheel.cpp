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
#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_iefw_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <functional>
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

TEST_CASE("iOptron iEFW Filter Wheel Driver - Defaults", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Default model is the iEFW-15; "iefw18" selects the other name.
    CHECK(driver->get_name() == "iOptron iEFW-15");
    CHECK(alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0, "iefw18")->get_name() ==
          "iOptron iEFW-18");
    CHECK(alpacacore::vendor::ioptron::create_iefw_filterwheel(0, "/dev/null", "iefw18")->get_name() ==
          "iOptron iEFW-18");
    CHECK(driver->get_connecting() == false);
    // Firmware is only known while connected; the web UI hides the row otherwise.
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Device metadata", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "iOptron iEFW Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron iEFW Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "IOPTRON_IEFW_3");
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Not connected throws", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(1, "/dev/ttyUSB0");

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
    // Never opened the port, so the sync disconnect path is a no-op.
    REQUIRE_NOTHROW(driver->set_connected(false));
    REQUIRE(driver->get_connected() == false);
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Unsupported actions", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Names and offsets before connect", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);

    // Slot count is unknown until the handshake, so config-time values are
    // accepted at any length and resized at connect (ZWO EFW semantics).
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0, 10, -5}));
    REQUIRE_NOTHROW(driver->set_names({"L", "", "B"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0, 10, -5});
    // Empty entries get the "Filter N" default immediately.
    REQUIRE(driver->get_names() == std::vector<std::string>{"L", "Filter 2", "B"});
    // A lone shorthand token is left alone until the slot count is known.
    REQUIRE_NOTHROW(driver->set_names({"LRGBSHOC"}));
    REQUIRE(driver->get_names() == std::vector<std::string>{"LRGBSHOC"});
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Value range validation", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(2, "/dev/ttyUSB0");

    // Range validation precedes the connection check (ASCOM precedence): a
    // negative position is InvalidValue even while disconnected.
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(-100); }, alpacacore::AlpacaError::InvalidValue);
    // The upper bound depends on the slot count, which needs the handshake,
    // so a large value while disconnected is NotConnected, not InvalidValue.
    require_alpaca_error([&]() { driver->set_position(8); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - State machine", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(4, 1);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: Position throws while disconnected and is
    // omitted, leaving just the TimeStamp.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        REQUIRE(entry.name != "Position");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Model codes", "[ioptron][filterwheel][unit]") {
    using alpacacore::vendor::ioptron::iefw_model_name;
    using alpacacore::vendor::ioptron::iefw_slot_count;
    using alpacacore::vendor::ioptron::is_iefw_model;

    // :DeviceInfo# model codes (INDI ioptron_wheel.cpp): 99 = iEFW-15,
    // 98 = iEFW-18. Confirmed on an iEFW-15 (code 99, 5 slots).
    CHECK(is_iefw_model(99));
    CHECK(is_iefw_model(98));
    // The focusers (2 = iEAF, 3 = iAFS2/3) share the handshake but must be rejected here.
    CHECK_FALSE(is_iefw_model(2));
    CHECK_FALSE(is_iefw_model(3));
    CHECK_FALSE(is_iefw_model(0));
    CHECK_FALSE(is_iefw_model(97));

    CHECK(iefw_slot_count(99) == 5);
    CHECK(iefw_slot_count(98) == 8);
    CHECK(iefw_slot_count(2) == 0);

    CHECK(iefw_model_name(99) == "iEFW-15");
    CHECK(iefw_model_name(98) == "iEFW-18");
    CHECK(iefw_model_name(0) == "iEFW");
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Unique IDs", "[ioptron][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);
    auto d1 = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(1, 0);
    auto d5 = alpacacore::vendor::ioptron::create_iefw_filterwheel(5, "/dev/ttyUSB1");

    CHECK(d0->get_unique_id() != d1->get_unique_id());
    CHECK(d5->get_unique_id() == "IOPTRON_IEFW_5");
    CHECK(d5->get_device_number() == 5);
}
