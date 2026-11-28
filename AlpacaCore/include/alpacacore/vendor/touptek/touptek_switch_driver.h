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
#include <alpacacore/util/gpio_line_backend.h>
#include <alpacacore/util/power_port_state_store.h>
#include <alpacacore/util/task_clock.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::touptek {

// One 12V DC output of the ToupTek StellaVita power box.
//
// The StellaVita is a Raspberry Pi CM4 (BCM2711) controller. Four DC barrel
// jacks are wired to GPIOs on the main 40-pin bank (/dev/gpiochip0,
// pinctrl-bcm2711) where the libgpiod line offset equals the BCM GPIO number:
// Port 1 -> 18, Port 2 -> 10, Port 3 -> 17, Port 4 -> 4. The board's
// config.txt drives all four high at boot (gpio=...=op,dh,pu), so the ports are
// energised on power-up; a connect reads and keeps whatever level they hold.
//
// BCM GPIO 9 and 11 also exist on the header but power the on-board Cypress USB
// hub — they are deliberately NOT exposed as switch channels, because cutting
// them would drop every USB device (cameras, focusers) attached to the board.
struct StellaVitaPortConfig {
    std::string name;
    std::uint32_t gpio_line;   // libgpiod offset on gpio_chip_path (== BCM GPIO on BCM2711)
    bool writable = true;      // false makes a port read-only (always-on)
    bool pwm_enabled = false;  // soft-PWM dimming (0-100% duty) instead of on/off
};

struct TouptekSwitchConfig {
    // The StellaVita's main GPIO bank. The CM4 (BCM2711) exposes the 40-pin
    // header as /dev/gpiochip0 (pinctrl-bcm2711), where the libgpiod line
    // offset equals the BCM GPIO number. Override via the gpioChip config field
    // if a future board/kernel relabels the bank.
    std::string gpio_chip_path = "/dev/gpiochip0";
    std::vector<StellaVitaPortConfig> ports;
    // Bit-bang frequency for any port configured as PWM (1..100000 Hz).
    // 100 Hz is the panel-friendly default, validated on StellaVita hardware as
    // the sweet spot: a flat panel's LED driver still fully cycles each ~10 ms
    // period and visibly dims, while the higher rate (vs the iMate/ASIAIR Plus
    // 50 Hz) is smoother and avoids visible flicker on some panels. ~1 kHz gets
    // smoothed by the driver's input cap into an on/off gate. Resistive loads
    // (dew heaters) dim at any frequency.
    std::uint32_t pwm_frequency_hz = 100;
    // Marketing model used in the device Name/Description/DriverInfo.
    std::string model_name = "StellaVita";
    // What the ports do when the server starts (decision 0021): `powerOnState`.
    util::PowerOnState power_on_state = util::PowerOnState::Leave;
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

/**
 * @brief Test seam (decision 0027): as create_touptek_switch(), over a caller-owned GPIO backend and task clock.
 *
 * @p backend and @p clock must outlive the driver. The config supplies the port table; the backend supplies
 * the line levels.
 */
std::unique_ptr<SwitchDriver> create_touptek_switch(int device_number, const TouptekSwitchConfig& config,
                                                    util::GpioLineBackend& backend,
                                                    util::TaskClock& clock = util::default_task_clock());

}  // namespace alpacacore::vendor::touptek
