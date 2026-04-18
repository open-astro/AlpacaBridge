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
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include "catch2_compat.h"

#include <alpacacore/vendor/svbony/svbony_camera_driver.h>

TEST_CASE("SVBONY Camera Driver - Defaults", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Name is "SVBONY Camera" when no camera is plugged in, or the SDK FriendlyName when detected
    CHECK(driver->get_name().find("SVBONY") != std::string::npos);
}

TEST_CASE("SVBONY Camera Driver - Device metadata", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "SVBONY Camera Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore SVBONY Camera Driver");
    CHECK(driver->get_driver_version() == "1.0.0");
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "SVBONY_3");
}

TEST_CASE("SVBONY Camera Driver - Not connected throws", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

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

TEST_CASE("SVBONY Camera Driver - Disconnected state", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_has_shutter() == false);
}

TEST_CASE("SVBONY Camera Driver - Unsupported actions", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("anything", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("SVBONY Camera Driver - Sub-exposure not supported", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
}
