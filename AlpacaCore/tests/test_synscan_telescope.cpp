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

#include <catch2/catch_all.hpp>

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

using alpacacore::DeviceType;

TEST_CASE("SynScan Telescope Driver - Defaults", "[synscan][telescope][unit]") {
    alpacacore::vendor::synscan::ConnectionInfo conn;
    conn.type = alpacacore::vendor::synscan::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, conn, alpacacore::vendor::synscan::SynScanVersion::Auto);

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
}
