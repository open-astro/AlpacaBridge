// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#pragma once

#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/touptek/touptek_powerbox_wrapper.h>

#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::touptek {

struct TouptekSwitchConfig {
    // The StellaVita's main GPIO bank. The CM4 (BCM2711) exposes the 40-pin
    // header as /dev/gpiochip0 (pinctrl-bcm2711), where the libgpiod line
    // offset equals the BCM GPIO number. Override via the gpioChip config field
    // if a future board/kernel relabels the bank.
    std::string gpio_chip_path = "/dev/gpiochip0";
    std::vector<StellaVitaPortConfig> ports;
    // Bit-bang frequency for any port configured as PWM (1..100000 Hz).
    // 50 Hz is the panel-friendly default (same value the iMate/ASIAIR Plus use):
    // a flat panel's LED driver fully cycles each ~20 ms period and visibly
    // dims, where ~1 kHz gets smoothed by the driver's input cap into an on/off
    // gate. Resistive loads (dew heaters) dim at any frequency.
    std::uint32_t pwm_frequency_hz = 50;
    // Marketing model used in the device Name/Description/DriverInfo.
    std::string model_name = "StellaVita";
};

// Returns the default StellaVita layout (verified on CM4 hardware):
//   Switch 0 = "Port 1" — gpiochip0 line 18 (BCM GPIO 18).
//   Switch 1 = "Port 2" — gpiochip0 line 10 (BCM GPIO 10).
//   Switch 2 = "Port 3" — gpiochip0 line 17 (BCM GPIO 17).
//   Switch 3 = "Port 4" — gpiochip0 line 4  (BCM GPIO 4).
// All four ports default to boolean on/off and can be switched to soft-PWM.
// BCM GPIO 9 and 11 (Cypress USB hub power) are intentionally excluded.
TouptekSwitchConfig default_stellavita_config();

std::unique_ptr<SwitchDriver> create_touptek_switch(int device_number, TouptekSwitchConfig config);

}  // namespace alpacacore::vendor::touptek
