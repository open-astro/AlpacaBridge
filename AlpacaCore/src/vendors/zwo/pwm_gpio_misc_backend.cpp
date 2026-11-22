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
#include <sys/ioctl.h>
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

std::vector<int> PwmGpioMiscBackend::open(const std::string& device_path, const std::vector<unsigned int>& lines) {
    const int fd = ::open(device_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        throw AlpacaException("Failed to open '" + device_path + "': " + util::errno_string(err),
                              AlpacaError::NotConnected);
    }
    fd_ = fd;
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
    const int kernel_index = kernel_indices_[line_index];
    if (!initialized_[line_index]) {
        // Every transition needs SET_MODE then ENABLE (which calls gpiod_direction_output_raw) before a
        // level: without them the module logs "NOT in GPIO OUTPUT mode" and leaves the pad alone. Once per
        // line since open(): the sequence is not repeated per toggle, which would triple the ioctls of a
        // PWM edge.
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

void PwmGpioMiscBackend::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace alpacacore::vendor::zwo
