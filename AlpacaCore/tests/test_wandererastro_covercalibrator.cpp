// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>
#include <alpacacore/version.h>

#include <functional>
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
    // and are omitted, leaving just the TimeStamp. The non-compliant "Connected"
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
    REQUIRE(has_timestamp);
}

TEST_CASE("WandererAstro CoverCalibrator Driver - HaltCover is implemented", "[wandererastro][covercalibrator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, "/dev/ttyUSB0");

    // ASCOM requires HaltCover to function on a cover-capable device, so it must
    // NOT throw NotImplemented. With no hardware it requires a connection and
    // therefore reports NotConnected (0x407) rather than MethodNotImplemented.
    require_alpaca_error([&]() { driver->halt_cover(); }, alpacacore::AlpacaError::NotConnected);
}
