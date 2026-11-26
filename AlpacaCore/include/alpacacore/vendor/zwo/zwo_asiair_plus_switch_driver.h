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

#include <alpacacore/switch_driver.h>
#include <alpacacore/util/power_port_state_store.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

struct AsiairPlusPortConfig {
    std::string name;
    bool pwm_enabled;
};

struct AsiairPlusSwitchConfig {
    std::string device_path = "/dev/pwm-gpio-misc";
    std::uint32_t pwm_frequency_hz = 50;
    std::vector<AsiairPlusPortConfig> ports;
    // What the ports do when the server starts (decision 0021): `powerOnState`.
    util::PowerOnState power_on_state = util::PowerOnState::Leave;
};

// Returns the default ASIAIR Plus RK3568 layout: 4 boolean ports labelled
// "Port 1".."Port 4", PWM frequency 50 Hz, opening /dev/pwm-gpio-misc.
// The four ports are lines 0..3 of the port table; PwmGpioMiscBackend maps them
// onto the kernel module's DC-port ioctl indices (4..7).
AsiairPlusSwitchConfig default_asiair_plus_rk3568_config();

// The kernel module behind config.device_path picks the ioctl numbering at each connect: the OpenAstro mainline
// image's pwm_gpio_compat gets the corrected one, anything else (ZWO's stock pwm_gpio.ko) the legacy one.
std::unique_ptr<SwitchDriver> create_zwo_asiair_plus_switch(int device_number, const AsiairPlusSwitchConfig& config);

// The same driver, looking the device's kernel driver up under @p sysfs_dev_char_root instead of /sys/dev/char. A test
// seam: a fake tree there selects the pwm_gpio_compat numbering for a stub device.
std::unique_ptr<SwitchDriver> create_zwo_asiair_plus_switch(int device_number, const AsiairPlusSwitchConfig& config,
                                                            const std::string& sysfs_dev_char_root);

} // namespace alpacacore::vendor::zwo
