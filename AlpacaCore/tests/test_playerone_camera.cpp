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
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "camera_percent_cooler_checks.h"
#include "catch2_compat.h"
#include "fake_playerone_sdk.h"
#include "fake_task_clock.h"
#include "locked_playerone_sdk.h"

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

// Records which interface method it received and the camera id, so the forward
// sweep below proves every LockedPlayerOneSDK override lands on its own
// counterpart (not merely that the decorator compiles).
class RecordingPlayerOneSDK final : public alpacacore::vendor::playerone::PlayerOneSDK {
public:
    using PlayerOneCameraInfo = alpacacore::vendor::playerone::PlayerOneCameraInfo;
    using PlayerOneConfigCaps = alpacacore::vendor::playerone::PlayerOneConfigCaps;
    using PlayerOneImageFormat = alpacacore::vendor::playerone::PlayerOneImageFormat;
    using PlayerOneCameraState = alpacacore::vendor::playerone::PlayerOneCameraState;
    using PlayerOneGuideDirection = alpacacore::vendor::playerone::PlayerOneGuideDirection;

    std::string get_sdk_version() override {
        record("get_sdk_version", -1);
        return "recorded-sdk";
    }
    std::vector<PlayerOneCameraInfo> enumerate_cameras() override {
        record("enumerate_cameras", -1);
        return {};
    }
    void open_camera(int camera_id) override { record("open_camera", camera_id); }
    void init_camera(int camera_id) override { record("init_camera", camera_id); }
    void close_camera(int camera_id) override { record("close_camera", camera_id); }
    PlayerOneCameraInfo get_camera_properties_by_id(int camera_id) override {
        record("get_camera_properties_by_id", camera_id);
        return {};
    }
    PlayerOneConfigCaps probe_config_caps(int camera_id) override {
        record("probe_config_caps", camera_id);
        return {};
    }
    long get_config_int(int camera_id, int /*config_id*/, bool* is_auto) override {
        record("get_config_int", camera_id);
        saw_is_auto_ptr = is_auto != nullptr;
        return 0;
    }
    void set_config_int(int camera_id, int /*config_id*/, long /*value*/, bool /*is_auto*/) override {
        record("set_config_int", camera_id);
    }
    double get_config_float(int camera_id, int /*config_id*/, bool* /*is_auto*/) override {
        record("get_config_float", camera_id);
        return 0.0;
    }
    bool get_config_bool(int camera_id, int /*config_id*/, bool* /*is_auto*/) override {
        record("get_config_bool", camera_id);
        return false;
    }
    void set_config_float(int camera_id, int /*config_id*/, double /*value*/, bool /*is_auto*/) override {
        record("set_config_float", camera_id);
    }
    void set_config_bool(int camera_id, int /*config_id*/, bool /*value*/, bool /*is_auto*/) override {
        record("set_config_bool", camera_id);
    }
    PlayerOneImageFormat get_image_format(int camera_id) override {
        record("get_image_format", camera_id);
        return {};
    }
    void set_image_format(int camera_id, PlayerOneImageFormat /*format*/) override {
        record("set_image_format", camera_id);
    }
    void get_image_size(int camera_id, int& /*width*/, int& /*height*/) override {
        record("get_image_size", camera_id);
    }
    void set_image_size(int camera_id, int /*width*/, int /*height*/) override { record("set_image_size", camera_id); }
    void get_image_start_pos(int camera_id, int& /*start_x*/, int& /*start_y*/) override {
        record("get_image_start_pos", camera_id);
    }
    void set_image_start_pos(int camera_id, int /*start_x*/, int /*start_y*/) override {
        record("set_image_start_pos", camera_id);
    }
    int get_image_bin(int camera_id) override {
        record("get_image_bin", camera_id);
        return 0;
    }
    void set_image_bin(int camera_id, int /*bin*/) override { record("set_image_bin", camera_id); }
    void start_exposure(int camera_id, bool /*single_frame*/) override { record("start_exposure", camera_id); }
    void stop_exposure(int camera_id) override { record("stop_exposure", camera_id); }
    PlayerOneCameraState get_camera_state(int camera_id) override {
        record("get_camera_state", camera_id);
        return {};
    }
    bool image_ready(int camera_id) override {
        record("image_ready", camera_id);
        return false;
    }
    bool get_image_data(int camera_id, std::uint8_t* /*buffer*/, std::size_t /*buffer_size*/,
                        int /*timeout_ms*/) override {
        record("get_image_data", camera_id);
        return false;
    }
    void pulse_guide_on(int camera_id, PlayerOneGuideDirection /*direction*/) override {
        record("pulse_guide_on", camera_id);
    }
    void pulse_guide_off(int camera_id, PlayerOneGuideDirection /*direction*/) override {
        record("pulse_guide_off", camera_id);
    }
    double get_temperature_c(int camera_id) override {
        record("get_temperature_c", camera_id);
        return 0.0;
    }
    bool get_cooler_on(int camera_id) override {
        record("get_cooler_on", camera_id);
        return false;
    }
    void set_cooler_on(int camera_id, bool /*on*/) override { record("set_cooler_on", camera_id); }
    int get_target_temp_c(int camera_id) override {
        record("get_target_temp_c", camera_id);
        return 0;
    }
    void set_target_temp_c(int camera_id, int /*target_c*/) override { record("set_target_temp_c", camera_id); }
    int get_cooler_power_percent(int camera_id) override {
        record("get_cooler_power_percent", camera_id);
        return 0;
    }
    double get_egain(int camera_id) override {
        record("get_egain", camera_id);
        return 0.0;
    }
    int get_heater_power_percent(int camera_id) override {
        record("get_heater_power_percent", camera_id);
        return 0;
    }
    void set_heater_power_percent(int camera_id, int /*percent*/) override {
        record("set_heater_power_percent", camera_id);
    }
    int get_fan_power_percent(int camera_id) override {
        record("get_fan_power_percent", camera_id);
        return 0;
    }
    void set_fan_power_percent(int camera_id, int /*percent*/) override { record("set_fan_power_percent", camera_id); }

