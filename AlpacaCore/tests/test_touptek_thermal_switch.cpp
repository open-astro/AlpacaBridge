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
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>
#include <alpacacore/version.h>

#include <functional>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_touptek_sdk.h"

namespace {

using alpacacore::test::require_alpaca_error;

}  // namespace

TEST_CASE("ToupTek Thermal Switch Driver - Defaults", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ToupTek Thermal Switch");
    // Disconnected, the bound is the potential element count (dew heater + fan +
    // tail LED); the per-model count is probed at connect.
    REQUIRE(driver->get_max_switch() == 3);
}

TEST_CASE("ToupTek Thermal Switch Driver - Device metadata", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek camera dew heater and fan switch");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek Thermal Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    // Without a connected camera the serial number is unknown, so the unique id
    // falls back to the device number.
    CHECK(driver->get_unique_id() == "TOUPTEK_THERMALSW_3");
}

TEST_CASE("ToupTek Thermal Switch Driver - Not connected throws", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    require_alpaca_error([&]() { driver->get_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(0, 0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_name(1); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_write(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_min_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_switch_value(1); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Thermal Switch Driver - Unsupported actions", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("ToupTek Thermal Switch Driver - Invalid Switch ID", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(3, 50.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(3); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ToupTek Thermal Switch Driver - Async not supported", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    // Async members validate the ID, then require a connection. Out-of-range ID
    // still wins.
    require_alpaca_error([&]() { driver->set_async(3, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async_value(3, 50.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async(0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(0, 50.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_async(0); }, alpacacore::AlpacaError::NotConnected);

    // Connected over the fake: no switch is asynchronous.
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    auto connected = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0, fake);
    connected->set_connected(true);
    for (int id = 0; id < 3; ++id) {
        CHECK_FALSE(connected->get_can_async(id));
        require_alpaca_error([&]() { connected->set_async(id, true); }, alpacacore::AlpacaError::NotImplemented);
        require_alpaca_error([&]() { connected->set_async_value(id, 1.0); }, alpacacore::AlpacaError::NotImplemented);
    }
    connected->set_connected(false);
}

TEST_CASE("ToupTek Thermal Switch Driver - State machine", "[touptek][switch][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0);

    REQUIRE_FALSE(driver->get_connected());
    REQUIRE_FALSE(driver->get_connecting());
    // DeviceState is empty while disconnected per the DeviceState contract.
    {
        // DeviceState is the empty list while disconnected: the SwitchDriver
        // base builds it from the public getters, which throw NotConnected and
        // are omitted, and TimeStamp itself is withheld too (ASCOM read-all FAQ).
        const auto state = driver->get_device_state();
        REQUIRE(state.empty());
    }
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Thermal Switch Driver - Connect fails on invalid camera index", "[touptek][switch][unit]") {
    // With no camera attached connect throws NotConnected before the index is
    // looked at, so the index check needs one attached camera.
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 999, fake);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
}

// Falsified by: touptek_thermal_switch_driver.cpp get_switch_description drops the " (<camera name>)" suffix,
//   set_switch_name writes the wrong element, or get_state_change_complete returns false.
TEST_CASE("ToupTek Thermal Switch Driver - Connected names, descriptions and state change", "[touptek][switch][unit]") {
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0, fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_max_switch() == 3);

    // Every element is writable (dew heater, fan, tail LED); no read-only id exists to test.
    CHECK(driver->get_switch_description(0) == "ToupTek camera anti-fog dew heater level (FakeCam One)");
    CHECK(driver->get_switch_description(1) == "ToupTek camera radiator fan speed (FakeCam One)");
    for (int id = 0; id < 3; ++id) {
        CHECK(driver->get_can_write(id));
        CHECK(driver->get_state_change_complete(id));
    }

    driver->set_switch_name(1, "Radiator");
    CHECK(driver->get_switch_name(1) == "Radiator");
    CHECK(driver->get_switch_name(0) == "DewHeater");
    CHECK(driver->get_switch_description(1) == "ToupTek camera radiator fan speed (FakeCam One)");

    for (const int id : {-1, 3}) {
        require_alpaca_error([&]() { (void)driver->get_switch_description(id); },
                             alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->set_switch_name(id, "x"); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { (void)driver->get_state_change_complete(id); },
                             alpacacore::AlpacaError::InvalidValue);
    }
    driver->set_connected(false);
}

// Falsified by: touptek_thermal_switch_driver.cpp:386 get_max_switch_value returning min_value, :389 get_switch_step
//   returning a value other than 1, or get_can_async returning true.
TEST_CASE("ToupTek Thermal Switch Driver - Connected capability and range members", "[touptek][switch][unit]") {
    alpacacore::test::FakeToupTekSDK fake;
    fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0, fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_max_switch() == 3);

    // Ranges come from the fake SDK: heat level 0-10 (get_heat_max), fan 0-3 (max_fan_speed), tail LED 0-1.
    const double expected_max[] = {10.0, 3.0, 1.0};
    for (int id = 0; id < 3; ++id) {
        CHECK(driver->get_can_write(id));
        CHECK_FALSE(driver->get_can_async(id));
        CHECK(driver->get_min_switch_value(id) == 0.0);
        CHECK(driver->get_max_switch_value(id) == expected_max[id]);
        CHECK(driver->get_switch_step(id) == 1.0);
    }

    for (const int id : {-1, 3}) {
        require_alpaca_error([&]() { (void)driver->get_can_write(id); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { (void)driver->get_can_async(id); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { (void)driver->get_min_switch_value(id); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { (void)driver->get_max_switch_value(id); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { (void)driver->get_switch_step(id); }, alpacacore::AlpacaError::InvalidValue);
    }
    driver->set_connected(false);
}
