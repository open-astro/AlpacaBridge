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
    // Marketing model used in the device Name/Description/DriverInfo.
    std::string model_name = "iMate PowerBox";
};

// Returns the default iMate PowerBox layout:
//   Switch 0 = "DC3 (always on)" — hardwired pass-through, read-only.
//   Switch 1 = "DC1"             — gpiochip1 line 118 (PD22).
//   Switch 2 = "DC2"             — gpiochip1 line 114 (PD18).
// All ports are boolean on/off.
IoptronSwitchConfig default_imate_powerbox_config();

std::unique_ptr<SwitchDriver> create_ioptron_switch(int device_number, IoptronSwitchConfig config);

} // namespace alpacacore::vendor::ioptron
