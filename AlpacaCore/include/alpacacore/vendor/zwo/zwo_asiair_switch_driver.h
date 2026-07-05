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
    // Marketing model this libgpiod driver is standing in for, used in the
    // device Name/Description/DriverInfo. The Pi 4 ASIAIR Pro and the Pi CM4
    // ASIAIR Plus share identical on-board GPIO wiring and use this same
    // driver, so the only difference clients see is this label. Defaults to the
    // Pro layout; the router overrides it for the CM4 Plus.
    std::string model_name = "ASIAIR Pro";
};

// Returns the default ASIAIR Pro Pi4 layout: 4 ports on /dev/gpiochip0
// (Port 1=GPIO12, Port 2=GPIO13, Port 3=GPIO26, Port 4=GPIO18), all boolean,
// PWM frequency 1000 Hz.
AsiairSwitchConfig default_asiair_pro_config();

std::unique_ptr<SwitchDriver> create_zwo_asiair_switch(int device_number, AsiairSwitchConfig config);

} // namespace alpacacore::vendor::zwo
