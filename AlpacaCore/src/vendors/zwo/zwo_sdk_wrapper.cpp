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

#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <ASICamera2.h>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <unordered_map>

namespace alpacacore::vendor::zwo {

namespace {

ZWOImageType from_asi_image_type(ASI_IMG_TYPE type) {
    switch (type) {
    case ASI_IMG_RAW8:
        return ZWOImageType::Raw8;
    case ASI_IMG_RGB24:
        return ZWOImageType::Rgb24;
    case ASI_IMG_RAW16:
        return ZWOImageType::Raw16;
    case ASI_IMG_Y8:
        return ZWOImageType::Y8;
    default:
        return ZWOImageType::Raw8;
    }
}

ASI_IMG_TYPE to_asi_image_type(ZWOImageType type) {
    switch (type) {
    case ZWOImageType::Raw8:
        return ASI_IMG_RAW8;
    case ZWOImageType::Rgb24:
        return ASI_IMG_RGB24;
    case ZWOImageType::Raw16:
        return ASI_IMG_RAW16;
    case ZWOImageType::Y8:
        return ASI_IMG_Y8;
    }
    return ASI_IMG_RAW8;
}

ZWOBayerPattern from_asi_bayer_pattern(ASI_BAYER_PATTERN pattern) {
    switch (pattern) {
    case ASI_BAYER_RG:
        return ZWOBayerPattern::RG;
    case ASI_BAYER_BG:
        return ZWOBayerPattern::BG;
    case ASI_BAYER_GR:
        return ZWOBayerPattern::GR;
    case ASI_BAYER_GB:
        return ZWOBayerPattern::GB;
    default:
        return ZWOBayerPattern::None;
    }
}

ZWOExposureStatus from_asi_exposure_status(ASI_EXPOSURE_STATUS status) {
    switch (status) {
    case ASI_EXP_IDLE:
        return ZWOExposureStatus::Idle;
    case ASI_EXP_WORKING:
        return ZWOExposureStatus::Working;
    case ASI_EXP_SUCCESS:
        return ZWOExposureStatus::Success;
    case ASI_EXP_FAILED:
        return ZWOExposureStatus::Failed;
    default:
        return ZWOExposureStatus::Failed;
    }
}

ASI_GUIDE_DIRECTION to_asi_guide_direction(ZWOGuideDirection direction) {
    switch (direction) {
    case ZWOGuideDirection::North:
        return ASI_GUIDE_NORTH;
    case ZWOGuideDirection::South:
        return ASI_GUIDE_SOUTH;
    case ZWOGuideDirection::East:
        return ASI_GUIDE_EAST;
    case ZWOGuideDirection::West:
        return ASI_GUIDE_WEST;
    }
    return ASI_GUIDE_NORTH;
}

std::optional<ZWOControlType> to_control_type(ASI_CONTROL_TYPE type) {
    switch (type) {
    case ASI_GAIN:
        return ZWOControlType::Gain;
    case ASI_EXPOSURE:
        return ZWOControlType::Exposure;
    case ASI_OFFSET:
        return ZWOControlType::Offset;
    case ASI_TEMPERATURE:
        return ZWOControlType::Temperature;
    case ASI_COOLER_ON:
        return ZWOControlType::CoolerOn;
    case ASI_COOLER_POWER_PERC:
        return ZWOControlType::CoolerPower;
    case ASI_TARGET_TEMP:
        return ZWOControlType::TargetTemperature;
    case ASI_HIGH_SPEED_MODE:
        return ZWOControlType::HighSpeedMode;
    case ASI_ANTI_DEW_HEATER:
        return ZWOControlType::AntiDewHeater;
    default:
        return std::nullopt;
    }
}

ASI_CONTROL_TYPE to_asi_control_type(ZWOControlType type) {
    switch (type) {
    case ZWOControlType::Gain:
        return ASI_GAIN;
    case ZWOControlType::Exposure:
        return ASI_EXPOSURE;
    case ZWOControlType::Offset:
        return ASI_OFFSET;
    case ZWOControlType::Temperature:
        return ASI_TEMPERATURE;
    case ZWOControlType::CoolerOn:
        return ASI_COOLER_ON;
    case ZWOControlType::CoolerPower:
        return ASI_COOLER_POWER_PERC;
    case ZWOControlType::TargetTemperature:
        return ASI_TARGET_TEMP;
    case ZWOControlType::HighSpeedMode:
        return ASI_HIGH_SPEED_MODE;
    case ZWOControlType::AntiDewHeater:
        return ASI_ANTI_DEW_HEATER;
    }
    return ASI_GAIN;
}

std::string error_code_to_string(ASI_ERROR_CODE code) {
    switch (code) {
    case ASI_SUCCESS:
        return "ASI_SUCCESS";
    case ASI_ERROR_INVALID_INDEX:
        return "ASI_ERROR_INVALID_INDEX";
    case ASI_ERROR_INVALID_ID:
        return "ASI_ERROR_INVALID_ID";
    case ASI_ERROR_INVALID_CONTROL_TYPE:
        return "ASI_ERROR_INVALID_CONTROL_TYPE";
    case ASI_ERROR_CAMERA_CLOSED:
        return "ASI_ERROR_CAMERA_CLOSED";
    case ASI_ERROR_CAMERA_REMOVED:
        return "ASI_ERROR_CAMERA_REMOVED";
    case ASI_ERROR_INVALID_PATH:
        return "ASI_ERROR_INVALID_PATH";
    case ASI_ERROR_INVALID_FILEFORMAT:
        return "ASI_ERROR_INVALID_FILEFORMAT";
    case ASI_ERROR_INVALID_SIZE:
        return "ASI_ERROR_INVALID_SIZE";
    case ASI_ERROR_INVALID_IMGTYPE:
        return "ASI_ERROR_INVALID_IMGTYPE";
    case ASI_ERROR_OUTOF_BOUNDARY:
        return "ASI_ERROR_OUTOF_BOUNDARY";
    case ASI_ERROR_TIMEOUT:
        return "ASI_ERROR_TIMEOUT";
    case ASI_ERROR_INVALID_SEQUENCE:
        return "ASI_ERROR_INVALID_SEQUENCE";
    case ASI_ERROR_BUFFER_TOO_SMALL:
        return "ASI_ERROR_BUFFER_TOO_SMALL";
    case ASI_ERROR_VIDEO_MODE_ACTIVE:
        return "ASI_ERROR_VIDEO_MODE_ACTIVE";
    case ASI_ERROR_EXPOSURE_IN_PROGRESS:
        return "ASI_ERROR_EXPOSURE_IN_PROGRESS";
    case ASI_ERROR_GENERAL_ERROR:
        return "ASI_ERROR_GENERAL_ERROR";
    case ASI_ERROR_INVALID_MODE:
        return "ASI_ERROR_INVALID_MODE";
    case ASI_ERROR_GPS_NOT_SUPPORTED:
        return "ASI_ERROR_GPS_NOT_SUPPORTED";
    case ASI_ERROR_GPS_VER_ERR:
        return "ASI_ERROR_GPS_VER_ERR";
    case ASI_ERROR_GPS_FPGA_ERR:
        return "ASI_ERROR_GPS_FPGA_ERR";
    case ASI_ERROR_GPS_PARAM_OUT_OF_RANGE:
        return "ASI_ERROR_GPS_PARAM_OUT_OF_RANGE";
    case ASI_ERROR_GPS_DATA_INVALID:
        return "ASI_ERROR_GPS_DATA_INVALID";
    default:
        return "ASI_ERROR_UNKNOWN";
    }
}

int map_error_code(ASI_ERROR_CODE code) {
    switch (code) {
    case ASI_ERROR_INVALID_INDEX:
    case ASI_ERROR_INVALID_ID:
    case ASI_ERROR_INVALID_CONTROL_TYPE:
    case ASI_ERROR_INVALID_SIZE:
    case ASI_ERROR_INVALID_IMGTYPE:
    case ASI_ERROR_OUTOF_BOUNDARY:
    case ASI_ERROR_INVALID_MODE:
        return AlpacaError::InvalidValue;
    case ASI_ERROR_CAMERA_CLOSED:
    case ASI_ERROR_CAMERA_REMOVED:
        return AlpacaError::NotConnected;
    case ASI_ERROR_EXPOSURE_IN_PROGRESS:
    case ASI_ERROR_VIDEO_MODE_ACTIVE:
        return AlpacaError::InvalidOperation;
    default:
        return AlpacaError::DriverException;
    }
}

void throw_on_error(ASI_ERROR_CODE code, const std::string& context) {
    if (code == ASI_SUCCESS) {
        return;
    }
    throw AlpacaException(context + ": " + error_code_to_string(code), map_error_code(code));
}

ZWOCameraInfo convert_camera_info(const ASI_CAMERA_INFO& info) {
    ZWOCameraInfo out;
    out.camera_id = info.CameraID;
    out.name = info.Name;
    out.max_width = static_cast<int>(info.MaxWidth);
    out.max_height = static_cast<int>(info.MaxHeight);
    out.is_color = (info.IsColorCam == ASI_TRUE);
    out.bayer_pattern = from_asi_bayer_pattern(info.BayerPattern);
    out.pixel_size_um = info.PixelSize;
    out.has_shutter = (info.MechanicalShutter == ASI_TRUE);
    out.has_st4_port = (info.ST4Port == ASI_TRUE);
    out.has_cooler = (info.IsCoolerCam == ASI_TRUE);
    out.electrons_per_adu = info.ElecPerADU;
    out.bit_depth = info.BitDepth;

    out.supported_bins.clear();
    for (int i = 0; i < 16; ++i) {
        if (info.SupportedBins[i] <= 0) {
            break;
        }
        out.supported_bins.push_back(info.SupportedBins[i]);
    }

    out.supported_formats.clear();
    for (int i = 0; i < 8; ++i) {
        if (info.SupportedVideoFormat[i] == ASI_IMG_END) {
            break;
        }
        out.supported_formats.push_back(from_asi_image_type(info.SupportedVideoFormat[i]));
    }

    return out;
}

} // namespace

class ZWOSDKWrapper::Impl {
public:
    std::mutex mutex_;
    struct CameraUsage {
        int open_count = 0;
        bool initialized = false;
    };
    std::unordered_map<int, CameraUsage> usage_;
};

ZWOSDKWrapper::ZWOSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ZWOSDKWrapper::~ZWOSDKWrapper() = default;

ZWOSDKWrapper& ZWOSDKWrapper::instance() {
    static ZWOSDKWrapper wrapper;
    return wrapper;
}

std::vector<ZWOCameraInfo> ZWOSDKWrapper::enumerate_cameras() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int count = ASIGetNumOfConnectedCameras();
    std::vector<ZWOCameraInfo> result;
    if (count <= 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        ASI_CAMERA_INFO info{};
        throw_on_error(ASIGetCameraProperty(&info, i), "ASIGetCameraProperty");
        result.push_back(convert_camera_info(info));
    }
    return result;
}

