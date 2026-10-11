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
#include <alpacacore/vendor/svbony/svbony_frame_validation.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_svbony_sdk.h"

namespace {

using namespace std::chrono_literals;

void wait_until_ready(alpacacore::CameraDriver& camera) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (camera.get_image_ready()) return;
        std::this_thread::sleep_for(1ms);
    }
    FAIL("SVBONY exposure did not become ready");
}

void wait_for_driver_exception(alpacacore::CameraDriver& camera, std::string_view expected_message) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            static_cast<void>(camera.get_image_ready());
        } catch (const alpacacore::AlpacaException& ex) {
            REQUIRE(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find(expected_message) != std::string::npos);
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    FAIL("SVBONY exposure failure was not published");
}

void require_array_failure(alpacacore::CameraDriver& camera) {
    try {
        static_cast<void>(camera.get_image_array());
        FAIL("ImageArray should report the failed exposure");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
}

void require_alpaca_error(const std::function<void()>& operation, int expected) {
    try {
        operation();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == expected);
    }
}

std::unique_ptr<alpacacore::CameraDriver> connected_camera(alpacacore::test::FakeSVBSDK& sdk) {
    auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    camera->set_connected(true);
    REQUIRE(camera->get_connected());
    return camera;
}

struct ReleaseVideoDataOnExit {
    alpacacore::test::FakeSVBSDK& sdk;
    ~ReleaseVideoDataOnExit() { sdk.release_video_data(); }
};

}  // namespace

TEST_CASE("SVBONY Camera - fake SDK publishes a complete frame", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    const auto image = camera->get_image_array();

    CHECK(image.rank == 2);
    CHECK(image.width == 16);
    CHECK(image.height == 8);
    REQUIRE(image.data.size() == 128);
    CHECK(image.data[0] == 256);
    CHECK(image.data[1] == 770);
    CHECK(sdk.open_count() == 1);
    const auto events = sdk.events();
    const auto event_index = [&events](const std::string& event) {
        return static_cast<std::size_t>(std::distance(events.begin(), std::find(events.begin(), events.end(), event)));
    };
    CHECK(event_index("get_roi_format") < event_index("get_output_image_type"));
    CHECK(event_index("get_output_image_type") < event_index("start_video_capture"));
    CHECK(event_index("start_video_capture") < event_index("get_video_data"));
}

TEST_CASE("SVBONY Camera - transient frame-read failure is retried", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.timeout_next_video_data();
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(sdk.call_count("get_video_data") == 2);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - frame reads use a finite SDK wait", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(sdk.last_video_wait_ms() == 500);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - aligned ROI is cropped to the requested shape", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    camera->set_num_x(9);
    camera->set_num_y(5);

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    const auto image = camera->get_image_array();

    CHECK(image.rank == 2);
    CHECK(image.width == 9);
    CHECK(image.height == 5);
    REQUIRE(image.data.size() == 45);
    CHECK(image.data[8] == 0x1110);
    CHECK(image.data[9] == 0x2120);
}

TEST_CASE("SVBONY Camera - RGB24 channels are reordered for Alpaca", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    sdk.set_supported_formats({alpacacore::vendor::svbony::SVBImageType::Rgb24});
    std::vector<std::uint8_t> frame(16 * 8 * 3);
    for (std::size_t i = 0; i < frame.size(); ++i) frame[i] = static_cast<std::uint8_t>(i);
    sdk.set_frame_data(std::move(frame));
    auto camera = connected_camera(sdk);

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    const auto image = camera->get_image_array();

    CHECK(image.rank == 3);
    CHECK(image.width == 16);
    CHECK(image.height == 8);
    REQUIRE(image.data.size() == 16 * 8 * 3);
    CHECK(image.data[0] == 2);
    CHECK(image.data[1] == 1);
    CHECK(image.data[2] == 0);
}

