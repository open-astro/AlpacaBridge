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

// Connect/disconnect/operate concurrency stress for the gphoto camera
// (issue #241). Runs against FakeGPhotoSDK wrapped in LockedGPhotoSDK (issue
// #489's fault-injection seam) so a real connect path -- open/configure/prime/
// close -- is what gets storm-tested, not just the fast-fail-at-autodetect
// path a hardware-free host previously hit for every connect attempt.

#include <alpacacore/camera_driver.h>
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_gphoto_sdk.h"
#include "fake_raw_decoder.h"
#include "locked_gphoto_sdk.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeGPhotoSDK;
using alpacacore::test::FakeRawDecoder;
using alpacacore::test::LockedGPhotoSDK;
using alpacacore::test::reset_gphoto_sensor_cache;
using alpacacore::test::unique_test_model;

namespace {

FakeGPhotoSDK::FakeCamera make_stress_camera() {
    FakeGPhotoSDK::FakeCamera cam;
    cam.model = unique_test_model("Nikon DSC D5300 (stress)");
    cam.port = "usb:001,099";
    cam.choices["iso"] = {"100", "200", "400"};
    cam.choice_value["iso"] = "200";
    cam.choices["shutterspeed2"] = {"1/200", "1", "bulb"};
    cam.choice_value["shutterspeed2"] = "1/200";
    cam.toggle_value["bulb"] = false;
    return cam;
}

}  // namespace

TEST_CASE("GPhoto camera - concurrent connect/disconnect/operate stress", "[gphoto][camera][stress]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_stress_camera());
    LockedGPhotoSDK locked_sdk(fake);
    FakeRawDecoder decoder;
    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, locked_sdk, decoder);

    // open-astro#326: one guard per call -- before this the callback stopped
    // at the first throw, so only the first operation was ever storm-tested.
    // This registration runs CONNECTED over the fake seam, so widen the
    // expected set the way ci-gates.instructions.md requires (the ctor arg REPLACES the
    // default, so NotConnected stays listed): today's four calls can only
    // throw NotConnected, but the next call added here -- set_gain
    // (InvalidValue/InvalidOperation), get_ccd_temperature
    // (PropertyNotImplemented), start_exposure (InvalidValue) -- would turn
    // the storm into a nondeterministic red otherwise (review of #546).
    alpacacore::test::StressCallGuard guard{
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
         alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::PropertyNotImplemented}};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { static_cast<void>(camera.get_camera_state()); });
        guard([&] { static_cast<void>(camera.get_gain()); });
        guard([&] { static_cast<void>(camera.get_image_ready()); });
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

TEST_CASE("GPhoto camera - destruction races an in-flight connect", "[gphoto][camera][stress]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_stress_camera());
    LockedGPhotoSDK locked_sdk(fake);
    FakeRawDecoder decoder;
    alpacacore::test::run_destruction_during_connect_stress(
        [&]() { return alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, locked_sdk, decoder); });
}
