// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacacore/vendor/svbony/svbony_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <SVBCameraSDK.h>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace alpacacore::vendor::svbony {

namespace {

SVBImageType from_svb_image_type(SVB_IMG_TYPE type) {
    switch (type) {
    case SVB_IMG_RAW8:
        return SVBImageType::Raw8;
    case SVB_IMG_RAW16:
        return SVBImageType::Raw16;
    case SVB_IMG_Y8:
        return SVBImageType::Y8;
    case SVB_IMG_Y16:
        return SVBImageType::Y16;
    case SVB_IMG_RGB24:
        return SVBImageType::Rgb24;
    case SVB_IMG_RGB32:
        return SVBImageType::Rgb32;
    default:
        return SVBImageType::Raw8;
    }
}

SVB_IMG_TYPE to_svb_image_type(SVBImageType type) {
    switch (type) {
    case SVBImageType::Raw8:
        return SVB_IMG_RAW8;
    case SVBImageType::Raw16:
        return SVB_IMG_RAW16;
    case SVBImageType::Y8:
        return SVB_IMG_Y8;
    case SVBImageType::Y16:
        return SVB_IMG_Y16;
    case SVBImageType::Rgb24:
        return SVB_IMG_RGB24;
    case SVBImageType::Rgb32:
        return SVB_IMG_RGB32;
    }
    return SVB_IMG_RAW8;
}

SVBBayerPattern from_svb_bayer_pattern(SVB_BAYER_PATTERN pattern) {
    switch (pattern) {
    case SVB_BAYER_RG:
        return SVBBayerPattern::RG;
    case SVB_BAYER_BG:
        return SVBBayerPattern::BG;
    case SVB_BAYER_GR:
        return SVBBayerPattern::GR;
    case SVB_BAYER_GB:
        return SVBBayerPattern::GB;
    default:
        return SVBBayerPattern::None;
    }
}

SVB_GUIDE_DIRECTION to_svb_guide_direction(SVBGuideDirection direction) {
    // SVBONY guide directions match ASCOM: North=0, South=1, East=2, West=3
    switch (direction) {
    case SVBGuideDirection::North:
        return SVB_GUIDE_NORTH;
    case SVBGuideDirection::South:
        return SVB_GUIDE_SOUTH;
    case SVBGuideDirection::East:
        return SVB_GUIDE_EAST;
    case SVBGuideDirection::West:
        return SVB_GUIDE_WEST;
    }
    return SVB_GUIDE_NORTH;
}

std::optional<SVBControlType> to_control_type(SVB_CONTROL_TYPE type) {
    switch (type) {
    case SVB_GAIN:
        return SVBControlType::Gain;
    case SVB_EXPOSURE:
        return SVBControlType::Exposure;
    case SVB_GAMMA:
        return SVBControlType::Gamma;
    case SVB_BLACK_LEVEL:
        return SVBControlType::Offset;
    case SVB_FLIP:
        return SVBControlType::Flip;
    case SVB_FRAME_SPEED_MODE:
        return SVBControlType::FrameSpeedMode;
    case SVB_CONTRAST:
        return SVBControlType::Contrast;
    case SVB_SHARPNESS:
        return SVBControlType::Sharpness;
    case SVB_SATURATION:
        return SVBControlType::Saturation;
    case SVB_AUTO_TARGET_BRIGHTNESS:
        return SVBControlType::AutoTargetBrightness;
    case SVB_COOLER_ENABLE:
        return SVBControlType::CoolerEnable;
    case SVB_TARGET_TEMPERATURE:
        return SVBControlType::TargetTemperature;
    case SVB_CURRENT_TEMPERATURE:
        return SVBControlType::CurrentTemperature;
    case SVB_COOLER_POWER:
        return SVBControlType::CoolerPower;
    case SVB_BAD_PIXEL_CORRECTION_ENABLE:
        return SVBControlType::BadPixelCorrectionEnable;
    case SVB_BAD_PIXEL_CORRECTION_THRESHOLD:
        return SVBControlType::BadPixelCorrectionThreshold;
    default:
        return std::nullopt;
    }
}

SVB_CONTROL_TYPE to_svb_control_type(SVBControlType type) {
    switch (type) {
    case SVBControlType::Gain:
        return SVB_GAIN;
    case SVBControlType::Exposure:
        return SVB_EXPOSURE;
    case SVBControlType::Gamma:
        return SVB_GAMMA;
    case SVBControlType::Offset:
        return SVB_BLACK_LEVEL;
    case SVBControlType::Flip:
        return SVB_FLIP;
    case SVBControlType::FrameSpeedMode:
        return SVB_FRAME_SPEED_MODE;
    case SVBControlType::Contrast:
        return SVB_CONTRAST;
    case SVBControlType::Sharpness:
        return SVB_SHARPNESS;
    case SVBControlType::Saturation:
        return SVB_SATURATION;
    case SVBControlType::AutoTargetBrightness:
        return SVB_AUTO_TARGET_BRIGHTNESS;
    case SVBControlType::BlackLevel:
        return SVB_BLACK_LEVEL;
    case SVBControlType::CoolerEnable:
        return SVB_COOLER_ENABLE;
    case SVBControlType::TargetTemperature:
        return SVB_TARGET_TEMPERATURE;
    case SVBControlType::CurrentTemperature:
        return SVB_CURRENT_TEMPERATURE;
    case SVBControlType::CoolerPower:
        return SVB_COOLER_POWER;
    case SVBControlType::BadPixelCorrectionEnable:
        return SVB_BAD_PIXEL_CORRECTION_ENABLE;
    case SVBControlType::BadPixelCorrectionThreshold:
        return SVB_BAD_PIXEL_CORRECTION_THRESHOLD;
    }
    return SVB_GAIN;
}

std::string error_code_to_string(SVB_ERROR_CODE code) {
    switch (code) {
    case SVB_SUCCESS:
        return "SVB_SUCCESS";
    case SVB_ERROR_INVALID_INDEX:
        return "SVB_ERROR_INVALID_INDEX";
    case SVB_ERROR_INVALID_ID:
        return "SVB_ERROR_INVALID_ID";
    case SVB_ERROR_INVALID_CONTROL_TYPE:
        return "SVB_ERROR_INVALID_CONTROL_TYPE";
    case SVB_ERROR_CAMERA_CLOSED:
        return "SVB_ERROR_CAMERA_CLOSED";
    case SVB_ERROR_CAMERA_REMOVED:
        return "SVB_ERROR_CAMERA_REMOVED";
    case SVB_ERROR_INVALID_PATH:
        return "SVB_ERROR_INVALID_PATH";
    case SVB_ERROR_INVALID_FILEFORMAT:
        return "SVB_ERROR_INVALID_FILEFORMAT";
    case SVB_ERROR_INVALID_SIZE:
        return "SVB_ERROR_INVALID_SIZE";
    case SVB_ERROR_INVALID_IMGTYPE:
        return "SVB_ERROR_INVALID_IMGTYPE";
    case SVB_ERROR_OUTOF_BOUNDARY:
        return "SVB_ERROR_OUTOF_BOUNDARY";
    case SVB_ERROR_TIMEOUT:
        return "SVB_ERROR_TIMEOUT";
    case SVB_ERROR_INVALID_SEQUENCE:
        return "SVB_ERROR_INVALID_SEQUENCE";
    case SVB_ERROR_BUFFER_TOO_SMALL:
        return "SVB_ERROR_BUFFER_TOO_SMALL";
    case SVB_ERROR_VIDEO_MODE_ACTIVE:
        return "SVB_ERROR_VIDEO_MODE_ACTIVE";
    case SVB_ERROR_EXPOSURE_IN_PROGRESS:
        return "SVB_ERROR_EXPOSURE_IN_PROGRESS";
    case SVB_ERROR_GENERAL_ERROR:
        return "SVB_ERROR_GENERAL_ERROR";
    case SVB_ERROR_INVALID_MODE:
        return "SVB_ERROR_INVALID_MODE";
    case SVB_ERROR_INVALID_DIRECTION:
        return "SVB_ERROR_INVALID_DIRECTION";
    case SVB_ERROR_UNKNOW_SENSOR_TYPE:
        return "SVB_ERROR_UNKNOW_SENSOR_TYPE";
    default:
        return "SVB_ERROR_UNKNOWN";
    }
}

int map_error_code(SVB_ERROR_CODE code) {
    switch (code) {
    case SVB_ERROR_INVALID_INDEX:
    case SVB_ERROR_INVALID_ID:
    case SVB_ERROR_INVALID_CONTROL_TYPE:
    case SVB_ERROR_INVALID_SIZE:
    case SVB_ERROR_INVALID_IMGTYPE:
    case SVB_ERROR_OUTOF_BOUNDARY:
    case SVB_ERROR_INVALID_MODE:
    case SVB_ERROR_INVALID_DIRECTION:
        return AlpacaError::InvalidValue;
    case SVB_ERROR_CAMERA_CLOSED:
    case SVB_ERROR_CAMERA_REMOVED:
        return AlpacaError::NotConnected;
    case SVB_ERROR_EXPOSURE_IN_PROGRESS:
    case SVB_ERROR_VIDEO_MODE_ACTIVE:
        return AlpacaError::InvalidOperation;
    default:
        return AlpacaError::DriverException;
    }
}

void throw_on_error(SVB_ERROR_CODE code, const std::string& context) {
    if (code == SVB_SUCCESS) {
        return;
    }
    throw AlpacaException(context + ": " + error_code_to_string(code), map_error_code(code));
}

SVBCameraInfo convert_camera_info(const SVB_CAMERA_INFO& info, const SVB_CAMERA_PROPERTY& prop,
                                   const SVB_CAMERA_PROPERTY_EX& prop_ex) {
    SVBCameraInfo out;
    out.camera_id = info.CameraID;
    out.name = info.FriendlyName;
    out.serial_number = info.CameraSN;
    out.max_width = static_cast<int>(prop.MaxWidth);
    out.max_height = static_cast<int>(prop.MaxHeight);
    out.is_color = (prop.IsColorCam == SVB_TRUE);
    out.bayer_pattern = from_svb_bayer_pattern(prop.BayerPattern);
    out.supports_pulse_guide = (prop_ex.bSupportPulseGuide == SVB_TRUE);
    out.supports_cooler = (prop_ex.bSupportControlTemp == SVB_TRUE);
    out.bit_depth = prop.MaxBitDepth;

    out.supported_bins.clear();
    for (int i = 0; i < 16; ++i) {
        if (prop.SupportedBins[i] <= 0) {
            break;
        }
        out.supported_bins.push_back(prop.SupportedBins[i]);
    }

    out.supported_formats.clear();
    for (int i = 0; i < 8; ++i) {
        if (prop.SupportedVideoFormat[i] == SVB_IMG_END) {
            break;
        }
        out.supported_formats.push_back(from_svb_image_type(prop.SupportedVideoFormat[i]));
    }

    return out;
}

} // namespace