    std::vector<std::pair<std::string, int>> seen;
    bool saw_is_auto_ptr{true};

private:
    void record(const char* name, int camera_id) { seen.emplace_back(name, camera_id); }
};

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

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_last_exposure_duration(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_last_exposure_start_time(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_is_pulse_guiding() == false);
    CHECK(driver->get_can_abort_exposure() == true);
    CHECK(driver->get_can_stop_exposure() == false);
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

    require_alpaca_error([&]() { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_image_ready(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == false);
}

TEST_CASE("Player One Camera Driver - StopExposure is not implemented and leaves the exposure running",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    std::vector<std::uint8_t> frame(16 * 8 * 2, 0);
    frame[0] = 0x78;
    frame[1] = 0x56;
    sdk.set_frame_data(std::move(frame));
    sdk.set_image_ready(false);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_can_stop_exposure() == false);

    // POAStopExposure discards the frame on hardware (issue #1023), so the
    // driver must refuse StopExposure without touching the SDK and without
    // ending the exposure the client is still waiting on.
    driver->start_exposure(10.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
    const int stops_before = sdk.stop_count();
    require_alpaca_error([&] { driver->stop_exposure(); }, alpacacore::AlpacaError::MethodNotImplemented);
    CHECK(sdk.stop_count() == stops_before);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);

    // The exposure still completes and publishes its frame.
    sdk.set_image_ready(true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    CHECK(driver->get_image_array().data.front() == 0x5678);

    // Idle: StopExposure is still MethodNotImplemented, never a silent no-op.
    require_alpaca_error([&] { driver->stop_exposure(); }, alpacacore::AlpacaError::MethodNotImplemented);
    CHECK(driver->get_image_ready());
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - AbortExposure discards the frame and keeps a finished one",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    std::vector<std::uint8_t> frame(16 * 8 * 2, 0);
    frame[0] = 0x78;
    frame[1] = 0x56;
    sdk.set_frame_data(std::move(frame));
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);

    sdk.set_image_ready(false);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
    driver->abort_exposure();
    require_alpaca_error([&] { (void)driver->get_last_exposure_duration(); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { (void)driver->get_last_exposure_start_time(); },
                         alpacacore::AlpacaError::InvalidOperation);

    sdk.set_image_ready(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    const int stops_after_frame = sdk.stop_count();
    const auto completed_start = driver->get_last_exposure_start_time();
    driver->abort_exposure();  // AbortExposure while idle must preserve the completed frame.
    CHECK(sdk.stop_count() == stops_after_frame);
    CHECK(driver->get_image_ready());
    CHECK(driver->get_image_array().data.front() == 0x5678);

    sdk.set_image_ready(false);
    driver->start_exposure(10.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 3; }));
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    const int stops_before_abort = sdk.stop_count();
    driver->abort_exposure();
    CHECK(sdk.stop_count() > stops_before_abort);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);
    CHECK(driver->get_last_exposure_duration() == Catch::Approx(0.001));
    CHECK(driver->get_last_exposure_start_time() == completed_start);
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::InvalidOperation);
    const int abort_stop_count = sdk.stop_count();
    driver->abort_exposure();  // Idle AbortExposure is a no-op.
    CHECK(sdk.stop_count() == abort_stop_count);

    // The camera is ready for the next exposure after an abort.
    sdk.set_image_ready(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    CHECK(driver->get_image_array().data.front() == 0x5678);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - Rejects malformed frames and recovers", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    sdk.set_frame_data(std::vector<std::uint8_t>(20 * 10 * 2, 0));
    require_alpaca_error([&] { static_cast<void>(driver->get_last_exposure_duration()); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { (void)driver->get_last_exposure_start_time(); },
                         alpacacore::AlpacaError::InvalidOperation);

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
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { (void)driver->get_last_exposure_start_time(); },
                         alpacacore::AlpacaError::InvalidOperation);

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
    const auto successful_start = driver->get_last_exposure_start_time();

    sdk.set_returned_size(0, 8);
    driver->start_exposure(0.002, true);
    REQUIRE(eventually(failed));
    ALPACA_REQUIRE_APPROX(driver->get_last_exposure_duration(), 0.001);
    CHECK(driver->get_last_exposure_start_time() == successful_start);
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
    require_alpaca_error([&] { (void)driver->get_last_exposure_duration(); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { (void)driver->get_last_exposure_start_time(); },
                         alpacacore::AlpacaError::InvalidOperation);
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

TEST_CASE("Player One Camera Driver - StartExposure during an exposure is refused", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
    const int stops_before = sdk.stop_count();
    require_alpaca_error([&] { driver->start_exposure(1.0, true); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(sdk.stop_count() == stops_before);
    CHECK(sdk.exposure_start_count() == 1);
    driver->abort_exposure();
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - StartExposure after abort or a finished frame succeeds",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
    driver->abort_exposure();

    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 2; }));
    sdk.set_image_ready(true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));

    sdk.set_image_ready(false);
    driver->start_exposure(1.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 3; }));
    driver->abort_exposure();
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - StartExposure after a failed exposure succeeds", "[playerone][camera][unit]") {
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
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);

    sdk.set_stop_failure(false);
    sdk.set_readiness_error(false);
    CHECK_NOTHROW(driver->start_exposure(1.0, true));
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 2; }));
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

