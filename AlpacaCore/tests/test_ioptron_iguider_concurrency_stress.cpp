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

// Connect/disconnect/operate concurrency stress for the iOptron iGuider camera,
// over FakeIGuiderV4L2 behind LockedIGuiderV4L2, so every connect really opens,
// streams and starts the stream worker, and ThreadSanitizer findings point at
// driver code rather than at the unhardened fake.

#include <alpacacore/camera_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_camera_driver.h>

#include <chrono>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_iguider_v4l2.h"
#include "locked_iguider_v4l2.h"

using alpacacore::AlpacaDriver;
namespace AlpacaError = alpacacore::AlpacaError;

TEST_CASE("iOptron iGuider camera - connected acquisition and lifecycle stress", "[ioptron][camera][stress]") {
    alpacacore::test::FakeIGuiderV4L2 fake;
    alpacacore::test::LockedIGuiderV4L2 v4l2(fake);
    auto driver = alpacacore::vendor::ioptron::create_iguider_camera(0, "", v4l2);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    // The connect reached the stream before the storm, so the storm runs over a
    // live worker, not over fail-fast connection checks only.
    REQUIRE(fake.wait_for_frames(2, std::chrono::seconds(2)));

    alpacacore::test::StressCallGuard guard({AlpacaError::NotConnected, AlpacaError::InvalidOperation});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { camera.start_exposure(0.01, true); });
        guard([&] { static_cast<void>(camera.get_image_ready()); });
        guard([&] { static_cast<void>(camera.get_image_array()); });
        guard([&] { static_cast<void>(camera.get_camera_state()); });
        guard([&] { static_cast<void>(camera.get_percent_completed()); });
        guard([&] { camera.set_gain(5); });
        guard([&] { camera.set_num_x(640); });
        guard([&] { camera.abort_exposure(); });
        guard([&] { static_cast<void>(camera.get_link_fault()); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.open_handles() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("iOptron iGuider camera - destruction races an in-flight connect", "[ioptron][camera][stress]") {
    alpacacore::test::FakeIGuiderV4L2 fake;
    alpacacore::test::LockedIGuiderV4L2 v4l2(fake);
    alpacacore::test::run_destruction_during_connect_stress(
        [&]() { return alpacacore::vendor::ioptron::create_iguider_camera(0, "", v4l2); });
    // Every destructor joined its task and closed what it opened.
    CHECK(fake.open_handles() == 0);
}

TEST_CASE("iOptron iGuider camera - racing disconnect is never dropped", "[ioptron][camera][stress]") {
    alpacacore::test::FakeIGuiderV4L2 fake;
    alpacacore::test::LockedIGuiderV4L2 v4l2(fake);
    auto driver = alpacacore::vendor::ioptron::create_iguider_camera(0, "", v4l2);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
    CHECK(fake.open_handles() == 0);
}

TEST_CASE("iOptron iGuider camera - disconnect racing a no-op connect is never dropped", "[ioptron][camera][stress]") {
    alpacacore::test::FakeIGuiderV4L2 fake;
    alpacacore::test::LockedIGuiderV4L2 v4l2(fake);
    auto driver = alpacacore::vendor::ioptron::create_iguider_camera(0, "", v4l2);
    CHECK(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false) == false);
    CHECK(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true) == false);
    CHECK(fake.open_handles() == 0);
}