class SVBSDKWrapper::Impl {
public:
    std::mutex mutex_;
    struct CameraUsage {
        int open_count = 0;
    };
    std::unordered_map<int, CameraUsage> usage_;
};

SVBSDKWrapper::SVBSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

SVBSDKWrapper::~SVBSDKWrapper() = default;

SVBSDKWrapper& SVBSDKWrapper::instance() {
    static SVBSDKWrapper wrapper;
    return wrapper;
}

std::vector<SVBCameraInfo> SVBSDKWrapper::enumerate_cameras() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int count = SVBGetNumOfConnectedCameras();
    std::vector<SVBCameraInfo> result;
    if (count <= 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        SVB_CAMERA_INFO info{};
        SVB_ERROR_CODE rc = SVBGetCameraInfo(&info, i);
        if (rc != SVB_SUCCESS) {
            continue;
        }

        SVB_CAMERA_PROPERTY prop{};
        rc = SVBGetCameraProperty(info.CameraID, &prop);
        // Some models need the camera opened before SVBGetCameraProperty works;
        // populate what we can from SVBGetCameraInfo alone.
        if (rc != SVB_SUCCESS) {
            prop = {};
        }

        SVB_CAMERA_PROPERTY_EX prop_ex{};
        // GetCameraPropertyEx requires camera to be opened on some models;
        // tolerate failure and default to no pulse guide / no cooler.
        if (SVBGetCameraPropertyEx(info.CameraID, &prop_ex) != SVB_SUCCESS) {
            prop_ex = {};
        }

        auto cam_info = convert_camera_info(info, prop, prop_ex);

        // Try to get pixel size (may fail before camera is opened)
        float pixel_size = 0.0f;
        if (SVBGetSensorPixelSize(info.CameraID, &pixel_size) == SVB_SUCCESS) {
            cam_info.pixel_size_um = static_cast<double>(pixel_size);
        }

        result.push_back(std::move(cam_info));
    }
    return result;
}

