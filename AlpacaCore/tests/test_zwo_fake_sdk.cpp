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
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_task_clock.h"
#include "fake_zwo_sdk.h"
#include "locked_zwo_sdk.h"
#include "zwo_exposure_wait.h"

namespace {

using alpacacore::AlpacaException;
using alpacacore::test::FakeZWOSDK;
using alpacacore::vendor::zwo::create_zwo_camera;
namespace AlpacaError = alpacacore::AlpacaError;

void connect_camera(alpacacore::CameraDriver& camera) {
    camera.set_connected(true);
    REQUIRE(camera.get_connected());
}

std::vector<std::uint8_t> make_raw16_frame(int width, int height) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2);
    for (int i = 0; i < width * height; ++i) {
        const auto value = static_cast<std::uint16_t>(i + 1);
        bytes[static_cast<std::size_t>(i) * 2] = static_cast<std::uint8_t>(value & 0xff);
        bytes[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    return bytes;
}

int error_code(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const AlpacaException& ex) {
        return ex.error_code();
    }
    return -1;
}

}  // namespace

TEST_CASE("ZWO USB Limit defaults clamp and only real connects write", "[zwo][camera][unit][usb-bandwidth]") {
    FakeZWOSDK sdk;
    long expected = 40;
    SECTION("ordinary range") {}
    SECTION("range above default") { sdk.bandwidth_caps->min_value = expected = 60; }
    SECTION("range below default") {
        sdk.bandwidth_caps->min_value = 5;
        sdk.bandwidth_caps->max_value = expected = 30;
    }
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    long value = 0;
    bool is_auto = true;
    REQUIRE(
        sdk.raw_get_control_value(sdk.camera.camera_id, FakeZWOSDK::ControlType::BandwidthOverload, value, is_auto));
    CHECK(value == expected);
    CHECK_FALSE(is_auto);
    CHECK(sdk.call_count("set_bandwidth") == 1);
    camera->set_connected(true);
    sdk.script_exposure_status({FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Success});
    camera->start_exposure(0.01, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(sdk.call_count("start_exposure") == 2);  // a retry trigger, then the completed acquisition
    REQUIRE(camera->get_image_ready());
    CHECK_FALSE(camera->get_image_array().data.empty());
    CHECK(sdk.call_count("set_bandwidth") == 1);
    camera->set_connected(false);
    connect_camera(*camera);
    CHECK(sdk.call_count("set_bandwidth") == 2);
}

TEST_CASE("ZWO USB Limit applies explicit values and rejects unsupported settings",
          "[zwo][camera][unit][usb-bandwidth]") {
    FakeZWOSDK sdk;
    alpacacore::vendor::zwo::ZwoCameraBinding binding;
    binding.identity.camera_id = sdk.camera.camera_id;
    bool accepted = true;
    SECTION("lower endpoint") { binding.usb_bandwidth = 40; }
    SECTION("upper endpoint") { binding.usb_bandwidth = 100; }
    SECTION("custom") { binding.usb_bandwidth = 55; }
    SECTION("zero when supported") {
        sdk.bandwidth_caps->min_value = 0;
        binding.usb_bandwidth = 0;
    }
    SECTION("below camera range") {
        binding.usb_bandwidth = 39;
        accepted = false;
    }
    SECTION("above camera range") {
        sdk.bandwidth_caps->max_value = 90;
        binding.usb_bandwidth = 100;
        accepted = false;
    }
    SECTION("outside setup range") {
        binding.usb_bandwidth = std::numeric_limits<std::int64_t>::max();
        accepted = false;
    }
    SECTION("no control default") { sdk.bandwidth_caps.reset(); }
    SECTION("read only default") { sdk.bandwidth_caps->is_writable = false; }
    SECTION("no control explicit") {
        sdk.bandwidth_caps.reset();
        binding.usb_bandwidth = 40;
        accepted = false;
    }
    SECTION("read only explicit") {
        sdk.bandwidth_caps->is_writable = false;
        binding.usb_bandwidth = 40;
        accepted = false;
    }
    auto camera = alpacacore::vendor::zwo::create_zwo_camera_bound(0, binding, sdk);
    if (accepted) {
        connect_camera(*camera);
        if (binding.usb_bandwidth) {
            long value = 0;
            bool is_auto = true;
            REQUIRE(sdk.raw_get_control_value(sdk.camera.camera_id, FakeZWOSDK::ControlType::BandwidthOverload, value,
                                              is_auto));
            CHECK(value == *binding.usb_bandwidth);
            CHECK_FALSE(is_auto);
        } else {
            CHECK(sdk.call_count("set_bandwidth") == 0);
        }
    } else {
        CHECK(error_code([&] { camera->set_connected(true); }) == AlpacaError::DriverException);
        CHECK_FALSE(camera->get_connected());
        CHECK(sdk.open_count() == 0);
        CHECK(sdk.call_count("set_bandwidth") == 0);
    }
}

TEST_CASE("ZWO USB Limit initialization failures close and allow reconnect", "[zwo][camera][unit][usb-bandwidth]") {
    FakeZWOSDK sdk;
    SECTION("malformed range") { sdk.bandwidth_caps->min_value = 101; }
    SECTION("write failure") { sdk.fail_bandwidth_write = true; }
    SECTION("read failure") { sdk.fail_bandwidth_read = true; }
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    try {
        camera->set_connected(true);
        FAIL("USB Limit initialization should fail");
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::DriverException);
        CHECK(std::string(e.what()).find("USB Limit") != std::string::npos);
    }
    CHECK_FALSE(camera->get_connected());
    CHECK(sdk.open_count() == 0);
    CHECK(sdk.call_count("open_camera") == sdk.call_count("close_camera"));
    sdk.bandwidth_caps->min_value = 40;
    sdk.fail_bandwidth_write = sdk.fail_bandwidth_read = sdk.bandwidth_auto_readback = false;
    sdk.bandwidth_readback.reset();
    connect_camera(*camera);
    CHECK(sdk.open_count() == 1);
}

TEST_CASE("ZWO USB Limit readback mismatches warn only for the implicit default",
          "[zwo][camera][unit][usb-bandwidth]") {
    FakeZWOSDK sdk;
    SECTION("value mismatch") { sdk.bandwidth_readback = 80; }
    SECTION("auto mismatch") { sdk.bandwidth_auto_readback = true; }
    SECTION("value and auto mismatch") {
        sdk.bandwidth_readback = 80;
        sdk.bandwidth_auto_readback = true;
    }
    for (const bool explicit_setting : {false, true}) {
        std::vector<std::string> warnings;
        bool claimed_applied = false;
        struct SinkGuard {
            alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
            alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
            ~SinkGuard() {
                alpacacore::logging::set_log_sink(previous);
                alpacacore::logging::set_log_level(level);
            }
        } sink_guard;
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Info);
        alpacacore::logging::set_log_sink([&](auto level, auto component, auto message) {
            if (component != "ZWO" || message.find("USB Limit") == std::string_view::npos) return;
            if (level == alpacacore::logging::LogLevel::Warn) warnings.emplace_back(message);
            if (level == alpacacore::logging::LogLevel::Info && message.find(" applied ") != std::string_view::npos)
                claimed_applied = true;
        });
        alpacacore::vendor::zwo::ZwoCameraBinding binding;
        binding.identity.camera_id = sdk.camera.camera_id;
        if (explicit_setting) binding.usb_bandwidth = 40;
        auto camera = alpacacore::vendor::zwo::create_zwo_camera_bound(0, binding, sdk);
        if (explicit_setting) {
            CHECK(error_code([&] { camera->set_connected(true); }) == AlpacaError::DriverException);
            CHECK_FALSE(camera->get_connected());
            CHECK(sdk.open_count() == 0);
            CHECK(sdk.call_count("open_camera") == sdk.call_count("close_camera"));
        } else {
            connect_camera(*camera);
            CHECK(sdk.open_count() == 1);
            REQUIRE(warnings.size() == 1);
            CHECK(warnings.front().find("requested 40") != std::string::npos);
            CHECK(warnings.front().find("read back " + std::to_string(sdk.bandwidth_readback.value_or(40))) !=
                  std::string::npos);
            CHECK(warnings.front().find(sdk.bandwidth_auto_readback ? "auto" : "manual") != std::string::npos);
            camera->set_connected(false);
        }
        CHECK_FALSE(claimed_applied);
    }
}