// The reconnect clear site on its own (ascom-contract: "test each clear site
// on its own"); StartExposure's clear is the case "StartExposure after a
// failed exposure succeeds".
TEST_CASE("Player One Camera Driver - Reconnect clears a kept exposure failure and the last exposure",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_frame_data(std::vector<std::uint8_t>(16 * 8 * 2, 0));
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));

    sdk.set_download_failure(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    }));
    CHECK(driver->get_last_exposure_duration() == Catch::Approx(0.001));  // the failure keeps the earlier success
    sdk.set_download_failure(false);

    driver->set_connected(false);
    driver->set_connected(true);
    CHECK_NOTHROW(static_cast<void>(driver->get_image_ready()));
    CHECK_FALSE(driver->get_image_ready());
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_alpaca_error([&] { static_cast<void>(driver->get_image_array()); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { static_cast<void>(driver->get_last_exposure_duration()); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { static_cast<void>(driver->get_last_exposure_start_time()); },
                         alpacacore::AlpacaError::InvalidOperation);
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

// --- Waits on the task clock (issue #1279) ------------------------------------

namespace {

using namespace std::chrono_literals;

std::unique_ptr<alpacacore::CameraDriver> make_clocked_driver(alpacacore::test::FakePlayerOneSDK& sdk,
                                                              alpacacore::test::FakeTaskClock& clock,
                                                              std::chrono::steady_clock::duration grace = 15s) {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk, grace, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    return driver;
}

}  // namespace

TEST_CASE("Player One Camera Driver - pulse guide timer waits on the task clock",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_has_st4_port(true);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock);

    driver->pulse_guide(0, 500);
    REQUIRE(clock.wait_for_wait_count(1, 5s));  // the pulse timer's wait
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(499ms);
    CHECK(driver->get_is_pulse_guiding());
    CHECK(sdk.pulse_off_count() == 0);
    clock.advance(1ms);
    CHECK(eventually([&] { return !driver->get_is_pulse_guiding(); }));
    CHECK(sdk.pulse_off_count() == 1);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - disconnect and destruction cancel the pulse clock wait",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_has_st4_port(true);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock);

    driver->pulse_guide(0, 10000);
    REQUIRE(clock.wait_for_wait_count(1, 5s));
    // No advance: disconnecting and destroying the driver must wake the wait.
    // A lost cancel or notify would park the fake-clock wait for ever, so both
    // run in one bounded async and the case fails instead of hanging.
    auto work = std::async(std::launch::async, [&] {
        driver->set_connected(false);
        driver.reset();
    });
    if (work.wait_for(5s) != std::future_status::ready) {
        clock.advance(10s);  // unblock the join so the case can report
        work.wait();
        FAIL("disconnect or destruction did not wake the pulse timer wait");
    }
    work.get();
    CHECK(sdk.pulse_off_count() == 1);
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - exposure deadline reads the task clock", "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_hold_image_data(true);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock);

    driver->start_exposure(1.0, true);  // deadline: 1 s exposure + 15 s grace
    REQUIRE(eventually([&] { return sdk.image_data_started(); }));
    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Download);
    clock.advance(15999ms);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Download);
    clock.advance(1ms);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    require_alpaca_error([&] { static_cast<void>(driver->get_image_ready()); },
                         alpacacore::AlpacaError::DriverException);
    sdk.set_hold_image_data(false);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - readiness poll sleeps on the task clock",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock);

    const auto before_start = std::chrono::system_clock::now();
    driver->start_exposure(1.0, true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));  // the poll's 50 ms sleep
    const auto after_start = std::chrono::system_clock::now();
    CHECK(driver->get_percent_completed() == Catch::Approx(0.0));
    clock.advance(250ms);
    REQUIRE(clock.wait_for_wait_count(2, 5s));
    CHECK(driver->get_percent_completed() == Catch::Approx(25.0));
    sdk.set_image_ready(true);
    // Real time passes, virtual time does not: the poll must not look again.
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(driver->get_image_ready());
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    CHECK(driver->get_percent_completed() == Catch::Approx(25.0));
    clock.advance(50ms);
    CHECK(eventually([&] { return driver->get_image_ready(); }));
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_last_exposure_duration() == Catch::Approx(1.0));
    CHECK(driver->get_last_exposure_start_time() >= before_start);
    CHECK(driver->get_last_exposure_start_time() <= after_start);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - readiness poll gives up at the deadline on the task clock",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock, 80ms);

    driver->start_exposure(0.020, true);  // deadline: 20 ms + 80 ms = 100 ms
    std::uint64_t waits = 1;
    REQUIRE(clock.wait_for_wait_count(waits, 5s));
    clock.advance(50ms);  // 50 ms: before the deadline, the poll sleeps again
    REQUIRE(clock.wait_for_wait_count(++waits, 5s));
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    clock.advance(49ms);  // 99 ms: the body wakes at 100 ms, not yet
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    clock.advance(1ms);  // 100 ms: the session watchdog fails the exposure
    CHECK(eventually([&] {
        try {
            static_cast<void>(driver->get_image_ready());
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    }));
    driver->set_connected(false);
}