bool SVBSDKWrapper::get_camera_info_by_index(int camera_index, SVBCameraInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    SVB_CAMERA_INFO sdk_info{};
    SVB_ERROR_CODE code = SVBGetCameraInfo(&sdk_info, camera_index);
    if (code != SVB_SUCCESS) {
        return false;
    }

    SVB_CAMERA_PROPERTY prop{};
    code = SVBGetCameraProperty(sdk_info.CameraID, &prop);
    if (code != SVB_SUCCESS) {
        return false;
    }

    SVB_CAMERA_PROPERTY_EX prop_ex{};
    SVBGetCameraPropertyEx(sdk_info.CameraID, &prop_ex); // tolerate failure

    info = convert_camera_info(sdk_info, prop, prop_ex);

    float pixel_size = 0.0f;
    if (SVBGetSensorPixelSize(sdk_info.CameraID, &pixel_size) == SVB_SUCCESS) {
        info.pixel_size_um = static_cast<double>(pixel_size);
    }

    return true;
}

void SVBSDKWrapper::open_camera(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[camera_id];
    if (usage.open_count == 0) {
        throw_on_error(SVBOpenCamera(camera_id), "SVBOpenCamera");
    }
    ++usage.open_count;
}

void SVBSDKWrapper::close_camera(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto it = pimpl_->usage_.find(camera_id);
    if (it == pimpl_->usage_.end() || it->second.open_count <= 0) {
        return;
    }
    --it->second.open_count;
    if (it->second.open_count == 0) {
        throw_on_error(SVBCloseCamera(camera_id), "SVBCloseCamera");
        pimpl_->usage_.erase(it);
    }
}

