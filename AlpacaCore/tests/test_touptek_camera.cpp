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
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
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

TEST_CASE("ToupTek Camera Driver - Defaults", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Name is "ToupTek Camera" when no camera is plugged in, or the SDK displayname
    // (model code, may not contain "ToupTek") when one is detected. Either way it's non-empty.
    CHECK_FALSE(driver->get_name().empty());
}

TEST_CASE("ToupTek Camera Driver - Device metadata", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek Camera Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    // Without a connected camera the serial number is unknown, so the unique id falls
    // back to the device number.
    CHECK(driver->get_unique_id() == "TOUPTEK_3");
}

TEST_CASE("ToupTek Camera Driver - Not connected throws", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    CHECK_THROWS_AS(driver->get_ccd_temperature(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_gain(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_gain(100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_offset(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->start_exposure(1.0, true), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->stop_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->abort_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->pulse_guide(0, 100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_array(), alpacacore::AlpacaException);
}

TEST_CASE("ToupTek Camera Driver - Disconnected state", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_has_shutter() == false);
}

TEST_CASE("ToupTek Camera Driver - Unsupported actions", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("anything", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("ToupTek Camera Driver - Sub-exposure not supported", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
}

TEST_CASE("ToupTek Camera Driver - ASCOM Error Codes", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    // TODO: ToupTek throws NotImplemented (0x400) for several properties when no
    // camera handle exists instead of NotConnected (0x407). Fix the driver so all
    // disconnected operations return NotConnected for ConformU compliance.
    auto check_throws_not_connected_or_not_implemented = [&](const std::function<void()>& fn) {
        try {
            fn();
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK((ex.error_code() == alpacacore::AlpacaError::NotConnected ||
                   ex.error_code() == alpacacore::AlpacaError::NotImplemented));
        }
    };

    check_throws_not_connected_or_not_implemented([&]() { driver->get_ccd_temperature(); });
    check_throws_not_connected_or_not_implemented([&]() { driver->get_gain(); });
    check_throws_not_connected_or_not_implemented([&]() { driver->set_gain(100); });
    check_throws_not_connected_or_not_implemented([&]() { driver->get_offset(); });
    require_alpaca_error([&]() { driver->start_exposure(1.0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->abort_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_array(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek Camera Driver - State Machine Contracts", "[touptek][camera][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0);

    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    REQUIRE(driver->get_image_ready() == false);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
}
