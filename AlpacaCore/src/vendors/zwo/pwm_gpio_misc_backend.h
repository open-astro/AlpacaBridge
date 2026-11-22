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
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

// The ASIAIR Plus (Rockchip RK3568) kernel interface for util::SoftPwmPorts: ZWO's closed-source
// pwm_gpio.ko owns the device tree's "airplus-gpios" node and registers a misc device at
// /dev/pwm-gpio-misc (ioctl numbers and struct layouts in pwm_gpio.h). The four 12 V DC ports are
// kernel indices 4..7; the engine's port table lists them as lines 0..3 and this backend adds the 4.
//
// open() is read-only: it opens the device and writes no kernel-side state. Earlier revisions drove the
// master-enable line (kernel index 3) at open, and both polarities dropped every DC port to 0 V on every
// ASCOM connect; SET_MODE or SET_LEVEL on a port at connect did the same. The module's GET_LEVEL reads back
// its own "last written" cache, which starts at 0 whatever the pads were driven to at boot, so open() reports
// no levels (the engine keeps its "on" cache). The first write to a line sends SET_MODE(GPIO) and ENABLE, which
// flips the pad from input to output (SET_LEVEL is refused until then), then SET_LEVEL.
//
// SET_LEVEL polarity is inverted from typical gpiod semantics: raw 0 releases the line and the external
// pull-up powers the load (ON); raw 1 drives it low (OFF). This class hides that: write(i, true) is ON. The
// module's own PWM mode is broken (SET_MODE(PWM) drops the pad), so every port, PWM-configured or not, is
// driven as GPIO.
class PwmGpioMiscBackend final : public util::GpioLineBackend {
public:
    PwmGpioMiscBackend() = default;
    ~PwmGpioMiscBackend() { close(); }

    PwmGpioMiscBackend(const PwmGpioMiscBackend&) = delete;
    PwmGpioMiscBackend& operator=(const PwmGpioMiscBackend&) = delete;
    PwmGpioMiscBackend(PwmGpioMiscBackend&&) = delete;
    PwmGpioMiscBackend& operator=(PwmGpioMiscBackend&&) = delete;

    std::vector<int> open(const std::string& device_path, const std::vector<unsigned int>& lines) override;
    bool write(std::size_t line_index, bool high) override;
    void close() override;

private:
    int fd_ = -1;
    std::vector<int> kernel_indices_;  // the kernel ioctl index of each opened line
    std::vector<bool> initialized_;    // SET_MODE and ENABLE already sent for the line since open()
};

}  // namespace alpacacore::vendor::zwo
