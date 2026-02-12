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
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include "catch2_compat.h"

#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>
#include <cmath>

TEST_CASE("WeeWX current parsing", "[weewx]") {
    const std::string payload = R"json(
{
    "lcd_datasheet": {
        "current": {
            "outTemp": {"value": 50.0, "units": "\u00b0F"},
            "outHumidity": {"value": 40.0, "units": "%"},
            "dewpoint": {"value": 32.0, "units": "\u00b0F"},
            "wind_speed": {"value": 10.0, "units": "mph"},
            "barometer": {"value": 30.0, "units": "inHg"},
            "sqm": {"value": 21.3},
            "sqmTemp": {"value": 41.0}
        },
        "daily_captures": {
            "rows": [
                [1, 2, , 4]
            ]
        }
    }
}
)json";

    auto values = alpacacore::vendor::weewx::parse_weewx_current(payload);
    REQUIRE(values.has_value());

    const auto& v = values.value();
    REQUIRE(std::abs(v.temperature_c - 10.0) < 1e-6);
    REQUIRE(std::abs(v.humidity - 40.0) < 1e-6);
    REQUIRE(std::abs(v.dewpoint_c - 0.0) < 1e-6);
    REQUIRE(std::abs(v.wind_speed_ms - 4.4704) < 1e-4);
    REQUIRE(std::abs(v.pressure_hpa - 1015.9166) < 1e-3);
    REQUIRE(std::abs(v.sky_quality - 21.3) < 1e-6);
    REQUIRE(std::abs(v.sky_temperature_c - 5.0) < 1e-6);
}
