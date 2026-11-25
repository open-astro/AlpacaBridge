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

#include <alpacacore/util/gpio_line_backend.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::zwo {

// Which ioctl numbering the kernel module behind /dev/pwm-gpio-misc speaks (pwm_gpio.h).
enum class PwmGpioAbi : std::uint8_t {
    // ZWO's stock pwm_gpio.ko (kernel 4.19.219), driven with the legacy decode: the field-validated stock path.
    Legacy,
    // The OpenAstro mainline image's clean-room pwm_gpio_compat module, driven with the corrected numbering.
    Compat,
};

// The sysfs directory that maps a character device number to its device ("<major>:<minor>").
inline constexpr const char* kSysfsDevChar = "/sys/dev/char";

// The basename of /sys/dev/char/<major>:<minor>/device/driver for the OpenAstro mainline image's module.
inline constexpr const char* kPwmGpioCompatDriver = "pwm_gpio_compat";

// Picks the ABI for the open @p fd: Compat only when @p fd is a character device whose sysfs driver link under
// @p sysfs_dev_char_root ends in exactly kPwmGpioCompatDriver. Anything else (not a character device, no sysfs entry,
// an unreadable or truncated link, any other driver name) is Legacy, the stock behaviour. Reads only; writes
// nothing to the device. @p driver_name, when given, receives the link's basename (empty when none was read).
PwmGpioAbi detect_pwm_gpio_abi(int fd, const std::string& sysfs_dev_char_root, std::string* driver_name = nullptr);

// The ASIAIR Plus (Rockchip RK3568) kernel interface for util::SoftPwmPorts: a kernel module owns the device tree's
// "airplus-gpios" node and registers a misc device at /dev/pwm-gpio-misc (ioctl numbers and struct layouts in
// pwm_gpio.h). On stock firmware that module is ZWO's closed-source pwm_gpio.ko; on the OpenAstro mainline image it
// is the clean-room pwm_gpio_compat (rk-flashtool). The four 12 V DC ports are kernel indices 4..7; the engine's
// port table lists them as lines 0..3 and this backend adds the 4.
//
// open() picks the ABI once from the opened fd (detect_pwm_gpio_abi) and logs it at INFO:
//
// - Legacy (stock pwm_gpio.ko). The ioctl stream below is the field-validated one and must not change. The command
//   numbers are the legacy decode in pwm_gpio.h, which is shifted from the module's real numbering; the header
//   explains what each one really does on stock, and so what the observations below are artifacts of.
//
//   open() is read-only: it opens the device and writes no kernel-side state. Earlier revisions drove the
//   master-enable line (kernel index 3) at open, and both polarities dropped every DC port to 0 V on every
//   ASCOM connect; SET_MODE or SET_LEVEL on a port at connect did the same. The module's GET_LEVEL reads back
//   its own "last written" cache, which starts at 0 whatever the pads were driven to at boot, so open() reports
//   no levels (the engine keeps its "on" cache). The first write to a line sends SET_MODE(GPIO) and ENABLE, which
//   flips the pad from input to output (SET_LEVEL is refused until then), then SET_LEVEL.
//
//   SET_LEVEL polarity is inverted from typical gpiod semantics: raw 0 releases the line and the external
//   pull-up powers the load (ON); raw 1 drives it low (OFF). This class hides that: write(i, true) is ON. The
//   module's own PWM mode is broken (SET_MODE(PWM) drops the pad), so every port, PWM-configured or not, is
//   driven as GPIO.
//
// - Compat (pwm_gpio_compat). The first write to a line since open() sends PWM_GPIO_ABI_SET_MODE(GPIO), which only
//   records the mode; every write sends PWM_GPIO_ABI_SET_LEVEL, level 1 = ON, 0 = OFF. No PWM_ENABLE is ever sent
//   (it starts the module's own soft-PWM). open() is read-only here too.
class PwmGpioMiscBackend final : public util::GpioLineBackend {
public:
    // @p sysfs_dev_char_root is where open() looks the device's driver up (kSysfsDevChar; a test points it at a
    // fake tree).
    explicit PwmGpioMiscBackend(std::string sysfs_dev_char_root = kSysfsDevChar)
        : sysfs_dev_char_root_(std::move(sysfs_dev_char_root)) {}
    ~PwmGpioMiscBackend() { close(); }

    PwmGpioMiscBackend(const PwmGpioMiscBackend&) = delete;
    PwmGpioMiscBackend& operator=(const PwmGpioMiscBackend&) = delete;
    PwmGpioMiscBackend(PwmGpioMiscBackend&&) = delete;
    PwmGpioMiscBackend& operator=(PwmGpioMiscBackend&&) = delete;

    std::vector<int> open(const std::string& device_path, const std::vector<unsigned int>& lines) override;
    bool write(std::size_t line_index, bool high) override;
    void close() override;

private:
    bool write_legacy(std::size_t line_index, bool high);
    bool write_compat(std::size_t line_index, bool high);

    std::string sysfs_dev_char_root_;
    PwmGpioAbi abi_ = PwmGpioAbi::Legacy;  // chosen at open()
    int fd_ = -1;
    std::vector<int> kernel_indices_;  // the kernel ioctl index of each opened line
    std::vector<bool> initialized_;    // the once-per-line ioctls already sent for the line since open()
};

}  // namespace alpacacore::vendor::zwo