namespace {

// Runs `fn` while the readiness poll waits on the fake clock, and reports
// whether it returned within 5 s of real time with no advance. If it did not,
// advances the clock until it does, so the case fails instead of hanging.
bool returns_without_advance(alpacacore::test::FakeTaskClock& clock, const std::function<void()>& fn) {
    auto work = std::async(std::launch::async, fn);
    if (work.wait_for(5s) == std::future_status::ready) {
        work.get();
        return true;
    }
    while (work.wait_for(1ms) != std::future_status::ready) {
        clock.advance(50ms);
    }
    work.get();
    return false;
}

}  // namespace

TEST_CASE("Player One Camera Driver - AbortExposure wakes the readiness poll without a clock advance",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock);

    driver->start_exposure(10.0, true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));  // the readiness poll's wait
    const int stops_before = sdk.stop_count();
    CHECK(returns_without_advance(clock, [&] { driver->abort_exposure(); }));
    CHECK(sdk.stop_count() > stops_before);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK_FALSE(driver->get_image_ready());
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - disconnect wakes the readiness poll without a clock advance",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock);

    driver->start_exposure(10.0, true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));
    CHECK(returns_without_advance(clock, [&] { driver->set_connected(false); }));
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(sdk.operation_after_close());
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - destruction wakes the readiness poll without a clock advance",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock);

    driver->start_exposure(10.0, true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));
    CHECK(returns_without_advance(clock, [&] { driver.reset(); }));
    CHECK_FALSE(sdk.operation_after_close());
    CHECK(sdk.open_count() == sdk.close_count());
}

TEST_CASE("Player One Camera Driver - PercentCompleted follows the task clock",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock);

    driver->start_exposure(10.0, true);
    REQUIRE(clock.wait_for_wait_count(1, 5s));
    CHECK(driver->get_percent_completed() == Catch::Approx(0.0).margin(1.0));
    clock.advance(5s);  // half the exposure, well inside the 15 s grace
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Exposing);
    CHECK(driver->get_percent_completed() == Catch::Approx(50.0).margin(1.0));
    // Bounded: the abort case above pins that abort needs no advance.
    static_cast<void>(returns_without_advance(clock, [&] { driver->abort_exposure(); }));
    driver->set_connected(false);
}

// The capture session casts the duration to the clock's tick type, which is undefined for a NaN, an infinity or a
// huge value: the driver must refuse each one before it starts a session.
TEST_CASE("Player One Camera Driver - StartExposure rejects NaN, infinity and a huge Duration before the session",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);

    for (const double duration :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1.0e300, 9.0e12}) {
        require_alpaca_error([&] { driver->start_exposure(duration, true); }, alpacacore::AlpacaError::InvalidValue);
    }
    CHECK(sdk.exposure_start_count() == 0);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK_FALSE(driver->get_image_ready());  // no failure was kept either

    driver->start_exposure(0.001, true);  // the refusals left the session usable
    CHECK(eventually([&] { return driver->get_image_ready(); }));
    driver->set_connected(false);
}

