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

// Altair camera driver: the ToupTek camera driver with Altair identity over
// the Altair SDK. Every case runs over FakeToupTekSDK through the same seam
// AltairSDKWrapper implements, so none of them touches libaltaircam or USB,
// and the connected cases are deterministic. The fake camera mirrors the
// ALTAIR178M3 the driver was validated on: mono, uncooled, ST4.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/altair/altair_camera_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <string>

#include "catch2_compat.h"
#include "fake_touptek_sdk.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::FakeToupTekSDK;

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// An IMX178 mono camera with no cooler, as libaltaircam 60.31589 enumerates
// the ALTAIR178M3 (USB 16d0:0d82, 3040x2048, 2.40 um, flags CMOS | MONO |
// USB30 | ST4 | GETTEMPERATURE | RAW14 | BLACKLEVEL | TRIGGER_SOFTWARE).
FakeToupTekSDK::ToupCameraInfo altair_178m(const std::string& id) {
    auto info = FakeToupTekSDK::default_camera(id, "ALTAIR178M3");
    info.model_name = "ALTAIR178M3";
    info.max_width = 3040;
    info.max_height = 2048;
    info.pixel_size_um_x = 2.4F;
    info.pixel_size_um_y = 2.4F;
    info.is_color = false;
    info.supported_bins = {1, 2, 3, 4};
    info.supports_pulse_guide = true;
    info.supports_blacklevel = true;
    info.supports_cooler = false;
    info.supports_tec_onoff = false;
    info.supports_heat = false;
    info.supports_fan = false;
    info.max_fan_speed = 0;
    info.bit_depth_max = 14;
    return info;
}

FakeToupTekSDK make_fake_with_178m() {
    FakeToupTekSDK fake;
    fake.cameras.push_back(altair_178m("altair-178m-0"));
    return fake;
}

}  // namespace

TEST_CASE("Altair Camera Driver - Defaults", "[altair][camera][unit]") {
    FakeToupTekSDK fake;  // no camera attached
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, fake);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Altair Camera");
    CHECK(driver->get_sensor_name() == "Altair Sensor");
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_has_shutter() == false);
}

TEST_CASE("Altair Camera Driver - Device metadata", "[altair][camera][unit]") {
    FakeToupTekSDK fake;
    auto driver = alpacacore::vendor::altair::create_altair_camera(3, 1, fake);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Altair Camera Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Altair Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    // No serial number until connected: the unique id falls back to the device
    // number, under the Altair prefix so it can never collide with a ToupTek
    // camera's id on the same server.
    CHECK(driver->get_unique_id() == "ALTAIR_3");
}

TEST_CASE("Altair Camera Driver - Connected identity comes from the Altair SDK", "[altair][camera][unit]") {
    auto fake = make_fake_with_178m();
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, fake);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.last_opened_id == "altair-178m-0");
    CHECK(fake.ref_count("altair-178m-0") == 1);
    CHECK(driver->get_name() == "ALTAIR178M3");
    CHECK(driver->get_sensor_name() == "ALTAIR178M3");
    CHECK(driver->get_unique_id() == "ALTAIR_SN_FAKESN0001");
    CHECK(driver->get_camera_x_size() == 3040);
    CHECK(driver->get_camera_y_size() == 2048);
    // Connect reads the pixel size from the SDK's get_PixelSize, which wins over
    // the enumerated model value; the fake answers 3.76 there.
    CHECK(driver->get_pixel_size_x() == Catch::Approx(3.76));
    CHECK(driver->get_sensor_type() == alpacacore::SensorType::Monochrome);
    CHECK(driver->get_can_pulse_guide() == true);
    CHECK(driver->get_can_set_ccd_temperature() == false);
    CHECK(driver->get_can_get_cooler_power() == false);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.ref_count("altair-178m-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("Altair Camera Driver - Not connected throws", "[altair][camera][unit]") {
    FakeToupTekSDK fake;
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, fake);

    require_alpaca_error([&]() { driver->start_exposure(1.0, true); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->stop_exposure(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->abort_exposure(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_array(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_readout_mode(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_offset(0); }, AlpacaError::NotConnected);
}

TEST_CASE("Altair Camera Driver - Connect refuses a missing camera", "[altair][camera][unit]") {
    // Nothing enumerated: the connect refusal names the brand the operator
    // configured, not ToupTek.
    FakeToupTekSDK fake;
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, fake);
    try {
        driver->set_connected(true);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::NotConnected);
        CHECK(std::string(ex.what()).find("Altair") != std::string::npos);
    }
    CHECK_FALSE(driver->get_connected());

    // An index past the enumerated cameras is InvalidValue, and opens nothing.
    auto fake_one = make_fake_with_178m();
    auto second = alpacacore::vendor::altair::create_altair_camera(0, 1, fake_one);
    require_alpaca_error([&]() { second->set_connected(true); }, AlpacaError::InvalidValue);
    CHECK(fake_one.physical_opens == 0);
}

TEST_CASE("Altair Camera Driver - Unsupported actions", "[altair][camera][unit]") {
    FakeToupTekSDK fake;
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, fake);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); }, AlpacaError::MethodNotImplemented);
}

TEST_CASE("Altair Camera Driver - Value range validation", "[altair][camera][unit]") {
    FakeToupTekSDK fake;
    auto disconnected = alpacacore::vendor::altair::create_altair_camera(0, 0, fake);
    // The readout-mode index is range-checked before the connection check.
    require_alpaca_error([&]() { disconnected->set_readout_mode(-1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { disconnected->set_readout_mode(999999); }, AlpacaError::InvalidValue);

    auto fake_cam = make_fake_with_178m();
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, fake_cam);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    require_alpaca_error([&]() { driver->start_exposure(-0.1, true); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_bin_x(0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_bin_x(5); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_gain(99); }, AlpacaError::InvalidValue);  // fake range [100, 300]
    require_alpaca_error([&]() { driver->set_gain(301); }, AlpacaError::InvalidValue);

    driver->set_connected(false);
    CHECK(fake_cam.physical_opens == fake_cam.physical_closes);
}

TEST_CASE("Altair Camera Driver - State machine", "[altair][camera][unit]") {
    auto fake = make_fake_with_178m();
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, fake);

    // Disconnected: Idle, not guiding.
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    REQUIRE(driver->get_is_pulse_guiding() == false);

    // Connected, before any exposure: still Idle with no image.
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);
    CHECK(driver->get_is_pulse_guiding() == false);
    driver->set_connected(false);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
}

TEST_CASE("Altair Camera Driver - Unsupported members", "[altair][camera][unit]") {
    auto fake = make_fake_with_178m();
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, fake);

    // Never implemented, whatever the hardware.
    require_alpaca_error([&]() { driver->get_sub_exposure_duration(); }, AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_sub_exposure_duration(1.0); }, AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->get_offsets(); }, AlpacaError::PropertyNotImplemented);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    // Mono sensor: no Bayer offsets.
    require_alpaca_error([&]() { driver->get_bayer_offset_x(); }, AlpacaError::PropertyNotImplemented);
    // Uncooled: switching the cooler on is not implemented; off is a no-op.
    require_alpaca_error([&]() { driver->set_cooler_on(true); }, AlpacaError::NotImplemented);
    driver->set_cooler_on(false);
    CHECK(driver->get_cooler_on() == false);
    driver->set_connected(false);
}