TEST_CASE("ZWO fake SDK - LastExposure publishes only acquisition success", "[zwo][camera][unit][last-exposure]") {
    FakeZWOSDK sdk;
    sdk.auto_complete_exposure = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    const auto expect_unset = [&](int code) {
        CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); }) == code);
        CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_start_time()); }) == code);
    };
    expect_unset(AlpacaError::NotConnected);
    connect_camera(*camera);
    expect_unset(AlpacaError::InvalidOperation);
    camera->start_exposure(0.01, true);
    CHECK_FALSE(camera->get_image_ready());
    expect_unset(AlpacaError::InvalidOperation);
    camera->abort_exposure();
    expect_unset(AlpacaError::InvalidOperation);
    sdk.set_exposure_status_error("first acquisition failed");
    camera->start_exposure(0.01, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    expect_unset(AlpacaError::InvalidOperation);

    sdk.auto_complete_exposure = true;
    camera->start_exposure(0.0100006, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 1);  // the frame is in before ImageReady
    const auto start_a = camera->get_last_exposure_start_time();
    const auto duration_a = camera->get_last_exposure_duration();
    CHECK(duration_a == 0.010001);
    const auto expect_a = [&] {
        CHECK(camera->get_last_exposure_duration() == duration_a);
        CHECK(camera->get_last_exposure_start_time() == start_a);
    };
    camera->set_connected(true);  // redundant connection is not a new session
    expect_a();
    CHECK(error_code([&] { camera->start_exposure(-1.0, true); }) == AlpacaError::InvalidValue);
    expect_a();
    sdk.auto_complete_exposure = false;
    camera->start_exposure(0.02, true);
    CHECK_FALSE(camera->get_image_ready());
    expect_a();
    camera->abort_exposure();
    expect_a();
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
    sdk.set_exposure_status_error("later acquisition failed");
    camera->start_exposure(0.03, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    expect_a();
    sdk.auto_complete_exposure = true;
    camera->start_exposure(0.04, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.04);
    CHECK(camera->get_last_exposure_start_time() > start_a);

    // LastExposure* commit with the downloaded frame (issue #1981): a failed transfer keeps the earlier values.
    const auto start_b = camera->get_last_exposure_start_time();
    sdk.download_error = "late transfer failure";
    camera->start_exposure(0.05, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(camera->get_last_exposure_duration() == 0.04);
    CHECK(camera->get_last_exposure_start_time() == start_b);
    camera->set_connected(false);
    expect_unset(AlpacaError::NotConnected);
    camera->set_connected(true);
    expect_unset(AlpacaError::InvalidOperation);
}

TEST_CASE("ZWO fake SDK - LastExposure timing follows each SDK trigger", "[zwo][camera][unit][last-exposure]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    std::mutex stamps_mutex;
    std::chrono::system_clock::time_point setup_finished, trigger_entered;
    sdk.before_call = [&](const std::string& call) {
        std::lock_guard lock(stamps_mutex);
        if (call == "set_control_value") setup_finished = std::chrono::system_clock::now();
        if (call == "start_exposure") trigger_entered = std::chrono::system_clock::now();
    };
    const auto stamps = [&] {
        std::lock_guard lock(stamps_mutex);
        return std::make_pair(setup_finished, trigger_entered);
    };
    camera->start_exposure(0.0200006, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_start_time() >= stamps().first);
    CHECK(camera->get_last_exposure_start_time() <= stamps().second);
    CHECK(camera->get_last_exposure_duration() == 0.020001);
    const auto start_a = camera->get_last_exposure_start_time();

    sdk.auto_complete_exposure = false;
    camera->start_exposure(2.0000006, true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the status poll
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
    clock.advance(std::chrono::seconds(3));
    REQUIRE(sdk.wait_for_call_count("start_exposure", 3, std::chrono::seconds(5)));  // the retry
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
    CHECK(camera->get_last_exposure_start_time() == start_a);
    clock.advance(std::chrono::milliseconds(500));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    CHECK(camera->get_percent_completed() == Catch::Approx(50.0 / 2.000001));  // from the retry's trigger
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_start_time() >= stamps().first);
    CHECK(camera->get_last_exposure_start_time() <= stamps().second);
    CHECK(camera->get_last_exposure_duration() == 2.000001);
    CHECK(sdk.call_count("start_exposure") == 3);
    CHECK(sdk.call_count("get_data_after_exposure") == 2);
}

namespace {

// Issue #2187: the SDK reports the running exposure Failed `latency` after the body's last trigger. The body polls
// the status every 50 ms on the fake clock; one advance covers the whole latency, and the next 50 ms step is the
// poll that reads Failed.
void fail_exposure_after(FakeZWOSDK& sdk, alpacacore::test::FakeTaskClock& clock, std::chrono::milliseconds latency) {
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the status poll
    clock.advance(latency - std::chrono::milliseconds(50));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
    clock.advance(std::chrono::milliseconds(50));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
}

std::string kept_failure(const alpacacore::CameraDriver& camera) {
    try {
        static_cast<void>(camera.get_image_ready());
    } catch (const alpacacore::AlpacaException& e) {
        return e.what();
    }
    return {};
}

}  // namespace

// Issue #2187: a retry is one more full exposure, and PHD2 drops a camera whose frame is not ready 15 s past the
// exposure (24 drops of an ASI120MM Mini in one night). A retry starts only when it is expected to end within 10 s
// of the exposure's own end, the expectation being the longer of the attempt that just failed and one full
// exposure. A 4 s exposure whose
// failures come 6 s after each trigger: the first retry would end at ~12.3 s, inside 4 + 10 s, and runs; the second
// would end at ~18.6 s and is not started, so the client hears of the failure at ~12.4 s instead of ~18.7 s.
TEST_CASE("ZWO fake SDK - a retry that would run past the client's exposure wait is not started",
          "[zwo][camera][unit][capture-session][retry-budget]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    sdk.auto_complete_exposure = false;

    camera->start_exposure(4.0, true);
    REQUIRE(sdk.wait_for_call_count("start_exposure", 1, std::chrono::seconds(2)));
    fail_exposure_after(sdk, clock, std::chrono::seconds(6));
    REQUIRE(sdk.wait_for_call_count("start_exposure", 2, std::chrono::seconds(5)));  // the first retry runs
    fail_exposure_after(sdk, clock, std::chrono::seconds(6));
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    CHECK(sdk.call_count("start_exposure") == 2);  // no second retry
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);
    const std::string message = kept_failure(*camera);
    CHECK(message.find("a retry would run past the client's exposure wait") != std::string::npos);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);

    // The next StartExposure clears the kept failure and starts a fresh budget.
    sdk.auto_complete_exposure = true;
    camera->start_exposure(0.01, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    CHECK(camera->get_image_ready());
}

// Issue #2187: the budget leaves a short guide exposure both retries. A 1 s exposure whose failures come 2.2 s after
// each trigger (the ASI120MM Mini's measured latency) runs the first attempt and two retries, ending at ~6.6 s,
// inside 1 + 10 s, and only then keeps the exhausted-retries failure.
TEST_CASE("ZWO fake SDK - a short exposure whose retries fit the client's wait keeps both of them",
          "[zwo][camera][unit][capture-session][retry-budget]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    sdk.auto_complete_exposure = false;

    camera->start_exposure(1.0, true);
    REQUIRE(sdk.wait_for_call_count("start_exposure", 1, std::chrono::seconds(2)));
    fail_exposure_after(sdk, clock, std::chrono::milliseconds(2200));
    REQUIRE(sdk.wait_for_call_count("start_exposure", 2, std::chrono::seconds(5)));  // retry 1
    fail_exposure_after(sdk, clock, std::chrono::milliseconds(2200));
    REQUIRE(sdk.wait_for_call_count("start_exposure", 3, std::chrono::seconds(5)));  // retry 2
    fail_exposure_after(sdk, clock, std::chrono::milliseconds(2200));
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    CHECK(sdk.call_count("start_exposure") == 3);
    const std::string message = kept_failure(*camera);
    CHECK(message.find("camera exposure failed after SDK retries") != std::string::npos);
    CHECK(message.find("client's exposure wait") == std::string::npos);
}

// Issue #2187: a retry is at least one full exposure, however early the failure came. A 30 s exposure whose failure
// arrives 15 s after the trigger: a retry would end at ~45.3 s, past 30 + 10 s, so none starts, where the failed
// attempt's own length (15 s) alone would have allowed one ending ~20 s past the client's wait. A failure 5 s after
// the trigger still earns a retry (~35.3 s).
TEST_CASE("ZWO fake SDK - a retry of a long exposure counts a full exposure however early the failure came",
          "[zwo][camera][unit][capture-session][retry-budget]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    sdk.auto_complete_exposure = false;

    SECTION("a failure 15 s into a 30 s exposure is kept at once") {
        camera->start_exposure(30.0, true);
        REQUIRE(sdk.wait_for_call_count("start_exposure", 1, std::chrono::seconds(2)));
        fail_exposure_after(sdk, clock, std::chrono::seconds(15));
        REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
        CHECK(sdk.call_count("start_exposure") == 1);  // no retry
        CHECK(kept_failure(*camera).find("a retry would run past the client's exposure wait") != std::string::npos);
    }

    SECTION("a failure 5 s into a 30 s exposure still earns one retry") {
        camera->start_exposure(30.0, true);
        REQUIRE(sdk.wait_for_call_count("start_exposure", 1, std::chrono::seconds(2)));
        fail_exposure_after(sdk, clock, std::chrono::seconds(5));
        REQUIRE(sdk.wait_for_call_count("start_exposure", 2, std::chrono::seconds(5)));  // the retry runs
        CHECK(camera->get_camera_state() == alpacacore::CameraState::Exposing);
        camera->abort_exposure();
    }
}

TEST_CASE("ZWO fake SDK - exhausted retries never publish candidate metadata", "[zwo][camera][unit][last-exposure]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    for (const bool prior_success : {false, true}) {
        if (prior_success) {
            camera->start_exposure(0.01, true);
            REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
            REQUIRE(camera->get_image_ready());
        }
        std::chrono::system_clock::time_point prior_start{};
        if (prior_success) prior_start = camera->get_last_exposure_start_time();
        sdk.script_exposure_status({FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed,
                                    FakeZWOSDK::ExposureStatus::Failed});
        camera->start_exposure(0.02, true);
        REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
        CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
        if (prior_success) {
            CHECK(camera->get_last_exposure_duration() == 0.01);
            CHECK(camera->get_last_exposure_start_time() == prior_start);
        } else {
            CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); }) ==
                  AlpacaError::InvalidOperation);
            CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_start_time()); }) ==
                  AlpacaError::InvalidOperation);
        }
    }
}