TEST_CASE("SVBONY Camera - connect selects only supported default formats", "[svbony][camera][unit]") {
    using alpacacore::vendor::svbony::SVBImageType;

    SECTION("Y-only format list chooses Y16") {
        alpacacore::test::FakeSVBSDK sdk;
        sdk.set_supported_formats({SVBImageType::Y8, SVBImageType::Y16});
        auto camera = connected_camera(sdk);
        CHECK(sdk.get_output_image_type(17) == SVBImageType::Y16);
        camera->set_connected(false);
        CHECK(sdk.open_count() == 0);
    }

    SECTION("color RAW16 preserves the full Bayer frame when supported") {
        alpacacore::test::FakeSVBSDK sdk;
        sdk.set_supported_formats({SVBImageType::Raw16, SVBImageType::Rgb24});
        auto camera = connected_camera(sdk);
        CHECK(sdk.get_output_image_type(17) == SVBImageType::Raw16);
        camera->set_connected(false);
        CHECK(sdk.open_count() == 0);
    }

    SECTION("Y16 is preferred over RGB24 for a monochrome sensor") {
        alpacacore::test::FakeSVBSDK sdk;
        sdk.set_color_sensor(false);
        sdk.set_supported_formats({SVBImageType::Y16, SVBImageType::Rgb24});
        auto camera = connected_camera(sdk);
        CHECK(sdk.get_output_image_type(17) == SVBImageType::Y16);
        camera->set_connected(false);
        CHECK(sdk.open_count() == 0);
    }

    SECTION("color RAW8 is preferred over Y16") {
        alpacacore::test::FakeSVBSDK sdk;
        sdk.set_supported_formats({SVBImageType::Raw8, SVBImageType::Y16});
        auto camera = connected_camera(sdk);
        CHECK(sdk.get_output_image_type(17) == SVBImageType::Raw8);
        camera->set_connected(false);
        CHECK(sdk.open_count() == 0);
    }

    SECTION("color RGB24 is preferred over Y16 when RAW output is unavailable") {
        alpacacore::test::FakeSVBSDK sdk;
        sdk.set_supported_formats({SVBImageType::Rgb24, SVBImageType::Y16});
        auto camera = connected_camera(sdk);
        CHECK(sdk.get_output_image_type(17) == SVBImageType::Rgb24);
        camera->set_connected(false);
        CHECK(sdk.open_count() == 0);
    }

    SECTION("empty formats refuse connect and balance SDK open") {
        alpacacore::test::FakeSVBSDK sdk;
        sdk.set_supported_formats({});
        auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
        try {
            camera->set_connected(true);
            FAIL("Connect should reject an empty supported-format list");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find("verify this camera is supported") != std::string::npos);
        }
        CHECK_FALSE(camera->get_connected());
        CHECK(sdk.open_count() == 0);
    }

    SECTION("unsupported-only formats refuse connect and balance SDK open") {
        alpacacore::test::FakeSVBSDK sdk;
        sdk.set_supported_formats({SVBImageType::Rgb32});
        auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
        CHECK_THROWS_AS(camera->set_connected(true), alpacacore::AlpacaException);
        CHECK_FALSE(camera->get_connected());
        CHECK(sdk.open_count() == 0);
    }
}

