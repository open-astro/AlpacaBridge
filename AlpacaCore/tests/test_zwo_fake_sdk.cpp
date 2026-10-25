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
    camera->start_exposure(0.01, true);
    sdk.set_exposure_status_error("first acquisition failed");
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    expect_unset(AlpacaError::InvalidOperation);
    sdk.set_exposure_status_error("");

    camera->start_exposure(0.0100006, true);
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    REQUIRE(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 0);
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
    camera->start_exposure(0.02, true);
    CHECK_FALSE(camera->get_image_ready());
    expect_a();
    camera->abort_exposure();
    expect_a();
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
    camera->start_exposure(0.03, true);
    sdk.set_exposure_status_error("later acquisition failed");
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    expect_a();
    sdk.set_exposure_status_error("");
    camera->start_exposure(0.04, true);
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.04);
    CHECK(camera->get_last_exposure_start_time() > start_a);

    // Acquisition is validated SDK Success, not the later lazy USB transfer.
    const auto start_b = camera->get_last_exposure_start_time();
    sdk.download_error = "late transfer failure";
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
    std::chrono::system_clock::time_point setup_finished, trigger_entered;
    bool observe_retry_setup = false;
    double retry_setup_progress = -1.0;
    sdk.before_call = [&](const std::string& call) {
        if (call == "set_control_value") {
            if (observe_retry_setup) retry_setup_progress = camera->get_percent_completed();
            setup_finished = std::chrono::system_clock::now();
        }
        if (call == "start_exposure") trigger_entered = std::chrono::system_clock::now();
    };
    camera->start_exposure(0.0200006, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_start_time() >= setup_finished);
    CHECK(camera->get_last_exposure_start_time() <= trigger_entered);
    CHECK(camera->get_last_exposure_duration() == 0.020001);
    const auto start_a = camera->get_last_exposure_start_time();

    sdk.auto_complete_exposure = false;
    camera->start_exposure(2.0000006, true);
    clock.advance(std::chrono::seconds(3));
    observe_retry_setup = true;
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
    // Direct retrieval drives the retry but remains InvalidOperation until an image is ready.
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
    CHECK(retry_setup_progress == 0.0);
    CHECK(camera->get_last_exposure_start_time() == start_a);
    clock.advance(std::chrono::milliseconds(500));
    CHECK(camera->get_percent_completed() == Catch::Approx(50.0 / 2.000001));
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_start_time() >= setup_finished);
    CHECK(camera->get_last_exposure_start_time() <= trigger_entered);
    CHECK(camera->get_last_exposure_duration() == 2.000001);
    CHECK(sdk.call_count("start_exposure") == 3);
    CHECK(sdk.call_count("get_data_after_exposure") == 0);
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
            REQUIRE(camera->get_image_ready());
        }
        std::chrono::system_clock::time_point prior_start{};
        if (prior_success) prior_start = camera->get_last_exposure_start_time();
        camera->start_exposure(0.02, true);
        for (int retry = 0; retry < 2; ++retry) {
            sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
            CHECK_FALSE(camera->get_image_ready());
            clock.advance(std::chrono::milliseconds(250));
        }
        sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
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

TEST_CASE("ZWO fake SDK - direct retrieval publishes at validated SDK success", "[zwo][camera][unit][last-exposure]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    sdk.fail_exposure_start = true;
    CHECK(error_code([&] { camera->start_exposure(0.01, true); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); }) ==
          AlpacaError::InvalidOperation);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_start_time()); }) ==
          AlpacaError::InvalidOperation);
    camera->start_exposure(0.02, true);
    sdk.roi_readback_override = FakeZWOSDK::ROIDescription{0, 8, 1, FakeZWOSDK::ImageType::Raw16};
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); }) ==
          AlpacaError::InvalidOperation);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_start_time()); }) ==
          AlpacaError::InvalidOperation);
    sdk.roi_readback_override.reset();
    camera->start_exposure(0.03, true);
    const auto image = camera->get_image_array();  // no ImageReady prerequisite
    CHECK(image.data.front() == 1);
    CHECK(camera->get_last_exposure_duration() == 0.03);
    camera->start_exposure(0.04, true);
    sdk.download_error = "transfer failed after acquisition";
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(camera->get_last_exposure_duration() == 0.04);
    CHECK(camera->get_last_exposure_start_time() != std::chrono::system_clock::time_point{});
}

