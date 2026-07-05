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
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <functional>
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

TEST_CASE("Player One PW Filter Wheel Driver - Defaults", "[playerone][filterwheel][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Player One Phoenix Wheel");
}

TEST_CASE("Player One PW Filter Wheel Driver - Disconnected Behavior", "[playerone][filterwheel][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: Position throws while disconnected and is omitted,
    // leaving just the TimeStamp.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
    // Range validation precedes the connection check (ASCOM precedence): a negative
    // position is InvalidValue even while disconnected (matches the ToupTek AFW).
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0}));
    REQUIRE_NOTHROW(driver->set_names({"L"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L"});

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Player One PW Filter Wheel Driver - Device metadata", "[playerone][filterwheel][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Player One Phoenix Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Player One Phoenix Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "PlayerOne_PW_3");
}

TEST_CASE("Player One PW Filter Wheel Driver - Default names applied", "[playerone][filterwheel][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);

    // Empty entries are replaced with the ASCOM-conventional "Filter N".
    driver->set_names({"Red", "", "Blue"});
    REQUIRE(driver->get_names() == std::vector<std::string>{"Red", "Filter 2", "Blue"});
}

TEST_CASE("Player One PW Filter Wheel Driver - Device Number Assignment", "[playerone][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);
    auto d1 = alpacacore::vendor::playerone::create_playerone_filterwheel(1, 0);
    auto d5 = alpacacore::vendor::playerone::create_playerone_filterwheel(5, 0);

    CHECK(d0->get_device_number() == 0);
    CHECK(d1->get_device_number() == 1);
    CHECK(d5->get_device_number() == 5);
}

TEST_CASE("Player One PW Filter Wheel Driver - Unique IDs", "[playerone][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);
    auto d1 = alpacacore::vendor::playerone::create_playerone_filterwheel(1, 0);

    CHECK(d0->get_unique_id() != d1->get_unique_id());
}