bool ZWOSDKWrapper::get_camera_info_by_id(int camera_id, ZWOCameraInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ASI_CAMERA_INFO sdk_info{};
    ASI_ERROR_CODE code = ASIGetCameraPropertyByID(camera_id, &sdk_info);
    if (code != ASI_SUCCESS) {
        return false;
    }
    info = convert_camera_info(sdk_info);
    return true;
}

bool ZWOSDKWrapper::get_camera_info_by_index(int camera_index, ZWOCameraInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ASI_CAMERA_INFO sdk_info{};
    ASI_ERROR_CODE code = ASIGetCameraProperty(&sdk_info, camera_index);
    if (code != ASI_SUCCESS) {
        return false;
    }
    info = convert_camera_info(sdk_info);
    return true;
}

void ZWOSDKWrapper::open_camera(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[camera_id];
    if (usage.open_count == 0) {
        throw_on_error(ASIOpenCamera(camera_id), "ASIOpenCamera");
    }
    ++usage.open_count;
}

void ZWOSDKWrapper::init_camera(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[camera_id];
    if (!usage.initialized) {
        throw_on_error(ASIInitCamera(camera_id), "ASIInitCamera");
        usage.initialized = true;
    }
}

void ZWOSDKWrapper::close_camera(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto it = pimpl_->usage_.find(camera_id);
    if (it == pimpl_->usage_.end() || it->second.open_count <= 0) {
        return;
    }
    --it->second.open_count;
    if (it->second.open_count == 0) {
        throw_on_error(ASICloseCamera(camera_id), "ASICloseCamera");
        it->second.initialized = false;
        pimpl_->usage_.erase(it);
    }
}

