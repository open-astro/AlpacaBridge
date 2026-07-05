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
// or any commercial offering, you must comply with all SSPL v1 requirements.

// Connect/disconnect/operate concurrency stress for the Player One Phoenix
// filter wheel (issue #101). Same rationale as the ZWO EFW stress file: no
// fake seam, so hardware-free hosts storm the failure path and the
// AsyncConnectable machinery; hosts with a wheel exercise the full connect.

#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

TEST_CASE("Player One Phoenix - concurrent connect/disconnect/operate stress", "[playerone][filterwheel][stress]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
        static_cast<void>(wheel.get_position());
        wheel.set_position(1);
        static_cast<void>(wheel.get_names());
        static_cast<void>(wheel.get_focus_offsets());
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Player One Phoenix - destruction races an in-flight connect", "[playerone][filterwheel][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0); });
}
