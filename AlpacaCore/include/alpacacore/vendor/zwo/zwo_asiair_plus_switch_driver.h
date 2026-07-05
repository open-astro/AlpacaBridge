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
#include <alpacacore/vendor/zwo/zwo_asiair_plus_protocol_wrapper.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

struct AsiairPlusSwitchConfig {
    std::string device_path = "/dev/pwm-gpio-misc";
    std::uint32_t pwm_frequency_hz = 50;
    std::vector<AsiairPlusPortConfig> ports;
};

// Returns the default ASIAIR Plus RK3568 layout: 4 boolean ports labelled
// "Port 1".."Port 4", PWM frequency 1000 Hz, opening /dev/pwm-gpio-misc.
// The four wrapper indices (0..3) map internally to the kernel module's
// DC-port ioctl indices (4..7).
AsiairPlusSwitchConfig default_asiair_plus_rk3568_config();

std::unique_ptr<SwitchDriver> create_zwo_asiair_plus_switch(int device_number,
                                                            AsiairPlusSwitchConfig config);

} // namespace alpacacore::vendor::zwo
