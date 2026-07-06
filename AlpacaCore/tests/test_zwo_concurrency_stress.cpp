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

// Connect/disconnect/operate concurrency stress for the ZWO EFW filter wheel
// (issue #101). No fake seam exists for the EFW SDK, so on a hardware-free
// host every connect fails fast at enumeration — which still storms the
// AsyncConnectable machinery, the failure-path cleanup, and the property
// getters racing the lifecycle: the exact surfaces where this driver family's
// review findings lived. With a wheel attached the same tests exercise the
// full connect path.

#include <alpacacore/camera_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
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

// Camera lifecycle storm (issue #116): the ZWO camera's operational calls
// were converted from snapshot-then-call to the held-mutex_ shape, and its
// disconnect now publishes disconnected before the SDK close. On a
// hardware-free host every connect fails fast at enumeration, which still
// storms the connect-failure cleanup, the with_camera gate, and the
// disconnect ordering from many threads; with a camera attached the same
// test exercises the full path.
TEST_CASE("ZWO camera - concurrent connect/disconnect/operate stress", "[zwo][camera][stress]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        static_cast<void>(camera.get_camera_state());
        static_cast<void>(camera.get_ccd_temperature());
        camera.set_gain(50);
        static_cast<void>(camera.get_image_ready());
        camera.stop_exposure();
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("ZWO camera - destruction races an in-flight connect", "[zwo][camera][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0); });
}