TEST_CASE("SVBONY Camera - exposure deadline watchdog reports blocked worker", "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.set_max_read_wait(16s);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};

    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));

    const auto deadline = std::chrono::steady_clock::now() + 17s;
    bool watchdog_seen = false;
    while (std::chrono::steady_clock::now() < deadline && !watchdog_seen) {
        try {
            static_cast<void>(camera->get_image_ready());
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find("completion deadline") != std::string::npos);
            watchdog_seen = true;
        }
        if (!watchdog_seen) std::this_thread::sleep_for(10ms);
    }
    REQUIRE(watchdog_seen);
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);
    const int watchdog_writes = sdk.call_count("set_roi_format") + sdk.call_count("set_output_image_type") +
                                sdk.call_count("set_control_value");
    require_alpaca_error([&] { camera->set_num_x(15); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { camera->set_num_y(7); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { camera->set_bin_x(2); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { camera->set_start_x(1); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { camera->set_gain(30); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { camera->set_offset(30); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { camera->set_fast_readout(true); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(sdk.call_count("set_roi_format") + sdk.call_count("set_output_image_type") +
              sdk.call_count("set_control_value") ==
          watchdog_writes);

    // Let the blocked SDK operation return late, but keep the stored failure
    // observable until the next StartExposure resets it.
    const int read_completions = sdk.completed_video_data_reads();
    const int writes_before_release = sdk.call_count("set_roi_format") + sdk.call_count("set_output_image_type") +
                                      sdk.call_count("set_control_value");
    sdk.release_video_data();
    REQUIRE(sdk.wait_for_completed_video_data(read_completions + 1, 2s));
    const auto worker_deadline = std::chrono::steady_clock::now() + 2s;
    bool worker_finished = false;
    while (std::chrono::steady_clock::now() < worker_deadline && !worker_finished) {
        try {
            camera->set_num_x(16);  // same value: probes worker completion without changing the configuration
            worker_finished = true;
        } catch (const alpacacore::AlpacaException& ex) {
            REQUIRE(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            std::this_thread::sleep_for(1ms);
        }
    }
    REQUIRE(worker_finished);
    CHECK(sdk.call_count("set_roi_format") + sdk.call_count("set_output_image_type") +
              sdk.call_count("set_control_value") ==
          writes_before_release);
    for (int attempt = 0; attempt < 3; ++attempt) {
        wait_for_driver_exception(*camera, "completion deadline");
        require_array_failure(*camera);
    }

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - late SDK failure cannot replace the watchdog result", "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.set_max_read_wait(16s);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));

    const auto deadline = std::chrono::steady_clock::now() + 17s;
    bool watchdog_seen = false;
    while (std::chrono::steady_clock::now() < deadline && !watchdog_seen) {
        try {
            static_cast<void>(camera->get_image_ready());
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find("completion deadline") != std::string::npos);
            watchdog_seen = true;
        }
        if (!watchdog_seen) std::this_thread::sleep_for(10ms);
    }
    REQUIRE(watchdog_seen);

    const int reads = sdk.completed_video_data_reads();
    sdk.fail_next_held_read();
    sdk.release_video_data();
    REQUIRE(sdk.wait_for_completed_video_data(reads + 1, 2s));
    const auto worker_deadline = std::chrono::steady_clock::now() + 2s;
    bool worker_finished = false;
    while (std::chrono::steady_clock::now() < worker_deadline && !worker_finished) {
        try {
            camera->set_num_x(16);
            worker_finished = true;
        } catch (const alpacacore::AlpacaException& ex) {
            REQUIRE(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            std::this_thread::sleep_for(1ms);
        }
    }
    REQUIRE(worker_finished);
    wait_for_driver_exception(*camera, "completion deadline");
    require_array_failure(*camera);
}

TEST_CASE("SVBONY Camera - repeated frame timeouts fail through both readers and recover", "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.timeout_video_data();
    camera->start_exposure(0.001, true);

    const auto deadline = std::chrono::steady_clock::now() + 12s;
    bool timeout_seen = false;
    while (std::chrono::steady_clock::now() < deadline && !timeout_seen) {
        try {
            static_cast<void>(camera->get_image_ready());
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find("did not deliver a frame before the timeout") != std::string::npos);
            timeout_seen = true;
        }
        if (!timeout_seen) std::this_thread::sleep_for(10ms);
    }
    REQUIRE(timeout_seen);
    try {
        static_cast<void>(camera->get_image_array());
        FAIL("ImageArray should report the ordinary frame timeout");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("did not deliver a frame before the timeout") != std::string::npos);
    }
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);

    sdk.timeout_video_data(false);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - abort wakes a held read and the next frame recovers", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};

    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    camera->abort_exposure();

    CHECK(camera->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK_FALSE(camera->get_image_ready());
    const auto events = sdk.events();
    const auto stop = std::find(events.begin(), events.end(), "stop_video_capture");
    const auto read_complete = std::find(events.begin(), events.end(), "get_video_data_complete");
    REQUIRE(stop != events.end());
    REQUIRE(read_complete != events.end());
    CHECK(stop < read_complete);
    CHECK(sdk.open_count() == 1);

    sdk.release_video_data();
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - replacement exposure joins the old read before changing controls",
          "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};

    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    camera->start_exposure(0.002, true);
    const auto events = sdk.events();
    const auto first_read_complete = std::find(events.begin(), events.end(), "get_video_data_complete");
    REQUIRE(first_read_complete != events.end());
    const auto second_exposure_write = std::find(first_read_complete + 1, events.end(), "set_control_value.Exposure");
    REQUIRE(second_exposure_write != events.end());
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - disconnect joins a held read before close and clears failure state",
          "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    camera->set_connected(false);
    CHECK(sdk.open_count() == 0);
    const auto events = sdk.events();
    const auto read_complete = std::find(events.begin(), events.end(), "get_video_data_complete");
    const auto close = std::find(events.begin(), events.end(), "close_camera");
    REQUIRE(read_complete != events.end());
    REQUIRE(close != events.end());
    CHECK(read_complete < close);

    sdk.release_video_data();
    camera->set_connected(true);
    CHECK_FALSE(camera->get_image_ready());
    require_alpaca_error([&] { static_cast<void>(camera->get_image_array()); },
                         alpacacore::AlpacaError::InvalidOperation);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
    camera->set_connected(false);
    CHECK(sdk.open_count() == 0);
}

TEST_CASE("SVBONY Camera - asynchronous disconnect joins a held read before close", "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));

    camera->disconnect();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && (camera->get_connecting() || camera->get_connected())) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK_FALSE(camera->get_connecting());
    CHECK_FALSE(camera->get_connected());
    CHECK(sdk.open_count() == 0);
    const auto events = sdk.events();
    const auto read_complete = std::find(events.begin(), events.end(), "get_video_data_complete");
    const auto close = std::find(events.begin(), events.end(), "close_camera");
    REQUIRE(read_complete != events.end());
    REQUIRE(close != events.end());
    CHECK(read_complete < close);

    sdk.release_video_data();
    camera->set_connected(true);
    CHECK_FALSE(camera->get_image_ready());
    require_alpaca_error([&] { static_cast<void>(camera->get_image_array()); },
                         alpacacore::AlpacaError::InvalidOperation);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
    camera->set_connected(false);
}

TEST_CASE("SVBONY Camera - abort reports stop failure and later recovers", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};

    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    sdk.fail_next("stop_video_capture");
    require_alpaca_error([&] { camera->abort_exposure(); }, alpacacore::AlpacaError::DriverException);

    const int reads = sdk.completed_video_data_reads();
    sdk.release_video_data();
    REQUIRE(sdk.wait_for_completed_video_data(reads + 1, 2s));
    CHECK_FALSE(camera->get_image_ready());

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - persistent stop failure blocks reuse until a retry succeeds", "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};

    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    sdk.set_persistent_failure("stop_video_capture");
    require_alpaca_error([&] { camera->abort_exposure(); }, alpacacore::AlpacaError::DriverException);

    const int reads = sdk.completed_video_data_reads();
    sdk.release_video_data();
    REQUIRE(sdk.wait_for_completed_video_data(reads + 1, 2s));
    wait_for_driver_exception(*camera, "capture stop was not confirmed");
    require_array_failure(*camera);
    CHECK(camera->get_camera_state() == alpacacore::CameraState::Error);
    CHECK(camera->get_percent_completed() == 0.0);

    const int exposure_writes = sdk.call_count("set_control_value.Exposure");
    require_alpaca_error([&] { camera->start_exposure(0.001, true); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { camera->set_gain(30); }, alpacacore::AlpacaError::DriverException);
    CHECK(sdk.call_count("set_control_value.Exposure") == exposure_writes);

    sdk.set_persistent_failure("stop_video_capture", false);
    camera->abort_exposure();
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - close and reopen clears an unconfirmed capture stop", "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};

    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    sdk.set_persistent_failure("stop_video_capture");
    require_alpaca_error([&] { camera->abort_exposure(); }, alpacacore::AlpacaError::DriverException);
    sdk.release_video_data();
    wait_for_driver_exception(*camera, "capture stop was not confirmed");

    camera->set_connected(false);
    CHECK(sdk.open_count() == 0);
    sdk.set_persistent_failure("stop_video_capture", false);
    camera->set_connected(true);
    CHECK_FALSE(camera->get_image_ready());
    require_alpaca_error([&] { static_cast<void>(camera->get_last_exposure_duration()); },
                         alpacacore::AlpacaError::ValueNotSet);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - positive ROI readback mismatches fail before download and recover",
          "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    const auto valid_roi = sdk.get_roi_format(17);
    const int reads_before = sdk.call_count("get_video_data");

    const std::vector<alpacacore::test::FakeSVBSDK::ROI> bad_rois{
        {1, valid_roi.start_y, valid_roi.width, valid_roi.height, valid_roi.bin},
        {valid_roi.start_x, 1, valid_roi.width, valid_roi.height, valid_roi.bin},
        {valid_roi.start_x, valid_roi.start_y, valid_roi.width - 1, valid_roi.height, valid_roi.bin},
        {valid_roi.start_x, valid_roi.start_y, valid_roi.width, valid_roi.height - 1, valid_roi.bin},
        {valid_roi.start_x, valid_roi.start_y, valid_roi.width, valid_roi.height, 2},
    };
    for (const auto roi : bad_rois) {
        sdk.return_roi(roi);
        camera->start_exposure(0.001, true);
        wait_for_driver_exception(*camera, "ROI readback does not match");
        require_array_failure(*camera);
    }
    CHECK(sdk.call_count("get_video_data") == reads_before);

    // A different but otherwise supported output type is still not the type
    // requested for this exposure and must be rejected before buffer access.
    sdk.return_roi(valid_roi);
    sdk.return_image_type(alpacacore::vendor::svbony::SVBImageType::Raw8);
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "unsupported image format");
    CHECK(sdk.call_count("get_video_data") == reads_before);

    sdk.return_image_type(alpacacore::vendor::svbony::SVBImageType::Raw16);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - invalid requested sensor bounds fail before SDK frame read", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    camera->set_start_x(16);
    const int reads_before = sdk.call_count("get_video_data");
    require_alpaca_error([&] { camera->start_exposure(0.001, true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(sdk.call_count("get_video_data") == reads_before);
}

TEST_CASE("SVBONY Camera - invalid ROI setter preserves the last image", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    const auto previous = camera->get_image_array();

    try {
        camera->start_exposure(-1.0, true);
        FAIL("negative replacement exposure must be rejected");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidValue);
    }
    CHECK(camera->get_image_array().data == previous.data);

    try {
        camera->set_num_x(0);
        FAIL("zero ROI width must be rejected");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidValue);
    }
    const auto after = camera->get_image_array();
    CHECK(after.width == previous.width);
    CHECK(after.height == previous.height);
    CHECK(after.rank == previous.rank);
    CHECK(after.data == previous.data);
}

TEST_CASE("SVBONY Camera - next exposure geometry preserves the completed image", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    const auto previous = camera->get_image_array();

    camera->set_num_x(8);
    camera->set_num_y(4);
    camera->set_bin_x(2);
    CHECK(camera->get_image_ready());
    const auto still_previous = camera->get_image_array();
    CHECK(still_previous.width == previous.width);
    CHECK(still_previous.height == previous.height);
    CHECK(still_previous.data == previous.data);

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    const auto next = camera->get_image_array();
    CHECK(next.width == 8);
    CHECK(next.height == 4);
    CHECK(next.data.size() == 32);
}

TEST_CASE("SVBONY Camera - active exposure rejects sensor writes including auto-gain disable",
          "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.set_auto(alpacacore::test::FakeSVBSDK::Control::Gain, true);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};

    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    const int writes_before = sdk.call_count("set_control_value");
    const int speed_writes_before = sdk.call_count("set_control_value.FrameSpeedMode");
    const auto expect_invalid_operation = [](auto&& operation) {
        try {
            operation();
            FAIL("sensor configuration write should be rejected during exposure");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        }
    };
    expect_invalid_operation([&] { camera->set_gain(20); });
    expect_invalid_operation([&] { camera->set_offset(20); });
    expect_invalid_operation([&] { camera->set_num_x(8); });
    expect_invalid_operation([&] { camera->set_num_y(4); });
    expect_invalid_operation([&] { camera->set_start_x(1); });
    expect_invalid_operation([&] { camera->set_start_y(1); });
    expect_invalid_operation([&] { camera->set_bin_x(2); });
    expect_invalid_operation([&] { camera->set_bin_y(2); });
    expect_invalid_operation([&] { camera->set_fast_readout(true); });
    CHECK(sdk.call_count("set_control_value") == writes_before);
    CHECK(sdk.call_count("set_control_value.FrameSpeedMode") == speed_writes_before);

    camera->abort_exposure();
    sdk.release_video_data();
    const int idle_writes_before = sdk.call_count("set_control_value");
    camera->set_gain(30);
    CHECK(sdk.call_count("set_control_value") == idle_writes_before + 2);
    long gain = 0;
    bool is_auto = true;
    REQUIRE(sdk.get_control_value(17, alpacacore::test::FakeSVBSDK::Control::Gain, gain, is_auto));
    CHECK(gain == 30);
    CHECK_FALSE(is_auto);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - unsupported StopExposure preserves active and completed images", "[svbony][camera][unit]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.block_video_data();
    ReleaseVideoDataOnExit release{sdk};
    camera->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(2s));
    const int reads_before = sdk.call_count("get_video_data");
    require_alpaca_error([&] { camera->stop_exposure(); }, alpacacore::AlpacaError::MethodNotImplemented);
    CHECK(sdk.call_count("get_video_data") == reads_before);
    sdk.release_video_data();
    wait_until_ready(*camera);
    const auto image = camera->get_image_array();
    require_alpaca_error([&] { camera->stop_exposure(); }, alpacacore::AlpacaError::MethodNotImplemented);
    CHECK(camera->get_image_array().data == image.data);
}

