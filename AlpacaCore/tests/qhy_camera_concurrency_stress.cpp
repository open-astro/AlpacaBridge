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
    const bool cooled = GENERATE(false, true);
    auto fake = cooled ? alpacacore::test::FakeQHYSDK::with_one_cooled_camera()
                       : alpacacore::test::FakeQHYSDK::with_one_camera();
    fake.read_directly = true;
    alpacacore::test::LockedQHYSDK sdk(fake);
    auto camera = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);

    // DriverException is deliberately NOT expected (open-astro#980). Only a body
    // failure or the watchdog leaves a failure in the CaptureSession (#1736), which
    // makes get_image_ready() and get_image_array() throw DriverException, and none
    // can happen in this storm:
    //  - set_connected(false) marks the exposure stopping (begin_stop), cancels the
    //    SDK exposure and joins the body BEFORE close_camera(), and StartExposure
    //    refuses from the moment the disconnect starts, so a live body never meets a
    //    closed handle (FakeQHYSDK::require_open) and a stopped one keeps no failure.
    //  - With the real SDK the cancel makes GetQHYCCDSingleFrame fail; the body drops
    //    that failure because the exposure is marked stopping. FakeQHYSDK::cancel_exposure
    //    is a no-op and frame_ok defaults to true, so no SDK call fails here. If a case
    //    changes either (frame_ok, throw_from, before_call), or makes the frame invalid
    //    (frame_width, frame_height, frame_bpp, frame_channels), widen this set.
    //  - The session watchdog needs the exposure duration plus the default 60 s
    //    margin to pass; this case never passes one, and the fake is instant.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*camera, [&guard, cooled](alpacacore::AlpacaDriver& device) {
        auto& cam = static_cast<alpacacore::CameraDriver&>(device);
        guard([&] { static_cast<void>(cam.get_name()); });
        guard([&] { static_cast<void>(cam.get_camera_x_size()); });
        guard([&] { static_cast<void>(cam.get_camera_y_size()); });
        guard([&] { static_cast<void>(cam.get_pixel_size_x()); });
        guard([&] { static_cast<void>(cam.get_pixel_size_y()); });
        guard([&] { static_cast<void>(cam.get_readout_modes()); });
        guard([&] { static_cast<void>(cam.get_camera_state()); });
        guard([&] { static_cast<void>(cam.get_image_ready()); });
        guard([&] { static_cast<void>(cam.get_last_exposure_duration()); });
        guard([&] { static_cast<void>(cam.get_last_exposure_start_time()); });
        guard([&] { static_cast<void>(cam.get_image_array()); });
        guard([&] { static_cast<void>(cam.get_gain()); });
        guard([&] { static_cast<void>(cam.get_gain_min()); });
        guard([&] { static_cast<void>(cam.get_gain_max()); });
        guard([&] { static_cast<void>(cam.get_offset()); });
        guard([&] { static_cast<void>(cam.get_offset_min()); });
        guard([&] { static_cast<void>(cam.get_offset_max()); });
        guard([&] { static_cast<void>(cam.get_max_bin_x()); });
        guard([&] { static_cast<void>(cam.get_max_bin_y()); });
        if (cooled) {
            guard([&] { cam.set_cooler_on(false); });
            guard([&] { cam.set_cooler_on(true); });
        }
        // Valid values only: InvalidOperation is a live exposure or a competing
        // configuration transition, not a reason to admit arbitrary SDK failures.
        guard([&] { cam.set_readout_mode(1); });
        guard([&] { cam.set_readout_mode(0); });
        guard([&] { cam.set_gain(42); });
        guard([&] { cam.set_offset(17); });
        guard([&] { cam.set_bin_x(2); });
        guard([&] { cam.set_bin_y(1); });
        guard([&] { cam.set_num_x(32); });
        guard([&] { cam.set_num_y(24); });
        guard([&] { cam.set_start_x(0); });
        guard([&] { cam.set_start_y(0); });
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
