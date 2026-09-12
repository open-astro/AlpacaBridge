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
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <string>

#include "catch2_compat.h"
#include "fake_qhy_sdk.h"
#include "locked_qhy_sdk.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

using alpacacore::test::FakeQHYSDK;
using alpacacore::test::LockedQHYSDK;

// Connect coverage runs over the SDK seam (issue #321). The real QHY SDK
// cannot initialise on a test runner at all -- its libusb hotplug init
// segfaults on a USB-less host -- so before the seam existed nothing in this
// file ever reached set_connected(true).
// One-camera fake, shared with the other QHY seam test files (issue #342):
// this was three verbatim copies, so a change to what a default test fake
// looks like had to be made in three places with nothing failing if it was
// made in two.
FakeQHYSDK make_fake(const std::string& id = "fake-qhy-0") { return FakeQHYSDK::with_one_camera(id); }

} // namespace

TEST_CASE("QHY Camera Driver - Defaults", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Name is "QHY Camera" when no camera is plugged in, or the SDK name when detected
    CHECK(driver->get_name().find("QHY") != std::string::npos);
}

TEST_CASE("QHY Camera Driver - Device metadata", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "QHY CCD Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    CHECK(driver->get_unique_id() == "QHY_3");
}

TEST_CASE("QHY Camera Driver - Not connected throws", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    // QHY get_ccd_temperature returns 0.0 when disconnected (no camera info) rather than throwing
    CHECK(driver->get_ccd_temperature() == 0.0);
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

TEST_CASE("QHY Camera Driver - Disconnected state", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_has_shutter() == false);
}

TEST_CASE("QHY Camera Driver - Unsupported actions", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("anything", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("QHY Camera Driver - Sub-exposure not supported", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
}

TEST_CASE("QHY Camera Driver - ASCOM Error Codes", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    // QHY get_ccd_temperature returns 0.0 when disconnected rather than throwing
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

TEST_CASE("QHY Camera Driver - State Machine Contracts", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
}

// ── Connect path (over the SDK seam, issue #321) ────────────────────────────

TEST_CASE("QHY Camera Driver - Connects over the SDK seam", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // The connect sequence the wrapper documents: open, init, then chip info
    // and the readout-mode enumeration.
    CHECK(fake.physical_opens == 1);
    CHECK(fake.init_calls == 1);
    CHECK(fake.call_count("get_chip_info") == 1);
    CHECK(fake.call_count("get_num_readout_modes") == 1);
    CHECK(driver->get_camera_x_size() == 64);
    CHECK(driver->get_camera_y_size() == 48);
    CHECK(driver->get_readout_modes().size() == 2);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_closes == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - A failed connect rolls back the ref-counted open", "[qhy][camera][unit]") {
    // An unmatched open pins the handle for the life of the process: the CFW
    // driver shares the same open_count, so CloseQHYCCD would never fire.
    auto fake = make_fake();
    fake.throw_from.insert("set_bits_mode");
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 1);
    CHECK(fake.physical_closes == 1);  // rolled back
    CHECK(fake.ref_count("fake-qhy-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - Reconnect reuses the driver cleanly", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    driver->set_connected(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.physical_opens == 2);
    CHECK(fake.physical_closes == 1);

    driver->set_connected(false);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - Connecting an unknown camera id fails and leaks nothing", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "not-a-camera", sdk);

    // DriverException, not NotConnected: the real OpenQHYCCD returning null
    // for an unrecognized id has no "not connected" concept, only "the open
    // failed" (matches QHYSDKWrapper::open_camera()).
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - An uninitialised SDK resource fails the connect", "[qhy][camera][unit]") {
    // The failure mode issue #321 is about: on real hardware this path is only
    // reachable by crashing the process inside libqhyccd.
    auto fake = make_fake();
    fake.sdk_resource_available = false;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("QHY Camera Driver - Gain and offset round-trip while connected", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    driver->set_connected(true);

    driver->set_gain(42);
    CHECK(driver->get_gain() == 42);
    driver->set_offset(17);
    CHECK(driver->get_offset() == 17);
    CHECK(driver->get_gain_min() == 0);
    CHECK(driver->get_gain_max() == 100);

    require_alpaca_error([&]() { driver->set_gain(1000); }, alpacacore::AlpacaError::InvalidValue);

    driver->set_connected(false);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - Connecting by index with no cameras detected fails", "[qhy][camera][unit]") {
    // The empty-enumeration branch of resolve_camera_id_locked() -- unreachable
    // by the by-id factory, only exercisable through _by_index.
    FakeQHYSDK fake;  // no cameras added
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0, sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("QHY Camera Driver - Connecting by an out-of-range index fails and leaks nothing", "[qhy][camera][unit]") {
    auto fake = make_fake();  // exactly one camera, index 0
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 5, sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Camera Driver - Connecting by index resolves the id and connects", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0, sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.physical_opens == 1);
    CHECK(fake.last_opened_id == "fake-qhy-0");

    driver->set_connected(false);
    CHECK(fake.physical_closes == 1);
}

TEST_CASE("QHY Camera Driver - an empty SDK version omits the DriverInfo suffix", "[qhy][camera][unit]") {
    // The real wrapper returns "" until the SDK resource comes up, which is
    // the branch get_driver_info()/get_device_sdk_version() special-case so
    // DriverInfo never renders a malformed "(SDK )". Reachable only because
    // the fake's version string is settable.
    auto fake = make_fake();
    fake.sdk_version = "";
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    CHECK(driver->get_driver_info() == "AlpacaCore QHY Camera Driver");
    CHECK_FALSE(driver->get_device_sdk_version().has_value());
}

TEST_CASE("QHY Camera Driver - a populated SDK version is surfaced in both places", "[qhy][camera][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    CHECK(driver->get_driver_info() == "AlpacaCore QHY Camera Driver (SDK fake-qhy-1.0)");
    REQUIRE(driver->get_device_sdk_version().has_value());
    CHECK(*driver->get_device_sdk_version() == "fake-qhy-1.0");
}
