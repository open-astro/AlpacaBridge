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

// Test-only: each switch driver over its roster fake, for the fake ConformU server (fake_conformu_server.cpp, #2078
// slice 6). One factory per kFakeConnectableRoster switch row, each guarded by the FAKE_CONFORMU_<VENDOR> macro the
// server's CMake sets when that vendor is built. Each fake is a function-local static, built when its row is served,
// so it outlives every driver the server builds. It lives here, not in AlpacaHTTP, so the vendor headers stay out of
// AlpacaHTTP (scripts/check_layering.py). The iOptron and ToupTek power boxes are compiled only with libgpiod, so their
// factories also wait for the ALPACACORE_IOPTRON_POWERBOX and ALPACACORE_TOUPTEK_STELLAVITA macros the build sets then.

#include <alpacacore/switch_driver.h>

#include <memory>

#ifdef FAKE_CONFORMU_ONSTEP
#include <alpacacore/vendor/onstep/onstep_switch_driver.h>

#include "conformu_focuser_factories.h"  // onstep_controller_host(): one controller serves focuser, rotator, switch
#include "onstep_controller_model.h"
#endif
#if defined(FAKE_CONFORMU_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>

#include "fake_ioptron_gpio.h"
#endif
#ifdef FAKE_CONFORMU_GEMINI
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>

#include "fake_gemini_pdh.h"
#endif
#if defined(FAKE_CONFORMU_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include "fake_touptek_powerbox_sdk.h"
#endif

namespace alpacacore::test::conformu {

#ifdef FAKE_CONFORMU_ONSTEP
inline std::unique_ptr<SwitchDriver> make_onstep_switch(int device_number) {
    return vendor::onstep::create_onstep_switch(device_number,
                                                onstep_controller::endpoint(onstep_controller_host().server.port()));
}
#endif

#if defined(FAKE_CONFORMU_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
inline FakeIoptronGpio& ioptron_gpio() {
    static FakeIoptronGpio gpio;
    return gpio;
}
inline std::unique_ptr<SwitchDriver> make_ioptron_switch(int device_number) {
    return vendor::ioptron::create_ioptron_switch(device_number, vendor::ioptron::default_imate_powerbox_config(),
                                                  ioptron_gpio());
}
#endif

#ifdef FAKE_CONFORMU_GEMINI
inline FakeGeminiPdh& gemini_pdh() {
    static FakeGeminiPdh fake;
    return fake;
}
inline std::unique_ptr<SwitchDriver> make_gemini_pdh_switch(int device_number) {
    return vendor::gemini::create_gemini_pdh_switch(device_number, gemini_pdh().slave_path());
}
#endif

#if defined(FAKE_CONFORMU_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
inline FakeTouptekPowerboxSDK& touptek_powerbox() {
    static FakeTouptekPowerboxSDK sdk;
    return sdk;
}
inline std::unique_ptr<SwitchDriver> make_touptek_switch(int device_number) {
    return vendor::touptek::create_touptek_switch(device_number, vendor::touptek::default_stellavita_config(),
                                                  touptek_powerbox());
}
#endif

}  // namespace alpacacore::test::conformu
