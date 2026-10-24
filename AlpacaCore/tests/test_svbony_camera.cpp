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
#include <limits>
#include <memory>
#include <thread>

#include "camera_percent_cooler_checks.h"
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
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::MethodNotImplemented);
    CHECK_THROWS_AS(driver->abort_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->pulse_guide(0, 100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_array(), alpacacore::AlpacaException);
}

TEST_CASE("SVBONY Camera Driver - Disconnected state", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == false);
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

    require_alpaca_error([&]() { driver->start_exposure(-1.0, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->start_exposure(std::numeric_limits<double>::quiet_NaN(), true); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->start_exposure(std::numeric_limits<double>::infinity(), true); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->start_exposure(1e30, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_gain(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_gain(100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->start_exposure(1.0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->abort_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_array(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("SVBONY Camera Driver - State Machine Contracts", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == false);
}

// --- SDK seam: driver over FakeSVBSDK / LockedSVBSDK --------------------------

TEST_CASE("SVBONY Camera Driver - connects over an injected SDK", "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);

    // The version comes from the injected SDK, not the singleton.
    REQUIRE(driver->get_device_sdk_version().value_or("") == "fake-svbony-sdk");
    CHECK_FALSE(driver->get_device_firmware().has_value());

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.open_count() == 1);
    CHECK(driver->get_device_firmware().value_or("") == "fake-firmware");
    CHECK(driver->get_camera_x_size() == 16);
    CHECK(driver->get_camera_y_size() == 8);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.open_count() == 0);
    CHECK(fake.call_count("close_camera") == 1);
}

TEST_CASE("SVBONY Camera Driver - failed connect closes the opened camera", "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    fake.fail_next("get_serial_number");
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);

    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.call_count("open_camera") == 1);
    CHECK(fake.call_count("close_camera") == 1);
    CHECK(fake.open_count() == 0);
}

TEST_CASE("SVBONY Camera Driver - no camera detected refuses the connect", "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    fake.set_camera_present(false);
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
    std::vector<std::uint8_t> buf(16 * 8, 0);

    CHECK(sdk.enumerate_cameras().size() == 1);
    CHECK(sdk.get_camera_info_by_index(0, info));
    CHECK_FALSE(sdk.get_camera_info_by_index(3, info));
    sdk.open_camera(17);
    CHECK(sdk.get_control_caps(17).size() == 5);
    sdk.set_control_value(17, SVBControlType::Gain, 42, false);
    CHECK(sdk.get_control_value(17, SVBControlType::Gain, value, is_auto));
    CHECK(value == 42);
    sdk.set_roi_format(17, 0, 0, 16, 8, 1);
    CHECK(sdk.get_roi_format(17).width == 16);
    sdk.set_output_image_type(17, SVBImageType::Raw8);
    CHECK(sdk.get_output_image_type(17) == SVBImageType::Raw8);
    sdk.start_video_capture(17);
    CHECK(sdk.get_video_data(17, buf.data(), static_cast<long>(buf.size()), 10) == SVBVideoDataResult::Frame);
    sdk.stop_video_capture(17);
    sdk.pulse_guide(17, SVBGuideDirection::North, 10);
    CHECK(sdk.get_sensor_pixel_size(17) > 3.0F);
    CHECK(sdk.get_serial_number(17) == "fake-17");
    CHECK(sdk.get_sdk_version() == "fake-svbony-sdk");
    CHECK(sdk.get_firmware_version(17) == "fake-firmware");
    sdk.set_camera_mode_normal(17);
    sdk.set_auto_save_param(17, false);
    sdk.restore_default_param(17);
    sdk.close_camera(17);

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

// Issue #1477: ICameraV4 PercentCompleted raises InvalidOperation outside Exposing; the cooler members raise
// PropertyNotImplemented on a camera without a cooler (the cooler controls the SDK reports are the capability).
namespace {

std::unique_ptr<alpacacore::CameraDriver> svbony_connected_camera(alpacacore::test::FakeSVBSDK& sdk) {
    auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    camera->set_connected(true);
    REQUIRE(camera->get_connected());
    return camera;
}

}  // namespace

TEST_CASE("SVBONY Camera - PercentCompleted only while Exposing", "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    // Declared after the camera so it runs first: a failed REQUIRE must not leave the SDK read held or a
    // scripted stop failure set while the driver joins its worker.
    struct ResetSdkOnExit {
        alpacacore::test::FakeSVBSDK& sdk;
        ~ResetSdkOnExit() {
            sdk.set_persistent_failure("stop_video_capture", false);
            sdk.release_video_data();
        }
    } reset_sdk{sdk};

    SECTION("idle before the first exposure") { alpacacore::test::require_percent_completed_unavailable(*camera); }
    SECTION("idle after a completed exposure") {
        camera->start_exposure(0.001, true);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!camera->get_image_ready() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(camera->get_image_ready());
        alpacacore::test::require_percent_completed_unavailable(*camera);
    }
    SECTION("while exposing") {
        sdk.block_video_data();
        camera->start_exposure(0.001, true);
        REQUIRE(sdk.wait_for_video_data(2s));
        alpacacore::test::require_percent_completed_available(*camera);
    }
    SECTION("in the Error state after an unconfirmed capture stop") {
        sdk.block_video_data();
        camera->start_exposure(0.001, true);
        REQUIRE(sdk.wait_for_video_data(2s));
        sdk.set_persistent_failure("stop_video_capture");
        require_alpaca_error([&] { camera->abort_exposure(); }, alpacacore::AlpacaError::DriverException);
        const int reads = sdk.completed_video_data_reads();
        sdk.release_video_data();
        REQUIRE(sdk.wait_for_completed_video_data(reads + 1, 2s));
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (camera->get_camera_state() != alpacacore::CameraState::Error &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(camera->get_camera_state() == alpacacore::CameraState::Error);
        alpacacore::test::require_percent_completed_unavailable(*camera);
    }
}

TEST_CASE("SVBONY Camera - cooler members need the cooler capability", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    CHECK_FALSE(camera->get_can_get_cooler_power());
    alpacacore::test::require_no_cooler_members(*camera);
}

TEST_CASE("SVBONY Camera - cooler members work with the cooler capability", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    sdk.add_cooler_controls();
    auto camera = svbony_connected_camera(sdk);
    REQUIRE(camera->get_can_get_cooler_power());
    camera->set_cooler_on(true);
    CHECK(camera->get_cooler_on());
    CHECK(camera->get_cooler_power() == 0.0);
}

// Issue #1277: the SVBONY SDK has no e-/ADU or full-well figure, so both raise PropertyNotImplemented instead of a
// made-up 1.0 or the ADU saturation; an unknown SensorName is "", not a placeholder.
TEST_CASE(
    "SVBONY Camera Driver - ElectronsPerADU and FullWellCapacity are not implemented, SensorName has no placeholder",
    "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);

    require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_full_well_capacity(); }, alpacacore::AlpacaError::NotConnected);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_full_well_capacity(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    driver->set_connected(false);
}

TEST_CASE("SVBONY Camera Driver - an unknown SensorName is empty", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK fake;
    fake.set_camera_present(false);
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);
    CHECK(driver->get_sensor_name().empty());
}
