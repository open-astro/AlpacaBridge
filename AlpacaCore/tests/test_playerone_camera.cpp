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
#include <alpacacore/vendor/playerone/playerone_camera_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_playerone_sdk.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

template <typename Predicate>
bool eventually(Predicate&& predicate, std::chrono::seconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}  // namespace

TEST_CASE("Player One Camera Driver - Defaults", "[playerone][camera][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Name is "Player One Camera" when no camera is plugged in, or the SDK model
    // name (which may not contain "Player One") when one is detected. Either
    // way it's non-empty.
    CHECK_FALSE(driver->get_name().empty());
}

TEST_CASE("Player One Camera Driver - Device metadata", "[playerone][camera][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(3, 1);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Player One Camera Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Player One Camera Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);  // ICameraV4 (Platform 7)
    // Without a connected camera the serial number is unknown, so the unique id falls
    // back to the device number.
    CHECK(driver->get_unique_id() == "PLAYERONE_3");
}

TEST_CASE("Player One Camera Driver - Not connected throws", "[playerone][camera][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

    CHECK_THROWS_AS(driver->get_ccd_temperature(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_gain(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_gain(100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_offset(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->start_exposure(1.0, true), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->stop_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->abort_exposure(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->pulse_guide(0, 100), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_image_array(), alpacacore::AlpacaException);
}

TEST_CASE("Player One Camera Driver - Disconnected state", "[playerone][camera][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == true);
    CHECK(driver->get_can_asymmetric_bin() == false);
    CHECK(driver->get_has_shutter() == false);
}

TEST_CASE("Player One Camera Driver - Actions", "[playerone][camera][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

    // Dew heater / fan power actions are a static driver capability.
    const auto actions = driver->get_supported_actions();
    REQUIRE(actions.size() == 4);
    CHECK(actions[0] == "GetHeaterPower");
    CHECK(actions[1] == "SetHeaterPower");
    CHECK(actions[2] == "GetFanPower");
    CHECK(actions[3] == "SetFanPower");

    // ASCOM action names are case-insensitive.
    CHECK(driver->can_action("GetHeaterPower"));
    CHECK(driver->can_action("setheaterpower"));
    CHECK(driver->can_action("GETFANPOWER"));
    CHECK(driver->can_action("SetFanPower"));
    CHECK(driver->can_action("anything") == false);

    // Unknown action -> ActionNotImplemented; known actions need a connection.
    require_alpaca_error([&]() { driver->action("anything", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->action("GetHeaterPower", ""); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("SetHeaterPower", "50"); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("GetFanPower", ""); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("SetFanPower", "50"); }, alpacacore::AlpacaError::NotConnected);

    CHECK_THROWS_AS(driver->command_blind("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("", false), alpacacore::AlpacaException);
}

TEST_CASE("Player One Camera Driver - Sub-exposure not supported", "[playerone][camera][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

    CHECK_THROWS_AS(driver->get_sub_exposure_duration(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_sub_exposure_duration(1.0), alpacacore::AlpacaException);
}

TEST_CASE("Player One Camera Driver - ASCOM Error Codes", "[playerone][camera][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

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

TEST_CASE("Player One Camera Driver - Rejects non-finite inputs and overflowing ROI starts",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_temperature_control_available(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);

    for (const double duration : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        require_alpaca_error([&] { driver->start_exposure(duration, true); }, alpacacore::AlpacaError::InvalidValue);
    }
    for (const double temperature :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1.0e300}) {
        require_alpaca_error([&] { driver->set_set_ccd_temperature(temperature); },
                             alpacacore::AlpacaError::InvalidValue);
    }

    driver->set_start_x(std::numeric_limits<int>::max());
    require_alpaca_error([&] { driver->start_exposure(0.001, true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(sdk.exposure_start_count() == 0);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - State Machine Contracts", "[playerone][camera][unit]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
}

TEST_CASE("Player One Camera Driver - Rejects malformed frames and recovers", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    sdk.set_frame_data(std::vector<std::uint8_t>(20 * 10 * 2, 0));
    require_alpaca_error([&] { static_cast<void>(driver->get_last_exposure_duration()); },
                         alpacacore::AlpacaError::ValueNotSet);

    const auto failed = [&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    };
    for (const auto& [width, height] : {std::pair{0, 8}, std::pair{16, 0}, std::pair{-1, 8}, std::pair{16, -1},
                                        std::pair{12, 8}, std::pair{16, 6}, std::pair{20, 8}, std::pair{16, 10}}) {
        sdk.set_returned_size(width, height);
        driver->start_exposure(0.001, true);
        REQUIRE(eventually(failed));
        require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                             alpacacore::AlpacaError::DriverException);
    }
    require_alpaca_error([&] { static_cast<void>(driver->get_last_exposure_duration()); },
                         alpacacore::AlpacaError::ValueNotSet);

    sdk.set_returned_size(16, 8);
    sdk.set_frame_data({0x34, 0x12});
    driver->start_exposure(0.001, true);
    REQUIRE(eventually(failed));
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::DriverException);

    std::vector<std::uint8_t> frame(16 * 8 * 2, 0);
    frame[0] = 0x34;
    frame[1] = 0x12;
    sdk.set_frame_data(std::move(frame));
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    const auto image = driver->get_image_array();
    CHECK(image.width == 16);
    CHECK(image.height == 8);
    CHECK(image.rank == 2);
    CHECK(image.data.size() == 16 * 8);
    CHECK(image.data.front() == 0x1234);
    ALPACA_REQUIRE_APPROX(driver->get_last_exposure_duration(), 0.001);

    sdk.set_returned_size(0, 8);
    driver->start_exposure(0.002, true);
    REQUIRE(eventually(failed));
    ALPACA_REQUIRE_APPROX(driver->get_last_exposure_duration(), 0.001);
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - Rejects exposure format readback mismatch", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    sdk.set_returned_format(alpacacore::vendor::playerone::PlayerOneImageFormat::Raw8);

    driver->start_exposure(0.001, true);
    const auto failed = [&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    };
    REQUIRE(eventually(failed));
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Readiness failure is raised by image readers", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_readiness_error(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(0.001, true);

    const auto failed = [&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    };
    REQUIRE(eventually(failed));
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Failed SDK stop keeps settings locked", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    sdk.set_readiness_error(true);
    sdk.set_stop_failure(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(0.001, true);

    REQUIRE(eventually([&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    }));
    require_alpaca_error([&] { driver->set_num_x(6); }, alpacacore::AlpacaError::InvalidOperation);

    sdk.set_stop_failure(false);
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - Readiness timeout is raised by image readers", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk, std::chrono::milliseconds(80));
    driver->set_connected(true);
    driver->start_exposure(0.001, true);

    const auto failed = [&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    };
    REQUIRE(eventually(failed));
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Late frame stays failed after watchdog", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_hold_image_data(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk, std::chrono::milliseconds(80));
    driver->set_connected(true);
    driver->start_exposure(0.001, true);

    REQUIRE(eventually([&] { return sdk.image_data_started(); }));
    REQUIRE(eventually([&] { return driver->get_camera_state() == alpacacore::CameraState::Idle; },
                       std::chrono::seconds(2)));
    require_alpaca_error([&] { static_cast<void>(driver->get_image_ready()); },
                         alpacacore::AlpacaError::DriverException);
    sdk.set_hold_image_data(false);
    REQUIRE(eventually([&] { return sdk.image_data_finished(); }));
    const int unchanged_width = driver->get_num_x();
    REQUIRE(eventually([&] {
        try {
            driver->set_num_x(unchanged_width);
            return true;
        } catch (const alpacacore::AlpacaException& e) {
            if (e.error_code() == alpacacore::AlpacaError::InvalidOperation) return false;
            throw;
        }
    }));
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Rejects unsupported SDK formats", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_supported_formats({alpacacore::vendor::playerone::PlayerOneImageFormat::Unknown});
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - StopExposure preserves partial frame; AbortExposure discards it",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    std::vector<std::uint8_t> frame(16 * 8 * 2, 0);
    frame[0] = 0x78;
    frame[1] = 0x56;
    sdk.set_frame_data(std::move(frame));
    sdk.set_image_ready(false);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);

    driver->start_exposure(10.0, true);
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    driver->stop_exposure();
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    const auto stopped_image = driver->get_image_array();
    CHECK(stopped_image.data.front() == 0x5678);
    CHECK(driver->get_last_exposure_duration() < 10.0);
    const int stop_count = sdk.stop_count();
    driver->stop_exposure();  // StopExposure while idle is a no-op.
    CHECK(sdk.stop_count() == stop_count);
    driver->abort_exposure();  // AbortExposure while idle must preserve the completed frame.
    CHECK(driver->get_image_ready());
    CHECK(driver->get_image_array().data.front() == 0x5678);

    sdk.set_image_ready(false);
    driver->start_exposure(10.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() >= 2; }));
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    const int stops_before_abort = sdk.stop_count();
    driver->abort_exposure();
    CHECK(sdk.stop_count() > stops_before_abort);
    CHECK(driver->get_image_ready() == false);
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::InvalidOperation);
    const int abort_stop_count = sdk.stop_count();
    driver->abort_exposure();  // Idle AbortExposure is a no-op.
    CHECK(sdk.stop_count() == abort_stop_count);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Abort stop race reports readout as invalid operation",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
    sdk.set_stop_failure(true);
    sdk.set_ready_on_stop_failure(true);

    require_alpaca_error([&] { driver->abort_exposure(); }, alpacacore::AlpacaError::InvalidOperation);
    sdk.set_stop_failure(false);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    CHECK(driver->get_image_array().data.size() == 16 * 8);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Aligns, pads, crops and bins requested ROI", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);

    driver->set_num_x(7);
    driver->set_num_y(5);
    std::vector<std::uint8_t> aligned_frame(4 * 4 * 2, 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const auto value = static_cast<std::uint16_t>(100 + y * 10 + x);
            const auto index = static_cast<std::size_t>((y * 4 + x) * 2);
            aligned_frame[index] = static_cast<std::uint8_t>(value & 0xff);
            aligned_frame[index + 1] = static_cast<std::uint8_t>(value >> 8);
        }
    }
    sdk.set_frame_data(std::move(aligned_frame));
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    const auto padded = driver->get_image_array();
    CHECK(padded.width == 7);
    CHECK(padded.height == 5);
    CHECK(padded.data[0] == 100);
    CHECK(padded.data[3] == 103);
    CHECK(padded.data[4] == 0);
    CHECK(padded.data[3 * 7 + 3] == 133);
    CHECK(padded.data[3 * 7 + 4] == 0);
    CHECK(padded.data[4 * 7] == 0);

    driver->set_bin_x(2);
    driver->set_num_x(5);
    driver->set_num_y(3);
    driver->set_start_x(1);
    driver->set_start_y(1);
    sdk.set_returned_size(6, 3);
    std::vector<std::uint8_t> larger_frame(6 * 3 * 2, 0);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 6; ++x) {
            const auto value = static_cast<std::uint16_t>(200 + y * 10 + x);
            const auto index = static_cast<std::size_t>((y * 6 + x) * 2);
            larger_frame[index] = static_cast<std::uint8_t>(value & 0xff);
            larger_frame[index + 1] = static_cast<std::uint8_t>(value >> 8);
        }
    }
    sdk.set_frame_data(std::move(larger_frame));
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    const auto cropped = driver->get_image_array();
    CHECK(cropped.width == 5);
    CHECK(cropped.height == 3);
    CHECK(cropped.data[0] == 200);
    CHECK(cropped.data[4] == 204);
    CHECK(cropped.data[5] == 210);  // source stride is 6, output stride is 5
    CHECK(cropped.data[2 * 5 + 4] == 224);
    CHECK(driver->get_bin_x() == 2);
    CHECK(driver->get_start_x() == 1);
    CHECK(driver->get_start_y() == 1);
    CHECK(sdk.get_image_bin(17) == 2);
    int sdk_start_x = 0;
    int sdk_start_y = 0;
    sdk.get_image_start_pos(17, sdk_start_x, sdk_start_y);
    CHECK(sdk_start_x == 1);
    CHECK(sdk_start_y == 1);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Rejects dirty ROI readback below requested aligned size",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->set_num_x(12);
    sdk.set_returned_size(8, 8);
    sdk.set_frame_data(std::vector<std::uint8_t>(20 * 10 * 2, 0));

    driver->start_exposure(0.001, true);
    const auto failed = [&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    };
    REQUIRE(eventually(failed));
    CHECK(sdk.exposure_start_count() == 0);
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Decodes RAW8 and Mono8 frame data", "[playerone][camera][unit]") {
    using ImageFormat = alpacacore::vendor::playerone::PlayerOneImageFormat;
    for (const auto format : {ImageFormat::Raw8, ImageFormat::Mono8}) {
        alpacacore::test::FakePlayerOneSDK sdk;
        sdk.set_supported_formats({format});
        auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
        driver->set_connected(true);

        std::vector<std::uint8_t> frame(16 * 8, 0);
        frame[0] = 0x80;
        frame[16 * 3 + 7] = 0xf1;
        sdk.set_frame_data(std::move(frame));
        driver->start_exposure(0.001, true);
        REQUIRE(eventually([&] {
            try {
                return driver->get_image_ready();
            } catch (...) {
                return false;
            }
        }));
        const auto image = driver->get_image_array();
        CHECK(image.rank == 2);
        CHECK(image.data.size() == 16 * 8);
        CHECK(image.data[0] == 0x80);
        CHECK(image.data[3 * 16 + 7] == 0xf1);
        driver->set_connected(false);
    }
}

TEST_CASE("Player One Camera Driver - Init failure balances open and allows reconnect", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_init_failure(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_count() == sdk.close_count());

    sdk.set_init_failure(false);
    driver->set_connected(true);
    CHECK(driver->get_connected());
    driver->set_connected(false);
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - Disconnect interrupts held download before close", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_hold_image_data(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] { return sdk.image_data_started(); }));

    std::exception_ptr disconnect_error;
    std::thread disconnecter([&] {
        try {
            driver->set_connected(false);
        } catch (...) {
            disconnect_error = std::current_exception();
        }
    });
    const bool stop_woke_download = eventually([&] { return sdk.stop_count() > 0; });
    sdk.set_hold_image_data(false);
    disconnecter.join();
    REQUIRE(disconnect_error == nullptr);
    CHECK(stop_woke_download);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(sdk.close_during_download());
    CHECK_FALSE(sdk.operation_after_close());
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - StopExposure ignores an active image download", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_hold_image_data(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] { return sdk.image_data_started(); }));
    sdk.set_image_ready(false);  // Readout has started; the exposure is no longer stoppable.

    const int stops_before = sdk.stop_count();
    driver->stop_exposure();
    CHECK(sdk.stop_count() == stops_before);
    require_alpaca_error([&] { driver->set_num_x(6); }, alpacacore::AlpacaError::InvalidOperation);

    sdk.set_hold_image_data(false);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    CHECK(driver->get_image_array().width == 16);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Failed disconnect stop is bounded and retryable", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_hold_image_data(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk, std::chrono::milliseconds(250));
    driver->set_connected(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] { return sdk.image_data_started(); }));
    sdk.set_stop_failure(true);

    const auto started = std::chrono::steady_clock::now();
    require_alpaca_error([&] { driver->set_connected(false); }, alpacacore::AlpacaError::DriverException);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(driver->get_connected());
    CHECK(sdk.open_count() > sdk.close_count());
    require_alpaca_error([&] { static_cast<void>(driver->get_image_ready()); },
                         alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { driver->set_num_x(6); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK_FALSE(sdk.close_during_download());

    sdk.set_stop_failure(false);
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - Destructor closes camera after persistent stop failure",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_hold_image_data(true);
    sdk.set_stop_failure(true);
    {
        auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk, std::chrono::milliseconds(80));
        driver->set_connected(true);
        driver->start_exposure(0.001, true);
        REQUIRE(eventually([&] { return sdk.image_data_started(); }));
    }
    CHECK_FALSE(sdk.close_during_download());
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - StopExposure before SDK start is remembered", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->set_num_x(7);
    sdk.set_hold_start_readback(true);
    driver->start_exposure(0.001, true);
    const bool setup_held = eventually([&] { return sdk.start_readback_started(); });
    if (!setup_held) sdk.set_hold_start_readback(false);
    REQUIRE(setup_held);

    const int stops_before_request = sdk.stop_count();
    driver->stop_exposure();
    const bool early_stop_deferred = sdk.stop_count() == stops_before_request;
    bool roi_change_rejected = false;
    try {
        driver->set_num_x(6);
    } catch (const alpacacore::AlpacaException& e) {
        roi_change_rejected = e.error_code() == alpacacore::AlpacaError::InvalidOperation;
    }
    sdk.set_hold_start_readback(false);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    CHECK(early_stop_deferred);
    CHECK(roi_change_rejected);
    CHECK(sdk.stop_count() > stops_before_request);
    CHECK(driver->get_image_array().width == 7);
    REQUIRE(eventually([&] { return driver->get_camera_state() == alpacacore::CameraState::Idle; }));

    driver->set_num_x(6);
    const int sizes_before_retry = sdk.image_size_set_count();
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    CHECK(sdk.image_size_set_count() > sizes_before_retry);
    CHECK(sdk.last_set_image_size().first == 4);
    CHECK(sdk.last_set_image_size().second == 8);
    CHECK(driver->get_image_array().width == 6);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Invalid replacement exposure preserves current image",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
    const int stops_before = sdk.stop_count();

    require_alpaca_error([&] { driver->start_exposure(20.0, true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    CHECK(sdk.stop_count() == stops_before);
    CHECK(sdk.exposure_start_count() == 1);

    driver->abort_exposure();
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Download SDK exceptions are reported and recover", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    sdk.set_download_failure(true);
    driver->start_exposure(0.001, true);
    const auto failed = [&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    };
    REQUIRE(eventually(failed));
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::DriverException);

    sdk.set_download_failure(false);
    sdk.set_frame_data(std::vector<std::uint8_t>(16 * 8 * 2, 0));
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    CHECK(driver->get_image_array().data.size() == 16 * 8);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Validates RGB24 frames", "[playerone][camera][unit]") {
    using ImageFormat = alpacacore::vendor::playerone::PlayerOneImageFormat;
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_supported_formats({ImageFormat::Rgb24});
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);

    std::vector<std::uint8_t> frame(16 * 8 * 3, 0);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            const auto index = static_cast<std::size_t>((y * 16 + x) * 3);
            frame[index] = static_cast<std::uint8_t>(y * 16 + x);   // SDK B
            frame[index + 1] = static_cast<std::uint8_t>(100 + x);  // SDK G
            frame[index + 2] = static_cast<std::uint8_t>(200 + y);  // SDK R
        }
    }
    sdk.set_frame_data(std::move(frame));
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));

    const auto image = driver->get_image_array();
    CHECK(image.width == 16);
    CHECK(image.height == 8);
    CHECK(image.rank == 3);
    CHECK(image.data.size() == 16 * 8 * 3);
    CHECK(image.data[0] == 200);
    CHECK(image.data[1] == 100);
    CHECK(image.data[2] == 0);
    CHECK(image.data[(3 * 16 + 7) * 3] == 203);
    CHECK(image.data[(3 * 16 + 7) * 3 + 1] == 107);
    CHECK(image.data[(3 * 16 + 7) * 3 + 2] == 55);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Joins pulse-off before destruction", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_has_st4_port(true);
    {
        auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
        driver->set_connected(true);
        driver->pulse_guide(0, 60'000);
    }
    CHECK(sdk.pulse_off_count() == 1);
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - Pulse-off failure remains observable", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_has_st4_port(true);
    sdk.set_pulse_off_failure(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->pulse_guide(0, 1);

    REQUIRE(eventually([&] {
        try {
            static_cast<void>(driver->get_is_pulse_guiding());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    }));
    CHECK(sdk.pulse_off_count() == 1);
    require_alpaca_error([&] { driver->set_connected(false); }, alpacacore::AlpacaError::DriverException);
    CHECK(driver->get_connected());
    require_alpaca_error([&] { static_cast<void>(driver->get_is_pulse_guiding()); },
                         alpacacore::AlpacaError::DriverException);
    sdk.set_pulse_off_failure(false);
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_count() == sdk.close_count());
}
