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
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/version.h>

#include <functional>

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

TEST_CASE("ZWO Dew Heater Switch Driver - Defaults", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ZWO Dew Heater");
    REQUIRE(driver->get_max_switch() == 1);
}

TEST_CASE("ZWO Dew Heater Switch Driver - Disconnected Behavior", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    REQUIRE_FALSE(driver->get_connected());
    // Reading or writing switch value requires connection (dew caps loaded from camera)
    REQUIRE_THROWS(driver->get_switch_value(0));
    REQUIRE_THROWS(driver->set_switch_value(0, 0.0));
    REQUIRE_THROWS(driver->get_switch(0));
    REQUIRE_THROWS(driver->set_switch(0, false));
}

TEST_CASE("ZWO Dew Heater Switch Driver - Invalid Switch ID", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    REQUIRE(driver->get_max_switch() == 1);
    REQUIRE_THROWS(driver->get_can_write(1));
    REQUIRE_THROWS(driver->get_switch_name(1));

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO Dew Heater Switch Driver - Device metadata", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(3, 0);

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO camera dew heater switch");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO Dew Heater Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_DEW_3");
}

TEST_CASE("ZWO Dew Heater Switch Driver - Unsupported actions", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    REQUIRE(driver != nullptr);

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("ZWO Dew Heater Switch Driver - ASCOM Error Codes", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    require_alpaca_error([&]() { driver->get_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(0, 0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, false); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO Dew Heater Switch Driver - Disconnected DeviceState", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    {
        // Only the TimeStamp survives while disconnected: the SwitchDriver base
        // builds DeviceState from the public getters, which throw NotConnected
        // and are omitted per the DeviceState contract.
        const auto state = driver->get_device_state();
        REQUIRE(state.size() == 1);
        CHECK(state[0].name == "TimeStamp");
    }
}