TEST_CASE("ZWO fake SDK - retry trigger failure stays failed without publishing",
          "[zwo][camera][unit][last-exposure]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    camera->start_exposure(0.01, true);
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
    sdk.fail_exposure_start = true;
    SECTION("ImageReady drives the failed retry") {
        CHECK_FALSE(camera->get_image_ready());  // retry replaced the polled generation
    }
    SECTION("Direct ImageArray reports the failed retry on its first read") {
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    }
    const int starts = sdk.call_count("start_exposure");
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
    camera->start_exposure(0.02, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.02);
}

TEST_CASE("ZWO fake SDK - stale failed poll cannot retry a newer exposure", "[zwo][camera][unit][last-exposure]") {
    FakeZWOSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    clock.advance(std::chrono::seconds(1));
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    connect_camera(*camera);
    camera->start_exposure(0.01, true);
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    std::atomic<int> polls{0};
    sdk.before_call = [&](const std::string& call) {
        if (call != "get_exposure_status" || polls.fetch_add(1) != 0) return;
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    std::atomic<bool> old_ready{true};
    std::thread reader([&] { old_ready.store(camera->get_image_ready()); });
    bool saw_poll;
    {
        std::unique_lock lock(mutex);
        saw_poll = cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    // Always release/join before a fatal assertion, including a failed rendezvous.
    if (saw_poll) {
        camera->start_exposure(0.02, true);
        sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Failed);
    }
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    reader.join();
    REQUIRE(saw_poll);
    CHECK_FALSE(old_ready.load());
    CHECK(sdk.call_count("start_exposure") == 2);
    CHECK(sdk.call_count("stop_exposure") == 0);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_duration()); }) ==
          AlpacaError::InvalidOperation);
    CHECK(error_code([&] { static_cast<void>(camera->get_last_exposure_start_time()); }) ==
          AlpacaError::InvalidOperation);
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_last_exposure_duration() == 0.02);
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

    CHECK(camera->get_image_ready());
    const int roi_reads = sdk.call_count("get_roi_format");
    const int start_reads = sdk.call_count("get_start_pos");
    CHECK(camera->get_image_ready());
    static_cast<void>(camera->get_camera_state());
    CHECK(sdk.call_count("get_roi_format") == roi_reads);
    CHECK(sdk.call_count("get_start_pos") == start_reads);
    CHECK(sdk.call_count("get_data_after_exposure") == 0);  // readiness never starts the USB bulk transfer
    CHECK(error_code([&] { camera->set_num_x(8); }) == AlpacaError::InvalidOperation);

    const auto image = camera->get_image_array();
    REQUIRE(image.rank == 2);
    REQUIRE(image.width == 7);
    REQUIRE(image.height == 3);
    CHECK(image.data[0] == 10);
    CHECK(image.data[6] == 16);
    CHECK(image.data[14] == 26);
    CHECK(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    camera->set_num_x(8);
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

TEST_CASE("ZWO fake SDK - failed exposure replacement preserves the previous image", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    const auto previous = camera->get_image_array();
    REQUIRE(camera->get_image_ready());

    sdk.fail_exposure_register_write = true;
    CHECK(error_code([&] { camera->start_exposure(0.002, true); }) == AlpacaError::DriverException);
    CHECK(camera->get_last_exposure_duration() == 0.001);
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data == previous.data);

    sdk.fail_exposure_start = true;
    CHECK(error_code([&] { camera->start_exposure(0.003, true); }) == AlpacaError::DriverException);
    CHECK(camera->get_last_exposure_duration() == 0.001);
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data == previous.data);
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
    CHECK(camera->get_image_ready());
    CHECK_FALSE(camera->get_image_array().data.empty());
}

