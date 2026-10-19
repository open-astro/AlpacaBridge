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
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::ioptron {

// One DC output of the iMate PowerBox.
//
// The iMate exposes three physical DC barrel jacks but only two are wired to
// a GPIO: DC1 -> gpiochip1 line 118 (PD22), DC2 -> gpiochip1 line
// 114 (PD18) on the OpenAstro (Armbian mainline) kernel. The third jack is a hardwired pass-through that is
// always energised and has no GPIO line; it is represented with
// has_line == false / writable == false so clients can still see it.
struct IoptronPowerPortConfig {
    std::string name;
    bool has_line;             // false for the always-on pass-through jack
    std::uint32_t gpio_line;   // libgpiod offset on gpio_chip_path; ignored when !has_line
    bool writable;             // false for the always-on pass-through jack
    bool pwm_enabled = false;  // soft-PWM dimming (0-100% duty) instead of on/off
};

// The GPIO side of the PowerBox: claim output lines, drive them, release them.
// The wrapper's default is libgpiod; a test passes its own through
// create_ioptron_switch(..., IoptronGpioBackend&) so a connect can succeed on
// a host with no GPIO chip. Nothing owns a backend: it must outlive every
// wrapper built on it, and the destructor is protected and non-virtual so
// `delete` through the interface does not compile (the QHYSDK shape).
class IoptronGpioBackend {
public:
    /// Claim @p offsets on @p chip_path as outputs driven to @p initial_values
    /// (0 or 1 each). Throws AlpacaException on failure; nothing stays claimed then.
    virtual void request(const std::string& chip_path, const std::vector<unsigned int>& offsets,
                         const std::vector<int>& initial_values) = 0;
    /// Drive one claimed line. Returns 0, or -1 with errno set.
    virtual int set_value(unsigned int offset, int value) = 0;
    /// Release the claimed lines and the chip. Safe when nothing is claimed.
    virtual void release() = 0;

protected:
    IoptronGpioBackend() = default;
    ~IoptronGpioBackend() = default;
    IoptronGpioBackend(const IoptronGpioBackend&) = default;
    IoptronGpioBackend& operator=(const IoptronGpioBackend&) = default;
};

// libgpiod-backed controller for the iMate PowerBox DC outputs.
//
// Each controllable line is driven either as a boolean (0 = off / low, 1 = on /
// high) or, when pwm_enabled, as a soft-PWM channel: a per-port worker thread
// bit-bangs the line at pwm_frequency_hz with a 0-100% duty cycle (same
// approach as the ZWO ASIAIR switch). Matches the stock iMate behaviour where
// the ports are configured as outputs and driven high at boot.
class IoptronPowerboxWrapper {
public:
    /// @p backend null = the libgpiod default; otherwise it must outlive this wrapper.
    IoptronPowerboxWrapper(std::string gpio_chip_path, std::vector<IoptronPowerPortConfig> ports,
                           std::uint32_t pwm_frequency_hz = 50, IoptronGpioBackend* backend = nullptr);
    ~IoptronPowerboxWrapper();

    IoptronPowerboxWrapper(const IoptronPowerboxWrapper&) = delete;
    IoptronPowerboxWrapper& operator=(const IoptronPowerboxWrapper&) = delete;
    IoptronPowerboxWrapper(IoptronPowerboxWrapper&&) = delete;
    IoptronPowerboxWrapper& operator=(IoptronPowerboxWrapper&&) = delete;

    void open();
    void close();
    bool is_open() const;

    std::size_t port_count() const;
    const IoptronPowerPortConfig& port_config(std::size_t index) const;

    // Boolean port: value is 0 (off / GPIO low) or 1 (on / GPIO high).
    // PWM port: value is the duty-cycle percent in [0, 100].
    // The always-on pass-through port always reads 1 and rejects writes.
    int get_value(std::size_t index) const;
    void set_value(std::size_t index, int value);

    std::string gpio_chip_path() const;
    std::uint32_t pwm_frequency_hz() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::ioptron
