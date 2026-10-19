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

#include <cstddef>
#include <cstdint>
#include <string>

namespace alpacacore::vendor::touptek {

// One 12V DC output of the ToupTek StellaVita power box.
//
// The StellaVita is a Raspberry Pi CM4 (BCM2711) controller. Four DC barrel
// jacks are wired to GPIOs on the main 40-pin bank (/dev/gpiochip0,
// pinctrl-bcm2711) where the libgpiod line offset equals the BCM GPIO number:
// Port 1 -> 18, Port 2 -> 10, Port 3 -> 17, Port 4 -> 4. The board's
// config.txt drives all four high at boot (gpio=...=op,dh,pu), so the ports are
// energised on power-up; the driver preserves that on connect.
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

/**
 * @brief Abstract StellaVita power-box surface used by the ToupTek switch driver.
 *
 * Production passes a TouptekPowerboxWrapper (libgpiod lines and soft-PWM
 * workers); tests inject a scripted fake through the factory overload in
 * touptek_switch_driver.h, so the driver runs without a GPIO chip. The
 * destructor is protected and non-virtual: the driver holds a reference and
 * nothing deletes through the interface.
 *
 * Contract: is_open() is lock-free and never waits behind open() or close()
 * (issue #382), because the driver answers get_connected() with it. A port
 * starts "on" (100 for a PWM port, 1 for a boolean one) and open() keeps that
 * cached value, so a connect never cuts power to attached gear. port_config()
 * returns by value so nothing points into a container another thread owns.
 */
class TouptekPowerboxSDK {
public:
    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual std::size_t port_count() const = 0;
    virtual StellaVitaPortConfig port_config(std::size_t index) const = 0;

    // Boolean port: 0 (off / GPIO low) or 1 (on / GPIO high).
    // PWM port: duty-cycle percent in [0, 100].
    virtual int get_value(std::size_t index) const = 0;
    virtual void set_value(std::size_t index, int value) = 0;

    virtual std::string gpio_chip_path() const = 0;
    virtual std::uint32_t pwm_frequency_hz() const = 0;

protected:
    ~TouptekPowerboxSDK() = default;
};

}  // namespace alpacacore::vendor::touptek