TEST_CASE("SVBONY Camera - image formats and binned sensor-edge padding", "[svbony][camera][unit]") {
    using alpacacore::test::FakeSVBSDK;
    using alpacacore::vendor::svbony::SVBImageType;

    for (const auto type :
         {SVBImageType::Raw8, SVBImageType::Raw16, SVBImageType::Y8, SVBImageType::Y16, SVBImageType::Rgb24}) {
        FakeSVBSDK sdk;
        sdk.set_supported_formats({type});
        auto camera = connected_camera(sdk);
        camera->start_exposure(0.001, true);
        wait_until_ready(*camera);
        const auto image = camera->get_image_array();
        CHECK(image.width == 16);
        CHECK(image.height == 8);
        CHECK(image.rank == (type == SVBImageType::Rgb24 ? 3 : 2));
        const auto expected_sensor_type = type == SVBImageType::Rgb24 ? alpacacore::SensorType::Color
                                          : (type == SVBImageType::Raw8 || type == SVBImageType::Raw16)
                                              ? alpacacore::SensorType::RGGB
                                              : alpacacore::SensorType::Monochrome;
        CHECK(camera->get_sensor_type() == expected_sensor_type);
        const int expected_max_adu =
            type == SVBImageType::Raw8 || type == SVBImageType::Y8 || type == SVBImageType::Rgb24 ? 255 : 4095;
        CHECK(camera->get_max_adu() == expected_max_adu);
        if (type == SVBImageType::Raw8 || type == SVBImageType::Raw16) {
            CHECK(camera->get_bayer_offset_x() == 0);
            CHECK(camera->get_bayer_offset_y() == 0);
        }
        const std::size_t bpp =
            type == SVBImageType::Rgb24 ? 3 : (type == SVBImageType::Raw16 || type == SVBImageType::Y16 ? 2 : 1);
        CHECK(sdk.last_buffer_capacity() == 16U * 8U * bpp);
        CHECK(image.data.size() == 16U * 8U * (type == SVBImageType::Rgb24 ? 3U : 1U));
        CHECK(image.data[0] ==
              (type == SVBImageType::Rgb24 ? 2 : (type == SVBImageType::Raw16 || type == SVBImageType::Y16 ? 256 : 0)));
    }

    {
        FakeSVBSDK shifted_sdk;
        auto shifted = connected_camera(shifted_sdk);
        shifted->set_num_x(9);
        shifted->set_num_y(5);
        shifted->set_start_x(7);
        shifted->set_start_y(3);
        shifted->start_exposure(0.001, true);
        wait_until_ready(*shifted);
        const auto image = shifted->get_image_array();
        CHECK(image.width == 9);
        CHECK(image.height == 5);
        CHECK(image.data[0] == 0x2f2e);  // shifted SDK origin, crop starts at sensor (7,3)
        CHECK(image.data[44] == 0xbfbe);
    }

    SECTION("bin-2 full frame pads the unaligned sensor edges") {
        FakeSVBSDK sdk;
        sdk.set_sensor_size(18, 10);
        auto camera = connected_camera(sdk);
        camera->set_bin_x(2);
        camera->start_exposure(0.001, true);
        wait_until_ready(*camera);
        const auto image = camera->get_image_array();
        CHECK(image.width == 9);
        CHECK(image.height == 5);
        REQUIRE(image.data.size() == 45);
        CHECK(image.data[0] == 0x0100);
        CHECK(image.data[8] == 0);
        CHECK(image.data[4 * 9] == 0);
        CHECK(sdk.last_buffer_capacity() == 8U * 4U * 2U);
        const auto roi = sdk.get_roi_format(17);
        CHECK(roi.start_x == 0);
        CHECK(roi.start_y == 0);
        CHECK(roi.width == 8);
        CHECK(roi.height == 4);
    }

    SECTION("shifted bin-2 crop stays inside the sensor") {
        FakeSVBSDK sdk;
        sdk.set_sensor_size(18, 10);
        auto camera = connected_camera(sdk);
        camera->set_bin_x(2);
        camera->set_num_x(7);
        camera->set_num_y(3);
        camera->set_start_x(2);
        camera->set_start_y(2);
        camera->start_exposure(0.001, true);
        wait_until_ready(*camera);
        const auto image = camera->get_image_array();
        REQUIRE(image.data.size() == 21);
        CHECK(image.data[0] == 0x1312);
        CHECK(image.data[20] == 0x3f3e);
        const auto roi = sdk.get_roi_format(17);
        CHECK(roi.start_x == 1);
        CHECK(roi.start_y == 1);
        CHECK(roi.width == 8);
        CHECK(roi.height == 4);
    }
}

