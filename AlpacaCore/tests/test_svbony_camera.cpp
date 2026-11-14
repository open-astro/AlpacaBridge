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
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/svbony/svbony_camera_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "camera_percent_cooler_checks.h"
#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_svbony_sdk.h"
#include "fake_task_clock.h"
#include "getter_stall_probe.h"
#include "locked_svbony_sdk.h"

namespace {

using alpacacore::test::require_alpaca_error;

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

    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::NotConnected);
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
    require_alpaca_error([&] { (void)driver->action("anything", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)driver->command_blind("", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_bool("", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_string("", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("SVBONY Camera Driver - Sub-exposure not supported", "[svbony][camera][unit]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    require_alpaca_error([&] { (void)driver->get_sub_exposure_duration(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_sub_exposure_duration(1.0); }, alpacacore::AlpacaError::NotConnected);
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
    require_alpaca_error([&]() { driver->stop_exposure(); }, alpacacore::AlpacaError::NotConnected);
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
    // Connected, StopExposure answers MethodNotImplemented (disconnected: NotConnected).
    require_alpaca_error([&] { driver->stop_exposure(); }, alpacacore::AlpacaError::MethodNotImplemented);
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

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.call_count("open_camera") == 1);
    CHECK(fake.call_count("close_camera") == 1);
    CHECK(fake.open_count() == 0);
}

// Decision 0016 rule 3: identity and capabilities are published at the end of connect, so a connect that fails
// after the open publishes none of what it read. The fake's FrameSpeedMode control is what CanFastReadout reports.
// Falsified by: svbony_camera_driver.cpp set_connected(true) loading control_caps_ before the post-open
// get_serial_number (CanFastReadout then reads true after the failed connect).
TEST_CASE("SVBONY Camera Driver - a failed connect publishes no capabilities and allows a reconnect",
          "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);
    const bool fast_readout_before = driver->get_can_fast_readout();
    const int x_size_before = driver->get_camera_x_size();
    const std::string name_before = driver->get_name();

    fake.fail_next("get_serial_number");
    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_can_fast_readout() == fast_readout_before);
    CHECK(driver->get_camera_x_size() == x_size_before);
    CHECK(driver->get_name() == name_before);
    CHECK(fake.open_count() == 0);

    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_can_fast_readout());
    CHECK(fake.open_count() == 1);
    driver->set_connected(false);
    CHECK(fake.open_count() == 0);
}

// Decision 0016 rules 1 and 4: Name and the other identity getters need no device access, so a connect that holds
// the SDK open must not park them, and the disconnected name refresh must not reach the SDK behind the open (the
// real wrapper serialises enumerate_cameras and open_camera on one mutex). The open is held on a latch; the getters
// run on the test thread.
// Falsified by: svbony_camera_driver.cpp set_connected(true) holding mutex_ across open_camera() (get_name() then
// waits until the latch is released and the timed call fails its budget), or refresh_cached_camera_info_if_needed()
// without its transition-in-flight early return (enumerate_cameras then runs behind the held open).
TEST_CASE("SVBONY Camera Driver - Name and identity getters answer while a connect holds the SDK open",
          "[svbony][camera][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeSVBSDK sdk;
    std::atomic<bool> open_entered{false};
    std::atomic<bool> release_open{false};
    std::atomic<int> enumerations_during_open{0};
    sdk.before_call = [&](const std::string& call) {
        if (call == "enumerate_cameras" && open_entered.load() && !release_open.load()) {
            ++enumerations_during_open;
        }
        if (call != "open_camera") return;
        open_entered = true;
        hold_until(release_open);
    };
    auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);

    std::thread connector([&] { camera->set_connected(true); });
    ReleaseAndJoin release{release_open, connector};
    REQUIRE(wait_for_flag(open_entered));

    CHECK(timed([&] { (void)camera->get_name(); }) < kGetterBudget);
    CHECK(timed([&] { (void)camera->get_sensor_name(); }) < kGetterBudget);
    CHECK(timed([&] { (void)camera->get_camera_x_size(); }) < kGetterBudget);
    CHECK(timed([&] { (void)camera->get_can_pulse_guide(); }) < kGetterBudget);
    CHECK(timed([&] { (void)camera->get_can_get_cooler_power(); }) < kGetterBudget);
    CHECK(timed([&] { CHECK_FALSE(camera->get_connected()); }) < kGetterBudget);
    CHECK(enumerations_during_open.load() == 0);
    CHECK_FALSE(release_open.load());  // the open is still held, so the timings above are not vacuous

    release_open = true;
    connector.join();
    CHECK(camera->get_connected());
    CHECK(camera->get_name() == "SVBONY Fake SVBONY Camera");
    camera->set_connected(false);
}

// Decision 0016 rule 6: a name refresh that read the SDK while a connect ran must not overwrite what the connect
// published. The refresh is held inside enumerate_cameras on a latch, a connect completes, the SDK then reports
// another name, and the refresh is released.
// Falsified by: svbony_camera_driver.cpp refresh_cached_camera_info_if_needed() writing camera_info_ without the
// connected_/transition_in_flight_ re-check under mutex_ (get_name() then returns the stale refresh name).
TEST_CASE("SVBONY Camera Driver - A name refresh that overlapped a connect does not overwrite the connect's info",
          "[svbony][camera][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeSVBSDK sdk;
    static thread_local bool is_refresher = false;
    std::atomic<bool> refresh_entered{false};
    std::atomic<bool> release_refresh{false};
    sdk.before_call = [&](const std::string& call) {
        if (call != "enumerate_cameras" || !is_refresher) return;
        refresh_entered = true;
        hold_until(release_refresh);
    };
    auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);

    std::thread refresher([&] {
        is_refresher = true;
        (void)camera->get_name();
    });
    ReleaseAndJoin release{release_refresh, refresher};
    REQUIRE(wait_for_flag(refresh_entered));

    camera->set_connected(true);
    sdk.set_camera_name("Stale Refresh Name");
    release_refresh = true;
    refresher.join();

    CHECK(camera->get_name() == "SVBONY Fake SVBONY Camera");
    CHECK(camera->get_sensor_name() == "Fake SVBONY Camera");
    camera->set_connected(false);
}