// A body that ends early must stop the SDK exposure it started: the watchdog cancels it here with no driver-side
// stop before it, the same state a StartExposure that raced a disconnect's stop leaves.
TEST_CASE("Player One Camera Driver - a cancelled exposure body stops the SDK exposure",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_image_ready(false);
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock, 80ms);

    driver->start_exposure(0.020, true);  // deadline: 20 ms + 80 ms = 100 ms
    REQUIRE(clock.wait_for_wait_count(1, 5s));
    REQUIRE(sdk.exposure_start_count() == 1);
    require_alpaca_error([&] { driver->set_start_x(0); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(sdk.stop_count() == 0);

    clock.advance(100ms);
    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);  // the watchdog cancels the body
    CHECK(eventually([&] { return sdk.stop_count() == 1; }));
    CHECK(eventually([&] {
        try {
            driver->set_start_x(0);  // the exposure is no longer running: the settings unlock
            return true;
        } catch (const alpacacore::AlpacaException&) {
            return false;
        }
    }));
    driver->set_connected(false);
    CHECK(sdk.stop_count() == 1);  // nothing left for the disconnect to stop
    CHECK(sdk.open_count() == sdk.close_count());
}

// A disconnect closes the session before the pulse stop that can still refuse it: the camera stays connected, so
// the session must open again or every StartExposure would raise NotConnected on a connected camera.
TEST_CASE("Player One Camera Driver - a disconnect that leaves the camera connected leaves the session open",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_has_st4_port(true);
    sdk.set_pulse_off_failure(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->pulse_guide(0, 1);
    REQUIRE(eventually([&] { return sdk.pulse_off_count() == 1; }));

    require_alpaca_error([&] { driver->set_connected(false); }, alpacacore::AlpacaError::DriverException);
    REQUIRE(driver->get_connected());

    driver->start_exposure(0.001, true);
    CHECK(eventually([&] { return driver->get_image_ready(); }));
    sdk.set_pulse_off_failure(false);
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Player One Camera Driver - a refused disconnect keeps the Ready frame and LastExposure",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_has_st4_port(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    const double duration = driver->get_last_exposure_duration();

    sdk.set_pulse_off_failure(true);
    driver->pulse_guide(0, 1);
    REQUIRE(eventually([&] { return sdk.pulse_off_count() == 1; }));
    require_alpaca_error([&] { driver->set_connected(false); }, alpacacore::AlpacaError::DriverException);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_image_ready());
    CHECK_NOTHROW(driver->get_image_array());
    CHECK(driver->get_last_exposure_duration() == duration);

    sdk.set_pulse_off_failure(false);
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Player One Camera Driver - a refused disconnect after a stopped exposure leaves the camera idle",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_has_st4_port(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);

    sdk.set_pulse_off_failure(true);
    driver->pulse_guide(0, 1);
    REQUIRE(eventually([&] { return sdk.pulse_off_count() == 1; }));
    sdk.set_image_ready(false);
    driver->start_exposure(10.0, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
    require_alpaca_error([&] { driver->set_connected(false); }, alpacacore::AlpacaError::DriverException);
    REQUIRE(driver->get_connected());

    // The SDK stop discarded the frame: the body must not keep polling for it.
    CHECK(driver->get_camera_state() != alpacacore::CameraState::Exposing);
    CHECK_NOTHROW(driver->start_exposure(1.0, true));

    sdk.set_pulse_off_failure(false);
    driver->abort_exposure();
    driver->set_connected(false);
}

// --- SDK seam: LockedPlayerOneSDK (issue #1256) -------------------------------

TEST_CASE("FakePlayerOneSDK-style recording SDK behind LockedPlayerOneSDK - forwards every entry point",
          "[playerone][camera][unit][fake-sdk]") {
    using namespace alpacacore::vendor::playerone;
    RecordingPlayerOneSDK recorder;
    alpacacore::test::LockedPlayerOneSDK sdk(recorder);
    std::vector<std::uint8_t> buf(16, 0);
    bool flag = false;
    int ia = 0;
    static_cast<void>(ia);
    static_cast<void>(flag);

    sdk.get_sdk_version();
    sdk.enumerate_cameras();
    sdk.open_camera(23);
    sdk.init_camera(23);
    sdk.close_camera(23);
    sdk.get_camera_properties_by_id(23);
    sdk.probe_config_caps(23);
    sdk.get_config_int(23, 1);
    sdk.set_config_int(23, 23, 5L, true);
    sdk.get_config_float(23, 23, &flag);
    sdk.get_config_bool(23, 23, &flag);
    sdk.set_config_float(23, 23, 1.5, true);
    sdk.set_config_bool(23, 23, true, true);
    sdk.get_image_format(23);
    sdk.set_image_format(23, PlayerOneImageFormat::Raw8);
    sdk.get_image_size(23, ia, ia);
    sdk.set_image_size(23, 23, 23);
    sdk.get_image_start_pos(23, ia, ia);
    sdk.set_image_start_pos(23, 23, 23);
    sdk.get_image_bin(23);
    sdk.set_image_bin(23, 23);
    sdk.start_exposure(23, true);
    sdk.stop_exposure(23);
    sdk.get_camera_state(23);
    sdk.image_ready(23);
    sdk.get_image_data(23, buf.data(), buf.size(), 23);
    sdk.pulse_guide_on(23, PlayerOneGuideDirection::North);
    sdk.pulse_guide_off(23, PlayerOneGuideDirection::North);
    sdk.get_temperature_c(23);
    sdk.get_cooler_on(23);
    sdk.set_cooler_on(23, true);
    sdk.get_target_temp_c(23);
    sdk.set_target_temp_c(23, 23);
    sdk.get_cooler_power_percent(23);
    sdk.get_egain(23);
    sdk.get_heater_power_percent(23);
    sdk.set_heater_power_percent(23, 23);
    sdk.get_fan_power_percent(23);
    sdk.set_fan_power_percent(23, 23);

    const std::vector<std::string> expected = {"get_sdk_version",
                                               "enumerate_cameras",
                                               "open_camera",
                                               "init_camera",
                                               "close_camera",
                                               "get_camera_properties_by_id",
                                               "probe_config_caps",
                                               "get_config_int",
                                               "set_config_int",
                                               "get_config_float",
                                               "get_config_bool",
                                               "set_config_float",
                                               "set_config_bool",
                                               "get_image_format",
                                               "set_image_format",
                                               "get_image_size",
                                               "set_image_size",
                                               "get_image_start_pos",
                                               "set_image_start_pos",
                                               "get_image_bin",
                                               "set_image_bin",
                                               "start_exposure",
                                               "stop_exposure",
                                               "get_camera_state",
                                               "image_ready",
                                               "get_image_data",
                                               "pulse_guide_on",
                                               "pulse_guide_off",
                                               "get_temperature_c",
                                               "get_cooler_on",
                                               "set_cooler_on",
                                               "get_target_temp_c",
                                               "set_target_temp_c",
                                               "get_cooler_power_percent",
                                               "get_egain",
                                               "get_heater_power_percent",
                                               "set_heater_power_percent",
                                               "get_fan_power_percent",
                                               "set_fan_power_percent"};
    REQUIRE(recorder.seen.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO(expected[i]);
        CHECK(recorder.seen[i].first == expected[i]);
        // Calls that carry a camera id forward it unchanged (get_sdk_version and
        // enumerate_cameras have none and record -1).
        CHECK((recorder.seen[i].second == 23 || recorder.seen[i].second == -1));
    }
    // The decorator repeats the interface's defaulted is_auto argument.
    CHECK_FALSE(recorder.saw_is_auto_ptr);
}

TEST_CASE("FakePlayerOneSDK behind LockedPlayerOneSDK - forwards values and out-parameters",
          "[playerone][camera][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOneSDK fake;
    alpacacore::test::LockedPlayerOneSDK sdk(fake);

    REQUIRE(sdk.get_sdk_version() == "fake-playerone-sdk");
    REQUIRE(sdk.enumerate_cameras().size() == 1);
    sdk.open_camera(17);
    CHECK(fake.open_count() == 1);
    sdk.set_config_int(17, 1, 42);
    bool is_auto = true;
    CHECK(sdk.get_config_int(17, 1, &is_auto) == 42);
    CHECK_FALSE(is_auto);
    sdk.set_image_size(17, 8, 4);
    int width = 0;
    int height = 0;
    sdk.get_image_size(17, width, height);
    CHECK(width == 8);
    CHECK(height == 4);
    sdk.close_camera(17);
    CHECK(fake.close_count() == 1);
}

TEST_CASE("Player One Camera Driver - FakePlayerOneSDK exposure behind LockedPlayerOneSDK",
          "[playerone][camera][unit][fake-sdk]") {
    alpacacore::test::FakePlayerOneSDK fake;
    alpacacore::test::LockedPlayerOneSDK sdk(fake);
    fake.set_supported_formats({alpacacore::vendor::playerone::PlayerOneImageFormat::Raw8});
    fake.set_frame_data(std::vector<std::uint8_t>(16 * 8, 0));
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] {
        try {
            return driver->get_image_ready();
        } catch (...) {
            return false;
        }
    }));
    CHECK(driver->get_image_array().data.size() == 16 * 8);
    CHECK(fake.exposure_start_count() == 1);
    driver->set_connected(false);
    CHECK(fake.close_count() == 1);
}

