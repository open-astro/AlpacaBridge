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
};

// libgpiod-backed controller for the iMate PowerBox DC outputs.
//
// Boolean only: each controllable line is driven 0 (off / low) or 1 (on /
// high). Matches the stock iMate behaviour where the ports are configured as
// outputs and driven high at boot.
class IoptronPowerboxWrapper {
public:
    IoptronPowerboxWrapper(std::string gpio_chip_path,
                           std::vector<IoptronPowerPortConfig> ports);
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
    // The always-on pass-through port always reads 1 and rejects writes.
    int get_value(std::size_t index) const;
    void set_value(std::size_t index, int value);

    std::string gpio_chip_path() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace alpacacore::vendor::ioptron