TEST_CASE("ZWO fake SDK - overlapping start is rejected without replacing the active exposure", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.auto_complete_exposure = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);

    CHECK_FALSE(camera->get_image_ready());
    const int starts = sdk.call_count("start_exposure");
    CHECK(error_code([&] { camera->start_exposure(0.002, true); }) == AlpacaError::InvalidOperation);
    CHECK(sdk.call_count("start_exposure") == starts);
    CHECK_FALSE(camera->get_image_ready());

    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    CHECK(camera->get_image_ready());

    camera->start_exposure(0.001, true);
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool readback_entered = false;
    bool release_readback = false;
    bool start_entered = false;
    bool release_start = false;
    std::atomic<int> status_calls{0};
    sdk.before_call = [&](const std::string& name) {
        if (name == "get_exposure_status" && status_calls.fetch_add(1) == 0) {
            std::unique_lock lock(gate_mutex);
            readback_entered = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release_readback; });
        } else if (name == "start_exposure") {
            std::unique_lock lock(gate_mutex);
            start_entered = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release_start; });
        }
    };

    std::atomic<int> array_code{0};
    std::atomic<bool> array_done{false};
    std::thread reader([&] {
        array_code.store(error_code([&] { static_cast<void>(camera->get_image_array()); }));
        array_done.store(true);
    });
    bool poll_held = false;
    {
        std::unique_lock lock(gate_mutex);
        poll_held = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return readback_entered; });
    }
    std::atomic<int> start_code{0};
    std::thread starter;
    bool trigger_held = false;
    if (poll_held) {
        starter = std::thread([&] { start_code.store(error_code([&] { camera->start_exposure(0.002, true); })); });
        std::unique_lock lock(gate_mutex);
        trigger_held = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return start_entered; });
    }
    std::atomic<bool> ready_during_start{true};
    const int ready_start_code =
        trigger_held ? error_code([&] { ready_during_start.store(camera->get_image_ready()); }) : 0;
    {
        std::lock_guard lock(gate_mutex);
        release_readback = true;
    }
    gate_cv.notify_all();
    const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (poll_held && trigger_held && !array_done.load() && std::chrono::steady_clock::now() < read_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool array_rejected_before_start_finished = array_done.load() && !release_start;
    const int data_calls_before_release = sdk.call_count("get_data_after_exposure");
    {
        std::lock_guard lock(gate_mutex);
        release_start = true;
    }
    gate_cv.notify_all();
    reader.join();
    if (starter.joinable()) starter.join();

    CHECK(poll_held);
    CHECK(trigger_held);
    CHECK(array_rejected_before_start_finished);
    CHECK(array_code.load() == AlpacaError::InvalidOperation);
    CHECK(data_calls_before_release == 0);
    CHECK(start_code.load() == -1);
    if (trigger_held) {
        CHECK(ready_start_code == -1);
        CHECK_FALSE(ready_during_start.load());
    }
}