std::vector<ZWOControlCaps> ZWOSDKWrapper::get_control_caps(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int num_controls = 0;
    throw_on_error(ASIGetNumOfControls(camera_id, &num_controls), "ASIGetNumOfControls");
    std::vector<ZWOControlCaps> caps;
    if (num_controls <= 0) {
        return caps;
    }
    caps.reserve(static_cast<std::size_t>(num_controls));
    for (int i = 0; i < num_controls; ++i) {
        ASI_CONTROL_CAPS sdk_caps{};
        throw_on_error(ASIGetControlCaps(camera_id, i, &sdk_caps), "ASIGetControlCaps");
        auto mapped = to_control_type(sdk_caps.ControlType);
        if (!mapped.has_value()) {
            continue;
        }
        ZWOControlCaps caps_entry;
        caps_entry.type = mapped.value();
        caps_entry.name = sdk_caps.Name;
        caps_entry.description = sdk_caps.Description;
        caps_entry.max_value = sdk_caps.MaxValue;
        caps_entry.min_value = sdk_caps.MinValue;
        caps_entry.default_value = sdk_caps.DefaultValue;
        caps_entry.is_auto_supported = (sdk_caps.IsAutoSupported == ASI_TRUE);
        caps_entry.is_writable = (sdk_caps.IsWritable == ASI_TRUE);
        caps.push_back(std::move(caps_entry));
    }
    return caps;
}