std::vector<SVBControlCaps> SVBSDKWrapper::get_control_caps(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int num_controls = 0;
    throw_on_error(SVBGetNumOfControls(camera_id, &num_controls), "SVBGetNumOfControls");
    std::vector<SVBControlCaps> caps;
    if (num_controls <= 0) {
        return caps;
    }
    caps.reserve(static_cast<std::size_t>(num_controls));
    for (int i = 0; i < num_controls; ++i) {
        SVB_CONTROL_CAPS sdk_caps{};
        throw_on_error(SVBGetControlCaps(camera_id, i, &sdk_caps), "SVBGetControlCaps");
        auto mapped = to_control_type(sdk_caps.ControlType);
        if (!mapped.has_value()) {
            continue;
        }
        SVBControlCaps caps_entry;
        caps_entry.type = mapped.value();
        caps_entry.name = sdk_caps.Name;
        caps_entry.description = sdk_caps.Description;
        caps_entry.max_value = sdk_caps.MaxValue;
        caps_entry.min_value = sdk_caps.MinValue;
        caps_entry.default_value = sdk_caps.DefaultValue;
        caps_entry.is_auto_supported = (sdk_caps.IsAutoSupported == SVB_TRUE);
        caps_entry.is_writable = (sdk_caps.IsWritable == SVB_TRUE);
        caps.push_back(std::move(caps_entry));
    }
    return caps;
}

bool SVBSDKWrapper::get_control_value(int camera_id, SVBControlType type, long& value, bool& is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    SVB_BOOL auto_flag = SVB_FALSE;
    SVB_ERROR_CODE code = SVBGetControlValue(camera_id, to_svb_control_type(type), &value, &auto_flag);
    if (code != SVB_SUCCESS) {
        return false;
    }
    is_auto = (auto_flag == SVB_TRUE);
    return true;
}

