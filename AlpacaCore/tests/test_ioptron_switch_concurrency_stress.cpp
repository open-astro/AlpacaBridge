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

// Connect/disconnect/operate concurrency stress for the iOptron iMate
// PowerBox Switch (#271). No fake seam exists for the libgpiod chip this
// driver opens, so on a hardware-free host every connect fails fast (the
// configured gpio_chip_path either doesn't exist or has no matching lines),
// which still storms the AsyncConnectable machinery and the failure-path
// cleanup -- same shape as the ZWO dew-heater-switch case. With the iMate
// PowerBox's GPIO bank present the same test exercises the full connect
// path.
//
// This is a SEPARATE file from test_ioptron_concurrency_stress.cpp because
// the PowerBox Switch driver, unlike the mount/filter-wheel/focuser above,
// is only compiled into alpacacore_ioptron when libgpiod >= 2.0 was found
// (ALPACACORE_IOPTRON_POWERBOX, see AlpacaCore/src/vendors/ioptron/CMakeLists.txt)
// -- putting this in the always-built file would fail to link whenever that
// feature is off. CMakeLists.txt only adds this file to TEST_SOURCES under
// the same guard as test_ioptron_switch.cpp.

#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

TEST_CASE("iMate PowerBox Switch - concurrent connect/disconnect/operate stress", "[ioptron][switch][stress]") {
    auto driver = alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config());

    // open-astro#326: one guard per call -- before this the callback stopped
    // at the first throw, so only get_max_switch() was ever storm-tested.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
        guard([&] { static_cast<void>(sw.get_max_switch()); });
        guard([&] { static_cast<void>(sw.get_switch(1)); });
        // switch 1 ("DC1"); switch 0 is read-only pass-through
        guard([&] { sw.set_switch(1, true); });
        guard([&] { static_cast<void>(sw.get_switch_value(1)); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected():
    // right after a storm the last async task may still be in flight, so a
    // single sync disconnect can legitimately no-op against the pending-
    // disconnect machinery and the CHECK below would fail on a correct driver.
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("iMate PowerBox Switch - destruction races an in-flight connect", "[ioptron][switch][stress]") {
    alpacacore::test::run_destruction_during_connect_stress([]() {
        return alpacacore::vendor::ioptron::create_ioptron_switch(
            0, alpacacore::vendor::ioptron::default_imate_powerbox_config());
    });
}
