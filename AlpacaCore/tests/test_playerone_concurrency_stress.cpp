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

// Connect/disconnect/operate concurrency stress for the Player One Phoenix
// filter wheel (issue #101). Same rationale as the ZWO EFW stress file: no
// fake seam, so hardware-free hosts storm the failure path and the
// AsyncConnectable machinery; hosts with a wheel exercise the full connect.

#include <alpacacore/camera_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/vendor/playerone/playerone_camera_driver.h>
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

// Camera lifecycle storm (issue #116): the Player One camera's operational
// calls were converted from snapshot-then-call (camera_id_copy) to the
// held-mutex_ with_camera shape, and its disconnect now publishes
// disconnected before the SDK close. Hardware-free hosts storm the
// connect-failure and gate paths; with a camera attached the same test
// exercises the full path.
TEST_CASE("Player One camera - concurrent connect/disconnect/operate stress", "[playerone][camera][stress]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        static_cast<void>(camera.get_camera_state());
        static_cast<void>(camera.get_ccd_temperature());
        static_cast<void>(camera.get_gain());
        static_cast<void>(camera.get_cooler_on());
        camera.stop_exposure();
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Player One camera - destruction races an in-flight connect", "[playerone][camera][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::playerone::create_playerone_camera(0, 0); });
}
