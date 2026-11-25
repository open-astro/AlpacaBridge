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
// libgpiod calls of the ASIAIR Pro (and Plus Pi CM4) util::GpiodLineBackend and the ioctl() of the ASIAIR Plus
// RK3568 PwmGpioMiscBackend. The driver cases run those real backends (the engine's own cases run on a fake
// backend, fake_gpio_line_backend.h), so zwo_asiair_stubs.cpp, compiled once into alpacacore_tests, defines the
// C symbols the backends call; the backends linked into the binary call them instead of the shared libraries. Each stub
// acts only on its own chip or file and forwards everything else to the real call (RTLD_NEXT), so every other test in
// the binary sees the real library. The definitions live in that one .cpp because an extern "C" override defined in two
// test files is a duplicate symbol.
//
// The libgpiod side models a chip in memory at kStubChip: a line's level outlives its request (as on the Pi
// kernels, persist_gpio_outputs), a request or reconfigure drives only the lines its config sets to output (an
// as-is line keeps its level), every real level change is counted, and the level each line held when its request
// was released is recorded (issue #300). Like the chip, it refuses a second request while one is live (EBUSY).
//
// The ioctl side plays one of two kernel modules on the device named by ioctl_stub().path (StubDeviceFile sets one
// up), chosen by ioctl_stub().module (pwm_gpio.h has both numberings):
// - StubModule::Legacy, ZWO's stock pwm_gpio.ko as the legacy decode sees it: answers PWM_GPIO_SET_MODE,
//   PWM_GPIO_ENABLE and PWM_GPIO_SET_LEVEL and records each SET_LEVEL. Raw SET_LEVEL polarity is the legacy path's:
//   0 = ON, 1 = OFF.
// - StubModule::Compat, the OpenAstro image's pwm_gpio_compat (corrected numbering, from its disassembly): answers
//   every 'C' ioctl on the device. PWM_GPIO_ABI_SET_MODE takes index 0..11 and mode 1 or 2, else EINVAL;
//   PWM_GPIO_ABI_SET_LEVEL takes index 0..11, else EINVAL, and records the level (1 = ON, 0 = OFF); every other
//   command, the legacy codes among them, is refused with EINVAL and recorded in `refused`.
// The backend picks its numbering from the device's sysfs driver link (detect_pwm_gpio_abi), which a StubDeviceFile
// models with a fake /sys/dev/char tree passed through create_zwo_asiair_plus_switch's sysfs overload. A plain temp
// file is not a character device, so a compat case opens /dev/null (`char_device`), which any user can open.
//
// KNOWN PARITY GAPS (prefer closing a gap):
// - the chip accepts any offset below kStubLines; the real one refuses an offset it does not have.
// - Legacy: PWM_GPIO_GET_LEVEL and every other ioctl go to the real ioctl() and fail on a plain file (ENOTTY), and the
//   stub accepts the legacy SET_LEVEL code before SET_MODE and ENABLE. The field-validated stock stream always sends
//   those two first; what stock does otherwise is unverified under the corrected numbering (pwm_gpio.h). The legacy
//   cases pin the order instead (set_modes, enables, calls).
// - Compat, harsher than the module: the real one answers GET_LEVEL, LEVEL_GET (the legacy SET_MODE code),
//   GET_CONFIG, SET_CONFIG, PWM_ENABLE and PWM_DISABLE (the legacy ENABLE code), and ENOTTY for an unknown 'C' code;
//   the stub refuses all of them with EINVAL, since the GPIO path must send none. It does not model the module's
//   PWM, its index 2 input line (SET_LEVEL there is EPERM) or SET_MODE(GPIO) stopping a running PWM.
// - The compat device is /dev/null, not a 10:55 misc node; the sysfs tree is keyed by its real major:minor.

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

// Which kernel module the ioctl stub plays (comment at the top of this file).
enum class StubModule : std::uint8_t { Legacy, Compat };

