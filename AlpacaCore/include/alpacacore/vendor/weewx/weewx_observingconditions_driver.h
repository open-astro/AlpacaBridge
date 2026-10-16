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

#include <alpacacore/observingconditions_driver.h>

#include <chrono>
#include <functional>
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
 * @brief Fetches the WeeWX JSON feed body for `url`, throwing on failure.
 *
 * Production uses the libcurl implementation; tests inject a scripted one.
 * It runs on the connect thread and the poll thread, so it must be thread-safe.
 */
using WeeWxFetcher = std::function<std::string(const std::string& url, std::chrono::milliseconds timeout)>;

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

/**
 * @brief As above, but fetching the feed through `fetcher` instead of libcurl.
 *
 * A test seam: the production overload delegates here with the libcurl fetch.
 */
std::unique_ptr<ObservingConditionsDriver> create_weewx_observingconditions(int device_number,
                                                                            const WeeWxHttpConfig& config,
                                                                            WeeWxFetcher fetcher);

} // namespace alpacacore::vendor::weewx
