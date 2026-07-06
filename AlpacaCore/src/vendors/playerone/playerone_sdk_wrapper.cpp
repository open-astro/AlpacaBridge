// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <PlayerOneCamera.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/playerone/playerone_sdk_wrapper.h>

#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace alpacacore::vendor::playerone {

namespace {

int map_poa_error(POAErrors err) {
    switch (err) {
    case POA_ERROR_INVALID_INDEX:
    case POA_ERROR_INVALID_ID:
    case POA_ERROR_INVALID_CONFIG:
    case POA_ERROR_INVALID_ARGU:
    case POA_ERROR_OUT_OF_LIMIT:
    case POA_ERROR_POINTER:
        return AlpacaError::InvalidValue;
    case POA_ERROR_NOT_OPENED:
    case POA_ERROR_DEVICE_NOT_FOUND:
    case POA_ERROR_ACCESS_DENIED:
        return AlpacaError::NotConnected;
    case POA_ERROR_CONF_CANNOT_WRITE:
    case POA_ERROR_CONF_CANNOT_READ:
        return AlpacaError::NotImplemented;
    case POA_ERROR_EXPOSING:
    case POA_ERROR_TIMEOUT:
    case POA_ERROR_SIZE_LESS:
        return AlpacaError::InvalidOperation;
    case POA_ERROR_EXPOSURE_FAILED:
    case POA_ERROR_OPERATION_FAILED:
    case POA_ERROR_MEMORY_FAILED:
    default:
        return AlpacaError::DriverException;
    }
}

void throw_on_error(POAErrors err, const char* context) {
    if (err == POA_OK) return;
    const char* msg = POAGetErrorString(err);
    std::string full = std::string(context) + ": " + (msg ? msg : "unknown POA error");
    throw AlpacaException(full, map_poa_error(err));
}

PlayerOneBayerPattern translate_bayer(POABayerPattern b) {
    switch (b) {
    case POA_BAYER_RG:   return PlayerOneBayerPattern::RG;
    case POA_BAYER_BG:   return PlayerOneBayerPattern::BG;
    case POA_BAYER_GR:   return PlayerOneBayerPattern::GR;
    case POA_BAYER_GB:   return PlayerOneBayerPattern::GB;
    case POA_BAYER_MONO:
    default:             return PlayerOneBayerPattern::None;
    }
}

PlayerOneImageFormat translate_format(POAImgFormat f) {
    switch (f) {
    case POA_RAW8:  return PlayerOneImageFormat::Raw8;
    case POA_RAW16: return PlayerOneImageFormat::Raw16;
    case POA_RGB24: return PlayerOneImageFormat::Rgb24;
    case POA_MONO8: return PlayerOneImageFormat::Mono8;
    case POA_END:
    default:        return PlayerOneImageFormat::Unknown;
    }
}

POAImgFormat to_poa_format(PlayerOneImageFormat f) {
    switch (f) {
    case PlayerOneImageFormat::Raw8:  return POA_RAW8;
    case PlayerOneImageFormat::Raw16: return POA_RAW16;
    case PlayerOneImageFormat::Rgb24: return POA_RGB24;
    case PlayerOneImageFormat::Mono8: return POA_MONO8;
    default:                          return POA_END;
    }
}

POAConfig guide_to_config(PlayerOneGuideDirection dir) {
    switch (dir) {
    case PlayerOneGuideDirection::North: return POA_GUIDE_NORTH;
    case PlayerOneGuideDirection::South: return POA_GUIDE_SOUTH;
    case PlayerOneGuideDirection::East:  return POA_GUIDE_EAST;
    case PlayerOneGuideDirection::West:  return POA_GUIDE_WEST;
    }
    return POA_GUIDE_NORTH;
}

PlayerOneCameraInfo props_to_info(int index, const POACameraProperties& p) {
    PlayerOneCameraInfo info;
    info.index = index;
    info.camera_id = p.cameraID;
    // Name = model + optional custom id, matches the Player One stock UI ("Mars-C [Juno]").
    info.name = p.cameraModelName;
    if (p.userCustomID[0] != '\0') {
        info.name += " [";
        info.name += p.userCustomID;
        info.name += "]";
    }
    info.sensor_model = p.sensorModelName;
    info.serial_number = p.SN;
    info.max_width = p.maxWidth;
    info.max_height = p.maxHeight;
    info.bit_depth = p.bitDepth;
    info.is_color = (p.isColorCamera == POA_TRUE);
    info.is_usb3 = (p.isUSB3Speed == POA_TRUE);
    info.bayer = translate_bayer(p.bayerPattern);
    info.pixel_size_um = p.pixelSize;
    info.has_st4_port = (p.isHasST4Port == POA_TRUE);
    info.has_cooler = (p.isHasCooler == POA_TRUE);
    for (int i = 0; i < 8 && p.bins[i] != 0; ++i) {
        info.supported_bins.push_back(p.bins[i]);
    }
    for (int i = 0; i < 8 && p.imgFormats[i] != POA_END; ++i) {
        info.supported_formats.push_back(translate_format(p.imgFormats[i]));
    }
    return info;
}

} // namespace