TEST_CASE("ZWO fake SDK - a status poll cannot publish ready during a new exposure start", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.auto_complete_exposure = false;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool poll_entered = false;
    bool release_poll = false;
    bool start_entered = false;
    bool release_start = false;
    std::atomic<int> status_calls{0};
    sdk.before_call = [&](const std::string& name) {
        if (name == "get_exposure_status" && status_calls.fetch_add(1) == 0) {
            std::unique_lock lock(gate_mutex);
            poll_entered = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release_poll; });
        } else if (name == "start_exposure") {
            std::unique_lock lock(gate_mutex);
            start_entered = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release_start; });
        }
    };

    std::atomic<int> ready_code{0};
    std::atomic<bool> ready_value{true};
    std::atomic<bool> ready_done{false};
    std::thread poller([&] {
        ready_code.store(error_code([&] { ready_value.store(camera->get_image_ready()); }));
        ready_done.store(true);
    });
    bool poll_held = false;
    {
        std::unique_lock lock(gate_mutex);
        poll_held = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return poll_entered; });
    }

    std::atomic<int> start_code{0};
    std::thread starter;
    bool trigger_held = false;
    if (poll_held) {
        starter = std::thread([&] { start_code.store(error_code([&] { camera->start_exposure(0.002, true); })); });
        std::unique_lock lock(gate_mutex);
        trigger_held = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return start_entered; });
    }

    std::atomic<bool> ready_during_start{true};
    const int immediate_ready_code =
        trigger_held ? error_code([&] { ready_during_start.store(camera->get_image_ready()); }) : 0;
    {
        std::lock_guard lock(gate_mutex);
        release_poll = true;
    }
    gate_cv.notify_all();
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (poll_held && trigger_held && !ready_done.load() && std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool poll_finished_during_start = ready_done.load() && !release_start;
    {
        std::lock_guard lock(gate_mutex);
        release_start = true;
    }
    gate_cv.notify_all();
    poller.join();
    if (starter.joinable()) starter.join();

    CHECK(poll_held);
    CHECK(trigger_held);
    CHECK(poll_finished_during_start);
    CHECK(ready_code.load() == -1);
    CHECK_FALSE(ready_value.load());
    CHECK(immediate_ready_code == -1);
    CHECK_FALSE(ready_during_start.load());
    CHECK(start_code.load() == -1);
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

    REQUIRE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);

    camera->abort_exposure();
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);

    sdk.download_error.clear();
    sdk.frame_bytes = make_raw16_frame(16, 8);
    camera->start_exposure(0.001, true);
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
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);

    sdk.set_exposure_status_error("");
    camera->start_exposure(0.001, true);
    sdk.script_exposure_status(
        {FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed, FakeZWOSDK::ExposureStatus::Failed});
    CHECK_FALSE(camera->get_image_ready());
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    CHECK_FALSE(camera->get_image_ready());
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);

    camera->start_exposure(0.001, true);
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
        camera->start_exposure(0.001, true);

        if (failure == Failure::Status) {
            sdk.set_exposure_status_error("scripted status-read failure");
            CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        } else if (failure == Failure::Roi) {
            sdk.roi_readback_override = FakeZWOSDK::ROIDescription{0, 8, 1, FakeZWOSDK::ImageType::Raw16};
            CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        } else {
            sdk.download_error = "scripted bulk-transfer failure";
            REQUIRE(camera->get_image_ready());
            CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
        }

        camera->set_connected(false);
        sdk.set_exposure_status_error("");
        sdk.roi_readback_override.reset();
        sdk.download_error.clear();
        camera->set_connected(true);
        CHECK_FALSE(camera->get_image_ready());
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);

        camera->start_exposure(0.001, true);
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

        const auto image = camera->get_image_array();
        CHECK(image.rank == 2);
        CHECK(image.data.front() == 0x5a);
    }
}

TEST_CASE("ZWO fake SDK - ImageReady cannot pass an in-flight poll after download failure", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.download_error = "scripted bulk-transfer failure";
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool poll_entered = false;
    bool release_poll = false;
    std::atomic<int> status_calls{0};
    sdk.before_call = [&](const std::string& name) {
        if (name != "get_exposure_status" || status_calls.fetch_add(1) != 0) return;
        std::unique_lock lock(gate_mutex);
        poll_entered = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_poll; });
    };

    std::atomic<int> ready_error{0};
    std::thread ready_reader([&] {
        try {
            static_cast<void>(camera->get_image_ready());
        } catch (const AlpacaException& ex) {
            ready_error.store(ex.error_code());
        }
    });
    bool first_poll_held = false;
    {
        std::unique_lock lock(gate_mutex);
        first_poll_held = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return poll_entered; });
    }
    if (!first_poll_held) {
        {
            std::lock_guard lock(gate_mutex);
            release_poll = true;
        }
        gate_cv.notify_all();
        ready_reader.join();
        CHECK(first_poll_held);
        return;
    }

    const int array_error = error_code([&] { static_cast<void>(camera->get_image_array()); });
    const bool download_seen = sdk.wait_for_call_count("get_data_after_exposure", 1, std::chrono::seconds(2));
    {
        std::lock_guard lock(gate_mutex);
        release_poll = true;
    }
    gate_cv.notify_all();
    ready_reader.join();

    CHECK(download_seen);
    CHECK(array_error == AlpacaError::DriverException);
    CHECK(ready_error.load() == AlpacaError::DriverException);
}

