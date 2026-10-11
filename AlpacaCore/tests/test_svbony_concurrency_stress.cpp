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

// Connect/disconnect/operate concurrency stress for the SVBONY camera
// (issue #116). The camera's operational calls were converted from
// snapshot-then-call to the held-mutex_ with_camera shape, and its
// disconnect now publishes disconnected before the SDK close. The driver
// takes the SDK through the SVBSDK seam, so the lifecycle case runs over
// FakeSVBSDK behind LockedSVBSDK and the connect reaches the connected state
// and the exposure, control and teardown paths; the destruction case keeps
// the connect-failure path (an unreachable camera) over the same seam.

#include <alpacacore/camera_driver.h>
#include <alpacacore/vendor/svbony/svbony_camera_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_svbony_sdk.h"
#include "locked_svbony_sdk.h"

using alpacacore::AlpacaDriver;

TEST_CASE("SVBONY camera - concurrent connect/disconnect/operate stress", "[svbony][camera][stress]") {
    alpacacore::test::FakeSVBSDK fake;
    alpacacore::test::LockedSVBSDK sdk(fake);
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);

    // open-astro#326: one guard per call -- before this the callback stopped
    // at the first throw, so only get_camera_state() was ever storm-tested.
    // Connected registration: set_gain() racing a running exposure is InvalidOperation (ensure_not_exposing_locked),
    // and the fake camera has no temperature sensor, so get_ccd_temperature() is PropertyNotImplemented.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation,
                                            alpacacore::AlpacaError::PropertyNotImplemented};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { static_cast<void>(camera.get_camera_state()); });
        guard([&] { static_cast<void>(camera.get_ccd_temperature()); });
        guard([&] { camera.set_gain(50); });
        guard([&] { static_cast<void>(camera.get_image_ready()); });
        guard([&] { camera.start_exposure(0.01, true); });
        guard([&] { camera.stop_exposure(); });
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

TEST_CASE("SVBONY camera - destruction races an in-flight connect", "[svbony][camera][stress]") {
    // The fake outlives every driver the harness builds on it.
    alpacacore::test::FakeSVBSDK fake;
    fake.cameras.clear();  // no camera: every connect takes the failure path
    alpacacore::test::LockedSVBSDK sdk(fake);
    alpacacore::test::run_destruction_during_connect_stress(
        [&sdk]() { return alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk); });
}
