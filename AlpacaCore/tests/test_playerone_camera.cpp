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
#include <functional>
#include <thread>
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
bool eventually(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace

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

    sdk.set_returned_size(0, 8);
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
    sdk.set_returned_size(16, 8);
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
    driver->set_connected(false);
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - Rejects unsupported SDK formats", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_supported_formats({alpacacore::vendor::playerone::PlayerOneImageFormat::Unknown});
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_count() == sdk.close_count());
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