// Rule 9 (issue #1981): LastExposure* are committed by the frame after the download, not at the SDK's Success status,
// and a download failure keeps the values of the last good frame.
TEST_CASE("ZWO fake SDK - LastExposure is committed with the frame after the download",
          "[zwo][camera][unit][capture-session][last-exposure]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.hold_download();
    camera->start_exposure(0.01, true);
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    const int held_duration = error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); });
    const int held_start = error_code([&] { static_cast<void>(camera->get_last_exposure_start_time()); });
    sdk.release_download();
    REQUIRE(download_started);
    CHECK(held_duration == AlpacaError::InvalidOperation);
    CHECK(held_start == AlpacaError::InvalidOperation);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.01);
    const auto good_start = camera->get_last_exposure_start_time();

    sdk.download_error = "transfer failed after acquisition";
    camera->start_exposure(0.02, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(camera->get_last_exposure_duration() == 0.01);
    CHECK(camera->get_last_exposure_start_time() == good_start);
}

TEST_CASE("ZWO fake SDK - retry trigger failure stays failed without publishing",
          "[zwo][camera][unit][last-exposure]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    sdk.auto_complete_exposure = false;
    camera->start_exposure(0.01, true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the body polls between waits
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
    sdk.fail_exposure_start = true;  // the retry's trigger fails
    clock.advance(std::chrono::milliseconds(50));
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    const int starts = sdk.call_count("start_exposure");
    CHECK(starts == 2);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 0);
    clock.advance(std::chrono::seconds(1));
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
    static_cast<void>(camera->get_camera_state());
    CHECK(sdk.call_count("start_exposure") == starts);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); }) ==
          AlpacaError::InvalidOperation);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_start_time()); }) ==
          AlpacaError::InvalidOperation);
    sdk.auto_complete_exposure = true;
    camera->start_exposure(0.02, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.02);
}

// Rules 4 and 5 (issue #1981): an AbortExposure during the retry pacing cancels the body, and the clock that would have
// released the next retry re-triggers nothing.
TEST_CASE("ZWO fake SDK - a cancelled body never re-triggers the exposure",
          "[zwo][camera][unit][capture-session][last-exposure]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    sdk.auto_complete_exposure = false;
    sdk.fail_every_exposure = true;  // every start reports Failed: the first retry runs at once, the second is paced
    camera->start_exposure(0.01, true);
    REQUIRE(sdk.wait_for_call_count("start_exposure", 2, std::chrono::seconds(2)));
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(2)));  // the 250 ms pacing wait before retry 2
    // Counted before the abort: AbortExposure joins the body, so a re-trigger that slips past the cancel lands inside.
    const int starts = sdk.call_count("start_exposure");
    camera->abort_exposure();
    clock.advance(std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(sdk.call_count("start_exposure") == starts);
    CHECK(clock.waiter_count() == 0);
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK_FALSE(camera->get_image_ready());  // an abort keeps no failure
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); }) ==
          AlpacaError::InvalidOperation);
}