bool ZWOSDKWrapper::get_control_value(int camera_id, ZWOControlType type, long& value, bool& is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ASI_BOOL auto_flag = ASI_FALSE;
    ASI_ERROR_CODE code = ASIGetControlValue(camera_id, to_asi_control_type(type), &value, &auto_flag);
    if (code != ASI_SUCCESS) {
        return false;
    }
    is_auto = (auto_flag == ASI_TRUE);
    return true;
}

void ZWOSDKWrapper::set_control_value(int camera_id, ZWOControlType type, long value, bool is_auto) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ASI_BOOL auto_flag = is_auto ? ASI_TRUE : ASI_FALSE;
    throw_on_error(ASISetControlValue(camera_id, to_asi_control_type(type), value, auto_flag), "ASISetControlValue");
}

ZWOROIFormat ZWOSDKWrapper::get_roi_format(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int width = 0;
    int height = 0;
    int bin = 1;
    ASI_IMG_TYPE img_type = ASI_IMG_RAW8;
    throw_on_error(ASIGetROIFormat(camera_id, &width, &height, &bin, &img_type), "ASIGetROIFormat");
    ZWOROIFormat format;
    format.width = width;
    format.height = height;
    format.bin = bin;
    format.image_type = from_asi_image_type(img_type);
    return format;
}

void ZWOSDKWrapper::set_roi_format(int camera_id, int width, int height, int bin, ZWOImageType type) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(ASISetROIFormat(camera_id, width, height, bin, to_asi_image_type(type)), "ASISetROIFormat");
}

