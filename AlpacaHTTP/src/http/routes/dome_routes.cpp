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

// The Dome method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/dome_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>

#include <memory>
#include <string>

#include "../device_call.h"

namespace alpacahttp {

Response Router::dispatch_dome_method(std::shared_ptr<alpacacore::DomeDriver> dome, const std::string& method_name,
                                      const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("dome method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "altitude") {
                return call.ok(dome->get_altitude());
            } else if (method_name == "athome") {
                return call.ok(dome->get_at_home());
            } else if (method_name == "atpark") {
                return call.ok(dome->get_at_park());
            } else if (method_name == "azimuth") {
                return call.ok(dome->get_azimuth());
            } else if (method_name == "canfindhome") {
                return call.ok(dome->get_can_find_home());
            } else if (method_name == "canpark") {
                return call.ok(dome->get_can_park());
            } else if (method_name == "cansetaltitude") {
                return call.ok(dome->get_can_set_altitude());
            } else if (method_name == "cansetazimuth") {
                return call.ok(dome->get_can_set_azimuth());
            } else if (method_name == "cansetpark") {
                return call.ok(dome->get_can_set_park());
            } else if (method_name == "cansetshutter") {
                return call.ok(dome->get_can_set_shutter());
            } else if (method_name == "cansyncazimuth") {
                return call.ok(dome->get_can_sync_azimuth());
            } else if (method_name == "canslave") {
                return call.ok(dome->get_can_slave());
            } else if (method_name == "slaved") {
                return call.ok(dome->get_slaved());
            } else if (method_name == "slewing") {
                return call.ok(dome->get_slewing());
            } else if (method_name == "shutterstatus") {
                return call.ok(dome->get_shutter_status());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "slaved") {
                bool value = call.get_bool("Slaved");
                dome->set_slaved(value);
                return call.ok_void();
            } else if (method_name == "abortslew") {
                dome->abort_slew();
                return call.ok_void();
            } else if (method_name == "closeshutter") {
                dome->close_shutter();
                return call.ok_void();
            } else if (method_name == "findhome") {
                dome->find_home();
                return call.ok_void();
            } else if (method_name == "openshutter") {
                dome->open_shutter();
                return call.ok_void();
            } else if (method_name == "park") {
                dome->park();
                return call.ok_void();
            } else if (method_name == "setpark") {
                dome->set_park();
                return call.ok_void();
            } else if (method_name == "slewtoazimuth") {
                double azimuth = call.get_double("Azimuth");
                dome->slew_to_azimuth(azimuth);
                return call.ok_void();
            } else if (method_name == "slewtoaltitude") {
                double altitude = call.get_double("Altitude");
                dome->slew_to_altitude(altitude);
                return call.ok_void();
            } else if (method_name == "synctoazimuth") {
                double azimuth = call.get_double("Azimuth");
                dome->sync_to_azimuth(azimuth);
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Dome");
    });
}

}  // namespace alpacahttp
