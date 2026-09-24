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

TEST_CASE("GPhoto Camera Driver - Defaults", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // No physical DSLR is ever attached to a CI runner, so libgphoto2's USB
    // autodetect always comes back empty and the driver serves this literal
    // fallback name (see get_name()/preload_camera_info_locked).
    CHECK(driver->get_name() == "DSLR / Mirrorless Camera (libgphoto2)");
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
    CHECK(driver->get_description() == "libgphoto2 DSLR/Mirrorless Camera Driver");
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

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_camera_x_size() == 0);
    CHECK(driver->get_camera_y_size() == 0);
    CHECK(driver->get_bin_x() == 1);
    CHECK(driver->get_bin_y() == 1);
    CHECK(driver->get_max_bin_x() == 1);
    CHECK(driver->get_max_bin_y() == 1);
    CHECK(driver->get_cooler_on() == false);
    CHECK(driver->get_cooler_power() == 0.0);
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
    require_alpaca_error([&]() { driver->start_exposure(-1.0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_bin_x(2); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_bin_y(0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_readout_mode(1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("GPhoto Camera Driver - Unsupported method error codes", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    // Properties this vendor never supports (no offset register, no cooler,
    // no fast readout) throw NotImplemented regardless of connection state --
    // distinct from the NotConnected-gated properties above.
    require_alpaca_error([&]() { driver->get_offset(); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_offset(0); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->get_offset_max(); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->get_offset_min(); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->get_offsets(); }, alpacacore::AlpacaError::PropertyNotImplemented);

    // Review PR #485: ASCOM's Gain interface has three mutually exclusive
    // modes (Value/Index/Not Implemented) -- this driver is "Gain Index"
    // (Gain/Gains work, see the connected test below), so GainMin/GainMax
    // must throw PropertyNotImplemented unconditionally, same as get_offsets
    // above. Un-tested, this fix regresses silently: only a hardware
    // ConformU run would catch GainMin/GainMax coming back readable again.
    require_alpaca_error([&]() { driver->get_gain_max(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&]() { driver->get_gain_min(); }, alpacacore::AlpacaError::PropertyNotImplemented);

    require_alpaca_error([&]() { driver->get_set_ccd_temperature(); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_set_ccd_temperature(0.0); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->get_fast_readout(); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_fast_readout(true); }, alpacacore::AlpacaError::NotImplemented);

    // Sensor geometry (Bayer phase) is unknown until a real exposure has been
    // decoded, independent of connection state -- InvalidOperation, not
    // NotConnected/NotImplemented (see gphoto_camera_driver.cpp class comment).
    require_alpaca_error([&]() { driver->get_bayer_offset_x(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&]() { driver->get_bayer_offset_y(); }, alpacacore::AlpacaError::InvalidOperation);
}

TEST_CASE("GPhoto Camera Driver - State machine contracts", "[gphoto][camera][unit]") {
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0);

    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    REQUIRE(driver->get_image_ready() == false);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
    // Percent completed is 0 (not ready) rather than throwing while idle and
    // disconnected -- matches the sibling camera drivers' convention.
    CHECK(driver->get_percent_completed() == 0.0);
}
