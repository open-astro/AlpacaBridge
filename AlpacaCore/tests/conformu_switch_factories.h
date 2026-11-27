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
// The ZWO build always needs libgpiod; its two ASIAIR switches run their real backends over the libgpiod and ioctl()
// stubs that zwo_asiair_stubs.cpp, compiled into the server, defines.

#include <alpacacore/switch_driver.h>

#include <chrono>
#include <memory>

#ifdef FAKE_CONFORMU_ZWO
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>

#include "fake_zwo_asiair_gpiod.h"
#include "fake_zwo_asiair_plus_ioctl.h"
#include "fake_zwo_sdk.h"
#endif
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
#ifdef FAKE_CONFORMU_WANDERERASTRO
#include <alpacacore/vendor/wandererastro/wandererastro_box_switch_driver.h>

#include "fake_serial_streamer.h"
#endif
#ifdef FAKE_CONFORMU_PLAYERONE
#include <alpacacore/util/thermal_camera_choice.h>
#include <alpacacore/vendor/playerone/playerone_switch_driver.h>

#include "fake_playerone_sdk.h"
#endif
#if defined(FAKE_CONFORMU_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include "fake_touptek_powerbox_sdk.h"
#endif
#ifdef FAKE_CONFORMU_TOUPTEK
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>

#include "fake_touptek_sdk.h"
#include "locked_touptek_sdk.h"
#endif

namespace alpacacore::test::conformu {

#ifdef FAKE_CONFORMU_ZWO
// The dew heater of the fake ASI camera (its AntiDewHeater control), through the dew heater's own SDK seam.
inline vendor::zwo::ZWOSdkDewHeaterSdk& zwo_dew_heater_sdk() {
    static FakeZWOSDK sdk;
    static vendor::zwo::ZWOSdkDewHeaterSdk dew_sdk{sdk};
    return dew_sdk;
}
inline std::unique_ptr<SwitchDriver> make_zwo_dew_heater_switch(int device_number) {
    return vendor::zwo::create_zwo_dew_heater_switch_by_index(device_number, 0, zwo_dew_heater_sdk());
}

// The ASIAIR Pro over the libgpiod stub chip, every line high as the stock boot leaves it.
inline std::unique_ptr<SwitchDriver> make_zwo_asiair_switch(int device_number) {
    static const bool booted = [] {
        stub_reset_levels(1);
        return true;
    }();
    (void)booted;
    return vendor::zwo::create_zwo_asiair_switch(device_number, stub_asiair_pro_config());
}

// The ASIAIR Plus (RK3568) over the ioctl stub playing pwm_gpio_compat, the OpenAstro image's module: the harsher of
// the two stub modules, which refuses every command the GPIO path must not send.
inline StubDeviceFile& zwo_asiair_plus_device() {
    static StubDeviceFile device(compat_device());
    return device;
}
inline std::unique_ptr<SwitchDriver> make_zwo_asiair_plus_switch(int device_number) {
    auto config = vendor::zwo::default_asiair_plus_rk3568_config();
    config.device_path = zwo_asiair_plus_device().path;
    return vendor::zwo::create_zwo_asiair_plus_switch(device_number, config, zwo_asiair_plus_device().sysfs_root());
}
#endif

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

#ifdef FAKE_CONFORMU_PLAYERONE
// The fake's default camera with a dew heater and a fan (0-100 percent each), bound by Auto as the one camera that
// has them. Its own SDK instance: the server serves one row, so it never shares a camera with the camera row.
inline FakePlayerOneSDK& playerone_thermal_sdk() {
    static FakePlayerOneSDK sdk;
    [[maybe_unused]] static const bool thermal = [] {
        sdk.set_thermal_controls(true, true);
        return true;
    }();
    return sdk;
}
inline std::unique_ptr<SwitchDriver> make_playerone_switch(int device_number) {
    return vendor::playerone::create_playerone_switch(device_number, util::kAutoThermalCamera, "",
                                                      playerone_thermal_sdk());
}
#endif

#ifdef FAKE_CONFORMU_TOUPTEK
// One cooled camera on the fake ToupTek SDK, behind the per-call lock: dew heater 0..10, fan 0..3 and the tail light,
// bound by index 0. The cooler stays off, so the fan interlock (#1993) never refuses a fan-off write.
struct ToupTekThermalHost {
    FakeToupTekSDK fake;
    LockedToupTekSDK sdk{fake};
    ToupTekThermalHost() { fake.cameras.push_back(FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One")); }
};
inline ToupTekThermalHost& touptek_thermal_host() {
    static ToupTekThermalHost host;
    return host;
}
inline std::unique_ptr<SwitchDriver> make_touptek_thermal_switch(int device_number) {
    return vendor::touptek::create_touptek_thermal_switch(device_number, 0, touptek_thermal_host().sdk);
}
#endif

#ifdef FAKE_CONFORMU_WANDERERASTRO
// The WandererBox Pro V3 status frame of the WandererAstro box tests, streamed every 500 ms. The streamer has no box
// mode, so no output state follows a command.
inline FakeSerialStreamer& wandererastro_box() {
    static FakeSerialStreamer streamer(
        "ZXWBProV3A20250410A-127.00A-127.00A-127.00A45.20A21.30A1.50A0.20A0.30A13.10A1A1A1A1A1A1A0A0A0A1A1A120A\n",
        std::chrono::milliseconds(500));
    return streamer;
}
inline std::unique_ptr<SwitchDriver> make_wandererastro_box_switch(int device_number) {
    return vendor::wandererastro::create_wandererastro_box_switch(device_number, wandererastro_box().slave_path());
}
#endif

}  // namespace alpacacore::test::conformu
