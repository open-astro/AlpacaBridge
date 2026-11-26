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

// The Rotator method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/rotator_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>

#include <memory>
#include <string>

#include "../device_call.h"

namespace alpacahttp {

Response Router::dispatch_rotator_method(std::shared_ptr<alpacacore::RotatorDriver> rotator,
                                         const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("rotator method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "canreverse") {
                return call.ok(rotator->get_can_reverse());
            } else if (method_name == "reverse") {
                return call.ok(rotator->get_reverse());
            } else if (method_name == "ismoving") {
                return call.ok(rotator->get_is_moving());
            } else if (method_name == "mechanicalposition") {
                return call.ok(rotator->get_mechanical_position());
            } else if (method_name == "position") {
                return call.ok(rotator->get_position());
            } else if (method_name == "stepsize") {
                return call.ok(rotator->get_step_size());
            } else if (method_name == "targetposition") {
                return call.ok(rotator->get_target_position());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "reverse") {
                bool value = call.get_bool("Reverse");
                rotator->set_reverse(value);
                return call.ok_void();
            } else if (method_name == "move") {
                double position = call.get_double("Position");
                rotator->move(position);
                return call.ok_void();
            } else if (method_name == "moveabsolute") {
                double position = call.get_double("Position");
                rotator->move_absolute(position);
                return call.ok_void();
            } else if (method_name == "movemechanical") {
                double position = call.get_double("Position");
                rotator->move_mechanical(position);
                return call.ok_void();
            } else if (method_name == "halt") {
                rotator->halt();
                return call.ok_void();
            } else if (method_name == "sync") {
                double position = call.get_double("Position");
                rotator->sync(position);
                return call.ok_void();
            } else if (method_name == "targetposition") {
                double position = call.get_double("TargetPosition");
                rotator->set_target_position(position);
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Rotator");
    });
}

}  // namespace alpacahttp
