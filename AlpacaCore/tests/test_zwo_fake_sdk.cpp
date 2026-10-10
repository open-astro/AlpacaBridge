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
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_zwo_sdk.h"

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

TEST_CASE("ZWO fake SDK - lazy download preserves padded ROI pixels", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);

    camera->set_num_x(7);
    camera->set_num_y(3);
    camera->set_start_x(9);  // the padded eight-pixel ROI shifts left and crops one column
    sdk.frame_bytes = make_raw16_frame(8, 4);
    CHECK(error_code([&] { camera->start_exposure(std::numeric_limits<double>::quiet_NaN(), true); }) ==
          AlpacaError::InvalidValue);
    camera->start_exposure(0.001, true);

    CHECK(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 0);  // readiness never starts the USB bulk transfer
    CHECK(error_code([&] { camera->set_num_x(8); }) == AlpacaError::InvalidOperation);

    const auto image = camera->get_image_array();
    REQUIRE(image.rank == 2);
    REQUIRE(image.width == 7);
    REQUIRE(image.height == 3);
    CHECK(image.data[0] == 2);
    CHECK(image.data[6] == 8);
    CHECK(image.data[7] == 10);
    CHECK(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 1);
    camera->set_num_x(8);
    CHECK_FALSE(camera->get_image_ready());
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::InvalidOperation);
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
    sdk.roi_readback_override = FakeZWOSDK::ROIDescription{0, 8, 1, FakeZWOSDK::ImageType::Raw16};
    camera->start_exposure(0.001, true);

    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("get_data_after_exposure") == 0);
    CHECK(error_code([&] { static_cast<void>(camera->get_image_array()); }) == AlpacaError::DriverException);

    sdk.roi_readback_override.reset();
    sdk.frame_bytes = make_raw16_frame(16, 8);
    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
}

TEST_CASE("ZWO fake SDK - abort invalidates an in-flight lazy download", "[zwo][camera][unit]") {
    FakeZWOSDK sdk;
    sdk.frame_bytes = make_raw16_frame(16, 8);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk);
    connect_camera(*camera);
    camera->start_exposure(0.001, true);
    sdk.hold_download();

    std::atomic<int> result{-1};
    std::thread download([&] { result.store(error_code([&] { static_cast<void>(camera->get_image_array()); })); });
    const bool download_started = sdk.wait_for_download(std::chrono::seconds(2));
    camera->abort_exposure();
    download.join();

    CHECK(download_started);
    CHECK(result.load() == AlpacaError::InvalidOperation);
    CHECK_FALSE(camera->get_image_ready());
    CHECK(sdk.call_count("get_data_after_exposure") == 1);

    camera->start_exposure(0.001, true);
    REQUIRE(camera->get_image_ready());
    CHECK(camera->get_image_array().data.front() == 1);
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
    sdk.release_download();
    download.join();
    disconnect.join();

    CHECK(download_started);
    CHECK(stop_seen);
    CHECK(close_waited);
    CHECK(download_code.load() == AlpacaError::InvalidOperation);
    CHECK_FALSE(sdk.closed_during_download());
    CHECK(sdk.open_count() == 0);
}
