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

#include <alpacacore/camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_qhy_sdk.h"
#include "locked_qhy_sdk.h"

TEST_CASE("QHY camera - concurrent connect/disconnect/exposure stress", "[qhy][camera][stress]") {
    auto fake = alpacacore::test::FakeQHYSDK::with_one_camera();
    fake.read_directly = true;
    alpacacore::test::LockedQHYSDK sdk(fake);
    auto camera = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*camera, [&guard](alpacacore::AlpacaDriver& device) {
        auto& cam = static_cast<alpacacore::CameraDriver&>(device);
        guard([&] { static_cast<void>(cam.get_camera_state()); });
        guard([&] { static_cast<void>(cam.get_image_ready()); });
        guard([&] { static_cast<void>(cam.get_image_array()); });
        guard([&] { cam.start_exposure(0.001, true); });
        guard([&] { cam.abort_exposure(); });
    });

    CHECK(alpacacore::test::settle_connected(*camera, true));
    CHECK(alpacacore::test::settle_connected(*camera, false));
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}
