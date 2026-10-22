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
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <thread>

#include "camera_percent_cooler_checks.h"
#include "catch2_compat.h"
#include "fake_task_clock.h"
#include "fake_zwo_sdk.h"

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

TEST_CASE("ZWO Camera Driver - Defaults", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Name is "ZWO Camera" when no camera is plugged in, or the SDK name when detected
    CHECK(driver->get_name().find("ZWO") != std::string::npos);
}

TEST_CASE("ZWO Camera Driver - Device metadata", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO ASI Camera Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    // Not derived from the device number or the enumeration index (#914).
    CHECK(driver->get_unique_id().rfind("ZWO_UID_", 0) == 0);
    CHECK(driver->get_unique_id() == driver->get_unique_id());
}

TEST_CASE("ZWO Camera Driver - UniqueID follows the config entry, not the device number", "[zwo][camera][unit]") {
    alpacacore::vendor::zwo::ZwoCameraBinding serial_less;
    serial_less.identity.camera_name = "ZWO ASI120MM Mini";
    serial_less.unique_id = "ZWO_UID_00112233445566ff";
    alpacacore::vendor::zwo::ZwoCameraBinding with_serial;
    with_serial.identity.serial = "0c190e111d020900";
    with_serial.unique_id = "ZWO_UID_ignored";

    // Same stored value on two starts and under two device numbers.
    CHECK(alpacacore::vendor::zwo::create_zwo_camera_bound(0, serial_less)->get_unique_id() ==
          "ZWO_UID_00112233445566ff");
    CHECK(alpacacore::vendor::zwo::create_zwo_camera_bound(1, serial_less)->get_unique_id() ==
          "ZWO_UID_00112233445566ff");
    CHECK(alpacacore::vendor::zwo::create_zwo_camera_bound(1, with_serial)->get_unique_id() ==
          "ZWO_SN_0c190e111d020900");
}

TEST_CASE("ZWO Camera Driver - DeviceState is empty when disconnected", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    // A disconnected driver's DeviceState is the empty list -- no TimeStamp
    // (ASCOM read-all FAQ), and none of the getters that answer a default
    // (CameraState, PercentCompleted) leak into it.
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("ZWO Camera Driver - Not connected throws", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    CHECK_THROWS_AS(driver->get_ccd_temperature(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_gain(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_gain(100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_offset(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->start_exposure(1.0, true), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->stop_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->abort_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->pulse_guide(0, 100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_array(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_ready(), alpacacore::AlpacaException);
}

TEST_CASE("ZWO Camera Driver - Disconnected state", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
}

TEST_CASE("ZWO Camera Driver - Unsupported actions", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("anything", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("ZWO Camera Driver - Sub-exposure not supported", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
}

TEST_CASE("ZWO Camera Driver - ASCOM Error Codes", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    require_alpaca_error([&]() { driver->start_exposure(-1.0, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->start_exposure(std::numeric_limits<double>::quiet_NaN(), true); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_gain(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_gain(100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->start_exposure(1.0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->abort_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_array(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO Camera Driver - State Machine Contracts", "[zwo][camera][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
}

namespace {

std::string connect_error(alpacacore::CameraDriver& driver) {
    try {
        driver.set_connected(true);
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.what();
    }
    return "";
}

}  // namespace

// Issue #738: a camera registered by cameraId lost that id the first time a
// client read its name while it was disconnected, so every later connect
// failed with "Camera ID not specified". A name query must not change what a
// connect does. The id is 256, the SDK's ASICAMERA_ID_MAX, which no attached
// camera can have, so the connect fails with or without cameras on the bus;
// the point is that both drivers fail the same way, on the configured id.
TEST_CASE("ZWO Camera Driver - A name query keeps the configured camera id", "[zwo][camera][unit]") {
    constexpr int kNoSuchCameraId = 256;
    auto untouched = alpacacore::vendor::zwo::create_zwo_camera(0, kNoSuchCameraId);
    const std::string expected = connect_error(*untouched);
    REQUIRE_FALSE(expected.empty());

    auto queried = alpacacore::vendor::zwo::create_zwo_camera(0, kNoSuchCameraId);
    (void)queried->get_name();
    const std::string actual = connect_error(*queried);

    CHECK(actual != "Camera ID not specified");
    CHECK(actual == expected);
}

// Issue #1477: ICameraV4 PercentCompleted raises InvalidOperation outside Exposing; the cooler members raise
// PropertyNotImplemented on a camera without a cooler.
TEST_CASE("ZWO Camera Driver - PercentCompleted only while Exposing", "[zwo][camera][unit]") {
    alpacacore::test::FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    // The retry window is measured from the epoch before the first retry.
    clock.advance(std::chrono::seconds(1));

    SECTION("idle before the first exposure") { alpacacore::test::require_percent_completed_unavailable(*camera); }
    SECTION("idle after a completed exposure") {
        camera->start_exposure(0.001, true);
        REQUIRE(camera->get_image_ready());
        alpacacore::test::require_percent_completed_unavailable(*camera);
    }
    SECTION("after a failed exposure") {
        // Three Failed statuses exhaust the retries (the driver waits 250 ms on the task clock between them);
        // the old value here was 100 because the failed frame still counted as an image.
        camera->start_exposure(0.001, true);
        sdk.script_exposure_status({alpacacore::vendor::zwo::ZWOExposureStatus::Failed,
                                    alpacacore::vendor::zwo::ZWOExposureStatus::Failed,
                                    alpacacore::vendor::zwo::ZWOExposureStatus::Failed});
        CHECK_FALSE(camera->get_image_ready());
        clock.advance(std::chrono::milliseconds(250));
        CHECK_FALSE(camera->get_image_ready());
        clock.advance(std::chrono::milliseconds(250));
        alpacacore::test::require_error_code([&] { (void)camera->get_image_ready(); },
                                             alpacacore::AlpacaError::DriverException);
        alpacacore::test::require_percent_completed_unavailable(*camera);
    }
    SECTION("while exposing") {
        sdk.auto_complete_exposure = false;
        camera->start_exposure(30.0, true);
        alpacacore::test::require_percent_completed_available(*camera);
    }
}

TEST_CASE("ZWO Camera Driver - cooler members need the cooler capability", "[zwo][camera][unit]") {
    alpacacore::test::FakeZWOSDK sdk;
    sdk.report_cooler_controls = false;
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);
    camera->set_connected(true);
    CHECK_FALSE(camera->get_can_get_cooler_power());
    alpacacore::test::require_no_cooler_members(*camera);
}

TEST_CASE("ZWO Camera Driver - cooler members work with the cooler capability", "[zwo][camera][unit]") {
    alpacacore::test::FakeZWOSDK sdk;
    auto camera = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);
    camera->set_connected(true);
    REQUIRE(camera->get_can_get_cooler_power());
    camera->set_cooler_on(true);
    CHECK(camera->get_cooler_on());
    CHECK(camera->get_cooler_power() >= 0.0);
}
