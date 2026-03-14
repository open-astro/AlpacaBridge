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

#include <alpacacore/vendor/zwo/zwo_switch_driver.h>

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
}
