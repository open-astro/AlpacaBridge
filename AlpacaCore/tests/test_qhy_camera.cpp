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
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include "catch2_compat.h"

#include <alpacacore/vendor/qhy/qhy_camera_driver.h>

TEST_CASE("QHY Camera Driver - Defaults", "[qhy][camera][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Camera);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "QHY Camera");
}
