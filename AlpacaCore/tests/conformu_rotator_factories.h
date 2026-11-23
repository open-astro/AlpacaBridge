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

// Test-only: each rotator driver over its roster fake, for the fake ConformU server (fake_conformu_server.cpp,
// #2078 slice 6). One factory per kFakeConnectableRoster rotator row, each guarded by the FAKE_CONFORMU_<VENDOR>
// macro the server's CMake sets when that vendor is built. Each fake is a function-local static, built when its row
// is served, so it outlives every driver the server builds. It lives here, not in AlpacaHTTP, so the vendor headers
// stay out of AlpacaHTTP (scripts/check_layering.py).

#include <alpacacore/rotator_driver.h>

#include <memory>

#ifdef FAKE_CONFORMU_ZWO
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>

#include "fake_zwo_caa_sdk.h"
#endif
#ifdef FAKE_CONFORMU_ONSTEP
#include <alpacacore/vendor/onstep/onstep_rotator_driver.h>

#include "conformu_focuser_factories.h"  // onstep_controller_host(): one controller serves focuser and rotator
#include "onstep_controller_model.h"
#endif
#ifdef FAKE_CONFORMU_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_ifr_rotator_driver.h>

#include "conformu_focuser_factories.h"  // ioptron_ifr(): the iFR focuser and rotator share one unit
#include "fake_ioptron_ifr.h"
#endif

namespace alpacacore::test::conformu {

#ifdef FAKE_CONFORMU_ZWO
// The CAA reports motion until the unit arrives, so ConformU sees the rotator turn; the fake's default settles at
// once. Counted in status reads (about 100 ms apart, the driver's frame TTL), not in time.
inline constexpr int kCaaMovingReads = 5;
inline FakeZWOCAASDK& zwo_caa() {
    static FakeZWOCAASDK sdk;
    static const bool configured = [] {
        sdk.moving_reads = kCaaMovingReads;
        return true;
    }();
    (void)configured;
    return sdk;
}
inline std::unique_ptr<RotatorDriver> make_zwo_rotator(int device_number) {
    return vendor::zwo::create_zwo_caa_rotator(device_number, zwo_caa().id, zwo_caa());
}
#endif

#ifdef FAKE_CONFORMU_ONSTEP
inline std::unique_ptr<RotatorDriver> make_onstep_rotator(int device_number) {
    return vendor::onstep::create_onstep_rotator(device_number,
                                                 onstep_controller::endpoint(onstep_controller_host().server.port()));
}
#endif

#ifdef FAKE_CONFORMU_IOPTRON
inline std::unique_ptr<RotatorDriver> make_ifr_rotator(int device_number) {
    return vendor::ioptron::create_ifr_rotator(device_number, ioptron_ifr().slave_path());
}
#endif

}  // namespace alpacacore::test::conformu