TEST_CASE("ZWO fake SDK - lazy download preserves padded ROI pixels", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);

    camera->set_num_x(7);
    camera->set_num_y(3);
    camera->set_start_x(9);  // the padded eight-pixel ROI shifts left and crops one column
    camera->set_start_y(5);  // the effective ROI shifts upward and crops one source row
    sdk.frame_bytes = make_raw16_frame(8, 4);
    CHECK(error_code([&] { camera->start_exposure(std::numeric_limits<double>::quiet_NaN(), true); }) ==
          AlpacaError::InvalidValue);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));

    CHECK(camera->get_image_ready());
    const int roi_reads = sdk.call_count("get_roi_format");
    const int start_reads = sdk.call_count("get_start_pos");
    CHECK(camera->get_image_ready());
    static_cast<void>(camera->get_camera_state());
    CHECK(sdk.call_count("get_roi_format") == roi_reads);  // the getters make no SDK call
    CHECK(sdk.call_count("get_start_pos") == start_reads);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);  // the body downloaded the frame before ImageReady

    const auto image = camera->get_image_array();
    REQUIRE(image.rank == 2);
    REQUIRE(image.width == 7);
    REQUIRE(image.height == 3);
    CHECK(image.data[0] == 10);
    CHECK(image.data[6] == 16);
    CHECK(image.data[14] == 26);
    CHECK(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    camera->set_num_x(8);  // a geometry write drops the Ready frame (issue #1981)
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
}

TEST_CASE("ZWO fake SDK - full-frame binned dimensions survive ROI alignment", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.max_width = 6248;
    sdk.camera.max_height = 4178;
    sdk.camera.supported_bins = {1, 2, 3, 4};
    sdk.frame_bytes.assign(static_cast<std::size_t>(6248 / 2) * (4178 / 2) * 2, 0xff);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);

    for (const int bin : {2, 3, 4}) {
        camera->set_bin_x(bin);
        camera->start_exposure(0.001, true);
        REQUIRE(alpacacore::test::wait_exposure_end(*camera));
        const auto image = camera->get_image_array();
        const int requested_width = 6248 / bin;
        const int requested_height = 4178 / bin;
        REQUIRE(image.rank == 2);
        CHECK(image.width == requested_width);
        CHECK(image.height == requested_height);
        CHECK(image.data.size() == static_cast<std::size_t>(requested_width) * requested_height);
        CHECK(image.data.front() == 65535);
        CHECK(image.data[static_cast<std::size_t>(requested_width - 1)] == 0);
        if (bin == 2) {
            const auto penultimate_row = static_cast<std::size_t>(requested_height - 2) * requested_width;
            const auto last_row = static_cast<std::size_t>(requested_height - 1) * requested_width;
            CHECK(image.data[penultimate_row] == 65535);
            CHECK(image.data[last_row] == 0);
        }
    }
}

// Rule 10 (issue #1981): an SDK error while setting the exposure register or triggering the exposure no longer raises
// from StartExposure; it ends the exposure and ImageReady raises it. The new start discarded the earlier image; the
// earlier LastExposure* stay.
TEST_CASE("ZWO fake SDK - an SDK error at the exposure start surfaces through ImageReady",
          "[zwo][camera][unit][capture-session]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    const auto start = camera->get_last_exposure_start_time();

    for (const bool register_write : {true, false}) {
        if (register_write) {
            sdk.fail_exposure_register_write = true;
        } else {
            sdk.fail_exposure_start = true;
        }
        CHECK(error_code([&] { camera->start_exposure(0.002, true); }) == -1);
        REQUIRE(alpacacore::test::wait_exposure_end(*camera));
        CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
        CHECK(camera->get_last_exposure_duration() == 0.001);
        CHECK(camera->get_last_exposure_start_time() == start);
    }
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
}

TEST_CASE("ZWO fake SDK - post-open connect failure closes the handle and can reconnect", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.supported_formats.clear();
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);

    CHECK(error_code([&] { camera->set_connected(true); }) == AlpacaError::DriverException);
    CHECK_FALSE(camera->get_connected());
    CHECK(sdk.open_count() == 0);
    CHECK(sdk.call_count("close_camera") == 1);

    sdk.camera.supported_formats = {FakeZWOSDK::ImageType::Raw16};
    camera->set_connected(true);
    CHECK(camera->get_connected());
    CHECK(sdk.open_count() == 1);
    camera->set_connected(false);
    CHECK(sdk.open_count() == 0);
}

TEST_CASE("ZWO fake SDK - geometry changes preserve last-exposure metadata", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    const auto last_image = camera->get_image_array();
    const auto last_start = camera->get_last_exposure_start_time();
    const double last_duration = camera->get_last_exposure_duration();
    const auto check_history = [&] {
        CHECK(camera->get_last_exposure_start_time() == last_start);
        CHECK(camera->get_last_exposure_duration() == last_duration);
    };

    const int stop_calls = sdk.call_count("stop_exposure");
    camera->abort_exposure();  // an idle abort leaves the downloaded image and metadata available
    check_history();
    CHECK(camera->get_image_array().data == last_image.data);
    CHECK(sdk.call_count("stop_exposure") == stop_calls);
    camera->stop_exposure();  // an idle stop is also a no-op
    check_history();
    CHECK(camera->get_image_array().data == last_image.data);
    CHECK(sdk.call_count("stop_exposure") == stop_calls);
    camera->set_bin_x(2);
    check_history();
    camera->set_bin_y(2);
    check_history();
    camera->set_num_x(7);
    check_history();
    camera->set_num_y(3);
    check_history();
    camera->set_start_x(1);
    check_history();
    camera->set_start_x(1);
    check_history();
    camera->set_start_y(1);
    check_history();
    camera->abort_exposure();
    check_history();
    camera->stop_exposure();
    check_history();
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
}

TEST_CASE("ZWO fake SDK - ImageArray while exposing is asked too early", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.auto_complete_exposure = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_call_count("get_exposure_status", 1, std::chrono::seconds(2)));  // triggered, now polling
    REQUIRE_FALSE(camera->get_image_ready());

    // Issue #1167: the "still exposing" answer is the ICameraV4 InvalidOperation, logged at DEBUG.
    bool thrown = false;
    try {
        static_cast<void>(camera->get_image_array());
    } catch (const AlpacaException& ex) {
        thrown = true;
        CHECK(ex.error_code() == AlpacaError::InvalidOperation);
        CHECK(ex.is_asked_too_early());
    }
    CHECK(thrown);

    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(camera->get_image_ready());
    CHECK_FALSE(camera->get_image_array().data.empty());
}

TEST_CASE("ZWO fake SDK - overlapping start is rejected without replacing the active exposure", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.auto_complete_exposure = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_call_count("get_exposure_status", 1, std::chrono::seconds(2)));

    CHECK_FALSE(camera->get_image_ready());
    const int starts = sdk.call_count("start_exposure");
    CHECK(error_code([&] { camera->start_exposure(0.002, true); }) == AlpacaError::InvalidOperation);
    CHECK(sdk.call_count("start_exposure") == starts);
    CHECK_FALSE(camera->get_image_ready());

    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.001);

    // A start during the download is refused as well, and the frame being downloaded is the one published.
    sdk.auto_complete_exposure = true;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    sdk.hold_download();
    camera->start_exposure(0.003, true);
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    const int code_in_download = error_code([&] { camera->start_exposure(0.004, true); });
    sdk.release_download();
    REQUIRE(download_started);
    CHECK(code_in_download == AlpacaError::InvalidOperation);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.003);
    CHECK(camera->get_image_array().data.front() == 1);
    CHECK(sdk.call_count("start_exposure") == starts + 1);
}

