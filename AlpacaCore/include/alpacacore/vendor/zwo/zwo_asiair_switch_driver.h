// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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
#include <alpacacore/vendor/zwo/zwo_asiair_protocol_wrapper.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

struct AsiairSwitchConfig {
    std::string gpio_chip_path = "/dev/gpiochip0";
    std::uint32_t pwm_frequency_hz = 1000;
    std::vector<AsiairPortConfig> ports;
};

// Returns the default ASIair Pro Pi4 layout: 4 ports on /dev/gpiochip0
// (Port 1=GPIO12, Port 2=GPIO13, Port 3=GPIO26, Port 4=GPIO18), all boolean,
// PWM frequency 1000 Hz.
AsiairSwitchConfig default_asiair_pro_config();

std::unique_ptr<SwitchDriver> create_zwo_asiair_switch(int device_number, AsiairSwitchConfig config);

} // namespace alpacacore::vendor::zwo
