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

// Connect/disconnect/operate concurrency stress for the Altair camera. The
// Altair camera is the ToupTek camera driver built through
// create_altair_camera(), so this registration runs the same storms as the
// ToupTek camera cases, through the Altair factory, hardware-free over the
// ToupTekSDK seam that AltairSDKWrapper implements. The fake is wrapped in
// LockedToupTekSDK so ThreadSanitizer findings point at DRIVER code, not at
// the deliberately unhardened fake.
//
// These pass on a correct driver under any build; their teeth come from the
// sanitizers-tsan CI job (and RUN_TSAN=1 ./scripts/ci_preflight.sh locally).

#include <alpacacore/camera_driver.h>
#include <alpacacore/vendor/altair/altair_camera_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_touptek_sdk.h"
#include "locked_touptek_sdk.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeToupTekSDK;
using alpacacore::test::LockedToupTekSDK;

namespace {

FakeToupTekSDK make_fake_with_camera() {
    FakeToupTekSDK fake;
    auto camera = FakeToupTekSDK::default_camera("altair-cam-0", "ALTAIR178M3");
    // Uncooled, like the ALTAIR178M3, so the storm also covers the no-cooler
    // paths (no thermal poller traffic) that a cooled fake never reaches.
    camera.supports_cooler = false;
    camera.supports_tec_onoff = false;
    camera.supports_pulse_guide = true;
    fake.cameras.push_back(camera);
    return fake;
}

}  // namespace

TEST_CASE("Altair camera - concurrent connect/disconnect/operate stress", "[altair][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, sdk);

    // The set REPLACES the default {NotConnected}. InvalidOperation: a set_gain()
    // landing while a previous iteration's exposure is still running hits the
    // driver's "no settings change during an exposure" guard, which is the
    // ASCOM contract working (same reasoning as the ToupTek camera case).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& cam = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { static_cast<void>(cam.get_camera_state()); });
        guard([&] { static_cast<void>(cam.get_gain()); });
        guard([&] { cam.set_gain(100); });
        guard([&] { static_cast<void>(cam.get_readout_mode()); });
        guard([&] { static_cast<void>(cam.get_cooler_on()); });
        guard([&] { cam.start_exposure(0.001, true); });
        guard([&] { cam.abort_exposure(); });
    });

    // The driver must still be usable after the storm, and the ref-counted
    // open ledger must balance once we settle it disconnected.
    CHECK(alpacacore::test::settle_connected(*driver, true));
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.ref_count("altair-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Altair camera - destruction races an in-flight connect", "[altair][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    alpacacore::test::run_destruction_during_connect_stress(
        [&]() { return alpacacore::vendor::altair::create_altair_camera(0, 0, sdk); });
    // Every destructor joined its task; no open can outlive its driver.
    CHECK(fake.ref_count("altair-cam-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("Altair camera - racing disconnect is never dropped", "[altair][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, sdk);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
    CHECK(fake.ref_count("altair-cam-0") == 0);
}

TEST_CASE("Altair camera - disconnect racing a NO-OP connect is never dropped", "[altair][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, sdk);
    CHECK(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false) == false);
    CHECK(fake.ref_count("altair-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
}

TEST_CASE("Altair camera - SYNC disconnect racing a NO-OP connect is never undone", "[altair][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::altair::create_altair_camera(0, 0, sdk);
    CHECK(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true) == false);
    CHECK(fake.ref_count("altair-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
}
