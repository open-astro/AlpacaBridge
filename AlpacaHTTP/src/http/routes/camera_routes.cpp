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

// The Camera method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/camera_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/log_text.h>
#include <alpacahttp/util/logging_adapter.h>

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "../device_call.h"
#include "../image_payload.h"

namespace alpacahttp {

// Moved verbatim from router.cpp (issue #2081), which predates the format gate for these lines;
// formatting the body is its own change, so the move stays byte-identical.
// clang-format off
Response Router::dispatch_camera_method(std::shared_ptr<alpacacore::CameraDriver> camera,
                                        const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    auto dispatch_start = std::chrono::steady_clock::now();
    alpacacore::logging::log(alpacacore::logging::LogLevel::Trace, "AlpacaHTTP",
        "dispatch_camera_method entry: method=" + method_name);
    struct DispatchExitLog {
        std::string method_name;
        std::chrono::steady_clock::time_point start;
        ~DispatchExitLog() {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            alpacacore::logging::log(alpacacore::logging::LogLevel::Trace, "AlpacaHTTP",
                "dispatch_camera_method exit: method=" + method_name + " duration_ms=" + std::to_string(ms));
        }
    } dispatch_exit_log{method_name, dispatch_start};

    auto format_utc = [](std::chrono::system_clock::time_point time_point) -> std::string {
        auto time_t = std::chrono::system_clock::to_time_t(time_point);
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
        return oss.str();
    };

    return call.run("camera method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "bayeroffsetx") {
                return call.ok(camera->get_bayer_offset_x());
            } else if (method_name == "bayeroffsety") {
                return call.ok(camera->get_bayer_offset_y());
            } else if (method_name == "binx") {
                return call.ok(camera->get_bin_x());
            } else if (method_name == "biny") {
                return call.ok(camera->get_bin_y());
            } else if (method_name == "camerastate") {
                return call.ok(static_cast<int>(camera->get_camera_state()));
            } else if (method_name == "cameraxsize") {
                return call.ok(camera->get_camera_x_size());
            } else if (method_name == "cameraysize") {
                return call.ok(camera->get_camera_y_size());
            } else if (method_name == "canabortexposure") {
                return call.ok(camera->get_can_abort_exposure());
            } else if (method_name == "canasymmetricbin") {
                return call.ok(camera->get_can_asymmetric_bin());
            } else if (method_name == "canfastreadout") {
                return call.ok(camera->get_can_fast_readout());
            } else if (method_name == "cangetcoolerpower") {
                return call.ok(camera->get_can_get_cooler_power());
            } else if (method_name == "canpulseguide") {
                return call.ok(camera->get_can_pulse_guide());
            } else if (method_name == "cansetccdtemperature") {
                return call.ok(camera->get_can_set_ccd_temperature());
            } else if (method_name == "canstopexposure") {
                return call.ok(camera->get_can_stop_exposure());
            } else if (method_name == "ccdtemperature") {
                return call.ok(camera->get_ccd_temperature());
            } else if (method_name == "cooleron") {
                return call.ok(camera->get_cooler_on());
            } else if (method_name == "coolerpower") {
                return call.ok(camera->get_cooler_power());
            } else if (method_name == "electronsperadu") {
                return call.ok(camera->get_electrons_per_adu());
            } else if (method_name == "exposuremax") {
                return call.ok(camera->get_exposure_max());
            } else if (method_name == "exposuremin") {
                return call.ok(camera->get_exposure_min());
            } else if (method_name == "exposureresolution") {
                return call.ok(camera->get_exposure_resolution());
            } else if (method_name == "fastreadout") {
                return call.ok(camera->get_fast_readout());
            } else if (method_name == "fullwellcapacity") {
                return call.ok(camera->get_full_well_capacity());
            } else if (method_name == "gain") {
                return call.ok(camera->get_gain());
            } else if (method_name == "gainmax") {
                return call.ok(camera->get_gain_max());
            } else if (method_name == "gainmin") {
                return call.ok(camera->get_gain_min());
            } else if (method_name == "gains") {
                nlohmann::json gains = nlohmann::json::array();
                for (const auto& gain : camera->get_gains()) {
                    gains.push_back(gain);
                }
                return call.ok(gains);
            } else if (method_name == "hasshutter") {
                return call.ok(camera->get_has_shutter());
            } else if (method_name == "heatsinktemperature") {
                return call.ok(camera->get_heat_sink_temperature());
            } else if (method_name == "imagearray") {
                const bool has_accept = request.has_header("accept");
                const std::string accept = has_accept ? request.get_header("accept") : std::string();
                util::log_debug("Camera imagearray Accept: " + (has_accept ? util::escape_for_log(accept) : "<none>") +
                                ", imagebytes=" + std::string(accepts_imagebytes(accept) ? "true" : "false"));
                auto payload =
                    build_shared(camera.get(), [&camera] { return camera->get_image_array(); }, accept, call);
                Response response;
                response.set_content_type(payload.content_type);
                response.set_body(std::move(payload.body), std::move(payload.tail), payload.tail_offset);
                return response;
            } else if (method_name == "imagearrayvariant") {
                if (camera->get_connecting() || !camera->get_connected()) {
                    throw alpacacore::AlpacaException("Camera is not connected", alpacacore::AlpacaError::NotConnected);
                }
                // ImageArrayVariant is COM-only; Alpaca must refuse it without retrieving a frame.
                throw alpacacore::AlpacaException("ImageArrayVariant is not supported by Alpaca; use ImageArray",
                                                  alpacacore::AlpacaError::PropertyNotImplemented);
            } else if (method_name == "imageready") {
                return call.ok(camera->get_image_ready());
            } else if (method_name == "ispulseguiding") {
                return call.ok(camera->get_is_pulse_guiding());
            } else if (method_name == "lastexposureduration") {
                return call.ok(camera->get_last_exposure_duration());
            } else if (method_name == "lastexposurestarttime") {
                return call.ok(format_utc(camera->get_last_exposure_start_time()));
            } else if (method_name == "maxadu") {
                return call.ok(camera->get_max_adu());
            } else if (method_name == "maxbinx") {
                return call.ok(camera->get_max_bin_x());
            } else if (method_name == "maxbiny") {
                return call.ok(camera->get_max_bin_y());
            } else if (method_name == "numx") {
                return call.ok(camera->get_num_x());
            } else if (method_name == "numy") {
                return call.ok(camera->get_num_y());
            } else if (method_name == "offset") {
                return call.ok(camera->get_offset());
            } else if (method_name == "offsetmax") {
                return call.ok(camera->get_offset_max());
            } else if (method_name == "offsetmin") {
                return call.ok(camera->get_offset_min());
            } else if (method_name == "offsets") {
                nlohmann::json offsets = nlohmann::json::array();
                for (const auto& offset : camera->get_offsets()) {
                    offsets.push_back(offset);
                }
                return call.ok(offsets);
            } else if (method_name == "percentcompleted") {
                auto percent = camera->get_percent_completed_checked();
                auto value = static_cast<std::int32_t>(percent);
                return call.ok(value);
            } else if (method_name == "pixelsizex") {
                return call.ok(camera->get_pixel_size_x());
            } else if (method_name == "pixelsizey") {
                return call.ok(camera->get_pixel_size_y());
            } else if (method_name == "readoutmode") {
                return call.ok(camera->get_readout_mode());
            } else if (method_name == "readoutmodes") {
                nlohmann::json modes = nlohmann::json::array();
                for (const auto& mode : camera->get_readout_modes()) {
                    modes.push_back(mode);
                }
                return call.ok(modes);
            } else if (method_name == "sensorname") {
                return call.ok(camera->get_sensor_name());
            } else if (method_name == "sensortype") {
                return call.ok(static_cast<int>(camera->get_sensor_type()));
            } else if (method_name == "setccdtemperature") {
                return call.ok(camera->get_set_ccd_temperature());
            } else if (method_name == "startx") {
                return call.ok(camera->get_start_x());
            } else if (method_name == "starty") {
                return call.ok(camera->get_start_y());
            } else if (method_name == "subexposureduration") {
                return call.ok(camera->get_sub_exposure_duration());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "binx") {
                int value = call.get_int("BinX");
                camera->set_bin_x(value);
                return call.ok_void();
            } else if (method_name == "biny") {
                int value = call.get_int("BinY");
                camera->set_bin_y(value);
                return call.ok_void();
            } else if (method_name == "fastreadout") {
                bool value = call.get_bool("FastReadout");
                camera->set_fast_readout(value);
                return call.ok_void();
            } else if (method_name == "gain") {
                int value = call.get_int("Gain");
                camera->set_gain(value);
                return call.ok_void();
            } else if (method_name == "cooleron") {
                bool value = call.get_bool("CoolerOn");
                camera->set_cooler_on(value);
                return call.ok_void();
            } else if (method_name == "setccdtemperature") {
                double value = call.get_double("SetCCDTemperature");
                camera->set_set_ccd_temperature(value);
                return call.ok_void();
            } else if (method_name == "numx") {
                int value = call.get_int("NumX");
                camera->set_num_x(value);
                return call.ok_void();
            } else if (method_name == "numy") {
                int value = call.get_int("NumY");
                camera->set_num_y(value);
                return call.ok_void();
            } else if (method_name == "offset") {
                int value = call.get_int("Offset");
                camera->set_offset(value);
                return call.ok_void();
            } else if (method_name == "readoutmode") {
                int value = call.get_int("ReadoutMode");
                camera->set_readout_mode(value);
                return call.ok_void();
            } else if (method_name == "startx") {
                int value = call.get_int("StartX");
                camera->set_start_x(value);
                return call.ok_void();
            } else if (method_name == "starty") {
                int value = call.get_int("StartY");
                camera->set_start_y(value);
                return call.ok_void();
            } else if (method_name == "subexposureduration") {
                double value = call.get_double("SubExposureDuration");
                camera->set_sub_exposure_duration(value);
                return call.ok_void();
            } else if (method_name == "startexposure") {
                double duration = call.get_double("Duration");
                bool light = call.get_bool("Light");
                camera->start_exposure(duration, light);
                return call.ok_void();
            } else if (method_name == "pulseguide") {
                int direction = call.get_int("Direction");
                int duration = call.get_int("Duration");
                camera->pulse_guide(direction, duration);
                return call.ok_void();
            } else if (method_name == "stopexposure") {
                camera->stop_exposure();
                return call.ok_void();
            } else if (method_name == "abortexposure") {
                camera->abort_exposure();
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Camera");
    });
}
// clang-format on

}  // namespace alpacahttp