// Driver lifecycle (camera exception) and decision 0016 item 4: a camera's disconnect clears state under mutex_ and
// runs the SDK close with it released, so identity getters answer while the close is held and operational members
// fail NotConnected at once instead of waiting behind it.
// Falsified by: svbony_camera_driver.cpp set_connected(false) holding mutex_ across close_camera() (get_name() then
// waits until the latch is released and the timed call fails its budget).
TEST_CASE("SVBONY Camera Driver - Name and identity getters answer while a disconnect holds the SDK close",
          "[svbony][camera][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeSVBSDK sdk;
    std::atomic<bool> close_entered{false};
    std::atomic<bool> release_close{false};
    std::atomic<int> enumerations_during_close{0};
    auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    camera->set_connected(true);
    sdk.before_call = [&](const std::string& call) {
        if (call == "enumerate_cameras" && close_entered.load() && !release_close.load()) {
            ++enumerations_during_close;
        }
        if (call != "close_camera") return;
        close_entered = true;
        hold_until(release_close);
    };

    std::thread disconnector([&] { camera->set_connected(false); });
    ReleaseAndJoin release{release_close, disconnector};
    REQUIRE(wait_for_flag(close_entered));

    CHECK(timed([&] { (void)camera->get_name(); }) < kGetterBudget);
    CHECK(timed([&] { (void)camera->get_sensor_name(); }) < kGetterBudget);
    CHECK(timed([&] { (void)camera->get_camera_x_size(); }) < kGetterBudget);
    CHECK(timed([&] { CHECK_FALSE(camera->get_connected()); }) < kGetterBudget);
    CHECK(timed([&] {
              require_alpaca_error([&] { (void)camera->get_gain(); }, alpacacore::AlpacaError::NotConnected);
          }) < kGetterBudget);
    CHECK(enumerations_during_close.load() == 0);
    CHECK_FALSE(release_close.load());  // the close is still held, so the timings above are not vacuous

    release_close = true;
    disconnector.join();
    CHECK(sdk.open_count() == 0);
    CHECK(sdk.call_count("close_camera") == 1);
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

namespace {
// Polls until ImageReady is true or raises; returns true when it raised (the exposure failed).
bool image_ready_raises_eventually(alpacacore::CameraDriver& driver) {
    for (int i = 0; i < 400; ++i) {
        try {
            if (driver.get_image_ready()) return false;
        } catch (const alpacacore::AlpacaException&) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
}  // namespace

// ICameraV4: LastExposure* raise InvalidOperation until an image has been successfully acquired (issue #1477).
TEST_CASE("SVBONY Camera Driver - LastExposure values follow successful images only",
          "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    alpacacore::test::LockedSVBSDK sdk(fake);
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    driver->set_connected(true);

    SECTION("before any exposure") { alpacacore::test::require_last_exposure_unset(*driver); }
    SECTION("a failed first exposure publishes nothing") {
        fake.fail_next("get_video_data");
        driver->start_exposure(0.05, true);
        REQUIRE(image_ready_raises_eventually(*driver));
        alpacacore::test::require_last_exposure_unset(*driver);
    }
    SECTION("a later failure keeps the earlier success") {
        driver->start_exposure(0.05, true);
        for (int i = 0; i < 400 && !driver->get_image_ready(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(driver->get_image_ready());
        const auto start = driver->get_last_exposure_start_time();
        CHECK(driver->get_last_exposure_duration() == 0.05);
        fake.fail_next("get_video_data");
        driver->start_exposure(0.07, true);
        REQUIRE(image_ready_raises_eventually(*driver));
        CHECK(driver->get_last_exposure_duration() == 0.05);
        CHECK(driver->get_last_exposure_start_time() == start);
    }
    driver->set_connected(false);
}

// ICameraV4: the start time is when the camera started to collect photons. The deferred FrameSpeedMode write runs
// in the worker before capture starts, so it must precede the stamp (issue #1477).
TEST_CASE("SVBONY Camera Driver - LastExposureStartTime is stamped after the deferred FrameSpeedMode write",
          "[svbony][camera][unit][fake-sdk]") {
    alpacacore::test::FakeSVBSDK fake;
    alpacacore::test::LockedSVBSDK sdk(fake);
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_can_fast_readout());
    driver->set_fast_readout(true);
    fake.set_frame_speed_write_delay(std::chrono::milliseconds(150));
    const auto writes_before = fake.call_count("set_control_value.FrameSpeedMode");

    driver->start_exposure(0.01, true);
    for (int i = 0; i < 400 && !driver->get_image_ready(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(driver->get_image_ready());
    REQUIRE(fake.call_count("set_control_value.FrameSpeedMode") == writes_before + 1);
    CHECK(driver->get_last_exposure_start_time() >= fake.frame_speed_written_at());
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
    // Decision 0020 rule 3: the failed exposure reads Idle; ImageReady raises the stop's message.
    SECTION("idle after an unconfirmed capture stop") {
        sdk.block_video_data();
        camera->start_exposure(0.001, true);
        REQUIRE(sdk.wait_for_video_data(2s));
        sdk.set_persistent_failure("stop_video_capture");
        require_alpaca_error([&] { camera->abort_exposure(); }, alpacacore::AlpacaError::DriverException);
        const int reads = sdk.completed_video_data_reads();
        sdk.release_video_data();
        REQUIRE(sdk.wait_for_completed_video_data(reads + 1, 2s));
        REQUIRE(camera->get_camera_state() == alpacacore::CameraState::Idle);
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

// --- Decision 0020: the exposure runs in a util::CaptureSession (issue #1980) ---------------------------------

namespace {

using namespace std::chrono_literals;

// Real time: each waits for a driver thread (the exposure body or the connection task), bounded at 2 s.
template <typename Pred>
bool eventually(Pred pred, std::chrono::milliseconds bound = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void wait_ready(alpacacore::CameraDriver& camera) {
    REQUIRE(eventually([&] { return camera.get_image_ready(); }));
}

// Polls ImageReady until it raises and returns the exception's code and text; {0, ""} when it never does.
std::pair<int, std::string> wait_for_raise(const alpacacore::CameraDriver& camera) {
    std::pair<int, std::string> raised{0, ""};
    static_cast<void>(eventually([&] {
        try {
            static_cast<void>(camera.get_image_ready());
            return false;
        } catch (const alpacacore::AlpacaException& ex) {
            raised = {ex.error_code(), ex.what()};
            return true;
        }
    }));
    return raised;
}

std::pair<int, std::string> error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& ex) {
        return {ex.error_code(), ex.what()};
    }
    return {0, ""};
}

std::unique_ptr<alpacacore::CameraDriver> svbony_camera_on_clock(alpacacore::test::FakeSVBSDK& sdk,
                                                                 alpacacore::test::FakeTaskClock& clock) {
    auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk, clock);
    camera->set_connected(true);
    REQUIRE(camera->get_connected());
    return camera;
}

// Declared after the camera, so it runs first: a held read is released before the driver joins its body.
struct ReleaseReadOnExit {
    alpacacore::test::FakeSVBSDK& sdk;
    ~ReleaseReadOnExit() {
        sdk.set_persistent_failure("stop_video_capture", false);
        sdk.release_video_data();
    }
};

}  // namespace

// Decision 0020 rule 3: the SDK text is kept on ImageReady and ImageArray, CameraState reads Idle, an idle
// AbortExposure clears nothing, and the next StartExposure clears it.
TEST_CASE("SVBONY Camera Driver - a failed frame read keeps its message until the next StartExposure",
          "[svbony][camera][unit][fake-sdk][capture-session]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    sdk.fail_next("get_video_data");
    camera->start_exposure(0.001, true);

    const auto [code, text] = wait_for_raise(*camera);
    CHECK(code == alpacacore::AlpacaError::DriverException);
    CHECK(text.find("scripted one-shot SVBONY failure: get_video_data") != std::string::npos);
    const auto array_error = error_of([&] { static_cast<void>(camera->get_image_array()); });
    CHECK(array_error.first == alpacacore::AlpacaError::DriverException);
    CHECK(array_error.second == text);
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);

    camera->abort_exposure();  // nothing is exposing: a no-op
    CHECK(error_of([&] { static_cast<void>(camera->get_image_ready()); }).second == text);

    camera->start_exposure(0.001, true);
    wait_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

// Decision 0020 rules 3 and 8: a reconnect clears the kept failure and LastExposure*.
TEST_CASE("SVBONY Camera Driver - reconnect clears a kept exposure failure and the last exposure",
          "[svbony][camera][unit][fake-sdk][capture-session]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    camera->start_exposure(0.001, true);
    wait_ready(*camera);
    REQUIRE(camera->get_last_exposure_duration() == 0.001);
    sdk.fail_next("get_video_data");
    camera->start_exposure(0.001, true);
    REQUIRE(wait_for_raise(*camera).first == alpacacore::AlpacaError::DriverException);

    camera->set_connected(false);
    camera->set_connected(true);
    alpacacore::test::require_last_exposure_unset(*camera);
    CHECK_FALSE(camera->get_image_ready());
}

// Decision 0020 rule 2: one body per camera; the running exposure is not replaced.
TEST_CASE("SVBONY Camera Driver - StartExposure during an exposure is refused and the exposure finishes",
          "[svbony][camera][unit][fake-sdk][capture-session]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    ReleaseReadOnExit release{sdk};
    sdk.block_video_data();
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));

    require_alpaca_error([&] { camera->start_exposure(0.002, true); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(sdk.call_count("start_video_capture") == 1);
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Exposing);
    sdk.release_video_data();
    wait_ready(*camera);
    CHECK(camera->get_last_exposure_duration() == 0.001);
    CHECK(sdk.call_count("start_video_capture") == 1);
}

// Decision 0020 rules 3 and 5: an SDK stop that fails keeps its text for ImageReady while CameraState reads Idle;
// StartExposure and the settings writers refuse until a later stop works.
TEST_CASE("SVBONY Camera Driver - an unconfirmed capture stop keeps the stop message and reads Idle",
          "[svbony][camera][unit][fake-sdk][capture-session]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    ReleaseReadOnExit release{sdk};
    sdk.block_video_data();
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    sdk.set_persistent_failure("stop_video_capture");
    const auto abort_error = error_of([&] { camera->abort_exposure(); });
    CHECK(abort_error.first == alpacacore::AlpacaError::DriverException);
    CHECK(abort_error.second.find("AbortExposure could not stop SVBONY video capture") != std::string::npos);
    const int reads = sdk.completed_video_data_reads();
    sdk.release_video_data();
    REQUIRE(sdk.wait_for_completed_video_data(reads + 1, 2s));

    CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);
    const auto [code, text] = wait_for_raise(*camera);
    CHECK(code == alpacacore::AlpacaError::DriverException);
    CHECK(text.find("capture stop was not confirmed") != std::string::npos);
    require_alpaca_error([&] { camera->start_exposure(0.001, true); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { camera->set_gain(30); }, alpacacore::AlpacaError::DriverException);

    sdk.set_persistent_failure("stop_video_capture", false);
    camera->abort_exposure();  // retries the stop the camera could not confirm
    camera->start_exposure(0.001, true);
    wait_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

// Decision 0020 rule 4 and issue #1477: PercentCompleted runs on the task clock from the capture start.
TEST_CASE("SVBONY Camera Driver - PercentCompleted follows the task clock while exposing",
          "[svbony][camera][unit][fake-sdk][capture-session]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_camera_on_clock(sdk, clock);
    ReleaseReadOnExit release{sdk};
    sdk.set_max_read_wait(5s);  // real-time bound for the held read, released below
    sdk.block_video_data();
    camera->start_exposure(10.0, true);
    REQUIRE(sdk.wait_for_video_data(2s));

    CHECK(camera->get_percent_completed_checked() == 0.0);
    clock.advance(5s);
    CHECK(camera->get_percent_completed_checked() == Catch::Approx(50.0).margin(0.01));
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Exposing);
    camera->abort_exposure();
}

// svbony.instructions.md (issue #1477): the duration is the rounded and clamped exposure register.
TEST_CASE("SVBONY Camera Driver - LastExposureDuration is the clamped exposure register",
          "[svbony][camera][unit][fake-sdk][capture-session]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    camera->start_exposure(0.0, true);  // below the SDK minimum of 1 us
    wait_ready(*camera);
    CHECK(camera->get_last_exposure_duration() == 0.000001);
    camera->start_exposure(0.0010004, true);  // rounds to 1000 us
    wait_ready(*camera);
    CHECK(camera->get_last_exposure_duration() == 0.001);
}

// Decision 0020 rule 8: the connect reopens the session the disconnect closed.
TEST_CASE("SVBONY Camera Driver - a StartExposure after a reconnect runs",
          "[svbony][camera][unit][fake-sdk][capture-session]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    camera->set_connected(false);
    camera->set_connected(true);
    camera->start_exposure(0.001, true);
    wait_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

// Decision 0020 rule 8: a StartExposure that passed its checks and waits in the session for an earlier body (one the
// watchdog failed, still in the SDK) is refused with NotConnected by a Disconnect and runs no body. The earlier
// body's own capture stop is held until the refused start has returned, so it cannot leave the session idle before
// the Disconnect closes it.
TEST_CASE("SVBONY Camera Driver - a StartExposure that raced a disconnect raises NotConnected and runs no body",
          "[svbony][camera][unit][fake-sdk][capture-session]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_camera_on_clock(sdk, clock);
    ReleaseReadOnExit release{sdk};
    sdk.set_max_read_wait(5s);
    sdk.block_video_data();
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    clock.advance(16s);  // past exposure + 15 s: the watchdog fails the exposure; its body is still in the read
    REQUIRE(camera->get_camera_state() == alpacacore::CameraState::Idle);

    const auto test_thread = std::this_thread::get_id();
    std::atomic<bool> start_returned{false};
    sdk.before_call = [&](const std::string& call) {
        if (call == "stop_video_capture" && std::this_thread::get_id() != test_thread) {
            alpacacore::test::hold_until(start_returned);  // the body's own stop
        }
    };
    std::atomic<int> start_code{-1};
    std::thread starter([&] {
        start_code = error_of([&] { camera->start_exposure(0.001, true); }).first;
        start_returned = true;
    });
    struct JoinStarter {
        std::thread& thread;
        std::atomic<bool>& returned;
        ~JoinStarter() {
            returned = true;
            if (thread.joinable()) thread.join();
        }
    } join_starter{starter, start_returned};
    REQUIRE(clock.wait_for_waiters(1, 2s));  // the start waits in the session for the earlier body

    camera->set_connected(false);
    starter.join();
    CHECK(start_code.load() == alpacacore::AlpacaError::NotConnected);
    CHECK(sdk.call_count("start_video_capture") == 1);
    CHECK(sdk.open_count() == 0);
    sdk.before_call = nullptr;
}

// --- Decision 0009: a removal code from the SDK is a lost link (issue #1986) ---------------------------------

// Rule 1 and rule 2: a removal from a control read drops Connected, members answer NotConnected with the SDK
// text, DeviceState is empty and the dead handle is closed.
TEST_CASE("SVBONY Camera Driver - a removal error on a control read drops Connected",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    sdk.fail_with_removal("get_control_value");

    const auto [code, text] = error_of([&] { static_cast<void>(camera->get_gain()); });
    CHECK(code == alpacacore::AlpacaError::NotConnected);
    CHECK(text.find("SVB_ERROR_CAMERA_REMOVED") != std::string::npos);
    CHECK_FALSE(camera->get_connected());
    CHECK(camera->get_device_state().empty());
    CHECK(sdk.call_count("close_camera") == 1);
    CHECK(sdk.open_count() == 0);
    CHECK(camera->get_link_fault() == text);
    for (const auto& member : std::vector<std::function<void()>>{
             [&] { static_cast<void>(camera->get_gain()); },
             [&] { camera->start_exposure(0.001, true); },
             [&] { static_cast<void>(camera->get_image_ready()); },
             [&] { static_cast<void>(camera->get_camera_state()); },
             [&] { camera->abort_exposure(); },
         }) {
        const auto error = error_of(member);
        CHECK(error.first == alpacacore::AlpacaError::NotConnected);
        CHECK(error.second == text);
    }
    const int reads = sdk.call_count("get_control_value");
    static_cast<void>(error_of([&] { static_cast<void>(camera->get_gain()); }));
    CHECK(sdk.call_count("get_control_value") == reads);  // nothing reaches the SDK while lost
}

// Rule 1: the control-write call-site group.
TEST_CASE("SVBONY Camera Driver - a removal error on a control write drops Connected",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    sdk.fail_with_removal("set_control_value");

    require_alpaca_error([&] { camera->set_gain(20); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(camera->get_connected());
    CHECK(sdk.open_count() == 0);
    CHECK(camera->get_link_fault().find("set_control_value: SVB_ERROR_CAMERA_REMOVED") != std::string::npos);
}

// Rule 1: the pulse-guide call-site group (a call made without the driver mutex).
TEST_CASE("SVBONY Camera Driver - a removal error from a pulse guide drops Connected",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    sdk.fail_with_removal("pulse_guide");

    require_alpaca_error([&] { camera->pulse_guide(0, 10); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(camera->get_connected());
    CHECK_FALSE(camera->get_is_pulse_guiding());
    CHECK(sdk.open_count() == 0);
}

// Rule 2 with decision 0020 rule 3: the body ends at the removal with no further read, Connected reads false, the
// session getters answer NotConnected with the SDK text, and the body closes the dead handle on its way out.
TEST_CASE("SVBONY Camera Driver - an exposure in flight when the camera goes fails and ends the body",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    ReleaseReadOnExit release{sdk};
    sdk.block_video_data();
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    sdk.remove_during_held_read();
    sdk.release_video_data();

    REQUIRE(eventually([&] { return sdk.call_count("close_camera") == 1; }));  // the body's way out
    const int reads = sdk.call_count("get_video_data");
    CHECK_FALSE(camera->get_connected());
    CHECK(camera->get_link_fault() == "SVBGetVideoData: SVB_ERROR_CAMERA_REMOVED");
    for (const auto& member : std::vector<std::function<void()>>{
             [&] { static_cast<void>(camera->get_image_ready()); },
             [&] { static_cast<void>(camera->get_image_array()); },
             [&] { static_cast<void>(camera->get_camera_state()); },
         }) {
        const auto error = error_of(member);
        CHECK(error.first == alpacacore::AlpacaError::NotConnected);
        CHECK(error.second == "SVBGetVideoData: SVB_ERROR_CAMERA_REMOVED");
    }
    CHECK(sdk.call_count("get_video_data") == reads);  // no retry after the loss
    CHECK(reads == 1);
    CHECK(sdk.open_count() == 0);
}

// Rule 1 negatives: only NotConnected from a device call (the wrapper's CAMERA_REMOVED or CAMERA_CLOSED) is a loss.
// SVB_ERROR_INVALID_ID (InvalidValue), a generic error, a run of them past the staleness bound and a read timeout
// keep the link.
TEST_CASE("SVBONY Camera Driver - only a removal code is a lost link", "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_camera_on_clock(sdk, clock);

    SECTION("an invalid-id error is not a loss") {
        sdk.fail_next_with("get_control_value", alpacacore::AlpacaError::InvalidValue,
                           "SVBGetControlValue: SVB_ERROR_INVALID_ID");
        require_alpaca_error([&] { static_cast<void>(camera->get_gain()); }, alpacacore::AlpacaError::InvalidValue);
    }
    SECTION("a generic SDK error is not a loss") {
        sdk.fail_next("get_control_value");
        require_alpaca_error([&] { static_cast<void>(camera->get_gain()); }, alpacacore::AlpacaError::DriverException);
    }
    SECTION("repeated failures past the staleness bound are not a loss") {
        for (int i = 0; i < 10; ++i) {
            sdk.fail_next("get_control_value");
            require_alpaca_error([&] { static_cast<void>(camera->get_gain()); },
                                 alpacacore::AlpacaError::DriverException);
        }
        clock.advance(alpacacore::util::kLinkStalenessBound + 1s);
    }
    SECTION("a frame-read timeout is not a loss") {
        sdk.timeout_next_video_data(3);
        camera->start_exposure(0.001, true);
        wait_ready(*camera);
    }
    CHECK(camera->get_connected());
    CHECK(camera->get_link_fault().empty());
    CHECK(sdk.call_count("close_camera") == 0);
    CHECK(camera->get_gain() == 10);
}

// Rule 1: the driver's own NotConnected (a member called while disconnected) is not evidence and sets no fault.
TEST_CASE("SVBONY Camera Driver - the driver's own NotConnected is not a loss",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    require_alpaca_error([&] { static_cast<void>(camera->get_gain()); }, alpacacore::AlpacaError::NotConnected);
    CHECK(camera->get_link_fault().empty());
    camera->set_connected(true);
    camera->set_connected(false);
    const auto error = error_of([&] { static_cast<void>(camera->get_gain()); });
    CHECK(error.first == alpacacore::AlpacaError::NotConnected);
    CHECK(error.second == "Camera not connected");
    CHECK(camera->get_link_fault().empty());
    camera->set_connected(true);
    CHECK(camera->get_connected());
}

// Rule 4: only the client relinks; a Connect after a loss opens the camera afresh, the kept loss failure and the
// last exposure are gone, and an exposure runs.
TEST_CASE("SVBONY Camera Driver - a Connect after a loss reconnects and exposes",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    ReleaseReadOnExit release{sdk};
    camera->start_exposure(0.001, true);
    wait_ready(*camera);
    sdk.block_video_data();
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    sdk.remove_during_held_read();
    sdk.release_video_data();
    REQUIRE(eventually([&] { return sdk.call_count("close_camera") == 1; }));
    REQUIRE_FALSE(camera->get_connected());
    CHECK(sdk.call_count("open_camera") == 1);  // no autonomous reopen

    camera->set_connected(true);
    CHECK(camera->get_connected());
    CHECK(sdk.call_count("open_camera") == 2);
    CHECK(sdk.open_count() == 1);
    CHECK(camera->get_link_fault().empty());
    CHECK_FALSE(camera->get_image_ready());
    alpacacore::test::require_last_exposure_unset(*camera);
    camera->start_exposure(0.001, true);
    wait_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

// Rule 5: get_link_fault() keeps the SDK text after the loss, on the task clock too, until a Disconnect or a
// Connect clears it.
TEST_CASE("SVBONY Camera Driver - LinkFault keeps the loss reason until the next Connect or Disconnect",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_camera_on_clock(sdk, clock);
    sdk.fail_with_removal("get_control_value");
    require_alpaca_error([&] { static_cast<void>(camera->get_gain()); }, alpacacore::AlpacaError::NotConnected);
    CHECK(camera->get_link_fault() == "get_control_value: SVB_ERROR_CAMERA_REMOVED");
    clock.advance(1h);
    CHECK(camera->get_link_fault() == "get_control_value: SVB_ERROR_CAMERA_REMOVED");

    SECTION("a Disconnect clears it") {
        camera->set_connected(false);
        CHECK(camera->get_link_fault().empty());
        CHECK_FALSE(camera->get_connected());
    }
    SECTION("a Connect clears it") {
        camera->set_connected(true);
        CHECK(camera->get_link_fault().empty());
        CHECK(camera->get_connected());
    }
}

// Rule 4: the close of the dead handle (at the loss, with no exposure body running) that meets a removal error is
// logged and swallowed, the Disconnect of the lost camera completes and clears what the connect published, and the
// next Connect opens the camera.
TEST_CASE("SVBONY Camera Driver - a Disconnect of a lost camera completes when the close meets a removal error",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    sdk.fail_with_removal("close_camera");
    sdk.fail_with_removal("get_control_value");
    const auto [code, text] = error_of([&] { static_cast<void>(camera->get_gain()); });
    CHECK(code == alpacacore::AlpacaError::NotConnected);
    CHECK(text == "get_control_value: SVB_ERROR_CAMERA_REMOVED");  // the loss, not the close
    CHECK(sdk.call_count("close_camera") == 1);
    CHECK(camera->get_device_firmware().has_value());  // what the connect read stays until the Disconnect

    camera->set_connected(false);
    CHECK_FALSE(camera->get_connected());
    CHECK(camera->get_link_fault().empty());
    CHECK_FALSE(camera->get_device_firmware().has_value());
    camera->set_connected(true);
    CHECK(camera->get_connected());
    CHECK(camera->get_gain() == 10);
}

// Rule 4: a camera pulled at idle answers no call until the client's Disconnect, whose own SDK stop and close then
// meet the removal codes. The Disconnect completes (the close's removal is logged and swallowed), and the next
// Connect opens the camera.
TEST_CASE("SVBONY Camera Driver - a Disconnect completes when its own close reports the camera removed",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    sdk.fail_with_removal("stop_video_capture");
    sdk.fail_with_removal("close_camera");

    const auto error = error_of([&] { camera->set_connected(false); });
    CHECK(error.first == 0);
    CHECK(error.second.empty());
    CHECK_FALSE(camera->get_connected());
    CHECK(camera->get_link_fault().empty());
    CHECK(sdk.call_count("close_camera") == 1);
    camera->set_connected(true);
    CHECK(camera->get_connected());
    CHECK(camera->get_gain() == 10);
}

// Rule 4 (#1289): with the link lost, a Disconnect queued against the client's Connect in flight still reaches the
// driver, so the reopened camera is closed again.
TEST_CASE("SVBONY Camera Driver - a Disconnect queued against a Connect in flight reaches the driver",
          "[svbony][camera][unit][fake-sdk][link]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = svbony_connected_camera(sdk);
    sdk.fail_with_removal("get_control_value");
    require_alpaca_error([&] { static_cast<void>(camera->get_gain()); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE_FALSE(camera->get_connected());

    std::atomic<bool> open_entered{false};
    std::atomic<bool> release_open{false};
    sdk.before_call = [&](const std::string& call) {
        if (call != "open_camera") return;
        open_entered = true;
        alpacacore::test::hold_until(release_open);
    };
    struct ReleaseOpenOnExit {
        std::atomic<bool>& release;
        ~ReleaseOpenOnExit() { release = true; }
    } release_on_exit{release_open};
    camera->connect();
    REQUIRE(alpacacore::test::wait_for_flag(open_entered));  // the relink is inside the SDK open
    camera->disconnect();
    release_open = true;
    REQUIRE(eventually([&] { return !camera->get_connecting(); }, 5000ms));
    CHECK_FALSE(camera->get_connected());
    CHECK(sdk.call_count("open_camera") == 2);
    CHECK(sdk.open_count() == 0);
    CHECK(sdk.call_count("close_camera") == 2);  // the loss, then the queued Disconnect
    sdk.before_call = nullptr;
}
