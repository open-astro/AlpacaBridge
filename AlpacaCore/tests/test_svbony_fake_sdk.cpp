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

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

std::unique_ptr<alpacacore::CameraDriver> connected_camera(alpacacore::test::FakeSVBSDK& sdk) {
    auto camera = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    camera->set_connected(true);
    REQUIRE(camera->get_connected());
    return camera;
}

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

TEST_CASE("SVBONY Camera - invalid SDK ROI fails and a later exposure recovers", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);

    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    REQUIRE(camera->get_image_array().data.size() == 128);

    sdk.return_roi({0, 0, 0, 8, 1});
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "invalid ROI metadata");
    require_array_failure(*camera);

    sdk.return_roi({0, 0, 16, 8, 1});
    camera->start_exposure(0.001, true);
    wait_until_ready(*camera);
    CHECK(camera->get_image_array().data.size() == 128);
}

TEST_CASE("SVBONY Camera - unsupported returned format is not published", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);

    sdk.return_image_type(alpacacore::vendor::svbony::SVBImageType::Unknown);
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "unsupported image format");
    require_array_failure(*camera);
    CHECK_THROWS_AS(camera->get_image_ready(), alpacacore::AlpacaException);

    sdk.return_image_type(alpacacore::vendor::svbony::SVBImageType::Rgb32);
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "unsupported image format");
    require_array_failure(*camera);
}

TEST_CASE("SVBONY Camera - capture failure is reported and cleared on recovery", "[svbony][camera][unit]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto camera = connected_camera(sdk);

    sdk.fail_next_capture_start();
    camera->start_exposure(0.001, true);
    wait_for_driver_exception(*camera, "scripted capture-start failure");
    require_array_failure(*camera);
    camera->stop_exposure();
    wait_for_driver_exception(*camera, "scripted capture-start failure");

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