// The command codes the stub knows (pwm_gpio.h; the tests do not include that header).
inline constexpr unsigned long kLegacySetLevel = 0x40084302UL;  // PWM_GPIO_SET_LEVEL (legacy decode)
inline constexpr unsigned long kLegacyEnable = 0x40044306UL;    // PWM_GPIO_ENABLE (legacy decode)
inline constexpr unsigned long kLegacySetMode = 0x40084307UL;   // PWM_GPIO_SET_MODE (legacy decode)
inline constexpr unsigned long kAbiSetMode = 0x40084302UL;      // PWM_GPIO_ABI_SET_MODE
inline constexpr unsigned long kAbiPwmEnable = 0x40044305UL;    // PWM_GPIO_ABI_PWM_ENABLE
inline constexpr unsigned long kAbiSetLevel = 0x40084308UL;     // PWM_GPIO_ABI_SET_LEVEL

// One ioctl the stub answered or refused.
struct IoctlCall {
    unsigned long request = 0;
    std::int32_t index = -1;  // the argument's first field (every command's is the kernel index)
    std::int32_t value = 0;   // its second field for a {index, level} or {index, mode} argument, else 0
    bool refused = false;     // answered -1 (EINVAL)
    bool operator==(const IoctlCall&) const = default;
};

struct IoctlStub {
    std::mutex m;
    std::condition_variable cv;
    std::string path;                                 // stub device file; empty = inactive
    StubModule module = StubModule::Legacy;           // the module the stub plays on `path`
    std::vector<std::int32_t> levels;                 // raw level of each accepted SET_LEVEL, in order
    std::map<std::int32_t, std::int32_t> last_level;  // kernel index -> raw level of its last SET_LEVEL
    int off_writes = 0;                               // logical OFF SET_LEVELs: raw 1 on Legacy, raw 0 on Compat
    int requests = 0;                                 // every stubbed ioctl, refused ones included
    std::vector<std::int32_t> set_modes;              // Legacy: kernel index of each SET_MODE, in order
    std::vector<std::int32_t> enables;                // Legacy: kernel index of each ENABLE, in order
    std::vector<IoctlCall> calls;                     // every stubbed ioctl, in order
    std::vector<unsigned long> refused;               // Compat: the request code of each refused ioctl
};

IoctlStub& ioctl_stub();

// What a StubDeviceFile stands for.
struct StubDeviceOptions {
    StubModule module = StubModule::Legacy;
    // Open /dev/null, a character device, instead of a temp file: detect_pwm_gpio_abi reads sysfs only for one.
    bool char_device = false;
    // The driver the fake sysfs tree's <major>:<minor>/device/driver link names; empty = no link. A plain temp file
    // gets its entry under 0:0, its st_rdev.
    std::string sysfs_driver;
};

// The OpenAstro mainline image: pwm_gpio_compat behind a character device.
StubDeviceOptions compat_device();

// Points the ioctl stub at a fresh device for one case (a temp file, or /dev/null), with a fake /sys/dev/char tree;
// restores the stub (Legacy, inactive) and removes the files it made on exit.
class StubDeviceFile {
public:
    StubDeviceFile();
    explicit StubDeviceFile(const StubDeviceOptions& options);
    ~StubDeviceFile();
    StubDeviceFile(const StubDeviceFile&) = delete;
    StubDeviceFile& operator=(const StubDeviceFile&) = delete;

    // How many of this process's file descriptors are open on the stub device, beyond those open when it was made
    // (stdin may be /dev/null).
    int open_fds() const;

    // The fake /sys/dev/char root, for create_zwo_asiair_plus_switch(number, config, sysfs_root()).
    const std::string& sysfs_root() const { return sysfs_root_; }

    char path[64] = "/tmp/asiair-plus-stub-XXXXXX";

private:
    int count_fds() const;

    bool char_device_ = false;
    int baseline_fds_ = 0;
    std::string sysfs_root_;
};

}  // namespace alpacacore::test