TEST_CASE("Player One Camera Driver - LastExposure describes only successful frames in this session",
          "[playerone][camera][unit][metadata]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    const auto require_unset = [&](int code) {
        require_alpaca_error([&] { (void)driver->get_last_exposure_duration(); }, code);
        require_alpaca_error([&] { (void)driver->get_last_exposure_start_time(); }, code);
    };
    require_unset(alpacacore::AlpacaError::NotConnected);
    driver->set_connected(true);
    require_unset(alpacacore::AlpacaError::InvalidOperation);

    sdk.set_hold_image_data(true);
    driver->start_exposure(0.001, true);
    REQUIRE(eventually([&] { return sdk.image_data_started(); }));
    require_unset(alpacacore::AlpacaError::InvalidOperation);
    CHECK_FALSE(driver->get_image_ready());
    sdk.set_download_failure(true);
    sdk.set_hold_image_data(false);
    const auto failed = [&] {
        try {
            (void)driver->get_image_ready();
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::DriverException;
        }
        return false;
    };
    REQUIRE(eventually(failed));
    require_unset(alpacacore::AlpacaError::InvalidOperation);

    sdk.set_download_failure(false);
    driver->start_exposure(0.002, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    const auto start_a = driver->get_last_exposure_start_time();
    const auto require_a = [&] {
        CHECK(driver->get_last_exposure_duration() == Catch::Approx(0.002));
        CHECK(driver->get_last_exposure_start_time() == start_a);
    };
    require_a();
    driver->set_connected(true);  // No new session: keep the last success.
    require_a();
    require_alpaca_error([&] { driver->start_exposure(20.0, true); }, alpacacore::AlpacaError::InvalidValue);
    require_a();
    CHECK(driver->get_image_ready());

    sdk.set_hold_image_data(true);
    driver->start_exposure(0.003, true);
    REQUIRE(eventually([&] { return sdk.exposure_start_count() == 3 && sdk.image_data_started(); }));
    require_a();
    CHECK_FALSE(driver->get_image_ready());
    sdk.set_download_failure(true);
    sdk.set_hold_image_data(false);
    REQUIRE(eventually(failed));
    require_a();
    require_alpaca_error([&] { (void)driver->get_image_array(); }, alpacacore::AlpacaError::DriverException);

    sdk.set_download_failure(false);
    const auto before_b = std::chrono::system_clock::now();
    driver->start_exposure(0.004, true);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    CHECK(driver->get_last_exposure_duration() == Catch::Approx(0.004));
    CHECK(driver->get_last_exposure_start_time() >= before_b);
    driver->set_connected(false);
    require_unset(alpacacore::AlpacaError::NotConnected);
    driver->set_connected(true);
    require_unset(alpacacore::AlpacaError::InvalidOperation);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - LastExposure uses SDK microseconds and post-setup start",
          "[playerone][camera][unit][metadata]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    driver->set_num_x(12);
    sdk.set_hold_start_readback(true);
    driver->start_exposure(0.0010006, true);
    const bool reached_setup = eventually([&] { return sdk.start_readback_started(); });
    const auto after_setup_wait = std::chrono::system_clock::now();
    // Always release before asserting so a failure cannot strand the worker in destruction.
    sdk.set_hold_start_readback(false);
    REQUIRE(reached_setup);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    CHECK(sdk.get_config_int(17, 0) == 1001);
    CHECK(driver->get_last_exposure_duration() == Catch::Approx(0.001001));
    CHECK(driver->get_last_exposure_start_time() >= after_setup_wait);
    CHECK(driver->get_last_exposure_start_time() <= std::chrono::system_clock::now());

    driver->start_exposure(0.0, false);
    REQUIRE(eventually([&] { return driver->get_image_ready(); }));
    CHECK(sdk.get_config_int(17, 0) == 1);
    CHECK(driver->get_last_exposure_duration() == Catch::Approx(0.000001));
    driver->set_connected(false);
}

