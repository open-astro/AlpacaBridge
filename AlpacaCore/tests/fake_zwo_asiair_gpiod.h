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

// The libgpiod side of the ZWO ASIAIR Pro (and Plus Pi CM4) switch, for every test that drives it (issues #772, #300,
// #1862) and for its fake ConformU host (conformu_switch_factories.h, #2697): the calls of util::GpiodLineBackend.
// The driver cases run that real backend (the engine's own cases run on a fake backend, fake_gpio_line_backend.h),
// so zwo_asiair_stubs.cpp defines the libgpiod C symbols the backend calls; the backend linked into the binary calls
// them instead of the shared library. Each stub acts only on its own chip and forwards everything else to the real
// call (RTLD_NEXT), so every other test in the binary sees the real library. The definitions live in that one .cpp,
// beside the ioctl stub of fake_zwo_asiair_plus_ioctl.h, because an extern "C" override defined in two files of one
// binary is a duplicate symbol: alpacacore_tests and fake_conformu_server each compile it once.
//
// The stub models a chip in memory at kStubChip: a line's level outlives its request (as on the Pi kernels,
// persist_gpio_outputs), a request or reconfigure drives only the lines its config sets to output (an as-is line
// keeps its level), every real level change is counted, and the level each line held when its request was released
// is recorded (issue #300). Like the chip, it refuses a second request while one is live (EBUSY). A request can be held
// open before it claims (stub_set_request_hold), so a connect stays in flight.
//
// KNOWN PARITY GAPS (prefer closing a gap):
// - the chip accepts any offset below kStubLines; the real one refuses an offset it does not have.

#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>

#include <chrono>

namespace alpacacore::test {

// ---- libgpiod (ASIAIR Pro, ASIAIR Plus Pi CM4) ----

inline constexpr const char* kStubChip = "/dev/gpiochip-asiair-stub";
inline constexpr unsigned int kStubLines = 64;

// The level @p offset held when its request was last released; -1 when no release recorded one since the last
// request.
int stub_level_at_release(unsigned int offset);
// Sets every line's level as the boot or a previous session left it, and clears the change count.
void stub_reset_levels(int level);
void stub_set_level(unsigned int offset, int level);
int stub_level(unsigned int offset);
// Writes that changed a line's level.
int stub_level_changes();
// Requests granted and released since the binary started; a live request is one more grant than release.
int stub_requests();
int stub_releases();
bool stub_claimed();
// Each later request waits this long before the chip claims its lines (a connect held open, connect_hold_gate.h);
// zero, the default, is no hold.
void stub_set_request_hold(std::chrono::milliseconds hold);

// The ASIAIR Pro board profile on the stub chip: the default port table and lines, with kStubChip for
// /dev/gpiochip0.
inline vendor::zwo::AsiairSwitchConfig stub_asiair_pro_config() {
    auto config = vendor::zwo::default_asiair_pro_config();
    config.gpio_chip_path = kStubChip;
    return config;
}

}  // namespace alpacacore::test
