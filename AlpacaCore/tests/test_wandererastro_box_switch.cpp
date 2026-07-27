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
#include <alpacacore/vendor/wandererastro/wandererastro_box_protocol_wrapper.h>
#include <alpacacore/vendor/wandererastro/wandererastro_box_switch_driver.h>
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

}  // namespace

TEST_CASE("WandererAstro Box Switch Driver - Defaults", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "WandererAstro WandererBox Pro V3");
    CHECK(driver->get_max_switch() == 24);
    CHECK_FALSE(driver->get_connecting());
}

TEST_CASE("WandererAstro Box Switch Driver - Device metadata", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(3, "/dev/null");

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "WandererAstro WandererBox Pro V3 Power Box Switch Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore WandererAstro WandererBox Switch Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "WANDERERASTRO_BOX_3");
    // Firmware surfaces via the web UI only, and only once connected.
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("WandererAstro Box Switch Driver - Not connected throws", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(2, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(4, 128.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async(2, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(4, 0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_write(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_async(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_name(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_name(0, "x"); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_description(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_min_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_step(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro Box Switch Driver - Unsupported actions", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("WandererAstro Box Switch Driver - Value range validation", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(24, 0.5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async_value(-1, 0.5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(24); }, alpacacore::AlpacaError::InvalidValue);
    // Position value validation itself runs after the connection check, so a
    // bad value on a disconnected driver reports NotConnected — same ordering
    // as the other Wanderer switch drivers.
    require_alpaca_error([&]() { driver->set_switch_value(4, 500.0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro Box Switch Driver - Disconnected DeviceState", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    // Only the TimeStamp survives while disconnected: the SwitchDriver base
    // builds DeviceState from the public getters, which throw NotConnected
    // and are omitted per the DeviceState contract.
    const auto state = driver->get_device_state();
    REQUIRE(state.size() == 1);
    CHECK(state[0].name == "TimeStamp");
}

TEST_CASE("WandererAstro Box Switch Driver - Connect failure on invalid port", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/nonexistent-box-port");

    // Synchronous connect against a missing port must fail with NotConnected
    // and leave the driver disconnected.
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    // set_connected(false) while already disconnected is an idempotent no-op.
    CHECK_NOTHROW(driver->set_connected(false));
}

TEST_CASE("WandererAstro Box Protocol Wrapper - Defaults and disconnected state",
          "[wandererastro][switch][unit]") {
    using namespace alpacacore::vendor::wandererastro;

    // Hardware constants the Switch surface is built on.
    static_assert(kBoxPwmMax == 255);
    CHECK(kBoxDc34VoltageMin == 5.0);
    CHECK(kBoxDc34VoltageMax == 13.2);
    CHECK(kBoxDc34VoltageStep == 0.1);
    CHECK(kBoxCalibratedPowerMinFirmware == 20240216);

    WandererBoxProtocolWrapper wrapper;
    CHECK_FALSE(wrapper.is_connected());
    CHECK_FALSE(wrapper.get_firmware_date().has_value());

    const auto state = wrapper.get_state();
    CHECK_FALSE(state.valid);
    CHECK(state.input_voltage == 0.0);
    CHECK_FALSE(state.dc3_4);

    // Operations on a disconnected wrapper report NotConnected; out-of-range
    // arguments report InvalidValue regardless of connection.
    require_alpaca_error([&]() { wrapper.set_dc3_4(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_pwm(5, 128); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_pwm(4, 128); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_pwm(5, 256); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dc3_4_voltage(4.9); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dc3_4_voltage(13.3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_usb(5, true); }, alpacacore::AlpacaError::InvalidValue);

    // Disconnect on a never-connected wrapper is a safe no-op.
    CHECK_NOTHROW(wrapper.disconnect());
}