class PlayerOneSDKWrapper::Impl {
public:
    struct CameraUsage {
        int open_count{0};
        bool initialized{false};
    };

    std::mutex mutex_;
    std::map<int, CameraUsage> usage_;
};

PlayerOneSDKWrapper::PlayerOneSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

PlayerOneSDKWrapper::~PlayerOneSDKWrapper() = default;

PlayerOneSDKWrapper& PlayerOneSDKWrapper::instance() {
    static PlayerOneSDKWrapper wrapper;
    return wrapper;
}

std::string PlayerOneSDKWrapper::get_sdk_version() {
    const char* v = POAGetSDKVersion();
    return v ? v : "unknown";
}

int PlayerOneSDKWrapper::get_api_version() {
    return POAGetAPIVersion();
}

std::vector<PlayerOneCameraInfo> PlayerOneSDKWrapper::enumerate_cameras() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int count = POAGetCameraCount();
    std::vector<PlayerOneCameraInfo> result;
    result.reserve(static_cast<std::size_t>(count > 0 ? count : 0));
    for (int i = 0; i < count; ++i) {
        POACameraProperties p{};
        POAErrors err = POAGetCameraProperties(i, &p);
        if (err != POA_OK) {
            continue;
        }
        result.push_back(props_to_info(i, p));
    }
    return result;
}

void PlayerOneSDKWrapper::open_camera(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[camera_id];
    if (usage.open_count == 0) {
        throw_on_error(POAOpenCamera(camera_id), "POAOpenCamera");
    }
    ++usage.open_count;
}

void PlayerOneSDKWrapper::init_camera(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[camera_id];
    if (!usage.initialized) {
        throw_on_error(POAInitCamera(camera_id), "POAInitCamera");
        usage.initialized = true;
    }
}

void PlayerOneSDKWrapper::close_camera(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto it = pimpl_->usage_.find(camera_id);
    if (it == pimpl_->usage_.end() || it->second.open_count <= 0) {
        return;
    }
    --it->second.open_count;
    if (it->second.open_count == 0) {
        POACloseCamera(camera_id);
        pimpl_->usage_.erase(it);
    }
}

PlayerOneCameraInfo PlayerOneSDKWrapper::get_camera_properties_by_id(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POACameraProperties p{};
    throw_on_error(POAGetCameraPropertiesByID(camera_id, &p),
                   "POAGetCameraPropertiesByID");
    return props_to_info(-1, p);
}

PlayerOneConfigCaps PlayerOneSDKWrapper::probe_config_caps(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    PlayerOneConfigCaps caps{};
    int count = 0;
    throw_on_error(POAGetConfigsCount(camera_id, &count), "POAGetConfigsCount");
    for (int i = 0; i < count; ++i) {
        POAConfigAttributes a{};
        POAErrors err = POAGetConfigAttributes(camera_id, i, &a);
        if (err != POA_OK) continue;
        switch (a.configID) {
        case POA_EXPOSURE:
            caps.has_exposure = true;
            caps.exposure_min_us = a.minValue.intValue;
            caps.exposure_max_us = a.maxValue.intValue;
            caps.exposure_default_us = a.defaultValue.intValue;
            break;
        case POA_GAIN:
            caps.has_gain = true;
            caps.gain_writable = (a.isWritable == POA_TRUE);
            caps.gain_min = a.minValue.intValue;
            caps.gain_max = a.maxValue.intValue;
            caps.gain_default = a.defaultValue.intValue;
            caps.gain_supports_auto = (a.isSupportAuto == POA_TRUE);
            break;
        case POA_OFFSET:
            caps.has_offset = true;
            caps.offset_writable = (a.isWritable == POA_TRUE);
            caps.offset_min = a.minValue.intValue;
            caps.offset_max = a.maxValue.intValue;
            caps.offset_default = a.defaultValue.intValue;
            break;
        case POA_TEMPERATURE:
            caps.has_temperature = true;
            break;
        case POA_COOLER:
            caps.has_cooler = true;
            break;
        case POA_TARGET_TEMP:
            caps.has_target_temp = true;
            caps.target_temp_min = a.minValue.intValue;
            caps.target_temp_max = a.maxValue.intValue;
            break;
        case POA_COOLER_POWER:
            caps.has_cooler_power = true;
            break;
        case POA_EGAIN:
            caps.has_egain = true;
            break;
        case POA_USB_BANDWIDTH_LIMIT:
            caps.has_usb_bandwidth = true;
            caps.usb_bandwidth_min = a.minValue.intValue;
            caps.usb_bandwidth_max = a.maxValue.intValue;
            break;
        case POA_FAN_POWER:
            caps.has_fan_power = true;
            caps.fan_power_writable = (a.isWritable == POA_TRUE);
            caps.fan_power_min = a.minValue.intValue;
            caps.fan_power_max = a.maxValue.intValue;
            caps.fan_power_default = a.defaultValue.intValue;
            break;
        case POA_HEATER_POWER:
            caps.has_heater_power = true;
            caps.heater_power_writable = (a.isWritable == POA_TRUE);
            caps.heater_power_min = a.minValue.intValue;
            caps.heater_power_max = a.maxValue.intValue;
            caps.heater_power_default = a.defaultValue.intValue;
            break;
        case POA_GUIDE_NORTH:
        case POA_GUIDE_SOUTH:
        case POA_GUIDE_EAST:
        case POA_GUIDE_WEST:
            caps.has_guide_st4 = true;
            break;
        default:
            break;
        }
    }
    return caps;
}

