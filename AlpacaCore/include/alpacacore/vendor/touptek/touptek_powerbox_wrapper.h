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

#include <cstddef>
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
// energised on power-up; the driver preserves that on connect.
//
// BCM GPIO 9 and 11 also exist on the header but power the on-board Cypress USB
// hub — they are deliberately NOT exposed as switch channels, because cutting
// them would drop every USB device (cameras, focusers) attached to the board.
struct StellaVitaPortConfig {
    std::string name;
    std::uint32_t gpio_line;    // libgpiod offset on gpio_chip_path (== BCM GPIO on BCM2711)
    bool writable = true;       // false makes a port read-only (always-on)
    bool pwm_enabled = false;   // soft-PWM dimming (0-100% duty) instead of on/off
};

// libgpiod-backed controller for the StellaVita DC outputs.
//
// Each line is driven either as a boolean (0 = off / low, 1 = on / high) or,
// when pwm_enabled, as a soft-PWM channel: a per-port worker thread bit-bangs
// the line at pwm_frequency_hz with a 0-100% duty cycle (same approach as the
// iOptron iMate PowerBox and ZWO ASIAIR switch). Ports default to "on" so
// connecting preserves the StellaVita's stock boot-high behaviour and never
// cuts power to attached gear.
class TouptekPowerboxWrapper {
public:
    TouptekPowerboxWrapper(std::string gpio_chip_path, std::vector<StellaVitaPortConfig> ports,
                           std::uint32_t pwm_frequency_hz = 100);
    ~TouptekPowerboxWrapper();

    TouptekPowerboxWrapper(const TouptekPowerboxWrapper&) = delete;
    TouptekPowerboxWrapper& operator=(const TouptekPowerboxWrapper&) = delete;
    TouptekPowerboxWrapper(TouptekPowerboxWrapper&&) = delete;
    TouptekPowerboxWrapper& operator=(TouptekPowerboxWrapper&&) = delete;

    void open();
    void close();
    bool is_open() const;

    std::size_t port_count() const;
    const StellaVitaPortConfig& port_config(std::size_t index) const;

    // Boolean port: value is 0 (off / GPIO low) or 1 (on / GPIO high).
    // PWM port: value is the duty-cycle percent in [0, 100].
    int get_value(std::size_t index) const;
    void set_value(std::size_t index, int value);

    std::string gpio_chip_path() const;
    std::uint32_t pwm_frequency_hz() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::touptek
