// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// The ObservingConditions method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/observingconditions_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "../device_call.h"

namespace alpacahttp {

Response Router::dispatch_observingconditions_method(
    std::shared_ptr<alpacacore::ObservingConditionsDriver> observingconditions, const std::string& method_name,
    const DeviceCall& call) {
    const Request& request = call.request();

    auto parse_property_name = [&]() -> std::string {
        if (request.has_query_param("PropertyName")) {
            return request.get_query_param("PropertyName");
        }
        if (request.has_query_param("SensorName")) {
            return request.get_query_param("SensorName");
        }
        if (const auto* val = call.json_field("PropertyName")) {
            if (!val->is_string()) {
                throw_bad_request("Invalid JSON value for parameter: PropertyName");
            }
            return val->get<std::string>();
        }
        if (const auto* val = call.json_field("SensorName")) {
            if (!val->is_string()) {
                throw_bad_request("Invalid JSON value for parameter: SensorName");
            }
            return val->get<std::string>();
        }
        if (auto value = call.form_field("PropertyName")) {
            return *value;
        }
        if (auto value = call.form_field("SensorName")) {
            return *value;
        }
        throw_bad_request("Missing parameter: SensorName");
    };

    return call.run("observing conditions method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "averageperiod") {
                return call.ok(observingconditions->get_average_period());
            } else if (method_name == "cloudcover") {
                return call.ok(observingconditions->get_cloud_cover());
            } else if (method_name == "dewpoint") {
                return call.ok(observingconditions->get_dew_point());
            } else if (method_name == "humidity") {
                return call.ok(observingconditions->get_humidity());
            } else if (method_name == "pressure") {
                return call.ok(observingconditions->get_pressure());
            } else if (method_name == "rainrate") {
                return call.ok(observingconditions->get_rain_rate());
            } else if (method_name == "skybrightness") {
                return call.ok(observingconditions->get_sky_brightness());
            } else if (method_name == "skyquality") {
                return call.ok(observingconditions->get_sky_quality());
            } else if (method_name == "skytemperature") {
                return call.ok(observingconditions->get_sky_temperature());
            } else if (method_name == "starfwhm") {
                return call.ok(observingconditions->get_star_fwhm());
            } else if (method_name == "temperature") {
                return call.ok(observingconditions->get_temperature());
            } else if (method_name == "winddirection") {
                return call.ok(observingconditions->get_wind_direction());
            } else if (method_name == "windgust") {
                return call.ok(observingconditions->get_wind_gust());
            } else if (method_name == "windspeed") {
                return call.ok(observingconditions->get_wind_speed());
            } else if (method_name == "timesincelastupdate") {
                std::string property_name = parse_property_name();
                return call.ok(observingconditions->get_time_since_last_update(property_name));
            } else if (method_name == "sensordescription") {
                std::string property_name = parse_property_name();
                return call.ok(observingconditions->get_sensor_description(property_name));
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "averageperiod") {
                double value = call.get_double("AveragePeriod");
                observingconditions->set_average_period(value);
                return call.ok_void();
            } else if (method_name == "refresh") {
                observingconditions->refresh();
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for ObservingConditions");
    });
}

}  // namespace alpacahttp
