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

// The SafetyMonitor method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/safetymonitor_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>

#include <memory>
#include <string>

#include "../device_call.h"

namespace alpacahttp {

Response Router::dispatch_safetymonitor_method(std::shared_ptr<alpacacore::SafetyMonitorDriver> safetymonitor,
                                               const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    // No SafetyMonitor method reads a parameter today, so the 400 text path of run()
    // is unreachable here. Kept so a method that adds one answers 400 text like the
    // other dispatchers.
    return call.run("safety monitor method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "issafe") {
                return call.ok(safetymonitor->get_is_safe());
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for SafetyMonitor");
    });
}

}  // namespace alpacahttp