TEST_CASE("SVBONY Camera - fatal frame-read failure is not retried and recovers", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.fail_next("get_video_data");

    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "scripted one-shot SVBONY failure: get_video_data");
    require_array_failure(*camera);
    CHECK(sdk.call_count("get_video_data") == 1);

    camera->set_connected(false);
    camera->set_connected(true);
    CHECK_FALSE(camera->get_image_ready());
    require_alpaca_error([&] { static_cast<void>(camera->get_image_array()); },
                         alpacacore::AlpacaError::InvalidOperation);

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - failed deferred configuration is retried", "[svbony][camera][unit]") {
    SECTION("ROI write") {
        alpacacore::test::FakeSVBSDK sdk;
        auto camera = connected_camera(sdk);
        camera->set_num_x(8);
        const auto initial_roi_writes = sdk.call_count("set_roi_format");
        const int writes_before = sdk.call_count("set_roi_format");
        sdk.fail_next("set_roi_format");

        camera->start_exposure(0.001, true);
        wait_for_driver_exception(*camera, "set_roi_format");
        CHECK(sdk.call_count("set_roi_format") == writes_before + 1);

        camera->start_exposure(0.001, true);
        wait_until_ready(*camera);
        CHECK(sdk.call_count("set_roi_format") == writes_before + 2);
        CHECK(sdk.get_roi_format(17).width == 8);
        CHECK(initial_roi_writes == writes_before);
    }

    SECTION("output image type failure retries the incomplete ROI configuration") {
        using alpacacore::vendor::svbony::SVBImageType;
        alpacacore::test::FakeSVBSDK sdk;
        auto camera = connected_camera(sdk);
        camera->set_num_x(8);
        camera->set_fast_readout(true);
        const int output_type_writes_before = sdk.call_count("set_output_image_type");
        const int roi_writes_before = sdk.call_count("set_roi_format");
        sdk.fail_next("set_output_image_type");
        camera->start_exposure(0.001, true);
        wait_for_driver_exception(*camera, "set_output_image_type");
        CHECK(sdk.call_count("set_roi_format") == roi_writes_before + 1);
        CHECK(sdk.call_count("set_output_image_type") == output_type_writes_before + 1);

        camera->start_exposure(0.001, true);
        wait_until_ready(*camera);
        CHECK(sdk.get_roi_format(17).width == 8);
        CHECK(sdk.call_count("set_roi_format") == roi_writes_before + 2);
        CHECK(sdk.call_count("set_output_image_type") == output_type_writes_before + 2);
        CHECK(sdk.get_output_image_type(17) == SVBImageType::Raw16);
    }

    SECTION("FrameSpeedMode write") {
        alpacacore::test::FakeSVBSDK sdk;
        auto camera = connected_camera(sdk);
        camera->set_fast_readout(true);
        const int roi_writes_before = sdk.call_count("set_roi_format");
        const int writes_before = sdk.call_count("set_control_value.FrameSpeedMode");
        sdk.fail_next("set_control_value.FrameSpeedMode");

        camera->start_exposure(0.001, true);
        wait_for_driver_exception(*camera, "FrameSpeedMode");
        CHECK(sdk.call_count("set_control_value.FrameSpeedMode") == writes_before + 1);

        camera->start_exposure(0.001, true);
        wait_until_ready(*camera);
        CHECK(sdk.call_count("set_control_value.FrameSpeedMode") == writes_before + 2);
        CHECK(sdk.call_count("set_roi_format") == roi_writes_before);
        long speed = -1;
        bool is_auto = true;
        REQUIRE(sdk.get_control_value(17, alpacacore::test::FakeSVBSDK::Control::FrameSpeedMode, speed, is_auto));
        CHECK(speed == 2);
        CHECK_FALSE(is_auto);
    }
}

