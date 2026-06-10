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

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#include <alpacacore/version.h>

#include <functional>

#include "catch2_compat.h"

using alpacacore::DeviceType;

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

} // namespace

TEST_CASE("Celestron Telescope Driver - Defaults", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_device_type() == DeviceType::Telescope);
    REQUIRE_FALSE(driver->get_connected());

    CHECK(driver->get_name() == "Celestron NexStar Telescope");

    REQUIRE(driver->get_can_slew());
    REQUIRE(driver->get_can_slew_async());
    REQUIRE_FALSE(driver->get_can_slew_alt_az());
    REQUIRE_FALSE(driver->get_can_slew_alt_az_async());
    REQUIRE(driver->get_can_sync());
    REQUIRE_FALSE(driver->get_can_sync_alt_az());
    REQUIRE(driver->get_can_find_home());
    REQUIRE(driver->get_can_park());
    REQUIRE(driver->get_can_unpark());
    REQUIRE(driver->get_can_set_park());
    REQUIRE_FALSE(driver->get_can_pulse_guide());
    REQUIRE_FALSE(driver->get_can_set_guide_rates());
    REQUIRE(driver->get_can_set_tracking());
    REQUIRE(driver->get_can_move_axis(0));
    REQUIRE(driver->get_can_move_axis(1));
    REQUIRE_FALSE(driver->get_can_move_axis(2));
}

TEST_CASE("Celestron Telescope Driver - Device metadata", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(3, conn);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Celestron NexStar Mount Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Celestron NexStar Driver v0.1");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "Celestron_3");
}

TEST_CASE("Celestron Telescope Driver - Disconnected Behavior", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    REQUIRE_FALSE(driver->get_connected());

    // Position queries require connection
    CHECK_THROWS_AS(driver->get_right_ascension(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_declination(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_altitude(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_azimuth(), alpacacore::AlpacaException);

    // Tracking requires connection
    CHECK_THROWS_AS(driver->get_tracking(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_tracking(true), alpacacore::AlpacaException);

    // Slew requires target set (throws before connection check)
    CHECK_THROWS_AS(driver->slew_to_target_async(), alpacacore::AlpacaException);

    // Park requires connection
    CHECK_THROWS_AS(driver->park(), alpacacore::AlpacaException);

    // Action and command methods
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Celestron Telescope Driver - Unsupported actions", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Celestron Telescope Driver - Target Range Validation", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    // Target not yet set
    REQUIRE_THROWS(driver->get_target_right_ascension());
    REQUIRE_THROWS(driver->get_target_declination());

    // RA range: [0, 24)
    REQUIRE_THROWS(driver->set_target_right_ascension(-0.1));
    REQUIRE_THROWS(driver->set_target_right_ascension(24.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(0.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(23.999));

    // Dec range: [-90, 90]
    REQUIRE_THROWS(driver->set_target_declination(-90.1));
    REQUIRE_THROWS(driver->set_target_declination(90.1));
    REQUIRE_NOTHROW(driver->set_target_declination(-90.0));
    REQUIRE_NOTHROW(driver->set_target_declination(0.0));
    REQUIRE_NOTHROW(driver->set_target_declination(90.0));
    REQUIRE_NOTHROW(driver->set_target_declination(45.0));
}

TEST_CASE("Celestron Telescope Driver - Axis Rate Ranges", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    // Primary axis (RA/Azimuth)
    auto primary = driver->get_axis_rate_range(0);
    REQUIRE(primary.first == 0.0);
    REQUIRE(primary.second > primary.first);

    // Secondary axis (Dec/Altitude)
    auto secondary = driver->get_axis_rate_range(1);
    REQUIRE(secondary.first == 0.0);
    REQUIRE(secondary.second > secondary.first);

    // Tertiary axis not supported
    auto tertiary_ranges = driver->get_axis_rate_ranges(2);
    REQUIRE(tertiary_ranges.empty());

    REQUIRE_THROWS(driver->get_axis_rate_range(2));
}

TEST_CASE("Celestron Telescope Driver - Target Coordinate Persistence", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 12.0);

    REQUIRE_NOTHROW(driver->set_target_declination(45.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(6.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 6.0);
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(0.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 0.0);
    REQUIRE_NOTHROW(driver->set_target_right_ascension(23.999));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 23.999);

    REQUIRE_NOTHROW(driver->set_target_declination(-90.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), -90.0);
    REQUIRE_NOTHROW(driver->set_target_declination(90.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 90.0);
}

TEST_CASE("Celestron Telescope Driver - Site Property Validation", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    require_alpaca_error([&]() { driver->set_site_elevation(-300.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(10000.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_site_elevation(-300.0));
    REQUIRE_NOTHROW(driver->set_site_elevation(10000.0));
    ALPACA_REQUIRE_APPROX(driver->get_site_elevation(), 10000.0);

    require_alpaca_error([&]() { driver->set_site_latitude(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(90.1); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&]() { driver->set_site_longitude(-180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(180.1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("Celestron Telescope Driver - Telescope Properties", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    CHECK(driver->get_interface_version() >= 3);

    auto eq = driver->get_equatorial_system();
    CHECK((eq == alpacacore::EquatorialSystem::Topocentric ||
           eq == alpacacore::EquatorialSystem::J2000 ||
           eq == alpacacore::EquatorialSystem::Other));

    auto align = driver->get_alignment_mode();
    CHECK((align == alpacacore::AlignmentMode::AltAz ||
           align == alpacacore::AlignmentMode::Polar ||
           align == alpacacore::AlignmentMode::GermanPolar));

    auto rates = driver->get_tracking_rates();
    CHECK_FALSE(rates.empty());

    CHECK(driver->get_slew_settle_time() >= 0);
}

TEST_CASE("Celestron Telescope Driver - ASCOM Error Codes", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    // TODO: Celestron check_connected() throws DriverException (0x500) instead of
    // NotConnected (0x407). Fix the driver, then change these to AlpacaError::NotConnected.
    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_altitude(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_tracking(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->set_tracking(true); }, alpacacore::AlpacaError::DriverException);

    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);
}