long PlayerOneSDKWrapper::get_config_int(int camera_id, int config_id, bool* is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POAConfigValue v{};
    POABool a = POA_FALSE;
    throw_on_error(POAGetConfig(camera_id, static_cast<POAConfig>(config_id), &v, &a),
                   "POAGetConfig(int)");
    if (is_auto) *is_auto = (a == POA_TRUE);
    return v.intValue;
}

double PlayerOneSDKWrapper::get_config_float(int camera_id, int config_id, bool* is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POAConfigValue v{};
    POABool a = POA_FALSE;
    throw_on_error(POAGetConfig(camera_id, static_cast<POAConfig>(config_id), &v, &a),
                   "POAGetConfig(float)");
    if (is_auto) *is_auto = (a == POA_TRUE);
    return v.floatValue;
}

bool PlayerOneSDKWrapper::get_config_bool(int camera_id, int config_id, bool* is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POAConfigValue v{};
    POABool a = POA_FALSE;
    throw_on_error(POAGetConfig(camera_id, static_cast<POAConfig>(config_id), &v, &a),
                   "POAGetConfig(bool)");
    if (is_auto) *is_auto = (a == POA_TRUE);
    return v.boolValue == POA_TRUE;
}

void PlayerOneSDKWrapper::set_config_int(int camera_id, int config_id, long value, bool is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POAConfigValue v{};
    v.intValue = value;
    throw_on_error(POASetConfig(camera_id, static_cast<POAConfig>(config_id), v,
                                is_auto ? POA_TRUE : POA_FALSE),
                   "POASetConfig(int)");
}

void PlayerOneSDKWrapper::set_config_float(int camera_id, int config_id, double value, bool is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POAConfigValue v{};
    v.floatValue = value;
    throw_on_error(POASetConfig(camera_id, static_cast<POAConfig>(config_id), v,
                                is_auto ? POA_TRUE : POA_FALSE),
                   "POASetConfig(float)");
}

void PlayerOneSDKWrapper::set_config_bool(int camera_id, int config_id, bool value, bool is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POAConfigValue v{};
    v.boolValue = value ? POA_TRUE : POA_FALSE;
    throw_on_error(POASetConfig(camera_id, static_cast<POAConfig>(config_id), v,
                                is_auto ? POA_TRUE : POA_FALSE),
                   "POASetConfig(bool)");
}

PlayerOneImageFormat PlayerOneSDKWrapper::get_image_format(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POAImgFormat f = POA_END;
    throw_on_error(POAGetImageFormat(camera_id, &f), "POAGetImageFormat");
    return translate_format(f);
}

void PlayerOneSDKWrapper::set_image_format(int camera_id, PlayerOneImageFormat format) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POAImgFormat f = to_poa_format(format);
    if (f == POA_END) {
        throw AlpacaException("Unknown Player One image format", AlpacaError::InvalidValue);
    }
    throw_on_error(POASetImageFormat(camera_id, f), "POASetImageFormat");
}

void PlayerOneSDKWrapper::get_image_size(int camera_id, int& width, int& height) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    width = 0;
    height = 0;
    throw_on_error(POAGetImageSize(camera_id, &width, &height), "POAGetImageSize");
}

void PlayerOneSDKWrapper::set_image_size(int camera_id, int width, int height) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(POASetImageSize(camera_id, width, height), "POASetImageSize");
}

void PlayerOneSDKWrapper::get_image_start_pos(int camera_id, int& start_x, int& start_y) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    start_x = 0;
    start_y = 0;
    throw_on_error(POAGetImageStartPos(camera_id, &start_x, &start_y), "POAGetImageStartPos");
}

