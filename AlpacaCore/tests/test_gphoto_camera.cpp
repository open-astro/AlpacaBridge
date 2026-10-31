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
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <limits>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_gphoto_sdk.h"
#include "fake_raw_decoder.h"

namespace {

using alpacacore::test::require_alpaca_error;

}  // namespace

TEST_CASE("GPhoto Camera Driver - Defaults", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // No physical DSLR is ever attached to a CI runner, so libgphoto2's USB
    // autodetect always comes back empty and the driver serves this literal
    // fallback name (see get_name()/preload_camera_info_locked).
    CHECK(driver->get_name() == "DSLR / Mirrorless Camera");
    CHECK(driver->get_sensor_name().empty());  // unknown: "" (ICameraV4), never a placeholder
    CHECK(driver->get_has_shutter() == true);  // DSLRs have a real mechanical shutter
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_can_pulse_guide() == false);
    CHECK(driver->get_can_set_ccd_temperature() == false);
}

TEST_CASE("GPhoto Camera Driver - Device metadata", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "DSLR / Mirrorless Camera Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore GPhoto Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    CHECK(driver->get_unique_id() == "GPHOTO_3");
    REQUIRE(driver->get_device_sdk_version().has_value());
    CHECK(driver->get_device_sdk_version()->find("libgphoto2") != std::string::npos);
}

