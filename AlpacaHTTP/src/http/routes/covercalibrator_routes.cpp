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

// The CoverCalibrator method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/covercalibrator_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>

#include <memory>
#include <string>

#include "../device_call.h"

namespace alpacahttp {

Response Router::dispatch_covercalibrator_method(std::shared_ptr<alpacacore::CoverCalibratorDriver> covercalibrator,
                                                 const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("cover calibrator method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "brightness") {
                return call.ok(covercalibrator->get_brightness());
            } else if (method_name == "calibratorchanging") {
                return call.ok(covercalibrator->get_calibrator_changing());
            } else if (method_name == "calibratorstate") {
                return call.ok(static_cast<int>(covercalibrator->get_calibrator_state()));
            } else if (method_name == "covermoving") {
                return call.ok(covercalibrator->get_cover_moving());
            } else if (method_name == "coverstate") {
                return call.ok(static_cast<int>(covercalibrator->get_cover_state()));
            } else if (method_name == "maxbrightness") {
                return call.ok(covercalibrator->get_max_brightness());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "calibratoron") {
                int brightness = call.get_int("Brightness");
                covercalibrator->calibrator_on(brightness);
                return call.ok_void();
            } else if (method_name == "calibratoroff") {
                covercalibrator->calibrator_off();
                return call.ok_void();
            } else if (method_name == "opencover") {
                covercalibrator->open_cover();
                return call.ok_void();
            } else if (method_name == "closecover") {
                covercalibrator->close_cover();
                return call.ok_void();
            } else if (method_name == "haltcover") {
                covercalibrator->halt_cover();
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for CoverCalibrator");
    });
}

}  // namespace alpacahttp