TEST_CASE("ZWO fake SDK - concurrent ImageArray reads share one lazy transfer", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    sdk.hold_download();

    alpacacore::ImageArray first;
    alpacacore::ImageArray second;
    std::atomic<int> first_code{0};
    std::atomic<int> second_code{0};
    std::atomic<bool> second_started{false};
    std::thread first_reader([&] {
        try {
            first = camera->get_image_array();
        } catch (const AlpacaException& ex) {
            first_code.store(ex.error_code());
        }
    });
    const bool transfer_started = sdk.wait_for_download(std::chrono::seconds(2));
    std::thread second_reader([&] {
        second_started.store(true);
        try {
            second = camera->get_image_array();
        } catch (const AlpacaException& ex) {
            second_code.store(ex.error_code());
        }
    });
    while (!second_started.load()) std::this_thread::yield();
    const bool second_reached_sdk = sdk.wait_for_call_count("get_roi_format", 3, std::chrono::milliseconds(100));
    CHECK_FALSE(second_reached_sdk);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    sdk.release_download();
    first_reader.join();
    second_reader.join();

    CHECK(transfer_started);
    CHECK(first_code.load() == 0);
    CHECK(second_code.load() == 0);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    CHECK(first.data == second.data);
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
        CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
        CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
        CHECK(sdk.call_count("get_data_after_exposure") == 0);
    }

    sdk.roi_readback_override.reset();
    sdk.start_readback_override = FakeZWOSDK::StartPosition{1, 0};
    camera->start_exposure(0.001, true);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 0);

    sdk.start_readback_override.reset();
    sdk.frame_bytes = make_raw16_frame(16, 8);
    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - abort rejects an in-flight lazy download", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    sdk.hold_download();

    std::atomic<int> result{-1};
    std::thread download([&] { result.store(error_code([&] { static_cast<void>(camera->get_image_array()); })); });
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    const int abort_code = error_code([&] { camera->abort_exposure(); });
    const int stop_calls = sdk.call_count("stop_exposure");
    sdk.release_download();
    download.join();

    CHECK(download_started);
    CHECK(abort_code == AlpacaError::InvalidOperation);
    CHECK(stop_calls == 0);
    CHECK(result.load() == -1);
    CHECK(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
    CHECK(sdk.call_count("get_data_after_exposure") == 1);

    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - stop leaves an in-flight lazy download alone", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    sdk.hold_download();

    std::atomic<int> result{0};
    std::thread download([&] { result.store(error_code([&] { static_cast<void>(camera->get_image_array()); })); });
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    const int stop_code = error_code([&] { camera->stop_exposure(); });
    const int stop_calls = sdk.call_count("stop_exposure");
    sdk.release_download();
    download.join();

    CHECK(download_started);
    CHECK(stop_code == -1);
    CHECK(stop_calls == 0);
    CHECK(result.load() == -1);
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
    camera->start_exposure(0.001, true);
    sdk.hold_download();

    std::atomic<int> download_code{0};
    std::atomic<bool> disconnected{false};
    std::thread download(
        [&] { download_code.store(error_code([&] { static_cast<void>(camera->get_image_array()); })); });
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
    download.join();
    disconnect.join();
    cached_getter.join();

    CHECK(download_started);
    CHECK(stop_seen);
    CHECK(close_waited);
    CHECK(cached_getter_unblocked);
    CHECK(cached_bin.load() == 1);
    CHECK(download_code.load() == AlpacaError::InvalidOperation);
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
    sdk.set_control_value(sdk.camera.camera_id, FakeZWOSDK::ControlType::Temperature, 215, false);

    CHECK(camera->get_ccd_temperature() == Catch::Approx(21.5));
    CHECK(sdk.call_count("get_control_value") >= 2);

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

    const auto start = std::chrono::steady_clock::now();
    CHECK(camera->get_ccd_temperature() == Catch::Approx(0.0));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(600));
    CHECK(sdk.call_count("get_control_value") == 6);

    // The retry is spent: the next read is a single SDK call.
    CHECK(camera->get_ccd_temperature() == Catch::Approx(0.0));
    CHECK(sdk.call_count("get_control_value") == 7);
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
    sdk.open_camera(id);
    CHECK(sdk.get_camera_info_by_id(id, info));
    CHECK(sdk.get_camera_info_by_index(0, info));
    CHECK_FALSE(sdk.get_camera_info_by_index(3, info));
    sdk.init_camera(id);
    CHECK(sdk.get_control_caps(id).size() == 9);
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
    CHECK(sdk.get_sdk_version() == "fake-zwo-sdk-1.0");
    sdk.close_camera(id);

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