void SVBSDKWrapper::set_control_value(int camera_id, SVBControlType type, long value, bool is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    SVB_BOOL auto_flag = is_auto ? SVB_TRUE : SVB_FALSE;
    SVB_CONTROL_TYPE sdk_type = to_svb_control_type(type);
    // SV905C2 (and possibly other models) intermittently returns
    // SVB_ERROR_GENERAL_ERROR from SVBSetControlValue on the first attempt for
    // some controls (notably Gain), even when the value is in range. The SDK
    // documents this code as "operate to camera hardware failed" — a transient
    // USB/firmware fault. Retry a few times with a short backoff before giving
    // up so ConformU and well-behaved clients see consistent success.
    SVB_ERROR_CODE code = SVB_SUCCESS;
    for (int attempt = 0; attempt < 3; ++attempt) {
        code = SVBSetControlValue(camera_id, sdk_type, value, auto_flag);
        if (code != SVB_ERROR_GENERAL_ERROR) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw_on_error(code, "SVBSetControlValue");
}

SVBROIFormat SVBSDKWrapper::get_roi_format(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int start_x = 0;
    int start_y = 0;
    int width = 0;
    int height = 0;
    int bin = 1;
    throw_on_error(SVBGetROIFormat(camera_id, &start_x, &start_y, &width, &height, &bin), "SVBGetROIFormat");
    return SVBROIFormat{start_x, start_y, width, height, bin};
}

void SVBSDKWrapper::set_roi_format(int camera_id, int start_x, int start_y, int width, int height, int bin) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBSetROIFormat(camera_id, start_x, start_y, width, height, bin), "SVBSetROIFormat");
}

SVBImageType SVBSDKWrapper::get_output_image_type(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    SVB_IMG_TYPE type = SVB_IMG_RAW8;
    throw_on_error(SVBGetOutputImageType(camera_id, &type), "SVBGetOutputImageType");
    return from_svb_image_type(type);
}

void SVBSDKWrapper::set_output_image_type(int camera_id, SVBImageType type) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBSetOutputImageType(camera_id, to_svb_image_type(type)), "SVBSetOutputImageType");
}

void SVBSDKWrapper::start_video_capture(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBStartVideoCapture(camera_id), "SVBStartVideoCapture");
}

void SVBSDKWrapper::stop_video_capture(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBStopVideoCapture(camera_id), "SVBStopVideoCapture");
}

void SVBSDKWrapper::get_video_data(int camera_id, std::uint8_t* buffer, long buffer_size, int wait_ms) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBGetVideoData(camera_id, buffer, buffer_size, wait_ms), "SVBGetVideoData");
}

void SVBSDKWrapper::pulse_guide(int camera_id, SVBGuideDirection direction, int duration_ms) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBPulseGuide(camera_id, to_svb_guide_direction(direction), duration_ms), "SVBPulseGuide");
}

float SVBSDKWrapper::get_sensor_pixel_size(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    float pixel_size = 0.0f;
    throw_on_error(SVBGetSensorPixelSize(camera_id, &pixel_size), "SVBGetSensorPixelSize");
    return pixel_size;
}

std::string SVBSDKWrapper::get_serial_number(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    SVB_SN sn{};
    SVB_ERROR_CODE code = SVBGetSerialNumber(camera_id, &sn);
    if (code != SVB_SUCCESS) {
        return "";
    }
    std::ostringstream oss;
    for (unsigned char byte : sn.id) {
        if (byte == 0) {
            break;
        }
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::string SVBSDKWrapper::get_sdk_version() {
    const char* version = SVBGetSDKVersion();
    if (!version) {
        return "unknown";
    }
    return version;
}

std::string SVBSDKWrapper::get_firmware_version(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    char version[64] = {};
    SVB_ERROR_CODE code = SVBGetCameraFirmwareVersion(camera_id, version);
    if (code != SVB_SUCCESS) {
        return "";
    }
    return version;
}

void SVBSDKWrapper::set_camera_mode_normal(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBSetCameraMode(camera_id, SVB_MODE_NORMAL), "SVBSetCameraMode");
}

void SVBSDKWrapper::set_auto_save_param(int camera_id, bool enable) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBSetAutoSaveParam(camera_id, enable ? SVB_TRUE : SVB_FALSE), "SVBSetAutoSaveParam");
}

void SVBSDKWrapper::restore_default_param(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(SVBRestoreDefaultParam(camera_id), "SVBRestoreDefaultParam");
}

} // namespace alpacacore::vendor::svbony
