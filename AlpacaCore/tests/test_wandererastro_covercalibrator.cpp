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
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <optional>
#include <variant>

#include "catch2_compat.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

}  // namespace

TEST_CASE("WandererAstro CoverCalibrator Driver - Defaults", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::CoverCalibrator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "WandererAstro WandererCover V4");
    // MaxBrightness is a static capability and must be readable without a connection.
    CHECK(driver->get_max_brightness() == 255);
}

TEST_CASE("WandererAstro CoverCalibrator Driver - Device metadata", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "WandererAstro WandererCover V4 CoverCalibrator Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore WandererAstro CoverCalibrator Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 2);
    CHECK(driver->get_unique_id() == "WANDERERASTRO_COVERCALIBRATOR_3");
}

TEST_CASE("WandererAstro CoverCalibrator Driver - Firmware unavailable when disconnected",
          "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, "/dev/ttyUSB0");

    // Firmware is read from the streamed status frame, so it is only known while
    // connected. Disconnected, the web UI shows no firmware row.
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_device_firmware() == std::nullopt);
}

TEST_CASE("WandererAstro CoverCalibrator Driver - Not connected throws", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, "/dev/ttyUSB0");

    // Every property/method that needs a live link must throw NotConnected
    // (0x407) so ConformU sees the correct Alpaca error number.
    require_alpaca_error([&]() { driver->get_brightness(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_calibrator_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_calibrator_changing(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_cover_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_cover_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->calibrator_off(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->open_cover(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->close_cover(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt_cover(); }, alpacacore::AlpacaError::NotConnected);
    // A valid-brightness CalibratorOn passes range validation, then trips on the
    // connection check.
    require_alpaca_error([&]() { driver->calibrator_on(128); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro CoverCalibrator Driver - Unsupported actions", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, "/dev/ttyUSB0");

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("WandererAstro CoverCalibrator Driver - Calibrator capabilities", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(2, "/dev/ttyUSB0");

    // MaxBrightness reflects the WandererCover PWM range and is connection-independent.
    REQUIRE(driver->get_max_brightness() == 255);

    // set_brightness shares CalibratorOn's validation: a valid value falls
    // through to the connection check, an invalid one is rejected up front.
    require_alpaca_error([&]() { driver->set_brightness(10); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_brightness(-5); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("WandererAstro CoverCalibrator Driver - Value range validation", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, "/dev/ttyUSB0");

    // Out-of-range brightness must throw InvalidValue (0x401), not normalize.
    require_alpaca_error([&]() { driver->calibrator_on(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->calibrator_on(256); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->calibrator_on(1000); }, alpacacore::AlpacaError::InvalidValue);

    // In-range boundary values are accepted by validation, then hit NotConnected.
    require_alpaca_error([&]() { driver->calibrator_on(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->calibrator_on(255); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro CoverCalibrator Driver - State machine", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, "/dev/ttyUSB0");

    // Not connecting and not connected at rest.
    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list. The non-compliant "Connected"
    // entry must never appear.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        REQUIRE(entry.name != "CoverState");
        REQUIRE(entry.name != "CalibratorState");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);
}

TEST_CASE("WandererAstro CoverCalibrator Driver - HaltCover is implemented", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, "/dev/ttyUSB0");

    // ASCOM requires HaltCover to function on a cover-capable device, so it must
    // NOT throw NotImplemented. With no hardware it requires a connection and
    // therefore reports NotConnected (0x407) rather than MethodNotImplemented.
    require_alpaca_error([&]() { driver->halt_cover(); }, alpacacore::AlpacaError::NotConnected);
}

// ---------------------------------------------------------------------------
// Issue #237: a device that stops streaming must not be served from the cache
// forever. Wire-level over a pty-backed streamer (fake_serial_streamer.h).
// ---------------------------------------------------------------------------

#include <chrono>
#include <thread>

#include "fake_serial_streamer.h"
#include "fake_task_clock.h"

namespace {

// Moves the fake clock by @p d, then waits (bounded, real time) for the
// reader thread's next pass: its silence check is the one clock read a muted
// or severed link makes per pass, taken under the lock the driver's getters
// also take, so a getter called after this returns sees that pass's verdict.
bool advance_one_pass(alpacacore::test::FakeTaskClock& clock, std::chrono::nanoseconds d) {
    clock.advance(d);
    return clock.wait_for_now_calls(clock.now_calls() + 1, std::chrono::milliseconds(2000));
}

// set_muted() can land just after the streamer committed one more frame to the
// pty; give the reader time to take it at the current virtual time before the
// clock moves, or it would restart the silence window.
void drain_after_mute() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }

template <typename Pred>
bool wait_until_cover(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

}  // namespace

// <model>A<fw>A<closePos>A<openPos>A<curPos>A<voltage>A<brightness>A<dew>A<asiair>
const char* const kCoverFrame = "WandererCoverV4ProA20240301A10.0A270.0A10.0A12.5A0A0A0\n";

TEST_CASE("WandererAstro CoverCalibrator Driver - Silent link reads Unknown and refuses commands (issue #237)",
          "[wandererastro][covercalibrator][unit][fake]") {
    using alpacacore::CalibratorState;
    using alpacacore::CoverState;
    alpacacore::test::FakeTaskClock clock;  // outlives the driver
    alpacacore::test::FakeSerialStreamer cover(kCoverFrame, std::chrono::milliseconds(300));
    auto driver =
        alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, cover.slave_path(), 19200, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_cover_state() == CoverState::Closed);
    driver->calibrator_on(100);
    CHECK(driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(driver->get_brightness() == 100);

    cover.set_muted(true);
    drain_after_mute();
    // The 10 s limit is inclusive: exactly 10 s of silence still serves the cache.
    REQUIRE(advance_one_pass(clock, std::chrono::seconds(10)));
    CHECK(driver->get_cover_state() == CoverState::Closed);
    CHECK(driver->get_brightness() == 100);
    // One millisecond more latches the fault.
    REQUIRE(advance_one_pass(clock, std::chrono::milliseconds(1)));
    CHECK(driver->get_cover_state() == CoverState::Unknown);
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().find("no status frame") != std::string::npos);  // surfaced to the management listing
    CHECK_FALSE(driver->get_cover_moving());
    // The panel is unreachable: its commanded state is no longer known to hold.
    CHECK(driver->get_calibrator_state() == CalibratorState::Unknown);
    require_alpaca_error([&]() { (void)driver->get_brightness(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->open_cover(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->close_cover(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->halt_cover(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->calibrator_on(50); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->calibrator_off(); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(cover.received("1001"));  // nothing went on the wire while faulted

    cover.set_muted(false);
    CHECK(wait_until_cover([&] { return driver->get_cover_state() == CoverState::Closed; },
                           std::chrono::milliseconds(3000)));
    CHECK(driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(driver->get_brightness() == 100);
    CHECK(driver->get_link_fault().empty());
    CHECK_NOTHROW(driver->set_connected(false));
}
