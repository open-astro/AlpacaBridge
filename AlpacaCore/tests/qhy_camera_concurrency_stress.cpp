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

    // DriverException is deliberately NOT expected (open-astro#980). Only a worker
    // failure, the watchdog, or a failed image build in get_image_array() publishes
    // QHYExposureStatus::Failed, which makes get_image_ready() and
    // get_image_array() throw DriverException, and none can happen in this storm:
    //  - set_connected(false) holds exposure_lifecycle_mutex_, cancels and joins the
    //    worker BEFORE close_camera() and keeps the mutex through the close, so
    //    start_exposure() cannot spawn a worker in the gap and a live worker never
    //    meets a closed handle (FakeQHYSDK::require_open). A join that times out
    //    marks the worker superseded before detaching it.
    //  - With the real SDK the cancel makes GetQHYCCDSingleFrame fail, so the worker
    //    would publish Failed while connected_ is still true. FakeQHYSDK::cancel_exposure
    //    is a no-op and frame_ok defaults to true, so no SDK call fails here. If a case
    //    changes either (frame_ok, throw_from, before_call), or makes the frame invalid
    //    (frame_width, frame_height, frame_bpp, frame_channels), widen this set.
    //  - expire_overdue_exposure_locked() needs the exposure duration plus the default
    //    60 s watchdog margin to pass; this case never passes one, and the fake is instant.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*camera, [&guard](alpacacore::AlpacaDriver& device) {
        auto& cam = static_cast<alpacacore::CameraDriver&>(device);
        guard([&] { static_cast<void>(cam.get_camera_state()); });
        guard([&] { static_cast<void>(cam.get_image_ready()); });
        guard([&] { static_cast<void>(cam.get_last_exposure_duration()); });
        guard([&] { static_cast<void>(cam.get_last_exposure_start_time()); });
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
