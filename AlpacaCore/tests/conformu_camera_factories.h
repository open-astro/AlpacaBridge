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

#pragma once

// Test-only: each camera driver over its roster fake, for the fake ConformU server (fake_conformu_server.cpp, #2078
// slice 5). One factory per kFakeConnectableRoster camera row, each guarded by the FAKE_CONFORMU_<VENDOR> macro the
// server's CMake sets when that vendor is built. Each fake is a function-local static, built when its row is served,
// so it outlives every driver the server builds. It lives here, not in AlpacaHTTP, so the vendor headers stay out of
// AlpacaHTTP (scripts/check_layering.py).

#include <alpacacore/camera_driver.h>

#include <memory>

#ifdef FAKE_CONFORMU_ZWO
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>

#include "fake_zwo_sdk.h"
#endif
#ifdef FAKE_CONFORMU_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_iguider_camera_driver.h>

#include "fake_iguider_v4l2.h"
#include "locked_iguider_v4l2.h"
#endif
#ifdef FAKE_CONFORMU_QHY
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>

#include "fake_qhy_sdk.h"
#include "locked_qhy_sdk.h"
#endif
#ifdef FAKE_CONFORMU_PLAYERONE
#include <alpacacore/vendor/playerone/playerone_camera_driver.h>

#include "fake_playerone_sdk.h"
#endif
#if defined(FAKE_CONFORMU_TOUPTEK) || defined(FAKE_CONFORMU_ALTAIR)
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>

#include "fake_touptek_sdk.h"
#include "locked_touptek_sdk.h"
#endif
#ifdef FAKE_CONFORMU_ALTAIR
#include <alpacacore/vendor/altair/altair_camera_driver.h>
#endif
#ifdef FAKE_CONFORMU_SVBONY
#include <alpacacore/vendor/svbony/svbony_camera_driver.h>

#include "fake_svbony_sdk.h"
#endif
#ifdef FAKE_CONFORMU_GPHOTO
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>

#include "fake_gphoto_sdk.h"
#include "fake_raw_decoder.h"
#include "locked_gphoto_sdk.h"
#endif

namespace alpacacore::test::conformu {

#ifdef FAKE_CONFORMU_ZWO
inline FakeZWOSDK& zwo_camera_sdk() {
    // Each exposure takes its length in real time, as on a camera: one reported ready sooner is a stale frame, which
    // the driver discards and retakes (#2414).
    static FakeZWOSDK sdk;
    [[maybe_unused]] static const bool timed = [] {
        sdk.camera = FakeZWOSDK::conformu_camera();
        sdk.exposure_clock = &util::default_task_clock();
        return true;
    }();
    return sdk;
}
inline std::unique_ptr<CameraDriver> make_zwo_camera(int device_number) {
    return vendor::zwo::create_zwo_camera(device_number, zwo_camera_sdk().camera.camera_id, zwo_camera_sdk());
}
#endif

#ifdef FAKE_CONFORMU_IOPTRON
// The fake iGuider behind the per-call lock its tests use (the contract sweep's IGuiderV4L2Hold).
struct IGuiderHost {
    FakeIGuiderV4L2 fake;
    LockedIGuiderV4L2 v4l2{fake};
};
inline IGuiderHost& iguider_host() {
    static IGuiderHost host;
    return host;
}
inline std::unique_ptr<CameraDriver> make_iguider_camera(int device_number) {
    return vendor::ioptron::create_iguider_camera(device_number, "", iguider_host().v4l2);
}
#endif

#ifdef FAKE_CONFORMU_QHY
struct QhyCameraHost {
    FakeQHYSDK fake = FakeQHYSDK::with_one_camera("fake-qhy-0");
    LockedQHYSDK sdk{fake};
};
inline QhyCameraHost& qhy_camera_host() {
    static QhyCameraHost host;
    return host;
}
inline std::unique_ptr<CameraDriver> make_qhy_camera(int device_number) {
    return vendor::qhy::create_qhy_camera(device_number, "fake-qhy-0", qhy_camera_host().sdk);
}
#endif

#ifdef FAKE_CONFORMU_PLAYERONE
inline FakePlayerOneSDK& playerone_camera_sdk() {
    static FakePlayerOneSDK sdk;
    return sdk;
}
inline std::unique_ptr<CameraDriver> make_playerone_camera(int device_number) {
    return vendor::playerone::create_playerone_camera(device_number, 0, playerone_camera_sdk());
}
#endif

#if defined(FAKE_CONFORMU_TOUPTEK) || defined(FAKE_CONFORMU_ALTAIR)
// One fake ToupTek-family SDK with one camera, behind the per-call lock (the contract sweep's ToupTekSdkHold). The
// ToupTek and Altair rows each build their own.
struct ToupTekCameraHost {
    FakeToupTekSDK fake;
    LockedToupTekSDK sdk{fake};
    ToupTekCameraHost() {
        fake.cameras.push_back(FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
        fake.deliver_frame = true;  // an exposure ends in a frame, as on a camera (the unit default is none)
    }
};
#endif
#ifdef FAKE_CONFORMU_TOUPTEK
inline ToupTekCameraHost& touptek_camera_host() {
    static ToupTekCameraHost host;
    return host;
}
inline std::unique_ptr<CameraDriver> make_touptek_camera(int device_number) {
    return vendor::touptek::create_touptek_camera(device_number, 0, touptek_camera_host().sdk);
}
#endif
#ifdef FAKE_CONFORMU_ALTAIR
inline ToupTekCameraHost& altair_camera_host() {
    static ToupTekCameraHost host;
    return host;
}
inline std::unique_ptr<CameraDriver> make_altair_camera(int device_number) {
    return vendor::altair::create_altair_camera(device_number, 0, altair_camera_host().sdk);
}
#endif

#ifdef FAKE_CONFORMU_SVBONY
inline FakeSVBSDK& svbony_camera_sdk() {
    static FakeSVBSDK sdk;
    return sdk;
}
inline std::unique_ptr<CameraDriver> make_svbony_camera(int device_number) {
    return vendor::svbony::create_svbony_camera(device_number, 0, svbony_camera_sdk());
}
#endif

#ifdef FAKE_CONFORMU_GPHOTO
// A Nikon D5300 with ISO and shutter-speed choices (the contract sweep's GPhotoSdkHold), its RAW decoder a canned
// frame. The model is the one libgphoto2 reports, so the driver's pixel-size table knows it (3.91 um).
struct GPhotoCameraHost {
    FakeGPhotoSDK fake;
    LockedGPhotoSDK sdk{fake};
    FakeRawDecoder decoder;
    GPhotoCameraHost() {
        FakeGPhotoSDK::FakeCamera cam;
        cam.model = "Nikon DSC D5300";
        cam.port = "usb:001,099";
        cam.choices["iso"] = {"100", "200", "400"};
        cam.choice_value["iso"] = "200";
        cam.choices["shutterspeed2"] = {"1/200", "1", "bulb"};
        cam.choice_value["shutterspeed2"] = "1/200";
        cam.toggle_value["bulb"] = false;
        fake.cameras.push_back(cam);
    }
};
inline GPhotoCameraHost& gphoto_camera_host() {
    static GPhotoCameraHost host;
    return host;
}
inline std::unique_ptr<CameraDriver> make_gphoto_camera(int device_number) {
    return vendor::gphoto::create_gphoto_camera(device_number, 0, gphoto_camera_host().sdk,
                                                gphoto_camera_host().decoder);
}
#endif

}  // namespace alpacacore::test::conformu