void PlayerOneSDKWrapper::set_image_start_pos(int camera_id, int start_x, int start_y) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(POASetImageStartPos(camera_id, start_x, start_y), "POASetImageStartPos");
}

int PlayerOneSDKWrapper::get_image_bin(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int bin = 1;
    throw_on_error(POAGetImageBin(camera_id, &bin), "POAGetImageBin");
    return bin;
}

void PlayerOneSDKWrapper::set_image_bin(int camera_id, int bin) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(POASetImageBin(camera_id, bin), "POASetImageBin");
}

void PlayerOneSDKWrapper::start_exposure(int camera_id, bool single_frame) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(POAStartExposure(camera_id, single_frame ? POA_TRUE : POA_FALSE),
                   "POAStartExposure");
}

void PlayerOneSDKWrapper::stop_exposure(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(POAStopExposure(camera_id), "POAStopExposure");
}

PlayerOneCameraState PlayerOneSDKWrapper::get_camera_state(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POACameraState s = STATE_CLOSED;
    throw_on_error(POAGetCameraState(camera_id, &s), "POAGetCameraState");
    switch (s) {
    case STATE_CLOSED:   return PlayerOneCameraState::Closed;
    case STATE_OPENED:   return PlayerOneCameraState::Opened;
    case STATE_EXPOSING: return PlayerOneCameraState::Exposing;
    }
    return PlayerOneCameraState::Closed;
}

bool PlayerOneSDKWrapper::image_ready(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    POABool ready = POA_FALSE;
    throw_on_error(POAImageReady(camera_id, &ready), "POAImageReady");
    return ready == POA_TRUE;
}

bool PlayerOneSDKWrapper::get_image_data(int camera_id,
                                         std::uint8_t* buffer,
                                         std::size_t buffer_size,
                                         int timeout_ms) {
    // Do NOT hold the wrapper mutex — POAGetImageData can block for the full
    // timeout, and we need abort / disconnect paths to stay responsive.
    POAErrors err = POAGetImageData(camera_id, buffer,
                                    static_cast<long>(buffer_size), timeout_ms);
    if (err == POA_ERROR_TIMEOUT) return false;
    throw_on_error(err, "POAGetImageData");
    return true;
}

void PlayerOneSDKWrapper::pulse_guide_on(int camera_id, PlayerOneGuideDirection dir) {
    set_config_bool(camera_id, guide_to_config(dir), true, false);
}

void PlayerOneSDKWrapper::pulse_guide_off(int camera_id, PlayerOneGuideDirection dir) {
    set_config_bool(camera_id, guide_to_config(dir), false, false);
}

double PlayerOneSDKWrapper::get_temperature_c(int camera_id) {
    return get_config_float(camera_id, POA_TEMPERATURE);
}

bool PlayerOneSDKWrapper::get_cooler_on(int camera_id) {
    return get_config_bool(camera_id, POA_COOLER);
}

void PlayerOneSDKWrapper::set_cooler_on(int camera_id, bool on) {
    set_config_bool(camera_id, POA_COOLER, on);
}

int PlayerOneSDKWrapper::get_target_temp_c(int camera_id) {
    return static_cast<int>(get_config_int(camera_id, POA_TARGET_TEMP));
}

void PlayerOneSDKWrapper::set_target_temp_c(int camera_id, int target_c) {
    set_config_int(camera_id, POA_TARGET_TEMP, static_cast<long>(target_c));
}

int PlayerOneSDKWrapper::get_cooler_power_percent(int camera_id) {
    return static_cast<int>(get_config_int(camera_id, POA_COOLER_POWER));
}

double PlayerOneSDKWrapper::get_egain(int camera_id) {
    return get_config_float(camera_id, POA_EGAIN);
}

int PlayerOneSDKWrapper::get_heater_power_percent(int camera_id) {
    return static_cast<int>(get_config_int(camera_id, POA_HEATER_POWER));
}

void PlayerOneSDKWrapper::set_heater_power_percent(int camera_id, int percent) {
    set_config_int(camera_id, POA_HEATER_POWER, static_cast<long>(percent));
}

int PlayerOneSDKWrapper::get_fan_power_percent(int camera_id) {
    return static_cast<int>(get_config_int(camera_id, POA_FAN_POWER));
}

void PlayerOneSDKWrapper::set_fan_power_percent(int camera_id, int percent) {
    set_config_int(camera_id, POA_FAN_POWER, static_cast<long>(percent));
}

int PlayerOneSDKWrapper::get_sensor_mode_count(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int count = 0;
    POAErrors err = POAGetSensorModeCount(camera_id, &count);
    if (err != POA_OK) return 0;
    return count;
}

} // namespace alpacacore::vendor::playerone
