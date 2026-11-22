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

// Test-only: each focuser driver over its roster fake, for the fake ConformU server (fake_conformu_server.cpp, #2078
// slice 3). One factory per kFakeConnectableRoster focuser row, each guarded by the FAKE_CONFORMU_<VENDOR> macro the
// server's CMake sets when that vendor is built. Each fake is a function-local static, built when its row is served,
// so it outlives every driver the server builds. It lives here, not in AlpacaHTTP, so the vendor headers stay out of
// AlpacaHTTP (scripts/check_layering.py).

#include <alpacacore/focuser_driver.h>

#include <memory>
#include <string>

#ifdef FAKE_CONFORMU_ZWO
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>

#include "fake_zwo_eaf_sdk.h"
#endif
#ifdef FAKE_CONFORMU_CELESTRON
#include <alpacacore/vendor/celestron/celestron_focuser_driver.h>

#include "fake_celestron_focuser.h"
#endif
#ifdef FAKE_CONFORMU_ONSTEP
#include <alpacacore/vendor/onstep/onstep_focuser_driver.h>

#include "fake_mount_server.h"
#include "onstep_controller_model.h"
#endif
#ifdef FAKE_CONFORMU_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_focuser_driver.h>

#include "fake_ioptron_ieaf.h"
#include "fake_ioptron_ifr.h"
#endif
#ifdef FAKE_CONFORMU_ASTROASIS
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>

#include "fake_astroasis_hid.h"
#include "locked_astroasis_hid_transport.h"
#endif
#ifdef FAKE_CONFORMU_GEMINI
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>

#include "fake_gemini_focuser.h"
#endif
#ifdef FAKE_CONFORMU_QHY
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>

#include "fake_qhy_qfocuser.h"
#endif

namespace alpacacore::test::conformu {

#ifdef FAKE_CONFORMU_ZWO
inline FakeZWOEAFSDK& zwo_eaf() {
    static FakeZWOEAFSDK sdk;
    return sdk;
}
inline std::unique_ptr<FocuserDriver> make_zwo_focuser(int device_number) {
    return vendor::zwo::create_zwo_eaf_focuser(device_number, zwo_eaf().id, zwo_eaf());
}
#endif

#ifdef FAKE_CONFORMU_CELESTRON
inline FakeCelestronFocuser& celestron_focuser() {
    static FakeCelestronFocuser fake;
    // A move travels over time, as the focus motor does, so ConformU's Move ends on its own. A stand-in rate: a
    // full 60000-count range takes 6 s, well inside ConformU's 60 s focuser timeout.
    static const bool travels = (fake.set_travel_rate(10000), true);
    static_cast<void>(travels);
    return fake;
}
inline std::unique_ptr<FocuserDriver> make_celestron_focuser(int device_number) {
    return vendor::celestron::create_celestron_focuser(device_number, celestron_focuser().slave_path());
}
#endif

#ifdef FAKE_CONFORMU_ONSTEP
// An OnStepX controller with focuser 1 on a loopback FakeMountServer (the controller of test_onstep_focuser.cpp).
struct OnStepFocuserHost {
    std::shared_ptr<onstep_controller::OnStepXController> state =
        std::make_shared<onstep_controller::OnStepXController>();
    FakeMountServer server{[s = state](const std::string& chunk) { return s->reply(chunk); }};
};
inline OnStepFocuserHost& onstep_controller_host() {
    static OnStepFocuserHost host;
    return host;
}
inline std::unique_ptr<FocuserDriver> make_onstep_focuser(int device_number) {
    return vendor::onstep::create_onstep_focuser(
        device_number, onstep_controller::endpoint(onstep_controller_host().server.port()), 1);
}
#endif

#ifdef FAKE_CONFORMU_IOPTRON
inline FakeIoptronIeaf& ioptron_ieaf() {
    static FakeIoptronIeaf fake;
    return fake;
}
inline std::unique_ptr<FocuserDriver> make_ioptron_ieaf_focuser(int device_number) {
    return vendor::ioptron::create_ieaf_focuser(device_number, ioptron_ieaf().slave_path());
}
inline FakeIoptronIfr& ioptron_ifr() {
    static FakeIoptronIfr fake;
    return fake;
}
inline std::unique_ptr<FocuserDriver> make_ifr_focuser(int device_number) {
    return vendor::ioptron::create_ifr_focuser(device_number, ioptron_ifr().slave_path());
}
#endif

#ifdef FAKE_CONFORMU_ASTROASIS
inline std::shared_ptr<FakeHidState>& astroasis_state() {
    static auto state = [] {
        auto s = std::make_shared<FakeHidState>();
        // A Move travels over time, as the Oasis focuser does; a stand-in rate: its 50000-step range takes 10 s,
        // well inside ConformU's 60 s focuser timeout.
        s->travel_rate = 5000;
        return s;
    }();
    return state;
}
inline std::unique_ptr<FocuserDriver> make_astroasis_focuser(int device_number) {
    return vendor::astroasis::create_astroasis_focuser(
        device_number, "/dev/hidraw-fake",
        std::make_unique<LockedAstroasisHidTransport>(std::make_unique<FakeHidTransport>(astroasis_state())));
}
#endif

#ifdef FAKE_CONFORMU_GEMINI
inline FakeGeminiFocuser& gemini_focuser() {
    static FakeGeminiFocuser fake;
    return fake;
}
inline std::unique_ptr<FocuserDriver> make_gemini_focuser(int device_number) {
    return vendor::gemini::create_gemini_focuser(device_number, gemini_focuser().slave_path());
}
#endif

#ifdef FAKE_CONFORMU_QHY
inline FakeQhyQFocuser& qhy_qfocuser() {
    static FakeQhyQFocuser fake;
    return fake;
}
inline std::unique_ptr<FocuserDriver> make_qhy_focuser(int device_number) {
    return vendor::qhy::create_qhy_focuser(device_number, qhy_qfocuser().slave_path());
}
#endif

}  // namespace alpacacore::test::conformu
