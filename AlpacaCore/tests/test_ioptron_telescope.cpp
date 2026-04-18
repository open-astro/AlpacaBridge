// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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

#include "catch2_compat.h"

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

using alpacacore::DeviceType;

TEST_CASE("iOptron Telescope Driver - Defaults", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_device_type() == DeviceType::Telescope);
    REQUIRE_FALSE(driver->get_connected());

    REQUIRE(driver->get_can_slew());
    REQUIRE(driver->get_can_slew_async());
    REQUIRE(driver->get_can_slew_alt_az());
    REQUIRE(driver->get_can_slew_alt_az_async());
    REQUIRE(driver->get_can_sync());
    REQUIRE_FALSE(driver->get_can_sync_alt_az());
    REQUIRE(driver->get_can_find_home());
    REQUIRE(driver->get_can_park());
    REQUIRE(driver->get_can_unpark());
    REQUIRE(driver->get_can_set_park());
    REQUIRE(driver->get_can_pulse_guide());
    REQUIRE(driver->get_can_set_guide_rates());
    REQUIRE(driver->get_can_move_axis(0));
    REQUIRE(driver->get_can_move_axis(1));
    REQUIRE_FALSE(driver->get_can_move_axis(2));
    REQUIRE(driver->get_can_set_tracking());
}

TEST_CASE("iOptron Telescope Driver - Target Range Validation", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    // When disconnected, get and set target throw (connection required)
    REQUIRE_THROWS(driver->get_target_right_ascension());
    REQUIRE_THROWS(driver->get_target_declination());

    REQUIRE_THROWS(driver->set_target_right_ascension(-0.1));
    REQUIRE_THROWS(driver->set_target_right_ascension(24.0));

    REQUIRE_THROWS(driver->set_target_declination(-90.1));
    REQUIRE_THROWS(driver->set_target_declination(90.1));
}

TEST_CASE("iOptron Telescope Driver - Axis Rate Ranges", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    auto primary = driver->get_axis_rate_range(0);
    REQUIRE(primary.second >= primary.first);

    auto secondary = driver->get_axis_rate_range(1);
    REQUIRE(secondary.second >= secondary.first);

    // Tertiary axis not supported; driver returns empty ranges (ConformU expects no 0..0 range).
    auto tertiary_ranges = driver->get_axis_rate_ranges(2);
    REQUIRE(tertiary_ranges.empty());

    // iOptron returns (0,0) for invalid axis rather than throwing
    auto invalid_axis_range = driver->get_axis_rate_range(2);
    REQUIRE(invalid_axis_range.first == 0.0);
    REQUIRE(invalid_axis_range.second == 0.0);
}

TEST_CASE("iOptron Telescope Driver - Disconnected Behavior", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    REQUIRE_FALSE(driver->get_connected());
    REQUIRE_THROWS(driver->get_right_ascension());
    REQUIRE_THROWS(driver->get_declination());
    REQUIRE_THROWS(driver->get_altitude());
    REQUIRE_THROWS(driver->get_azimuth());
}

TEST_CASE("iOptron Telescope Driver - Device metadata", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(3, conn);

    CHECK(driver->get_description() == "iOptron CEM120,70,40,26, GEM, HEM, HAE, HAZ series and SkyHunter Mount Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron Driver v1.0");
    CHECK(driver->get_driver_version() == "1.0.0");
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "iOptron_3");
}