ZWOStartPos ZWOSDKWrapper::get_start_pos(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int start_x = 0;
    int start_y = 0;
    throw_on_error(ASIGetStartPos(camera_id, &start_x, &start_y), "ASIGetStartPos");
    return ZWOStartPos{start_x, start_y};
}

void ZWOSDKWrapper::set_start_pos(int camera_id, int start_x, int start_y) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(ASISetStartPos(camera_id, start_x, start_y), "ASISetStartPos");
}

void ZWOSDKWrapper::start_video_capture(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(ASIStartVideoCapture(camera_id), "ASIStartVideoCapture");
}

void ZWOSDKWrapper::stop_video_capture(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(ASIStopVideoCapture(camera_id), "ASIStopVideoCapture");
}

bool ZWOSDKWrapper::get_video_data(int camera_id, std::uint8_t* buffer, long buffer_size, int wait_ms) {
    // Do NOT hold the wrapper mutex — ASIGetVideoData blocks up to wait_ms
    // waiting for the next frame, at 100+ fps this is the hot path, and
    // holding the global lock across it would stall every other SDK call.
    // Same exemption (and same racing-disconnect worst case) as
    // get_data_after_exposure above.
    const ASI_ERROR_CODE code = ASIGetVideoData(camera_id, buffer, buffer_size, wait_ms);
    if (code == ASI_ERROR_TIMEOUT) {
        return false;
    }
    throw_on_error(code, "ASIGetVideoData");
    return true;
}

int ZWOSDKWrapper::get_dropped_frames(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int dropped = 0;
    throw_on_error(ASIGetDroppedFrames(camera_id, &dropped), "ASIGetDroppedFrames");
    return dropped;
}

void ZWOSDKWrapper::start_exposure(int camera_id, bool is_dark) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ASI_BOOL dark_flag = is_dark ? ASI_TRUE : ASI_FALSE;
    throw_on_error(ASIStartExposure(camera_id, dark_flag), "ASIStartExposure");
}

void ZWOSDKWrapper::stop_exposure(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(ASIStopExposure(camera_id), "ASIStopExposure");
}

ZWOExposureStatus ZWOSDKWrapper::get_exposure_status(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ASI_EXPOSURE_STATUS status = ASI_EXP_IDLE;
    throw_on_error(ASIGetExpStatus(camera_id, &status), "ASIGetExpStatus");
    return from_asi_exposure_status(status);
}

void ZWOSDKWrapper::get_data_after_exposure(int camera_id, std::uint8_t* buffer, long buffer_size) {
    // Do NOT hold the wrapper mutex — ASIGetDataAfterExp is a blocking USB
    // bulk download that can take seconds on large sensors, and holding the
    // global lock across it would stall every other SDK call (stop_exposure,
    // status polls, other cameras). Same exemption as the Player One /
    // ToupTek blocking image waits: the driver publishes disconnected and
    // stops the exposure before closing, so the worst case for a racing
    // disconnect is an SDK error on a closed id — not a hang.
    throw_on_error(ASIGetDataAfterExp(camera_id, buffer, buffer_size), "ASIGetDataAfterExp");
}

void ZWOSDKWrapper::pulse_guide_on(int camera_id, ZWOGuideDirection direction) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(ASIPulseGuideOn(camera_id, to_asi_guide_direction(direction)), "ASIPulseGuideOn");
}

void ZWOSDKWrapper::pulse_guide_off(int camera_id, ZWOGuideDirection direction) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(ASIPulseGuideOff(camera_id, to_asi_guide_direction(direction)), "ASIPulseGuideOff");
}

std::string ZWOSDKWrapper::get_serial_number(int camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ASI_SN sn{};
    throw_on_error(ASIGetSerialNumber(camera_id, &sn), "ASIGetSerialNumber");
    std::ostringstream oss;
    for (unsigned char byte : sn.id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::string ZWOSDKWrapper::get_sdk_version() {
    const char* version = ASIGetSDKVersion();
    if (!version) {
        return "unknown";
    }
    return version;
}

} // namespace alpacacore::vendor::zwo