// Issue #1477: ICameraV4 PercentCompleted raises InvalidOperation outside Exposing; the cooler members raise
// PropertyNotImplemented on a camera without a cooler; an SDK failure reading CoolerPower is never reported as 0.
TEST_CASE("Player One Camera Driver - PercentCompleted only while Exposing", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);

    SECTION("idle before the first exposure") { alpacacore::test::require_percent_completed_unavailable(*driver); }
    SECTION("idle after a completed exposure") {
        driver->start_exposure(0.001, true);
        REQUIRE(eventually([&] {
            try {
                return driver->get_image_ready();
            } catch (...) {
                return false;
            }
        }));
        alpacacore::test::require_percent_completed_unavailable(*driver);
    }
    SECTION("idle after a failed download") {
        sdk.set_download_failure(true);
        driver->start_exposure(0.001, true);
        REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
        REQUIRE(eventually([&] {
            try {
                (void)driver->get_image_ready();
                return false;
            } catch (const alpacacore::AlpacaException&) {
                return true;
            }
        }));
        alpacacore::test::require_percent_completed_unavailable(*driver);
    }
    SECTION("while exposing") {
        sdk.set_image_ready(false);
        driver->start_exposure(10.0, true);
        REQUIRE(eventually([&] { return sdk.exposure_start_count() == 1; }));
        alpacacore::test::require_percent_completed_available(*driver);
        driver->abort_exposure();
    }
}

TEST_CASE("Player One Camera Driver - cooler members need the cooler capability", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_temperature_control_available(false);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    CHECK_FALSE(driver->get_can_get_cooler_power());
    alpacacore::test::require_no_cooler_members(*driver);
}

