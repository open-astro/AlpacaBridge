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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

struct AsiairPortConfig {
    std::string name;
    std::uint32_t gpio_line;
    bool pwm_enabled;
};

class AsiairProtocolWrapper {
public:
    AsiairProtocolWrapper(std::string gpio_chip_path,
                          std::vector<AsiairPortConfig> ports,
                          std::uint32_t pwm_frequency_hz);
    ~AsiairProtocolWrapper();

    AsiairProtocolWrapper(const AsiairProtocolWrapper&) = delete;
    AsiairProtocolWrapper& operator=(const AsiairProtocolWrapper&) = delete;
    AsiairProtocolWrapper(AsiairProtocolWrapper&&) = delete;
    AsiairProtocolWrapper& operator=(AsiairProtocolWrapper&&) = delete;

    void open();
    void close();
    bool is_open() const;

    std::size_t port_count() const;
    const AsiairPortConfig& port_config(std::size_t index) const;

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

} // namespace alpacacore::vendor::zwo
