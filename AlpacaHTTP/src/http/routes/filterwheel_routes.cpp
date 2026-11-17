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

// The FilterWheel method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/filterwheel_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "../device_call.h"

namespace alpacahttp {

Response Router::dispatch_filterwheel_method(std::shared_ptr<alpacacore::FilterWheelDriver> filterwheel,
                                             const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("filter wheel method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "position") {
                return call.ok(filterwheel->get_position());
            } else if (method_name == "names") {
                nlohmann::json names = nlohmann::json::array();
                for (const auto& name : filterwheel->get_names()) {
                    names.push_back(name);
                }
                return call.ok(names);
            } else if (method_name == "focusoffsets") {
                nlohmann::json offsets = nlohmann::json::array();
                for (int offset : filterwheel->get_focus_offsets()) {
                    offsets.push_back(offset);
                }
                return call.ok(offsets);
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "position") {
                int value = call.get_int("Position");
                filterwheel->set_position(value);
                return call.ok_void();
            } else if (method_name == "names") {
                const auto* names_val = call.json_field("Names");
                if (!names_val) {
                    throw_bad_request("Missing parameter: Names");
                }
                if (!names_val->is_array()) {
                    throw_bad_request("Names must be an array of strings");
                }
                for (const auto& name : *names_val) {
                    if (!name.is_string()) {
                        throw_bad_request("Names must be an array of strings");
                    }
                }
                std::vector<std::string> names = names_val->get<std::vector<std::string>>();
                for (const auto& name : names) {
                    refuse_over_long("A filter name", name, kMaxNameBytes);
                }
                filterwheel->set_names(names);
                return call.ok_void();
            } else if (method_name == "focusoffsets") {
                const auto* offsets_val = call.json_field("FocusOffsets");
                if (!offsets_val) {
                    throw_bad_request("Missing parameter: FocusOffsets");
                }
                if (!offsets_val->is_array()) {
                    throw_bad_request("FocusOffsets must be an array of integers");
                }
                std::vector<int> offsets;
                offsets.reserve(offsets_val->size());
                for (const auto& offset : *offsets_val) {
                    offsets.push_back(json_int_value(offset, "FocusOffsets"));
                }
                filterwheel->set_focus_offsets(offsets);
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for FilterWheel");
    });
}

}  // namespace alpacahttp
