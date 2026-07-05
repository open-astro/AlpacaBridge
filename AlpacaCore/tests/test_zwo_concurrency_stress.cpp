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

// Connect/disconnect/operate concurrency stress for the ZWO EFW filter wheel
// (issue #101). No fake seam exists for the EFW SDK, so on a hardware-free
// host every connect fails fast at enumeration — which still storms the
// AsyncConnectable machinery, the failure-path cleanup, and the property
// getters racing the lifecycle: the exact surfaces where this driver family's
// review findings lived. With a wheel attached the same tests exercise the
// full connect path.

#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

TEST_CASE("ZWO EFW - concurrent connect/disconnect/operate stress", "[zwo][filterwheel][stress]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
        static_cast<void>(wheel.get_position());
        wheel.set_position(1);
        static_cast<void>(wheel.get_names());
        static_cast<void>(wheel.get_focus_offsets());
    });

    // Still alive and coherent after the storm (Connected reflects whether a
    // physical wheel is attached; both outcomes are valid here).
    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("ZWO EFW - destruction races an in-flight connect", "[zwo][filterwheel][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0); });
}