// Rules 3 and 10 (issue #1981): StartExposure returns at once and the new start discards the previous frame, so
// ImageReady reads false, and ImageArray is asked too early, while the body is still in the SDK trigger.
TEST_CASE("ZWO fake SDK - ImageReady is false from StartExposure until the new frame is in",
          "[zwo][camera][unit][capture-session]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool start_entered = false;
    bool release_start = false;
    sdk.before_call = [&](const std::string& name) {
        if (name != "start_exposure") return;
        std::unique_lock lock(gate_mutex);
        start_entered = true;
        gate_cv.notify_all();
        gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return release_start; });  // bounded: no hang if red
    };
    const int start_code = error_code([&] { camera->start_exposure(0.002, true); });
    bool trigger_held = false;
    {
        std::unique_lock lock(gate_mutex);
        trigger_held = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return start_entered; });
    }
    const bool returned_before_trigger = !release_start;
    const int ready_code = error_code([&] { CHECK_FALSE(camera->get_image_ready()); });
    const int array_code = error_code([&] { static_cast<void>(camera->get_image_array()); });
    {
        std::lock_guard lock(gate_mutex);
        release_start = true;
    }
    gate_cv.notify_all();

    CHECK(start_code == -1);
    CHECK(trigger_held);
    CHECK(returned_before_trigger);
    CHECK(ready_code == -1);
    CHECK(array_code == AlpacaError::InvalidOperation);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.002);
}

TEST_CASE("ZWO fake SDK - RGB24 download converts BGR bytes to RGB channels", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.is_color = true;
    sdk.camera.supported_formats = {FakeZWOSDK::ImageType::Rgb24};
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->set_num_x(7);
    camera->set_num_y(3);
    camera->set_start_x(9);
    sdk.frame_bytes.resize(8 * 4 * 3);
    for (std::size_t pixel = 0; pixel < 8 * 4; ++pixel) {
        const auto offset = pixel * 3;
        sdk.frame_bytes[offset] = static_cast<std::uint8_t>(pixel * 3 + 3);      // B
        sdk.frame_bytes[offset + 1] = static_cast<std::uint8_t>(pixel * 3 + 2);  // G
        sdk.frame_bytes[offset + 2] = static_cast<std::uint8_t>(pixel * 3 + 1);  // R
    }
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));

    const auto image = camera->get_image_array();
    REQUIRE(image.rank == 3);
    REQUIRE(image.width == 7);
    REQUIRE(image.height == 3);
    CHECK(image.data[0] == 4);
    CHECK(image.data[1] == 5);
    CHECK(image.data[2] == 6);
    CHECK(image.data[3] == 7);
    CHECK(image.data[4] == 8);
    CHECK(image.data[5] == 9);
}

TEST_CASE("ZWO fake SDK - lazy download failure stays visible and later exposure recovers", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.download_error = "scripted bulk-transfer failure";
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));

    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);

    camera->abort_exposure();  // idle: the kept failure stays until the next start (issue #1981)
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);

    sdk.download_error.clear();
    sdk.frame_bytes = make_raw16_frame(16, 8);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    const auto image = camera->get_image_array();
    CHECK(image.width == 16);
    CHECK(image.height == 8);
    CHECK(image.data.front() == 1);
    CHECK(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 2);
}

TEST_CASE("ZWO fake SDK - status read failure and exhausted retries stay visible and recover", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);

    sdk.set_exposure_status_error("scripted status-read failure");
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);

    sdk.set_exposure_status_error("");
    sdk.script_exposure_status(
        {FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed});
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));  // the second retry waits 250 ms of real time
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("start_exposure") == 4);

    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - reconnect clears status, ROI and transfer failures", "[zwo][camera][unit]") {
    enum class Failure { Status, Roi, Download };
    for (const Failure failure : {Failure::Status, Failure::Roi, Failure::Download}) {
        FakeZWOSDK sdk;
        sdk.frame_bytes = make_raw16_frame(16, 8);
        auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
        connect_camera(*camera);
        if (failure == Failure::Status) {
            sdk.set_exposure_status_error("scripted status-read failure");
        } else if (failure == Failure::Roi) {
            sdk.roi_readback_override = FakeZWOSDK::ROIDescription{0, 8, 1, FakeZWOSDK::ImageType::Raw16};
        } else {
            sdk.download_error = "scripted bulk-transfer failure";
        }
        camera->start_exposure(0.001, true);
        REQUIRE(alpacacore::test::wait_exposure_end(*camera));
        CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);

        camera->set_connected(false);
        sdk.set_exposure_status_error("");
        sdk.roi_readback_override.reset();
        sdk.download_error.clear();
        camera->set_connected(true);
        CHECK_FALSE(camera->get_image_ready());
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);

        camera->start_exposure(0.001, true);
        REQUIRE(alpacacore::test::wait_exposure_end(*camera));
        REQUIRE(camera->get_image_ready());
        CHECK(camera->get_image_array().data.front() == 1);
        camera->set_connected(false);
        CHECK(sdk.open_count() == 0);
    }
}

TEST_CASE("ZWO fake SDK - successful raw16 conversion preserves values above one byte", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.max_width = 32;
    sdk.camera.max_height = 16;
    sdk.frame_bytes = make_raw16_frame(32, 16);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));

    const auto image = camera->get_image_array();
    REQUIRE(image.rank == 2);
    CHECK(image.data[255] == 256);
}

TEST_CASE("ZWO fake SDK - Raw8 and Y8 formats publish rank-two images", "[zwo][camera][unit]") {
    for (const auto format : {FakeZWOSDK::ImageType::Raw8, FakeZWOSDK::ImageType::Y8}) {
        FakeZWOSDK sdk;
        sdk.camera.supported_formats = {format};
        sdk.frame_bytes.resize(16 * 8, 0x5a);
        auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
        connect_camera(*camera);
        camera->start_exposure(0.001, true);
        REQUIRE(alpacacore::test::wait_exposure_end(*camera));

        const auto image = camera->get_image_array();
        CHECK(image.rank == 2);
        CHECK(image.data.front() == 0x5a);
    }
}

// Rules 4 and 10 (issue #1981): ImageReady is true only after the download, so a download that fails is never seen
// as ready: every read until the next StartExposure raises the kept failure.
TEST_CASE("ZWO fake SDK - a download failure is kept: ImageReady raises and is never true",
          "[zwo][camera][unit][capture-session]") {
    FakeZWOSDK sdk;
    sdk.download_error = "scripted bulk-transfer failure";
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);

    bool ever_ready = false;
    int code = 0;
    const bool raised = alpacacore::test::zwo_eventually([&] {
        code = error_code([&] { ever_ready = camera->get_image_ready() || ever_ready; });
        return code != -1;
    });
    CHECK(raised);
    CHECK_FALSE(ever_ready);
    CHECK(code == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
}