TEST_CASE("Player One Camera Driver - an SDK failure reading CoolerPower is not reported as 0",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_temperature_control_available(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_can_get_cooler_power());
    CHECK(driver->get_cooler_power() == 0.0);

    sdk.set_cooler_power_failure(true);
    require_alpaca_error([&] { (void)driver->get_cooler_power(); }, alpacacore::AlpacaError::DriverException);
    for (const auto& entry : driver->get_device_state()) {
        CHECK(entry.name != "CoolerPower");
    }
}

// Issue #1277: Player One reports a real per-gain e-/ADU (POA_EGAIN) when the camera has it; without it, or when the
// read fails, ElectronsPerADU raises PropertyNotImplemented instead of answering 1.0. The SDK has no full-well figure.
TEST_CASE("Player One Camera Driver - ElectronsPerADU is the POA_EGAIN read or PropertyNotImplemented",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    SECTION("camera with POA_EGAIN") {
        sdk.set_egain(0.37);
        auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
        require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); }, alpacacore::AlpacaError::NotConnected);
        driver->set_connected(true);
        CHECK(driver->get_electrons_per_adu() == Catch::Approx(0.37));
        driver->set_connected(false);
    }
    SECTION("camera without POA_EGAIN") {
        auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
        driver->set_connected(true);
        require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); },
                             alpacacore::AlpacaError::PropertyNotImplemented);
        driver->set_connected(false);
    }
    SECTION("the SDK read fails") {
        sdk.set_egain(0.37);
        auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
        driver->set_connected(true);
        sdk.set_egain_failure(true);
        require_alpaca_error([&] { (void)driver->get_electrons_per_adu(); },
                             alpacacore::AlpacaError::PropertyNotImplemented);
        driver->set_connected(false);
    }
}

TEST_CASE("Player One Camera Driver - FullWellCapacity is not implemented, SensorName has no placeholder",
          "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    require_alpaca_error([&] { (void)driver->get_full_well_capacity(); }, alpacacore::AlpacaError::NotConnected);
    driver->set_connected(true);
    require_alpaca_error([&] { (void)driver->get_full_well_capacity(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - an unknown SensorName is empty", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    sdk.set_no_cameras(true);
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    CHECK(driver->get_sensor_name().empty());
}

// A cancelled or failed body can still be configuring the camera, so the exposure settings stay locked until it
// returns and a client's write in that window is not lost.
TEST_CASE("Player One Camera Driver - settings stay locked while a cancelled body is still in setup",
          "[playerone][camera][unit][taskclock]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    alpacacore::test::FakeTaskClock clock;
    auto driver = make_clocked_driver(sdk, clock, 80ms);
    driver->set_num_x(12);  // makes the ROI dirty so the body reads the start position back
    const int client_start_x = driver->get_start_x();
    sdk.set_hold_start_readback(true);

    std::thread canceller;
    SECTION("watchdog timeout") {
        driver->start_exposure(0.020, true);
        REQUIRE(eventually([&] { return sdk.start_readback_started(); }));
        clock.advance(500ms);  // past the 20 ms + 80 ms deadline
        CHECK(eventually([&] {
            try {
                static_cast<void>(driver->get_image_ready());
            } catch (const alpacacore::AlpacaException&) {
                return true;
            }
            return false;
        }));
    }
    SECTION("AbortExposure") {
        driver->start_exposure(10.0, true);
        REQUIRE(eventually([&] { return sdk.start_readback_started(); }));
        canceller = std::thread([&] { driver->abort_exposure(); });
        REQUIRE(eventually([&] { return driver->get_camera_state() == alpacacore::CameraState::Idle; }));
    }
    // Release the held readback before asserting so a failure cannot strand the body in destruction.
    const auto refused = [&](const std::function<void()>& fn) {
        try {
            fn();
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code() == alpacacore::AlpacaError::InvalidOperation;
        }
        return false;
    };
    const bool start_refused = refused([&] { driver->set_start_x(client_start_x + 1); });
    const bool bin_refused = refused([&] { driver->set_bin_x(2); });
    const bool num_refused = refused([&] { driver->set_num_x(8); });
    sdk.set_hold_start_readback(false);
    CHECK(start_refused);
    CHECK(bin_refused);
    CHECK(num_refused);
    if (canceller.joinable()) canceller.join();
    CHECK(eventually([&] {
        try {
            driver->set_start_x(client_start_x + 1);
            return true;
        } catch (const alpacacore::AlpacaException&) {
            return false;
        }
    }));
    CHECK(driver->get_start_x() == client_start_x + 1);
    driver->set_connected(false);
}

TEST_CASE("Player One Camera Driver - ImageArray before any exposure is asked too early", "[playerone][camera][unit]") {
    alpacacore::test::FakePlayerOneSDK sdk;
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0, sdk);
    driver->set_connected(true);
    try {
        static_cast<void>(driver->get_image_array());
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.is_asked_too_early());
        CHECK(e.error_code() == alpacacore::AlpacaError::InvalidOperation);
    }
    driver->set_connected(false);
}
