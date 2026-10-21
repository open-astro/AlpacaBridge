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

// iOptron iGuider guide camera: the required driver cases (disconnected, on the
// production seam, which opens nothing until connect) and the connected cases
// over FakeIGuiderV4L2.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_camera_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_iguider_v4l2.h"
#include "fake_task_clock.h"
#include "locked_iguider_v4l2.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::CameraState;
using alpacacore::test::FakeIGuiderV4L2;
using Control = alpacacore::vendor::ioptron::IGuiderControl;

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// The fake behind the locked decorator, and a driver over them. The camera is
// declared last, so it is destroyed (and its stream worker joined) first.
struct Rig {
    FakeIGuiderV4L2 fake;
    alpacacore::test::LockedIGuiderV4L2 v4l2{fake};
    std::unique_ptr<alpacacore::CameraDriver> camera;

    explicit Rig(const std::string& path = "") {
        camera = alpacacore::vendor::ioptron::create_iguider_camera(0, path, v4l2);
    }
    Rig(const std::string& path, alpacacore::util::TaskClock& clock) {
        camera = alpacacore::vendor::ioptron::create_iguider_camera(0, path, v4l2, clock);
    }
};

std::unique_ptr<alpacacore::CameraDriver> disconnected_camera(int device_number = 0) {
    // The production seam: construction opens nothing, so this is hardware-free.
    return alpacacore::vendor::ioptron::create_iguider_camera(device_number);
}

alpacacore::ImageArray expose(alpacacore::CameraDriver& camera, double seconds) {
    camera.start_exposure(seconds, true);
    REQUIRE(wait_until([&] { return camera.get_image_ready(); }));
    return camera.get_image_array();
}

}  // namespace

// ---- Required cases ----------------------------------------------------------

TEST_CASE("iOptron iGuider Camera Driver - Defaults", "[ioptron][iguider][camera][unit]") {
    auto driver = disconnected_camera(0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "iOptron iGuider");
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == false);
    CHECK(driver->get_can_pulse_guide() == false);
    CHECK(driver->get_can_set_ccd_temperature() == false);
    CHECK(driver->get_can_get_cooler_power() == false);
    CHECK(driver->get_can_fast_readout() == false);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_has_shutter() == false);
}

TEST_CASE("iOptron iGuider Camera Driver - Device metadata", "[ioptron][iguider][camera][unit]") {
    auto driver = disconnected_camera(3);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "iOptron iGuider guide camera");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron iGuider Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    CHECK(driver->get_unique_id() == "IOPTRON_IGUIDER_3");
    CHECK_FALSE(driver->get_device_firmware().has_value());
    CHECK_FALSE(driver->get_device_sdk_version().has_value());
}

TEST_CASE("iOptron iGuider Camera Driver - Not connected throws", "[ioptron][iguider][camera][unit]") {
    auto driver = disconnected_camera();
    auto& c = *driver;
    const int nc = AlpacaError::NotConnected;

    require_alpaca_error([&] { (void)c.get_gain(); }, nc);
    require_alpaca_error([&] { c.set_gain(1); }, nc);
    require_alpaca_error([&] { c.start_exposure(1.0, true); }, nc);
    require_alpaca_error([&] { (void)c.get_image_array(); }, nc);
    require_alpaca_error([&] { (void)c.get_image_ready(); }, nc);
    require_alpaca_error([&] { (void)c.get_camera_state(); }, nc);
    require_alpaca_error([&] { (void)c.get_percent_completed(); }, nc);
    require_alpaca_error([&] { c.abort_exposure(); }, nc);
    require_alpaca_error([&] { (void)c.get_num_x(); }, nc);
    require_alpaca_error([&] { (void)c.get_camera_x_size(); }, nc);
    require_alpaca_error([&] { (void)c.get_exposure_max(); }, nc);
    require_alpaca_error([&] { (void)c.get_last_exposure_duration(); }, nc);
    require_alpaca_error([&] { (void)c.get_is_pulse_guiding(); }, nc);
    // Unsupported members answer NotConnected first, as the contract sweep expects.
    require_alpaca_error([&] { (void)c.get_ccd_temperature(); }, nc);
    require_alpaca_error([&] { c.set_cooler_on(true); }, nc);
    CHECK(c.get_device_state().empty());
}