// Rule 6 (issue #1981): the body downloads the frame once, before ImageReady is true; ImageArray reads copy it and
// make no SDK call, so concurrent reads return the one downloaded frame.
TEST_CASE("ZWO fake SDK - concurrent ImageArray reads return the one downloaded frame",
          "[zwo][camera][unit][capture-session]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 1);  // downloaded before any ImageArray read
    const int roi_reads = sdk.call_count("get_roi_format");

    alpacacore::ImageArray first;
    alpacacore::ImageArray second;
    std::atomic<int> first_code{-1};
    std::atomic<int> second_code{-1};
    std::thread first_reader([&] { first_code.store(error_code([&] { first = camera->get_image_array(); })); });
    std::thread second_reader([&] { second_code.store(error_code([&] { second = camera->get_image_array(); })); });
    first_reader.join();
    second_reader.join();

    CHECK(first_code.load() == -1);
    CHECK(second_code.load() == -1);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    CHECK(sdk.call_count("get_roi_format") == roi_reads);
    CHECK(first.data == second.data);
    CHECK(first.data.front() == 1);
}

TEST_CASE("ZWO fake SDK - mismatched ROI metadata cannot publish ready", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    for (const auto& roi : {FakeZWOSDK::ROIDescription{0, 8, 1, FakeZWOSDK::ImageType::Raw16},
                            FakeZWOSDK::ROIDescription{8, 8, 1, FakeZWOSDK::ImageType::Raw16},
                            FakeZWOSDK::ROIDescription{16, 6, 1, FakeZWOSDK::ImageType::Raw16},
                            FakeZWOSDK::ROIDescription{16, 8, 2, FakeZWOSDK::ImageType::Raw16},
                            FakeZWOSDK::ROIDescription{16, 8, 1, FakeZWOSDK::ImageType::Raw8}}) {
        sdk.roi_readback_override = roi;
        camera->start_exposure(0.001, true);
        REQUIRE(alpacacore::test::wait_exposure_end(*camera));
        CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
        CHECK(sdk.call_count("get_data_after_exposure") == 0);
    }

    sdk.roi_readback_override.reset();
    sdk.start_readback_override = FakeZWOSDK::StartPosition{1, 0};
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 0);

    sdk.start_readback_override.reset();
    sdk.frame_bytes = make_raw16_frame(16, 8);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - abort rejects an in-flight lazy download", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.hold_download();
    camera->start_exposure(0.001, true);

    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    const int abort_code = error_code([&] { camera->abort_exposure(); });
    const int stop_calls = sdk.call_count("stop_exposure");
    sdk.release_download();

    CHECK(download_started);
    CHECK(abort_code == AlpacaError::InvalidOperation);
    CHECK(stop_calls == 0);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);

    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - stop leaves an in-flight lazy download alone", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.hold_download();
    camera->start_exposure(0.001, true);

    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    const int stop_code = error_code([&] { camera->stop_exposure(); });
    const int stop_calls = sdk.call_count("stop_exposure");
    sdk.release_download();

    CHECK(download_started);
    CHECK(stop_code == -1);
    CHECK(stop_calls == 0);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
}

TEST_CASE("ZWO fake SDK - disconnect waits for the lazy download before close", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    sdk.stop_releases_download = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.hold_download();
    camera->start_exposure(0.001, true);

    std::atomic<bool> disconnected{false};
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    std::thread disconnect([&] {
        camera->set_connected(false);
        disconnected.store(true);
    });
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sdk.call_count("stop_exposure") == 0 && std::chrono::steady_clock::now() < stop_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool stop_seen = sdk.call_count("stop_exposure") > 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the Disconnect now waits for the body's transfer
    const bool close_waited = !disconnected.load() && sdk.open_count() == 1 && sdk.call_count("close_camera") == 0;
    std::atomic<int> cached_bin{-1};
    std::atomic<bool> cached_getter_done{false};
    std::thread cached_getter([&] {
        cached_bin.store(camera->get_bin_x());
        cached_getter_done.store(true);
    });
    const auto getter_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (!cached_getter_done.load() && std::chrono::steady_clock::now() < getter_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool cached_getter_unblocked = cached_getter_done.load();
    sdk.release_download();
    disconnect.join();
    cached_getter.join();

    CHECK(download_started);
    CHECK(stop_seen);
    CHECK(close_waited);
    CHECK(cached_getter_unblocked);
    CHECK(cached_bin.load() == 1);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::NotConnected);
    CHECK_FALSE(sdk.closed_during_download());
    CHECK(sdk.open_count() == 0);
}

TEST_CASE("ZWO fake SDK - first CCDTemperature read after connect skips the SDK's placeholder 0",
          "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.zero_temperature_reads_after_open = 2;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    // The control cache is only reachable through the SDK: seed the real reading.
    camera->set_connected(true);
    sdk.raw_set_control_value(sdk.camera.camera_id, FakeZWOSDK::ControlType::Temperature, 215, false);
    const int connect_reads = sdk.call_count("get_control_value");

    CHECK(camera->get_ccd_temperature() == Catch::Approx(21.5));
    CHECK(sdk.call_count("get_control_value") >= connect_reads + 2);

    // Later reads: one SDK call each, no added delay.
    const int before = sdk.call_count("get_control_value");
    const auto start = std::chrono::steady_clock::now();
    CHECK(camera->get_ccd_temperature() == Catch::Approx(21.5));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(40));
    CHECK(sdk.call_count("get_control_value") == before + 1);

    // Reconnect re-arms the first-read handling.
    camera->set_connected(false);
    camera->set_connected(true);
    CHECK(camera->get_ccd_temperature() == Catch::Approx(21.5));
}

TEST_CASE("ZWO fake SDK - a first CCDTemperature read that stays 0 returns 0.0 within the bound",
          "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.zero_temperature_reads_after_open = 1000;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    camera->set_connected(true);
    const int connect_reads = sdk.call_count("get_control_value");

    const auto start = std::chrono::steady_clock::now();
    CHECK(camera->get_ccd_temperature() == Catch::Approx(0.0));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(600));
    CHECK(sdk.call_count("get_control_value") == connect_reads + 6);

    // The retry is spent: the next read is a single SDK call.
    CHECK(camera->get_ccd_temperature() == Catch::Approx(0.0));
    CHECK(sdk.call_count("get_control_value") == connect_reads + 7);
}

