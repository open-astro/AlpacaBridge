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

#include "pwm_gpio_misc_backend.h"

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cerrno>

#include "pwm_gpio.h"  // first-party, AlpacaCore/src/vendors/zwo/

namespace alpacacore::vendor::zwo {

namespace {

constexpr const char* kLogCategory = "ZWO_ASIAIR_PLUS";

// Kernel ioctl indices reserved by the airplus-gpios device tree node (pwm_gpio.h has the full mapping):
// the DC ports 1..4 are 4..7.
constexpr int kKernelIndexDcPortBase = 4;

void log_ioctl_failure(const char* call, int kernel_index) {
    const int err = errno;
    ALPACA_LOG_ERROR(kLogCategory, std::string(call) + " failed on kernel index " + std::to_string(kernel_index) +
                                       ": " + util::errno_string(err));
}

}  // namespace

PwmGpioAbi detect_pwm_gpio_abi(int fd, const std::string& sysfs_dev_char_root, std::string* driver_name) {
    if (driver_name != nullptr) {
        driver_name->clear();
    }
    // Every early return is Legacy: a doubt about the module keeps the stock-firmware path.
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
        return PwmGpioAbi::Legacy;
    }
    const std::string link = sysfs_dev_char_root + "/" + std::to_string(major(st.st_rdev)) + ":" +
                             std::to_string(minor(st.st_rdev)) + "/device/driver";
    char target[PATH_MAX];
    const ssize_t n = ::readlink(link.c_str(), target, sizeof(target));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(target)) {
        return PwmGpioAbi::Legacy;  // no link, or one too long to have been read whole
    }
    const std::string path(target, static_cast<std::size_t>(n));
    const std::size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (driver_name != nullptr) {
        *driver_name = name;
    }
    return name == kPwmGpioCompatDriver ? PwmGpioAbi::Compat : PwmGpioAbi::Legacy;
}

std::vector<int> PwmGpioMiscBackend::open(const std::string& device_path, const std::vector<unsigned int>& lines) {
    const int fd = ::open(device_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        throw AlpacaException("Failed to open '" + device_path + "': " + util::errno_string(err),
                              AlpacaError::NotConnected);
    }
    fd_ = fd;
    std::string driver;
    abi_ = detect_pwm_gpio_abi(fd, sysfs_dev_char_root_, &driver);
    const std::string driver_text = driver.empty() ? "no driver link" : "driver " + driver;
    if (abi_ == PwmGpioAbi::Compat) {
        ALPACA_LOG_INFO(kLogCategory, "'" + device_path + "' (" + driver_text +
                                          "): corrected ioctl numbering (OpenAstro mainline pwm_gpio_compat)");
    } else {
        ALPACA_LOG_INFO(kLogCategory,
                        "'" + device_path + "' (" + driver_text + "): legacy ioctl numbering (stock ZWO pwm_gpio.ko)");
    }
    kernel_indices_.clear();
    for (const unsigned int line : lines) {
        kernel_indices_.push_back(kKernelIndexDcPortBase + static_cast<int>(line));
    }
    initialized_.assign(lines.size(), false);
    return {};
}

bool PwmGpioMiscBackend::write(std::size_t line_index, bool high) {
    if (fd_ < 0 || line_index >= kernel_indices_.size()) {
        return false;
    }
    return abi_ == PwmGpioAbi::Compat ? write_compat(line_index, high) : write_legacy(line_index, high);
}

// Stock firmware: the legacy decode of pwm_gpio.h, byte for byte the field-validated ioctl stream. The command names
// below are the legacy ones; pwm_gpio.h ("ioctl commands, LEGACY decode") lists what each really is on the stock
// module. Do not move this path to the corrected numbering until that is re-validated on stock firmware.
// TODO: re-validate the corrected numbering on stock pwm_gpio.ko, then retire this path.
bool PwmGpioMiscBackend::write_legacy(std::size_t line_index, bool high) {
    const int kernel_index = kernel_indices_[line_index];
    if (!initialized_[line_index]) {
        // A line needs SET_MODE then ENABLE (which calls gpiod_direction_output_raw) before its first
        // level: without them the module logs "NOT in GPIO OUTPUT mode" and leaves the pad alone. Sent once
        // per line since open(), not before every toggle, which would triple the ioctls of a PWM edge.
        work_mode_t mode{};
        mode.index = kernel_index;
        mode.mode = PWM_GPIO_MODE_GPIO;
        if (::ioctl(fd_, PWM_GPIO_SET_MODE, &mode) != 0) {
            log_ioctl_failure("PWM_GPIO_SET_MODE(GPIO)", kernel_index);
            return false;
        }
        int enable_arg = kernel_index;
        if (::ioctl(fd_, PWM_GPIO_ENABLE, &enable_arg) != 0) {
            log_ioctl_failure("PWM_GPIO_ENABLE", kernel_index);
            return false;
        }
        initialized_[line_index] = true;
    }
    // Polarity is inverted (class comment): raw 0 = ON, raw 1 = OFF. Do not "fix" this: the previous polarity
    // made NINA turn a port off when the user turned it on (2026-05-30).
    gpio_level_t level{};
    level.index = kernel_index;
    level.level = high ? 0 : 1;
    if (::ioctl(fd_, PWM_GPIO_SET_LEVEL, &level) != 0) {
        log_ioctl_failure("PWM_GPIO_SET_LEVEL", kernel_index);
        return false;
    }
    return true;
}

// pwm_gpio_compat: the corrected numbering. SET_MODE(GPIO) once per line since open() (it records the mode and, after
// a PWM mode, stops the module's PWM; it never touches the pad), then SET_LEVEL with normal polarity on every write,
// which drives the pad. No PWM_ENABLE: that would start the module's own soft-PWM.
bool PwmGpioMiscBackend::write_compat(std::size_t line_index, bool high) {
    const int kernel_index = kernel_indices_[line_index];
    if (!initialized_[line_index]) {
        work_mode_t mode{};
        mode.index = kernel_index;
        mode.mode = PWM_GPIO_MODE_GPIO;
        if (::ioctl(fd_, PWM_GPIO_ABI_SET_MODE, &mode) != 0) {
            log_ioctl_failure("PWM_GPIO_ABI_SET_MODE(GPIO)", kernel_index);
            return false;
        }
        initialized_[line_index] = true;
    }
    gpio_level_t level{};
    level.index = kernel_index;
    level.level = high ? 1 : 0;
    if (::ioctl(fd_, PWM_GPIO_ABI_SET_LEVEL, &level) != 0) {
        log_ioctl_failure("PWM_GPIO_ABI_SET_LEVEL", kernel_index);
        return false;
    }
    return true;
}

void PwmGpioMiscBackend::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    abi_ = PwmGpioAbi::Legacy;
}

}  // namespace alpacacore::vendor::zwo
