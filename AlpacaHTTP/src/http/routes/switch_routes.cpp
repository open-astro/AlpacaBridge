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

// The Switch method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/switch_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>

#include <memory>
#include <string>

#include "../device_call.h"

namespace alpacahttp {

Response Router::dispatch_switch_method(std::shared_ptr<alpacacore::SwitchDriver> sw, const std::string& method_name,
                                        const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("switch method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "maxswitch") {
                return call.ok(sw->get_max_switch());
            } else if (method_name == "cancelasync") {
                // The switch driver has no cancel member, so succeeding here
                // would report a cancel that never happened (#576). The Id is
                // still validated first: an out-of-range one is InvalidValue.
                (void)sw->get_can_async(call.get_int("Id"));
                throw alpacacore::AlpacaException("Switch does not support cancelling an asynchronous operation",
                                                  alpacacore::AlpacaError::NotImplemented);
            } else if (method_name == "canasync") {
                int id = call.get_int("Id");
                return call.ok(sw->get_can_async(id));
            } else if (method_name == "canwrite") {
                int id = call.get_int("Id");
                return call.ok(sw->get_can_write(id));
            } else if (method_name == "getswitch") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch(id));
            } else if (method_name == "getswitchvalue") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch_value(id));
            } else if (method_name == "getswitchname") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch_name(id));
            } else if (method_name == "getswitchdescription") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch_description(id));
            } else if (method_name == "minswitchvalue") {
                int id = call.get_int("Id");
                return call.ok(sw->get_min_switch_value(id));
            } else if (method_name == "maxswitchvalue") {
                int id = call.get_int("Id");
                return call.ok(sw->get_max_switch_value(id));
            } else if (method_name == "statechangecomplete") {
                int id = call.get_int("Id");
                return call.ok(sw->get_state_change_complete(id));
            } else if (method_name == "switchstep") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch_step(id));
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "setswitch") {
                int id = call.get_int("Id");
                bool state = call.get_bool("State");
                sw->set_switch(id, state);
                return call.ok_void();
            } else if (method_name == "cancelasync") {
                // The switch driver has no cancel member, so succeeding here
                // would report a cancel that never happened (#576). The Id is
                // still validated first: an out-of-range one is InvalidValue.
                (void)sw->get_can_async(call.get_int("Id"));
                throw alpacacore::AlpacaException("Switch does not support cancelling an asynchronous operation",
                                                  alpacacore::AlpacaError::NotImplemented);
            } else if (method_name == "setasync") {
                int id = call.get_int("Id");
                bool state = call.get_bool("State");
                sw->set_async(id, state);
                return call.ok_void();
            } else if (method_name == "setswitchvalue") {
                int id = call.get_int("Id");
                double value = call.get_double("Value");
                sw->set_switch_value(id, value);
                return call.ok_void();
            } else if (method_name == "setasyncvalue") {
                int id = call.get_int("Id");
                double value = call.get_double("Value");
                sw->set_async_value(id, value);
                return call.ok_void();
            } else if (method_name == "setswitchname") {
                int id = call.get_int("Id");
                std::string name = call.get_string("Name");
                refuse_over_long("Name", name, kMaxNameBytes);
                sw->set_switch_name(id, name);
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Switch");
    });
}

}  // namespace alpacahttp