TEST_CASE("FakeZWOSDK behind LockedZWOSDK - forwards every entry point", "[zwo][camera][unit][fake-sdk]") {
    using namespace alpacacore::vendor::zwo;
    FakeZWOSDK fake;
    alpacacore::test::LockedZWOSDK sdk(fake);
    ZWOCameraInfo info;
    long value = 0;
    bool is_auto = true;
    std::vector<std::uint8_t> buf(16 * 8 * 2, 0);
    fake.frame_bytes.assign(buf.size(), 0x5a);
    const int id = fake.camera.camera_id;

    CHECK(sdk.enumerate_cameras().size() == 1);
    CHECK(sdk.enumerate_identified_cameras().size() == 1);
    const auto opened = sdk.open_camera_link(id);
    CHECK(opened.serial == "FAKE-ZWO-17");
    CHECK(sdk.get_camera_info_by_id(id, info));
    CHECK(sdk.get_camera_info_by_index(0, info));
    CHECK_FALSE(sdk.get_camera_info_by_index(3, info));
    // Every per-device call runs under the open's link (issue #1667); the decorator forwards with_link too.
    sdk.with_link(id, opened.link, [&] {
        CHECK(sdk.get_control_caps(id).size() == 10);
        sdk.set_control_value(id, ZWOControlType::Gain, 42, false);
        CHECK(sdk.get_control_value(id, ZWOControlType::Gain, value, is_auto));
        CHECK(value == 42);
        sdk.set_roi_format(id, 16, 8, 1, ZWOImageType::Raw16);
        CHECK(sdk.get_roi_format(id).width == 16);
        sdk.set_start_pos(id, 2, 4);
        CHECK(sdk.get_start_pos(id).start_x == 2);
        sdk.start_exposure(id, false);
        CHECK(sdk.get_exposure_status(id) == ZWOExposureStatus::Success);
        sdk.get_data_after_exposure(id, buf.data(), static_cast<long>(buf.size()));
        CHECK(buf[0] == 0x5a);
        sdk.stop_exposure(id);
        sdk.pulse_guide_on(id, ZWOGuideDirection::North);
        sdk.pulse_guide_off(id, ZWOGuideDirection::North);
        CHECK(sdk.get_serial_number(id) == "FAKE-ZWO-17");
    });
    CHECK(sdk.get_sdk_version() == "fake-zwo-sdk-1.0");
    sdk.close_camera_link(id, opened.link);

    for (const char* name : {"enumerate_cameras",     "enumerate_identified_cameras",
                             "get_camera_info_by_id", "get_camera_info_by_index",
                             "open_camera",           "init_camera",
                             "close_camera",          "get_control_caps",
                             "get_control_value",     "set_control_value",
                             "get_roi_format",        "set_roi_format",
                             "get_start_pos",         "set_start_pos",
                             "start_exposure",        "stop_exposure",
                             "get_exposure_status",   "get_data_after_exposure",
                             "pulse_guide_on",        "pulse_guide_off",
                             "get_serial_number",     "get_sdk_version"}) {
        INFO(name);
        CHECK(fake.call_count(name) >= 1);
    }
}

// ---- The camera opens through the shared open table (issue #1667) ----

// Falsified by: SharedOpenTable::close() acting on a link that is not the open's current one (a close from a holder
// of an earlier open closes the handle a later Connect opened).
TEST_CASE("ZWO fake SDK - a stale close does not close another holder's open", "[zwo][camera][unit][link]") {
    FakeZWOSDK sdk;
    const int id = sdk.camera.camera_id;
    const auto first = sdk.open_camera_link(id);
    sdk.close_camera_link(id, first.link);
    CHECK(sdk.open_count() == 0);

    const auto second = sdk.open_camera_link(id);
    REQUIRE(second.link != first.link);
    sdk.close_camera_link(id, first.link);  // the earlier holder closes again
    CHECK(sdk.open_count() == 1);
    CHECK(sdk.call_count("close_camera") == 1);
    sdk.close_camera_link(id, second.link);
    CHECK(sdk.open_count() == 0);
}

// Falsified by: SharedOpenTable::open() sharing a held id without reading its serial (the dead handle of a replugged
// unit, or another camera behind the same id, is handed to the new holder).
TEST_CASE("ZWO fake SDK - an open of a held id whose serial changed closes it and reopens",
          "[zwo][camera][unit][link]") {
    FakeZWOSDK sdk;
    const int id = sdk.camera.camera_id;
    const auto first = sdk.open_camera_link(id);
    CHECK(first.serial == "FAKE-ZWO-17");
    sdk.serial = "FAKE-ZWO-99";

    const auto second = sdk.open_camera_link(id);

    CHECK(second.serial == "FAKE-ZWO-99");
    CHECK(second.link != first.link);
    CHECK(sdk.call_count("close_camera") == 1);
    CHECK(sdk.call_count("init_camera") == 2);
    CHECK(sdk.open_count() == 1);
    // The first holder's link is retired: its call is refused.
    CHECK(error_code([&] { sdk.with_link(id, first.link, [&] { (void)sdk.get_roi_format(id); }); }) ==
          AlpacaError::NotConnected);
}

// Finding on the old wrapper: close_camera() threw before it erased its bookkeeping, so after a failed close the next
// open skipped ASIInitCamera. Falsified by: SharedOpenTable::close() erasing the entry only after real_close()
// succeeded (the reopen shares the dead entry and the real open, which includes the init, never runs).
TEST_CASE("ZWO fake SDK - a close that throws still lets the next open initialise the camera",
          "[zwo][camera][unit][link]") {
    FakeZWOSDK sdk;
    const int id = sdk.camera.camera_id;
    const auto first = sdk.open_camera_link(id);
    sdk.fail_close = true;
    CHECK(error_code([&] { sdk.close_camera_link(id, first.link); }) == AlpacaError::DriverException);
    sdk.fail_close = false;

    const auto again = sdk.open_camera_link(id);

    CHECK(again.link != first.link);
    CHECK(sdk.call_count("init_camera") == 2);
    CHECK(sdk.holders(id) == 1);
    sdk.close_camera_link(id, again.link);
}

// Falsified by: fake_zwo_sdk.h get_control_value() checking the enumeration after every not-readable answer (the
// unreadable bandwidth control asks it too), where the wrapper checks only after a generic code (issue #1667).
TEST_CASE("FakeZWOSDK - only a generic not-readable control answer asks the enumeration",
          "[zwo][camera][unit][fake-sdk]") {
    FakeZWOSDK sdk;
    const int id = sdk.camera.camera_id;
    const auto opened = sdk.open_camera_link(id);
    long value = 0;
    bool is_auto = false;

    sdk.fail_bandwidth_read = true;  // a control the camera cannot read: no evidence of a pull
    sdk.with_link(id, opened.link, [&] {
        CHECK_FALSE(sdk.get_control_value(id, FakeZWOSDK::ControlType::BandwidthOverload, value, is_auto));
    });
    CHECK(sdk.call_count("listed") == 0);

    sdk.fail_heater_read = true;  // a generic code: the camera list is asked, and still lists it
    sdk.with_link(id, opened.link, [&] {
        CHECK_FALSE(sdk.get_control_value(id, FakeZWOSDK::ControlType::AntiDewHeater, value, is_auto));
    });
    CHECK(sdk.call_count("listed") == 1);
    sdk.close_camera_link(id, opened.link);
}

// User report (5.0.2, issue #1958): the SDK gives the bound camera's id to another body while the bound one is away,
// and the disconnected name was read by that id. Falsified by: zwo_camera_driver.cpp
// refresh_cached_camera_info_if_needed() adopting the by-id read without comparing its model to
// expected_model().
TEST_CASE("ZWO fake SDK - the disconnected name follows the bound camera, not another one at its id",
          "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.name = "ZWO ASI432MM";
    sdk.serial = "aaaa";
    alpacacore::vendor::zwo::ZwoCameraBinding binding;
    binding.identity.serial = "aaaa";
    binding.identity.camera_name = "ZWO ASI432MM";
    binding.identity.camera_id = sdk.camera.camera_id;
    binding.unique_id = "ZWO_UID_0123456789abcdef";
    auto camera = alpacacore::vendor::zwo::create_zwo_camera_bound(1, binding, sdk);
    // The real by-id read needs no open (issue #738); the fake answers only for an open id.
    const auto held = sdk.open_camera_link(sdk.camera.camera_id);
    CHECK(camera->get_name() == "ZWO ASI432MM");

    sdk.camera.name = "ZWO ASI533MC Pro";
    sdk.serial = "bbbb";
    CHECK(camera->get_name() == "ZWO ASI432MM");
    sdk.close_camera_link(sdk.camera.camera_id, held.link);
}

