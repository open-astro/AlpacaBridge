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
// (issue #116). The connected registration uses a scripted fake SDK so
// acquisition, cancellation, and disconnect paths run on every test host.

#include <alpacacore/camera_driver.h>
#include <alpacacore/vendor/svbony/svbony_camera_driver.h>

#include <chrono>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_svbony_sdk.h"

using alpacacore::AlpacaDriver;

TEST_CASE("SVBONY camera - concurrent connect/disconnect/operate stress", "[svbony][camera][stress]") {
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0);

    // open-astro#326: one guard per call -- before this the callback stopped
    // at the first throw, so only get_camera_state() was ever storm-tested.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { static_cast<void>(camera.get_camera_state()); });
        guard([&] { static_cast<void>(camera.get_ccd_temperature()); });
        guard([&] { camera.set_gain(50); });
        guard([&] { static_cast<void>(camera.get_image_ready()); });
        guard([&] { camera.abort_exposure(); });
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
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::svbony::create_svbony_camera(0, 0); });
}

TEST_CASE("SVBONY camera - connected acquisition and lifecycle stress", "[svbony][camera][stress]") {
    alpacacore::test::FakeSVBSDK sdk;
    auto driver = alpacacore::vendor::svbony::create_svbony_camera(0, 0, sdk);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));

    // Prove the connected seam reaches acquisition before the lifecycle storm;
    // otherwise repeated racing disconnects could leave this as fail-fast-only
    // coverage while still satisfying the stress guard's non-vacuity check.
    sdk.block_video_data();
    driver->start_exposure(0.001, true);
    REQUIRE(sdk.wait_for_video_data(std::chrono::seconds(2)));
    CHECK(sdk.call_count("start_video_capture") > 0);
    CHECK(sdk.call_count("get_video_data") > 0);
    sdk.release_video_data();
    driver->abort_exposure();

    alpacacore::test::StressCallGuard guard(
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
         alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::MethodNotImplemented});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { camera.start_exposure(0.001, true); });
        guard([&] { static_cast<void>(camera.get_image_ready()); });
        guard([&] { static_cast<void>(camera.get_image_array()); });
        guard([&] { static_cast<void>(camera.get_last_exposure_duration()); });
        guard([&] { static_cast<void>(camera.get_last_exposure_start_time()); });
        guard([&] { camera.abort_exposure(); });
        guard([&] { static_cast<void>(camera.get_gain()); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}
