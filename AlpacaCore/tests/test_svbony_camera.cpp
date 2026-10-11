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
#include <alpacacore/vendor/svbony/svbony_camera_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <thread>

#include "catch2_compat.h"
#include "fake_svbony_sdk.h"
#include "locked_svbony_sdk.h"

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
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
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
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
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

TEST_CASE("SVBONY Camera Driver - ASCOM Error Codes", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    require_alpaca_error([&]() { driver->get_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_gain(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_gain(100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->start_exposure(1.0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->abort_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_array(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("SVBONY Camera Driver - State Machine Contracts", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
}

// --- SDK seam: driver over FakeSVBSDK / LockedSVBSDK --------------------------

TEST_CASE("SVBONY Camera Driver - connects over an injected SDK", "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);

    // The version comes from the injected SDK, not the singleton.
    REQUIRE(driver->get_device_sdk_version().value_or("") == "1.13.0-fake");
    CHECK_FALSE(driver->get_device_firmware().has_value());

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.open_count[7] == 1);
    CHECK(driver->get_device_firmware().value_or("") == "1.2.3");
    CHECK(driver->get_camera_x_size() == 64);
    CHECK(driver->get_camera_y_size() == 48);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.close_count[7] == 1);
}

TEST_CASE("SVBONY Camera Driver - failed connect closes the opened camera", "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    fake.throw_from.insert("get_serial_number");
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.open_count[7] == 1);
    CHECK(fake.close_count[7] == 1);
}

TEST_CASE("SVBONY Camera Driver - no camera detected refuses the connect", "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    fake.cameras.clear();
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK(fake.call_count("open_camera") == 0);
}

TEST_CASE("SVBONY Camera Driver - camera index out of range is InvalidValue", "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 5, fake);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(fake.call_count("open_camera") == 0);
}

TEST_CASE("SVBONY Camera Driver - exposure delivers the fake frame", "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    alpacacore::test::LockedSVBSDK sdk(fake);
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->start_exposure(0.05, true);
    for (int i = 0; i < 200 && !driver->get_image_ready(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(driver->get_image_ready());
    CHECK(fake.call_count("start_video_capture") >= 1);
    CHECK(fake.call_count("get_video_data") >= 1);
    driver->set_connected(false);
}

TEST_CASE("FakeSVBSDK behind LockedSVBSDK - forwards every entry point", "[svbony][unit][fake-sdk]") {
    using namespace alpacacore::vendor::svbony;
    alpacacore::test::FakeSVBSDK fake;
    alpacacore::test::LockedSVBSDK sdk(fake);
    SVBCameraInfo info;
    long value = 0;
    bool is_auto = true;
    std::uint8_t buf[4] = {};

    CHECK(sdk.enumerate_cameras().size() == 1);
    CHECK(sdk.get_camera_info_by_index(0, info));
    CHECK_FALSE(sdk.get_camera_info_by_index(3, info));
    sdk.open_camera(7);
    sdk.close_camera(7);
    CHECK(sdk.get_control_caps(7).size() == 2);
    sdk.set_control_value(7, SVBControlType::Gain, 42, false);
    CHECK(sdk.get_control_value(7, SVBControlType::Gain, value, is_auto));
    CHECK(value == 42);
    sdk.set_roi_format(7, 0, 0, 32, 24, 2);
    CHECK(sdk.get_roi_format(7).width == 32);
    sdk.set_output_image_type(7, SVBImageType::Raw8);
    CHECK(sdk.get_output_image_type(7) == SVBImageType::Raw8);
    sdk.start_video_capture(7);
    sdk.get_video_data(7, buf, 4, 10);
    sdk.stop_video_capture(7);
    CHECK(buf[0] == fake.fill_byte);
    sdk.pulse_guide(7, SVBGuideDirection::North, 10);
    CHECK(sdk.get_sensor_pixel_size(7) > 3.0F);
    CHECK(sdk.get_serial_number(7) == "SVB-FAKE-0001");
    CHECK(sdk.get_sdk_version() == "1.13.0-fake");
    CHECK(sdk.get_firmware_version(7) == "1.2.3");
    sdk.set_camera_mode_normal(7);
    sdk.set_auto_save_param(7, false);
    sdk.restore_default_param(7);

    for (const char* name :
         {"enumerate_cameras",   "get_camera_info_by_index", "open_camera",           "close_camera",
          "get_control_caps",    "get_control_value",        "set_control_value",     "get_roi_format",
          "set_roi_format",      "get_output_image_type",    "set_output_image_type", "start_video_capture",
          "stop_video_capture",  "get_video_data",           "pulse_guide",           "get_sensor_pixel_size",
          "get_serial_number",   "get_sdk_version",          "get_firmware_version",  "set_camera_mode_normal",
          "set_auto_save_param", "restore_default_param"}) {
        INFO(name);
        CHECK(fake.call_count(name) >= 1);
    }
}

TEST_CASE("SVBONY Camera Driver - default factory still works without an SDK argument",
          "[svbony][camera][unit][fake-sdk]") {
    // The two-argument factory binds the real wrapper singleton; the driver reaches the SDK only on connect.
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(2, 0);
    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_number() == 2);
    CHECK_FALSE(driver->get_connected());
}
