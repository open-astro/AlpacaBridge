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

// Test-only: each cover calibrator driver over its roster fake, for the fake ConformU server
// (fake_conformu_server.cpp, #2078 slice 6). One factory per kFakeConnectableRoster cover calibrator row, each guarded
// by the FAKE_CONFORMU_<VENDOR> macro the server's CMake sets when that vendor is built. Each fake is a function-local
// static, built when its row is served, so it outlives every driver the server builds. It lives here, not in
// AlpacaHTTP, so the vendor headers stay out of AlpacaHTTP (scripts/check_layering.py).

#include <alpacacore/covercalibrator_driver.h>

#include <chrono>
#include <memory>

#ifdef FAKE_CONFORMU_GEMINI
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>

#include "fake_gemini_flatpanel.h"
#endif
#ifdef FAKE_CONFORMU_WANDERERASTRO
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>

#include "fake_serial_streamer.h"
#endif

namespace alpacacore::test::conformu {

#ifdef FAKE_CONFORMU_GEMINI
// The Motorized Flat Panel V3 (Pro) firmware, the one fake_gemini_flatpanel.h models. The real panel acks an open or
// close only once the cover has travelled (about 10 s), so the cover takes that long here too; without it the move is
// over before ConformU can see it, and it judges the cover synchronous.
inline constexpr std::chrono::milliseconds kGeminiCoverTravel{10000};
inline FakeGeminiFlatPanel& gemini_flatpanel() {
    static FakeGeminiFlatPanel fake;
    static const bool configured = [] {
        fake.set_reply_delay(">O#", kGeminiCoverTravel);
        fake.set_reply_delay(">C#", kGeminiCoverTravel);
        return true;
    }();
    (void)configured;
    return fake;
}
inline std::unique_ptr<CoverCalibratorDriver> make_gemini_flatpanel_pro(int device_number) {
    return vendor::gemini::create_gemini_flatpanel_pro(device_number, gemini_flatpanel().slave_path());
}
#endif

#ifdef FAKE_CONFORMU_WANDERERASTRO
// The WandererCover V4 Pro status frame of the WandererAstro cover tests (close angle 10.0, open angle 270.0, the
// cover closed, brightness 0), streamed every 300 ms. Cover mode: the angle follows an open or close command at 20
// degrees a second, as the bench cover does (ConformU on the V4 Pro: an open or close took 12.5 s, 250 degrees to
// within the driver's 10 degree tolerance). The brightness field never follows a command; the driver serves
// Brightness from what it commanded.
inline constexpr double kWandererCoverDegPerSecond = 20.0;
inline FakeSerialStreamer& wandererastro_cover() {
    static FakeSerialStreamer streamer("WandererCoverV4ProA20240301A10.0A270.0A10.0A12.5A0A0A0\n",
                                       std::chrono::milliseconds(300), std::chrono::milliseconds(0),
                                       std::chrono::milliseconds(0), kWandererCoverDegPerSecond);
    return streamer;
}
inline std::unique_ptr<CoverCalibratorDriver> make_wandererastro_covercalibrator(int device_number) {
    return vendor::wandererastro::create_wandererastro_covercalibrator(device_number,
                                                                       wandererastro_cover().slave_path());
}
#endif

}  // namespace alpacacore::test::conformu