TEST_CASE("SVBONY Camera - destruction stops and joins an active read", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    sdk.block_video_data();
    {
        auto camera = connected_camera(sdk);
        camera->start_exposure(0.001, true);
        REQUIRE(sdk.wait_for_video_data(2s));
    }
    CHECK(sdk.open_count() == 0);
    const auto events = sdk.events();
    const auto stop = std::find(events.begin(), events.end(), "stop_video_capture");
    const auto read_complete = std::find(events.begin(), events.end(), "get_video_data_complete");
    const auto close = std::find(events.begin(), events.end(), "close_camera");
    REQUIRE(stop != events.end());
    REQUIRE(read_complete != events.end());
    REQUIRE(close != events.end());
    CHECK(stop < read_complete);
    CHECK(stop < close);
}

TEST_CASE("SVBONY Camera - invalid SDK ROI fails and a later exposure recovers", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    REQUIRE(camera->get_image_array().data.size() == 128);

    const int reads_before = sdk.call_count("get_video_data");
    struct InvalidRoi {
        alpacacore::test::FakeSVBSDK::ROI roi;
        std::string_view error;
    };
    const std::vector<InvalidRoi> invalid_rois{
        {{0, 0, 0, 8, 1}, "invalid ROI metadata"},         {{0, 0, -1, 8, 1}, "invalid ROI metadata"},
        {{0, 0, 16, 0, 1}, "invalid ROI metadata"},        {{0, 0, 16, 8, 0}, "invalid ROI metadata"},
        {{-1, 0, 16, 8, 1}, "invalid ROI metadata"},       {{0, -1, 16, 8, 1}, "invalid ROI metadata"},
        {{0, 0, 17, 8, 1}, "ROI readback does not match"}, {{0, 0, 16, 9, 1}, "ROI readback does not match"},
    };
    for (const auto& invalid_roi : invalid_rois) {
        sdk.return_roi(invalid_roi.roi);
        camera->start_exposure(0.001, true);
        wait_for_driver_exception(*camera, invalid_roi.error);
        require_array_failure(*camera);
    }
    CHECK(sdk.call_count("get_video_data") == reads_before);

    sdk.return_roi({0, 0, 16, 8, 1});
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - pre-capture failure does not issue a stop", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    sdk.return_roi({0, 0, 0, 8, 1});

    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "invalid ROI metadata");
    CHECK(sdk.call_count("start_video_capture") == 0);
    CHECK(sdk.call_count("stop_video_capture") == 0);
}