TEST_CASE("GPhoto Camera Driver - Not connected throws", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    CHECK_THROWS_AS(driver->get_ccd_temperature(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_gain(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_gain(0), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_gains(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->start_exposure(1.0, true), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->stop_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->abort_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->pulse_guide(0, 100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_array(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_exposure_max(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_exposure_min(), alpacacore::AlpacaException);
}

TEST_CASE("GPhoto Camera Driver - Disconnected state", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_camera_x_size() == 0);
    CHECK(driver->get_camera_y_size() == 0);
    CHECK(driver->get_bin_x() == 1);
    CHECK(driver->get_bin_y() == 1);
    CHECK(driver->get_max_bin_x() == 1);
    CHECK(driver->get_max_bin_y() == 1);
    require_alpaca_error([&]() { driver->get_cooler_on(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_cooler_power(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_readout_modes() == std::vector<std::string>{"Normal"});
}

TEST_CASE("GPhoto Camera Driver - Unsupported actions", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("anything", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("GPhoto Camera Driver - Sub-exposure and binning not supported", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_bin_x(2), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_bin_y(2), alpacacore::AlpacaException);
}

TEST_CASE("GPhoto Camera Driver - Value range validation and ASCOM error codes", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    require_alpaca_error([&]() { driver->get_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_gain(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->start_exposure(1.0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_array(); }, alpacacore::AlpacaError::NotConnected);
    // Invalid arguments take precedence over disconnection; valid requests above still require a connection.
    for (const double duration : {-1.0, 3601.0, std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        require_alpaca_error([&]() { driver->start_exposure(duration, true); }, alpacacore::AlpacaError::InvalidValue);
    }
    for (const int bin : {0, 2}) {
        require_alpaca_error([&]() { driver->set_bin_x(bin); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->set_bin_y(bin); }, alpacacore::AlpacaError::InvalidValue);
    }
    for (const int mode : {-1, 1}) {
        require_alpaca_error([&]() { driver->set_readout_mode(mode); }, alpacacore::AlpacaError::InvalidValue);
    }
}

TEST_CASE("GPhoto Camera Driver - Unsupported method error codes", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    // Properties this vendor never supports (no offset register, no fast readout, no sub-exposure, no gain range)
    // throw NotConnected while disconnected: the connection check comes before the capability answer (AGENTS.md
    // "ASCOM contract precedence"). The connected NotImplemented is pinned in test_gphoto_fake_sdk.cpp.
    namespace AlpacaError = alpacacore::AlpacaError;
    require_alpaca_error([&]() { driver->get_offset(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_offset(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset_max(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offset_min(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_offsets(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_gain_max(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_gain_min(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_sub_exposure_duration(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_sub_exposure_duration(0.0); }, AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->get_set_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_set_ccd_temperature(0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_fast_readout(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_fast_readout(true); }, alpacacore::AlpacaError::NotConnected);

    // Sensor geometry (Bayer phase) is unknown until a real exposure has been
    // decoded, independent of connection state -- InvalidOperation, not
    // NotConnected/NotImplemented (see gphoto_camera_driver.cpp class comment).
    require_alpaca_error([&]() { driver->get_bayer_offset_x(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&]() { driver->get_bayer_offset_y(); }, alpacacore::AlpacaError::InvalidOperation);
}

TEST_CASE("GPhoto Camera Driver - State machine contracts", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
    require_alpaca_error([&]() { (void)driver->get_percent_completed(); }, alpacacore::AlpacaError::NotConnected);
}

// ── Connected members over the SDK fake ─────────────────────────────────────

namespace {

using alpacacore::test::FakeGPhotoSDK;
using alpacacore::test::FakeRawDecoder;

// A camera that connects and primes: an ISO widget and a shutter-speed list. Pass a unique_test_model() name:
// a plain model name would leave a line in the shared sensor cache.
FakeGPhotoSDK::FakeCamera make_connectable_camera(const std::string& model) {
    FakeGPhotoSDK::FakeCamera cam;
    cam.model = model;
    cam.port = "usb:001,004";
    cam.choices["iso"] = {"100", "200", "400", "800"};
    cam.choice_value["iso"] = "200";
    cam.choices["shutterspeed2"] = {"1/4000", "1/200", "1", "30", "bulb"};
    cam.choice_value["shutterspeed2"] = "1/200";
    cam.toggle_value["bulb"] = false;
    return cam;
}

// Polls ImageReady for up to 5 s; a fake exposure of 0.01 s finishes in a few polls.
bool wait_until_image_ready(const alpacacore::CameraDriver& driver) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (driver.get_image_ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

TEST_CASE("GPhoto Camera Driver - Connected capability members report the decoded frame", "[gphoto][camera][unit]") {
    // Falsified by: get_max_adu returning 65535 instead of the decoded frame's maximum, get_sensor_type
    // returning Monochrome, get_heat_sink_temperature not following the decoded sensor temperature,
    // get_can_fast_readout returning true, get_exposure_resolution returning 1e-6, get_image_array_variant
    // returning "Int16", or either pixel size returning a made-up non-zero value for an unlisted model.
    alpacacore::test::reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_connectable_camera(alpacacore::test::unique_test_model("Nikon DSC D5300")));
    FakeRawDecoder decoder;
    decoder.canned_frame.max_adu = 16383;
    decoder.canned_frame.sensor_temperature = 31.5;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    driver->start_exposure(0.01, true);
    REQUIRE(wait_until_image_ready(*driver));

    CHECK(driver->get_max_adu() == 16383);
    CHECK(driver->get_sensor_type() == alpacacore::SensorType::RGGB);
    CHECK(driver->get_ccd_temperature() == 31.5);
    CHECK(driver->get_heat_sink_temperature() == 31.5);
    CHECK_FALSE(driver->get_can_fast_readout());
    CHECK(driver->get_exposure_resolution() == 0.001);
    CHECK(driver->get_image_array_variant() == "Int32");
    CHECK(driver->get_readout_mode() == 0);
    // The fake's model is not in the pixel-size table (unique_test_model() suffixes it, and a listed model
    // would write a shared sensor-cache line), so both axes report ASCOM's "unknown".
    CHECK(driver->get_pixel_size_x() == 0.0);
    CHECK(driver->get_pixel_size_y() == 0.0);

    driver->set_connected(false);
}

TEST_CASE("GPhoto Camera Driver - Heat sink temperature is PropertyNotImplemented without a sensor reading",
          "[gphoto][camera][unit]") {
    // Falsified by: get_heat_sink_temperature returning 0.0 when the frame carries no sensor temperature.
    alpacacore::test::reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_connectable_camera(alpacacore::test::unique_test_model("Nikon DSC D5300")));
    FakeRawDecoder decoder;  // canned frame has no sensor temperature
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    require_alpaca_error([&] { (void)driver->get_heat_sink_temperature(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);

    driver->set_connected(false);
}

TEST_CASE("GPhoto Camera Driver - Connected ROI round-trips and rejects negative values", "[gphoto][camera][unit]") {
    // Falsified by: any of the four setters dropping the value, accepting a negative one, or a rejected write
    // changing the stored value; the 8x6 frame sets the default NumX/NumY.
    alpacacore::test::reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_connectable_camera(alpacacore::test::unique_test_model("Nikon DSC D5300")));
    FakeRawDecoder decoder;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    CHECK(driver->get_num_x() == 8);
    CHECK(driver->get_num_y() == 6);
    CHECK(driver->get_start_x() == 0);
    CHECK(driver->get_start_y() == 0);

    driver->set_num_x(4);
    driver->set_num_y(3);
    driver->set_start_x(2);
    driver->set_start_y(1);
    CHECK(driver->get_num_x() == 4);
    CHECK(driver->get_num_y() == 3);
    CHECK(driver->get_start_x() == 2);
    CHECK(driver->get_start_y() == 1);

    const int invalid = alpacacore::AlpacaError::InvalidValue;
    require_alpaca_error([&] { driver->set_num_x(-1); }, invalid);
    require_alpaca_error([&] { driver->set_num_y(-1); }, invalid);
    require_alpaca_error([&] { driver->set_start_x(-1); }, invalid);
    require_alpaca_error([&] { driver->set_start_y(-1); }, invalid);
    CHECK(driver->get_num_x() == 4);
    CHECK(driver->get_num_y() == 3);
    CHECK(driver->get_start_x() == 2);
    CHECK(driver->get_start_y() == 1);

    driver->set_connected(false);
}

TEST_CASE("GPhoto Camera Driver - ROI and cooler setters throw NotConnected while disconnected",
          "[gphoto][camera][unit]") {
    // Falsified by: dropping ensure_connected() from set_roi_dimension_locked or set_cooler_on.
    FakeGPhotoSDK fake;
    FakeRawDecoder decoder;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);

    const int not_connected = alpacacore::AlpacaError::NotConnected;
    require_alpaca_error([&] { driver->set_num_x(1); }, not_connected);
    require_alpaca_error([&] { driver->set_start_y(1); }, not_connected);
    require_alpaca_error([&] { driver->set_cooler_on(false); }, not_connected);
}

TEST_CASE("GPhoto Camera Driver - Connected set_cooler_on answers PropertyNotImplemented both ways",
          "[gphoto][camera][unit]") {
    // Falsified by: set_cooler_on(false) returning quietly, or either call throwing another code.
    alpacacore::test::reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_connectable_camera(alpacacore::test::unique_test_model("Nikon DSC D5300")));
    FakeRawDecoder decoder;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    require_alpaca_error([&] { driver->set_cooler_on(true); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_cooler_on(false); }, alpacacore::AlpacaError::PropertyNotImplemented);

    driver->set_connected(false);
}
