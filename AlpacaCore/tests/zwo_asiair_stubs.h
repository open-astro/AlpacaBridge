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

// The hardware side of the two ZWO ASIAIR switch drivers, for every test that drives them (issue #1862): the
// libgpiod calls of the ASIAIR Pro (and Plus Pi CM4) wrapper and the ioctl() of the ASIAIR Plus RK3568 wrapper.
// Neither driver has a seam: each wrapper calls the C library directly. So zwo_asiair_stubs.cpp, compiled once
// into alpacacore_tests, defines those C symbols; the wrappers linked into the binary call them instead of the
// shared libraries. Each stub acts only on its own chip or file and forwards everything else to the real call
// (RTLD_NEXT), so every other test in the binary sees the real library. The definitions live in that one .cpp
// because an extern "C" override defined in two test files is a duplicate symbol.
//
// The libgpiod side models a chip in memory at kStubChip: a line's level outlives its request (as on the Pi
// kernels, persist_gpio_outputs), a request or reconfigure drives only the lines its config sets to output (an
// as-is line keeps its level), every real level change is counted, and the level each line held when its request
// was released is recorded (issue #300). Like the chip, it refuses a second request while one is live (EBUSY).
//
// The ioctl side answers PWM_GPIO_SET_MODE, PWM_GPIO_ENABLE and PWM_GPIO_SET_LEVEL on a file named by
// ioctl_stub().path (StubDeviceFile sets one up) and records each SET_LEVEL. Raw SET_LEVEL polarity is the
// module's: 0 = ON, 1 = OFF.
//
// KNOWN PARITY GAPS (the real side is harsher here; prefer closing a gap):
// - the chip accepts any offset below kStubLines; the real one refuses an offset it does not have.
// - PWM_GPIO_GET_LEVEL and every other ioctl go to the real ioctl() and fail on a plain file.

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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

// ---- ioctl (ASIAIR Plus RK3568) ----

struct IoctlStub {
    std::mutex m;
    std::condition_variable cv;
    std::string path;                                 // stub device file; empty = inactive
    std::vector<std::int32_t> levels;                 // raw level of each SET_LEVEL, in order
    std::map<std::int32_t, std::int32_t> last_level;  // kernel index -> raw level of its last SET_LEVEL
    int off_writes = 0;                               // raw level 1 == logical OFF
    int requests = 0;                                 // every stubbed SET_MODE, ENABLE and SET_LEVEL
};

IoctlStub& ioctl_stub();

// Points the ioctl stub at a fresh temp file for one case; restores and removes it on exit.
class StubDeviceFile {
public:
    StubDeviceFile();
    ~StubDeviceFile();
    StubDeviceFile(const StubDeviceFile&) = delete;
    StubDeviceFile& operator=(const StubDeviceFile&) = delete;

    // How many of this process's file descriptors are open on the stub file.
    int open_fds() const;

    char path[64] = "/tmp/asiair-plus-stub-XXXXXX";
};

}  // namespace alpacacore::test