TEST_CASE("iOptron iGuider Camera Driver - Unsupported actions", "[ioptron][iguider][camera][unit]") {
    auto driver = disconnected_camera();

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&] { driver->action("test", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { driver->command_blind("test", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->command_bool("test", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->command_string("test", false); }, AlpacaError::MethodNotImplemented);
}

TEST_CASE("iOptron iGuider Camera Driver - Exposure steps", "[ioptron][iguider][camera][unit]") {
    using alpacacore::vendor::ioptron::iguider_exposure_step;
    using alpacacore::vendor::ioptron::kIGuiderDurations;
    using alpacacore::vendor::ioptron::kIGuiderTicks;

    // The USB-sniffed pairs (INDI PR #2012): the last tick is a 3.5 s exposure.
    CHECK(kIGuiderTicks[iguider_exposure_step(3.5)] == 2500);
    CHECK(kIGuiderTicks[iguider_exposure_step(1.0)] == 78);
    CHECK(kIGuiderTicks[iguider_exposure_step(0.01)] == 1);
    // Nearest step, as the vendor driver rounds.
    CHECK(kIGuiderDurations[iguider_exposure_step(0.0)] == 0.01);
    CHECK(kIGuiderDurations[iguider_exposure_step(1.2)] == 1.0);
    CHECK(kIGuiderDurations[iguider_exposure_step(1.3)] == 1.5);
    CHECK(kIGuiderDurations[iguider_exposure_step(0.3)] == 0.2);
    CHECK(kIGuiderDurations[iguider_exposure_step(3.4)] == 3.5);
}

TEST_CASE("iOptron iGuider Camera Driver - Value range validation", "[ioptron][iguider][camera][unit]") {
    auto driver = disconnected_camera();
    auto& c = *driver;
    const int iv = AlpacaError::InvalidValue;

    // InvalidValue precedes the connection check.
    require_alpaca_error([&] { c.start_exposure(-0.1, true); }, iv);
    require_alpaca_error([&] { c.start_exposure(std::numeric_limits<double>::quiet_NaN(), true); }, iv);
    require_alpaca_error([&] { c.start_exposure(3.51, true); }, iv);
    require_alpaca_error([&] { c.set_gain(-1); }, iv);
    require_alpaca_error([&] { c.set_gain(14); }, iv);
    require_alpaca_error([&] { c.set_bin_x(2); }, iv);
    require_alpaca_error([&] { c.set_bin_y(0); }, iv);
    require_alpaca_error([&] { c.set_readout_mode(1); }, iv);
    require_alpaca_error([&] { c.set_num_x(0); }, iv);
    require_alpaca_error([&] { c.set_start_y(-1); }, iv);

    // Connected: a window that leaves the sensor is refused at StartExposure.
    Rig rig;
    rig.camera->set_connected(true);
    rig.camera->set_start_x(1000);
    rig.camera->set_num_x(281);
    require_alpaca_error([&] { rig.camera->start_exposure(0.01, true); }, iv);
    rig.camera->set_num_x(280);
    rig.camera->set_start_y(900);
    rig.camera->set_num_y(61);
    require_alpaca_error([&] { rig.camera->start_exposure(0.01, true); }, iv);
    // StartX + NumX must not overflow past the check: INT_MAX + 1 wrapped
    // negative and passed, and the crop then read 2 GB past the frame.
    constexpr int kIntMax = std::numeric_limits<int>::max();
    rig.camera->set_start_y(0);
    rig.camera->set_num_y(960);
    rig.camera->set_start_x(kIntMax);
    rig.camera->set_num_x(1);
    require_alpaca_error([&] { rig.camera->start_exposure(0.01, true); }, iv);
    rig.camera->set_start_x(0);
    rig.camera->set_num_x(kIntMax);
    require_alpaca_error([&] { rig.camera->start_exposure(0.01, true); }, iv);
    rig.camera->set_num_x(1280);
    rig.camera->set_start_y(kIntMax);
    rig.camera->set_num_y(kIntMax);
    require_alpaca_error([&] { rig.camera->start_exposure(0.01, true); }, iv);
    CHECK(rig.camera->get_camera_state() == CameraState::Idle);
}

TEST_CASE("iOptron iGuider Camera Driver - State machine", "[ioptron][iguider][camera][unit]") {
    Rig rig;
    auto& c = *rig.camera;
    c.set_connected(true);

    CHECK(c.get_camera_state() == CameraState::Idle);
    CHECK(c.get_image_ready() == false);
    CHECK(c.get_is_pulse_guiding() == false);
    CHECK(c.get_percent_completed() == 0.0);
    require_alpaca_error([&] { (void)c.get_last_exposure_duration(); }, AlpacaError::ValueNotSet);
    try {
        (void)c.get_image_array();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::InvalidOperation);
        CHECK(e.is_asked_too_early());
    }

    // No frames: the exposure stays Exposing until it is aborted.
    rig.fake.stalled = true;
    c.start_exposure(0.01, true);
    CHECK(c.get_camera_state() == CameraState::Exposing);
    CHECK(c.get_image_ready() == false);
    c.abort_exposure();
    CHECK(c.get_camera_state() == CameraState::Idle);
    CHECK(c.get_image_ready() == false);

    rig.fake.stalled = false;
    expose(c, 0.01);
    CHECK(c.get_camera_state() == CameraState::Idle);
    CHECK(c.get_percent_completed() == 100.0);
    CHECK(c.get_last_exposure_duration() == 0.01);
}

TEST_CASE("iOptron iGuider Camera Driver - Unsupported methods", "[ioptron][iguider][camera][unit]") {
    auto disconnected = disconnected_camera();
    // Static capabilities: refused whatever the connection.
    require_alpaca_error([&] { disconnected->pulse_guide(0, 100); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { disconnected->stop_exposure(); }, AlpacaError::MethodNotImplemented);

    Rig rig;
    auto& c = *rig.camera;
    c.set_connected(true);
    const int pni = AlpacaError::PropertyNotImplemented;
    require_alpaca_error([&] { (void)c.get_ccd_temperature(); }, pni);
    require_alpaca_error([&] { (void)c.get_heat_sink_temperature(); }, pni);
    require_alpaca_error([&] { (void)c.get_cooler_on(); }, pni);
    require_alpaca_error([&] { c.set_cooler_on(true); }, pni);
    require_alpaca_error([&] { (void)c.get_cooler_power(); }, pni);
    require_alpaca_error([&] { c.set_set_ccd_temperature(-10.0); }, pni);
    require_alpaca_error([&] { (void)c.get_offset(); }, pni);
    require_alpaca_error([&] { (void)c.get_gains(); }, pni);
    require_alpaca_error([&] { (void)c.get_fast_readout(); }, pni);
    require_alpaca_error([&] { (void)c.get_sub_exposure_duration(); }, pni);
    require_alpaca_error([&] { c.set_sub_exposure_duration(1.0); }, pni);
    require_alpaca_error([&] { (void)c.get_bayer_offset_x(); }, pni);
    require_alpaca_error([&] { c.pulse_guide(1, 100); }, AlpacaError::MethodNotImplemented);
}

// ---- Connected behaviour over the fake ---------------------------------------

TEST_CASE("iOptron iGuider Camera Driver - Connect streams 1280x960 at the shortest exposure",
          "[ioptron][iguider][camera][unit]") {
    Rig rig;
    auto& c = *rig.camera;
    c.set_connected(true);

    REQUIRE(c.get_connected());
    CHECK(rig.fake.opens == 1);
    CHECK(rig.fake.streaming());
    CHECK(rig.fake.control(Control::AutoExposure) == 1);  // manual
    CHECK(rig.fake.control(Control::ExposureAbsolute) == 1);
    CHECK(rig.fake.control(Control::Gain) == 64);
    CHECK(c.get_camera_x_size() == 1280);
    CHECK(c.get_camera_y_size() == 960);
    CHECK(c.get_pixel_size_x() == 3.75);
    CHECK(c.get_max_adu() == 255);
    CHECK(c.get_sensor_type() == alpacacore::SensorType::Monochrome);
    CHECK(c.get_exposure_min() == 0.01);
    CHECK(c.get_exposure_max() == 3.5);
    CHECK(c.get_gain_min() == 0);
    CHECK(c.get_gain_max() == 13);

    // Exposures between connect and disconnect never stop the stream.
    expose(c, 0.01);
    expose(c, 0.05);
    CHECK(rig.fake.stream_starts == 1);
    CHECK(rig.fake.stream_stops == 0);

    c.set_connected(false);
    CHECK_FALSE(c.get_connected());
    CHECK(rig.fake.open_handles() == 0);
    CHECK_FALSE(rig.fake.streaming());
}

TEST_CASE("iOptron iGuider Camera Driver - Exposure rounds to a step and writes its tick",
          "[ioptron][iguider][camera][unit]") {
    Rig rig;
    auto& c = *rig.camera;
    c.set_connected(true);

    const auto image = expose(c, 1.2);
    CHECK(c.get_last_exposure_duration() == 1.0);
    CHECK(rig.fake.control(Control::ExposureAbsolute) == 78);
    CHECK(image.width == 1280);
    CHECK(image.height == 960);
    CHECK(image.rank == 2);
    REQUIRE(image.data.size() == 1280u * 960u);
    // The frame was taken at the new exposure, not one of the frames still in
    // flight at the old value when the write landed.
    CHECK(image.data[0] == FakeIGuiderV4L2::exposure_marker(78));
    CHECK(image.data[1] == FakeIGuiderV4L2::pixel_value(1, 0));
    CHECK(image.data[1280 * 2 + 5] == FakeIGuiderV4L2::pixel_value(5, 2));

    CHECK(expose(c, 3.5).data[0] == FakeIGuiderV4L2::exposure_marker(2500));
    CHECK(rig.fake.control(Control::ExposureAbsolute) == 2500);
    CHECK(c.get_last_exposure_duration() == 3.5);
    // And back down: the frames still at 3.5 s are not taken for the short one.
    CHECK(expose(c, 0.01).data[0] == FakeIGuiderV4L2::exposure_marker(1));
}

TEST_CASE("iOptron iGuider Camera Driver - Subframe crops the luma", "[ioptron][iguider][camera][unit]") {
    Rig rig;
    auto& c = *rig.camera;
    c.set_connected(true);
    c.set_start_x(100);
    c.set_start_y(50);
    c.set_num_x(64);
    c.set_num_y(32);

    const auto image = expose(c, 0.01);
    CHECK(image.width == 64);
    CHECK(image.height == 32);
    REQUIRE(image.data.size() == 64u * 32u);
    CHECK(image.data.front() == FakeIGuiderV4L2::pixel_value(100, 50));
    CHECK(image.data.back() == FakeIGuiderV4L2::pixel_value(163, 81));
}

TEST_CASE("iOptron iGuider Camera Driver - Gain maps onto the UVC gain", "[ioptron][iguider][camera][unit]") {
    Rig rig;
    auto& c = *rig.camera;
    c.set_connected(true);

    c.set_gain(13);
    CHECK(c.get_gain() == 13);
    CHECK(wait_until([&] { return rig.fake.control(Control::Gain) == 480; }));
    c.set_gain(3);
    CHECK(wait_until([&] { return rig.fake.control(Control::Gain) == 160; }));
    // A reconnect starts again at gain 0 (no persisted setting).
    c.set_connected(false);
    c.set_connected(true);
    CHECK(c.get_gain() == 0);
    CHECK(rig.fake.control(Control::Gain) == 64);
}

TEST_CASE("iOptron iGuider Camera Driver - A new exposure supersedes one in flight",
          "[ioptron][iguider][camera][unit]") {
    Rig rig;
    auto& c = *rig.camera;
    c.set_connected(true);

    rig.fake.stalled = true;
    c.start_exposure(2.0, true);
    c.start_exposure(0.01, true);
    rig.fake.stalled = false;
    REQUIRE(wait_until([&] { return c.get_image_ready(); }));
    CHECK(c.get_last_exposure_duration() == 0.01);
    CHECK(rig.fake.control(Control::ExposureAbsolute) == 1);
}

TEST_CASE("iOptron iGuider Camera Driver - A stalled stream latches a link fault and recovers",
          "[ioptron][iguider][camera][unit]") {
    alpacacore::test::FakeTaskClock clock;
    Rig rig("", clock);
    auto& c = *rig.camera;
    c.set_connected(true);
    REQUIRE(rig.fake.wait_for_frames(3, std::chrono::seconds(2)));

    rig.fake.stalled = true;
    c.start_exposure(0.01, true);
    clock.advance(std::chrono::seconds(11));
    REQUIRE(wait_until([&] { return !c.get_link_fault().empty(); }));
    CHECK(c.get_connected());  // a fault is not a disconnect (CONTEXT.md)
    CHECK(c.get_camera_state() == CameraState::Error);
    // The exposure has passed its deadline too: it fails instead of hanging.
    require_alpaca_error([&] { (void)c.get_image_ready(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.start_exposure(0.01, true); }, AlpacaError::DriverException);
    // The settings refuse while faulted too (decision 0009, rule 1).
    require_alpaca_error([&] { (void)c.get_gain(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.set_gain(1); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)c.get_num_x(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { c.set_start_x(0); }, AlpacaError::DriverException);
    // Static metadata keeps answering.
    CHECK(c.get_camera_x_size() == 1280);
    CHECK(wait_until([&] { return rig.fake.stream_starts >= 2; }));  // the stream was restarted

    rig.fake.stalled = false;
    REQUIRE(wait_until([&] { return c.get_link_fault().empty(); }));
    CHECK(c.get_camera_state() == CameraState::Idle);
    expose(c, 0.01);
}

TEST_CASE("iOptron iGuider Camera Driver - An unplugged camera is a lost link", "[ioptron][iguider][camera][unit]") {
    Rig rig;
    auto& c = *rig.camera;
    c.set_connected(true);

    rig.fake.unplugged = true;
    // ENODEV on the fd is a loss at once, not a fault (decision 0009, point 2a).
    REQUIRE(wait_until([&] { return !c.get_connected(); }));
    CHECK(c.get_link_fault().find("No such device") != std::string::npos);  // kept until Connect or Disconnect
    CHECK(rig.fake.open_handles() == 0);                                    // the transport is closed
    CHECK(c.get_device_state().empty());
    require_alpaca_error([&] { (void)c.get_camera_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)c.get_camera_x_size(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { c.start_exposure(0.01, true); }, AlpacaError::NotConnected);

    SECTION("a Connect after the replug relinks") {
        rig.fake.unplugged = false;
        c.set_connected(true);
        CHECK(c.get_connected());
        CHECK(c.get_link_fault().empty());
        CHECK(rig.fake.opens == 2);
        expose(c, 0.01);
    }
    SECTION("a Connect while still unplugged fails and stays down") {
        require_alpaca_error([&] { c.set_connected(true); }, AlpacaError::DriverException);
        CHECK_FALSE(c.get_connected());
        CHECK(rig.fake.open_handles() == 0);
    }
    SECTION("a Disconnect clears the fault text") {
        c.set_connected(false);
        CHECK_FALSE(c.get_connected());
        CHECK(c.get_link_fault().empty());
    }
}

TEST_CASE("iOptron iGuider Camera Driver - A fault standing past the staleness bound is a lost link",
          "[ioptron][iguider][camera][unit]") {
    alpacacore::test::FakeTaskClock clock;
    Rig rig("", clock);
    auto& c = *rig.camera;
    c.set_connected(true);
    REQUIRE(rig.fake.wait_for_frames(3, std::chrono::seconds(2)));

    rig.fake.stalled = true;
    clock.advance(std::chrono::seconds(11));
    REQUIRE(wait_until([&] { return !c.get_link_fault().empty(); }));
    CHECK(c.get_connected());  // younger than the bound: a fault, not a loss

    clock.advance(alpacacore::util::kLinkStalenessBound);
    REQUIRE(wait_until([&] { return !c.get_connected(); }));
    CHECK(c.get_link_fault().find("no frame") != std::string::npos);
    CHECK(rig.fake.open_handles() == 0);
    CHECK(c.get_device_state().empty());
}

TEST_CASE("iOptron iGuider Camera Driver - A failed connect closes the camera", "[ioptron][iguider][camera][unit]") {
    SECTION("no iGuider attached") {
        Rig rig;
        rig.fake.devices.clear();
        try {
            rig.camera->set_connected(true);
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == AlpacaError::DriverException);
            CHECK(std::string(e.what()).find("0130:1466") != std::string::npos);
        }
        CHECK(rig.fake.opens == 0);
    }
    SECTION("the stream does not start") {
        Rig rig;
        rig.fake.throw_from.insert("start_stream");
        require_alpaca_error([&] { rig.camera->set_connected(true); }, AlpacaError::DriverException);
        CHECK(rig.fake.opens == 1);
        CHECK(rig.fake.open_handles() == 0);
        CHECK_FALSE(rig.camera->get_connected());
        // A later connect is not left holding a stale open.
        rig.fake.throw_from.clear();
        rig.camera->set_connected(true);
        CHECK(rig.camera->get_connected());
    }
    SECTION("no first frame") {
        alpacacore::test::FakeTaskClock clock;  // frozen: the read count ends the wait at once
        Rig rig("", clock);
        rig.fake.stalled = true;
        require_alpaca_error([&] { rig.camera->set_connected(true); }, AlpacaError::DriverException);
        CHECK(rig.fake.open_handles() == 0);
        CHECK_FALSE(rig.camera->get_connected());
    }
    SECTION("an explicit path that is not there") {
        Rig rig("/dev/video9");
        require_alpaca_error([&] { rig.camera->set_connected(true); }, AlpacaError::DriverException);
        CHECK(rig.fake.open_handles() == 0);
    }
    SECTION("an explicit path that another UVC camera now holds") {
        // A replug can move another camera onto a pinned node (decision 0009
        // point 6): the card name is checked at open, not at the first frame.
        Rig rig("/dev/video0");
        rig.fake.devices[0].card = "Integrated Webcam: Integrated W";
        try {
            rig.camera->set_connected(true);
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == AlpacaError::DriverException);
            CHECK(std::string(e.what()).find("not an iOptron iGuider") != std::string::npos);
        }
        CHECK(rig.fake.open_handles() == 0);
        CHECK(rig.fake.stream_starts == 0);
    }
}

TEST_CASE("iOptron iGuider Camera Driver - Dropped frames do not cut the first-frame wait short",
          "[ioptron][iguider][camera][unit]") {
    Rig rig;
    // More instant drops than the 3 s budget holds 200 ms reads: the wait is
    // bounded by the clock, so the frame after them still connects.
    rig.fake.drop_frames = 40;
    rig.camera->set_connected(true);
    CHECK(rig.camera->get_connected());
    CHECK(rig.fake.drop_frames == 0);
}

TEST_CASE("iOptron iGuider Camera Driver - An explicit path skips the scan", "[ioptron][iguider][camera][unit]") {
    Rig rig("/dev/video0");
    rig.fake.throw_from.insert("enumerate");  // a scan would fail the connect
    rig.camera->set_connected(true);
    CHECK(rig.camera->get_connected());
}
