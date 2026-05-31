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

#include <alpacacore/observingconditions_driver.h>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace alpacacore::vendor::weewx {

struct WeeWxHttpConfig {
    std::string url;
    std::chrono::seconds poll_interval{std::chrono::seconds(900)};
    std::chrono::milliseconds timeout{std::chrono::milliseconds(5000)};
};

struct WeeWxCurrentValues {
    double temperature_c;
    double humidity;
    double dewpoint_c;
    double wind_speed_ms;
    double pressure_hpa;
    double sky_quality;
    double sky_temperature_c;
};

/**
 * @brief Parse the WeeWX current JSON payload into Alpaca ObservingConditions units.
 *
 * This parser is intentionally tolerant of invalid JSON outside the current block.
 * Returns std::nullopt if the current block cannot be located.
 */
std::optional<WeeWxCurrentValues> parse_weewx_current(std::string_view payload);

/**
 * @brief Factory function to create WeeWX ObservingConditions driver.
 *
 * @param device_number Alpaca device number
 * @param config HTTP configuration for WeeWX JSON feed
 * @return Unique pointer to observing conditions driver
 */
std::unique_ptr<ObservingConditionsDriver> create_weewx_observingconditions(
    int device_number,
    const WeeWxHttpConfig& config);

} // namespace alpacacore::vendor::weewx