// With nothing read from the SDK yet, the card shows the stored model rather than the generic name (issue #1958).
// Falsified by: zwo_camera_driver.cpp get_name() returning "ZWO Camera" without consulting expected_model().
TEST_CASE("ZWO fake SDK - a camera that never answered by id is named by its stored model", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;  // no open link: the fake's by-id read answers nothing
    alpacacore::vendor::zwo::ZwoCameraBinding binding;
    binding.identity.serial = "aaaa";
    binding.identity.camera_name = "ZWO ASI432MM ";  // padded, as the SDK pads model names
    binding.unique_id = "ZWO_UID_0123456789abcded";
    auto camera = alpacacore::vendor::zwo::create_zwo_camera_bound(3, binding, sdk);
    CHECK(camera->get_name() == "ZWO ASI432MM");

    alpacacore::vendor::zwo::ZwoCameraBinding unnamed;
    unnamed.identity.camera_index = 4;
    unnamed.unique_id = "ZWO_UID_0123456789abcdec";
    auto generic = alpacacore::vendor::zwo::create_zwo_camera_bound(4, unnamed, sdk);
    CHECK(generic->get_name() == "ZWO Camera");
}

// The preload at construction applies the same rule as the refresh (issue #1958). Falsified by:
// zwo_camera_driver.cpp preload_camera_info_locked() adopting the by-id read without comparing its model.
TEST_CASE("ZWO fake SDK - the construction preload ignores another camera at the bound id", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.camera.name = "ZWO ASI533MC Pro";  // the camera that now holds the id
    alpacacore::vendor::zwo::ZwoCameraBinding binding;
    binding.identity.serial = "aaaa";
    binding.identity.camera_name = "ZWO ASI432MM";
    binding.identity.camera_id = sdk.camera.camera_id;
    binding.unique_id = "ZWO_UID_0123456789abcdeb";
    auto held = sdk.open_camera_link(sdk.camera.camera_id);
    auto camera = alpacacore::vendor::zwo::create_zwo_camera_bound(5, binding, sdk);
    // Close before the first Name read, so only the preload could have cached a model.
    sdk.close_camera_link(sdk.camera.camera_id, held.link);
    CHECK(camera->get_name() == "ZWO ASI432MM");
    CHECK(camera->get_sensor_name().empty());
}

// ---- The exposure runs in a util::CaptureSession body (decision 0020, issue #1981) ----

// Rules 4 and 5: the retries run in the body; with none left the body fails the exposure, and ImageReady, ImageArray
// and an idle AbortExposure or StopExposure leave that failure until the next StartExposure clears it.
TEST_CASE("ZWO fake SDK - a failed exposure keeps its message until the next StartExposure",
          "[zwo][camera][unit][capture-session]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    sdk.script_exposure_status(
        {FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed});
    camera->start_exposure(0.01, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    CHECK(sdk.call_count("start_exposure") == 3);  // the first attempt and two retries

    const auto expect_failure = [&] {
        std::string message;
        try {
            static_cast<void>(camera->get_image_ready());
        } catch (const AlpacaException& ex) {
            CHECK(ex.error_code() == AlpacaError::DriverException);
            message = ex.what();
        }
        CHECK(message.find("camera exposure failed after SDK retries") != std::string::npos);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
        CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);
    };
    expect_failure();
    const int stops = sdk.call_count("stop_exposure");
    camera->abort_exposure();
    expect_failure();
    camera->stop_exposure();
    expect_failure();
    CHECK(sdk.call_count("stop_exposure") == stops);  // idle: no SDK call

    camera->start_exposure(0.01, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera, clock));
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

// Rule 5: AbortExposure is refused in Download and sends no SDK stop; the download completes and its frame is
// published.
TEST_CASE("ZWO fake SDK - AbortExposure during the download is refused and the frame arrives",
          "[zwo][camera][unit][capture-session]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.hold_download();
    camera->start_exposure(0.001, true);
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    const auto state = camera->get_camera_state();
    const int abort_code = error_code([&] { camera->abort_exposure(); });
    const int stop_calls = sdk.call_count("stop_exposure");
    sdk.release_download();

    REQUIRE(download_started);
    CHECK(state == alpacacore::CameraState::Download);
    CHECK(abort_code == AlpacaError::InvalidOperation);
    CHECK(stop_calls == 0);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
}

// Rule 5: StopExposure in Download returns without effect: no SDK stop, and the downloaded frame is published.
TEST_CASE("ZWO fake SDK - StopExposure during the download is ignored", "[zwo][camera][unit][capture-session]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.hold_download();
    camera->start_exposure(0.001, true);
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    const int stop_code = error_code([&] { camera->stop_exposure(); });
    const int stop_calls = sdk.call_count("stop_exposure");
    sdk.release_download();

    REQUIRE(download_started);
    CHECK(stop_code == -1);
    CHECK(stop_calls == 0);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

// Rule 7.3: the body that met the link loss failed the exposure; the next Connect joins it and clears the kept failure
// and the last exposure.
TEST_CASE("ZWO fake SDK - a reconnect after a link loss clears the kept failure and the last exposure",
          "[zwo][camera][unit][capture-session][link]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    REQUIRE(camera->get_last_exposure_duration() == 0.001);

    sdk.auto_complete_exposure = false;
    camera->start_exposure(30.0, true);
    sdk.removed = true;  // the body's next status poll meets the removal
    CHECK(alpacacore::test::zwo_eventually([&] { return !camera->get_connected(); }));

    camera->set_connected(false);
    sdk.replug({{sdk.camera.camera_id, sdk.serial}});
    camera->set_connected(true);
    REQUIRE(camera->get_connected());
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK_FALSE(camera->get_image_ready());  // no kept failure
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); }) ==
          AlpacaError::InvalidOperation);
    camera->set_connected(false);
}

// Rule 8: a geometry write is refused while the exposure runs; with a Ready frame it drops the frame and keeps
// LastExposure*.
TEST_CASE("ZWO fake SDK - a geometry write is refused while exposing and drops a Ready frame",
          "[zwo][camera][unit][capture-session]") {
    FakeZWOSDK sdk;
    sdk.auto_complete_exposure = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_call_count("get_exposure_status", 1, std::chrono::seconds(2)));  // triggered, now polling
    CHECK(error_code([&] { camera->set_bin_x(2); }) == AlpacaError::InvalidOperation);
    CHECK(error_code([&] { camera->set_num_x(7); }) == AlpacaError::InvalidOperation);
    CHECK(error_code([&] { camera->set_start_x(1); }) == AlpacaError::InvalidOperation);
    CHECK(camera->get_bin_x() == 1);

    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    REQUIRE(camera->get_image_ready());
    const auto start = camera->get_last_exposure_start_time();
    CHECK(error_code([&] { camera->set_num_x(7); }) == -1);
    CHECK(camera->get_num_x() == 7);
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
    CHECK(camera->get_last_exposure_duration() == 0.001);
    CHECK(camera->get_last_exposure_start_time() == start);
}
