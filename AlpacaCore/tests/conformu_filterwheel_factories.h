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

// Test-only: each filter wheel driver over its roster fake, for the fake ConformU server (fake_conformu_server.cpp,
// #2078 slice 4). One factory per kFakeConnectableRoster filter wheel row, each guarded by the FAKE_CONFORMU_<VENDOR>
// macro the server's CMake sets when that vendor is built. Each fake is a function-local static, built when its row
// is served, so it outlives every driver the server builds. It lives here, not in AlpacaHTTP, so the vendor headers
// stay out of AlpacaHTTP (scripts/check_layering.py).

#include <alpacacore/filterwheel_driver.h>

#include <chrono>
#include <memory>

#ifdef FAKE_CONFORMU_ZWO
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>

#include "fake_zwo_efw_sdk.h"
#endif
#ifdef FAKE_CONFORMU_QHY
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>

#include "fake_qhy_cfw3.h"
#endif
#ifdef FAKE_CONFORMU_PLAYERONE
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>

#include "fake_playerone_pw_sdk.h"
#endif
#ifdef FAKE_CONFORMU_WANDERERASTRO
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_driver.h>

#include "fake_serial_streamer.h"
#endif

namespace alpacacore::test::conformu {

// The SDK wheels report Position -1 for this many reads after a move, so ConformU sees the wheel turn (the real
// EFW and Phoenix report -1 while moving); the fakes' default settles at once.
inline constexpr int kWheelMovingReads = 3;

#ifdef FAKE_CONFORMU_ZWO
inline FakeZWOEFWSDK& zwo_efw() {
    static FakeZWOEFWSDK sdk;
    static const bool configured = [] {
        sdk.moving_reads = kWheelMovingReads;
        return true;
    }();
    (void)configured;
    return sdk;
}
inline std::unique_ptr<FilterWheelDriver> make_zwo_filterwheel(int device_number) {
    return vendor::zwo::create_zwo_efw_filterwheel(device_number, zwo_efw().id, zwo_efw());
}
#endif

#ifdef FAKE_CONFORMU_QHY
inline FakeQhyCfw3& qhy_cfw3() {
    static FakeQhyCfw3 wheel;
    return wheel;
}
inline std::unique_ptr<FilterWheelDriver> make_qhy_cfw3_filterwheel(int device_number) {
    // A pty never resets the fake, so no boot byte comes: keep the boot wait short, as the contract sweep does. The
    // reply and move budgets stay the driver's own.
    vendor::qhy::Cfw3Settings settings;
    settings.boot_timeout_ms = 150;
    return vendor::qhy::create_qhy_cfw3_filterwheel(device_number, qhy_cfw3().slave_path(), settings);
}
#endif

#ifdef FAKE_CONFORMU_PLAYERONE
inline FakePlayerOnePWSDK& playerone_pw() {
    static FakePlayerOnePWSDK sdk;
    static const bool configured = [] {
        sdk.set_moving_reads(kWheelMovingReads);
        return true;
    }();
    (void)configured;
    return sdk;
}
inline std::unique_ptr<FilterWheelDriver> make_playerone_filterwheel(int device_number) {
    return vendor::playerone::create_playerone_filterwheel(device_number, 0, playerone_pw());
}
#endif

#ifdef FAKE_CONFORMU_WANDERERASTRO
// The SFW status frame of the WandererAstro filter wheel tests (the wheel at Position 2), streamed every 50 ms.
// SFW wheel mode: a move command lands the wheel on its slot 600 ms later, as the SFW turns (Position is -1 until
// the streamed slot matches the target).
inline FakeSerialStreamer& wandererastro_sfw() {
    static FakeSerialStreamer streamer("WSFW368A20260124A3ABCDEFGHIA0A0A0A0A0A0A0A0A1A\n",
                                       std::chrono::milliseconds(50), std::chrono::milliseconds(0),
                                       std::chrono::milliseconds(600));
    return streamer;
}
inline std::unique_ptr<FilterWheelDriver> make_wandererastro_filterwheel(int device_number) {
    return vendor::wandererastro::create_wandererastro_filterwheel(device_number, wandererastro_sfw().slave_path());
}
#endif

}  // namespace alpacacore::test::conformu
