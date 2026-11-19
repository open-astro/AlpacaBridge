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

// The Focuser method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/focuser_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>

#include <memory>
#include <string>

#include "../device_call.h"

namespace alpacahttp {

Response Router::dispatch_focuser_method(std::shared_ptr<alpacacore::FocuserDriver> focuser,
                                         const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("focuser method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "absolute") {
                return call.ok(focuser->get_absolute());
            } else if (method_name == "ismoving") {
                return call.ok(focuser->get_is_moving());
            } else if (method_name == "maxincrement") {
                return call.ok(focuser->get_max_increment());
            } else if (method_name == "maxstep") {
                return call.ok(focuser->get_max_step());
            } else if (method_name == "position") {
                return call.ok(focuser->get_position());
            } else if (method_name == "stepsize") {
                return call.ok(focuser->get_step_size());
            } else if (method_name == "tempcompavailable") {
                return call.ok(focuser->get_temp_comp_available());
            } else if (method_name == "tempcomp") {
                return call.ok(focuser->get_temp_comp());
            } else if (method_name == "temperature") {
                return call.ok(focuser->get_temperature());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "tempcomp") {
                bool value = call.get_bool("TempComp");
                focuser->set_temp_comp(value);
                return call.ok_void();
            } else if (method_name == "move") {
                int position = call.get_int("Position");
                focuser->move(position);
                return call.ok_void();
            } else if (method_name == "halt") {
                focuser->halt();
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Focuser");
    });
}

}  // namespace alpacahttp
