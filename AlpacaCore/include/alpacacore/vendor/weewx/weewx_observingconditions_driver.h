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
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/task_clock.h>

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
 * @param clock The poll's wait, the link-fault stamp and the loss bound (decision 0005; a test seam, the
 *              clock must outlive the driver)
 * @return Unique pointer to observing conditions driver
 */
std::unique_ptr<ObservingConditionsDriver> create_weewx_observingconditions(
    int device_number, const WeeWxHttpConfig& config, util::TaskClock& clock = util::default_task_clock());

/**
 * @brief As above, but fetching the feed through `http` instead of the libcurl client.
 *
 * A test seam (decision 0015): the production overload delegates here with util::make_curl_http_client(). The
 * driver calls `http` from the connect thread and the poll thread, so it must be thread-safe.
 */
std::unique_ptr<ObservingConditionsDriver> create_weewx_observingconditions(int device_number,
                                                                            const WeeWxHttpConfig& config,
                                                                            std::shared_ptr<util::HttpClient> http);

/**
 * @brief As above, with the poll, the link-fault stamp and the loss bound on `clock` (a test seam; the clock must
 *        outlive the driver).
 */
std::unique_ptr<ObservingConditionsDriver> create_weewx_observingconditions(int device_number,
                                                                            const WeeWxHttpConfig& config,
                                                                            std::shared_ptr<util::HttpClient> http,
                                                                            util::TaskClock& clock);

} // namespace alpacacore::vendor::weewx
