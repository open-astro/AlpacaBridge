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
#include <alpacacore/vendor/ioptron/ioptron_powerbox_wrapper.h>

#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::ioptron {

struct IoptronSwitchConfig {
    // The iMate's main GPIO bank. On the OpenAstro (Armbian mainline) image the
    // H6 main pinctrl (300b000) is /dev/gpiochip1; the dead stock BSP kernel
    // exposed it as gpiochip0. Override via the gpioChip config field if needed.
    std::string gpio_chip_path = "/dev/gpiochip1";
    std::vector<IoptronPowerPortConfig> ports;
    // Bit-bang frequency for any port configured as PWM (1..100000 Hz).
    // 50 Hz is the panel-friendly default (same value ZWO drives the ASIAIR
    // Plus at): a flat panel's LED driver fully cycles each ~20 ms period and
    // visibly dims, where ~1 kHz gets smoothed by the driver's input cap into
    // an on/off gate. Resistive loads (dew heaters) dim at any frequency.
    std::uint32_t pwm_frequency_hz = 50;
    // Marketing model used in the device Name/Description/DriverInfo.
    std::string model_name = "iMate PowerBox";
};

// Returns the default iMate PowerBox layout:
//   Switch 0 = "DC3 (always on)" — hardwired pass-through, read-only.
//   Switch 1 = "DC1"             — gpiochip1 line 118 (PD22).
//   Switch 2 = "DC2"             — gpiochip1 line 114 (PD18).
// All ports default to boolean on/off; DC1/DC2 can be switched to soft-PWM.
IoptronSwitchConfig default_imate_powerbox_config();

std::unique_ptr<SwitchDriver> create_ioptron_switch(int device_number, IoptronSwitchConfig config);

}  // namespace alpacacore::vendor::ioptron