TEST_CASE("SVBONY Camera - unsupported returned format is not published", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);
    const int reads_before = sdk.call_count("get_video_data");

    sdk.return_image_type(alpacacore::vendor::svbony::SVBImageType::Unknown);
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "unsupported image format");
    require_array_failure(*camera);
    CHECK_THROWS_AS(camera->get_image_ready(), alpacacore::AlpacaException);
    CHECK(sdk.call_count("get_video_data") == reads_before);

    sdk.return_image_type(alpacacore::vendor::svbony::SVBImageType::Rgb32);
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "unsupported image format");
    require_array_failure(*camera);
    CHECK(sdk.call_count("get_video_data") == reads_before);
}

TEST_CASE("SVBONY Camera - capture failure is reported and cleared on recovery", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);

    sdk.fail_next_capture_start();
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "start_video_capture");
    require_array_failure(*camera);
    wait_for_driver_exception(*camera, "start_video_capture");

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);

    sdk.set_persistent_failure("start_video_capture");
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "start_video_capture");
    require_array_failure(*camera);
    sdk.set_persistent_failure("start_video_capture", false);
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - frame storage validation rejects short spans", "[svbony][camera][unit]") {
    using alpacacore::vendor::svbony::required_frame_storage;
    using alpacacore::vendor::svbony::SVBImageType;
    using alpacacore::vendor::svbony::validate_frame_storage;

    CHECK(required_frame_storage(16, 8, SVBImageType::Raw16) == 256);
    std::vector<std::uint8_t> empty;
    std::vector<std::uint8_t> short_frame(255);
    CHECK_THROWS_AS(validate_frame_storage(std::span<const std::uint8_t>(empty), 16, 8, SVBImageType::Raw16),
                    alpacacore::AlpacaException);
    CHECK_THROWS_AS(validate_frame_storage(std::span<const std::uint8_t>(short_frame), 16, 8, SVBImageType::Raw16),
                    alpacacore::AlpacaException);
}
