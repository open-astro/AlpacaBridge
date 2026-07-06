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

#include <alpacacore/vendor/zwo/zwo_efw_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <EFW_filter.h>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace alpacacore::vendor::zwo {

namespace {

std::string error_code_to_string(EFW_ERROR_CODE code) {
    switch (code) {
    case EFW_SUCCESS:
        return "EFW_SUCCESS";
    case EFW_ERROR_INVALID_INDEX:
        return "EFW_ERROR_INVALID_INDEX";
    case EFW_ERROR_INVALID_ID:
        return "EFW_ERROR_INVALID_ID";
    case EFW_ERROR_INVALID_VALUE:
        return "EFW_ERROR_INVALID_VALUE";
    case EFW_ERROR_REMOVED:
        return "EFW_ERROR_REMOVED";
    case EFW_ERROR_MOVING:
        return "EFW_ERROR_MOVING";
    case EFW_ERROR_ERROR_STATE:
        return "EFW_ERROR_ERROR_STATE";
    case EFW_ERROR_GENERAL_ERROR:
        return "EFW_ERROR_GENERAL_ERROR";
    case EFW_ERROR_NOT_SUPPORTED:
        return "EFW_ERROR_NOT_SUPPORTED";
    case EFW_ERROR_INVALID_LENGTH:
        return "EFW_ERROR_INVALID_LENGTH";
    case EFW_ERROR_CLOSED:
        return "EFW_ERROR_CLOSED";
    default:
        return "EFW_ERROR_UNKNOWN";
    }
}

int map_error_code(EFW_ERROR_CODE code) {
    switch (code) {
    case EFW_ERROR_INVALID_INDEX:
    case EFW_ERROR_INVALID_ID:
    case EFW_ERROR_INVALID_VALUE:
    case EFW_ERROR_INVALID_LENGTH:
        return AlpacaError::InvalidValue;
    case EFW_ERROR_CLOSED:
    case EFW_ERROR_REMOVED:
        return AlpacaError::NotConnected;
    case EFW_ERROR_MOVING:
        return AlpacaError::InvalidOperation;
    case EFW_ERROR_NOT_SUPPORTED:
        return AlpacaError::NotImplemented;
    default:
        return AlpacaError::DriverException;
    }
}

void throw_on_error(EFW_ERROR_CODE code, const std::string& context) {
    if (code == EFW_SUCCESS) {
        return;
    }
    throw AlpacaException(context + ": " + error_code_to_string(code), map_error_code(code));
}

ZWOEFWInfo convert_wheel_info(const EFW_INFO& info) {
    ZWOEFWInfo out;
    out.wheel_id = info.ID;
    out.name = info.Name;
    out.slot_count = info.slotNum;
    return out;
}

} // namespace

class ZWOEFWSDKWrapper::Impl {
public:
    std::mutex mutex_;
    struct WheelUsage {
        int open_count = 0;
    };
    std::unordered_map<int, WheelUsage> usage_;
};

ZWOEFWSDKWrapper::ZWOEFWSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ZWOEFWSDKWrapper::~ZWOEFWSDKWrapper() = default;

ZWOEFWSDKWrapper& ZWOEFWSDKWrapper::instance() {
    static ZWOEFWSDKWrapper wrapper;
    return wrapper;
}

std::vector<ZWOEFWInfo> ZWOEFWSDKWrapper::enumerate_wheels() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int count = EFWGetNum();
    std::vector<ZWOEFWInfo> result;
    if (count <= 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        int id = 0;
        throw_on_error(EFWGetID(i, &id), "EFWGetID");
        EFW_INFO info{};
        EFW_ERROR_CODE code = EFWGetProperty(id, &info);
        if (code == EFW_SUCCESS) {
            result.push_back(convert_wheel_info(info));
        } else {
            ZWOEFWInfo fallback;
            fallback.wheel_id = id;
            fallback.slot_count = 0;
            result.push_back(std::move(fallback));
        }
    }
    return result;
}

bool ZWOEFWSDKWrapper::get_wheel_info_by_id(int wheel_id, ZWOEFWInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    EFW_INFO sdk_info{};
    EFW_ERROR_CODE code = EFWGetProperty(wheel_id, &sdk_info);
    if (code != EFW_SUCCESS) {
        return false;
    }
    info = convert_wheel_info(sdk_info);
    return true;
}

bool ZWOEFWSDKWrapper::get_wheel_info_by_index(int wheel_index, ZWOEFWInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int wheel_id = 0;
    EFW_ERROR_CODE code = EFWGetID(wheel_index, &wheel_id);
    if (code != EFW_SUCCESS) {
        return false;
    }
    EFW_INFO sdk_info{};
    code = EFWGetProperty(wheel_id, &sdk_info);
    if (code != EFW_SUCCESS) {
        info = ZWOEFWInfo{wheel_id, std::string(), 0};
        return true;
    }
    info = convert_wheel_info(sdk_info);
    return true;
}

void ZWOEFWSDKWrapper::open_wheel(int wheel_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[wheel_id];
    if (usage.open_count == 0) {
        try {
            throw_on_error(EFWOpen(wheel_id), "EFWOpen");
        } catch (...) {
            // Nothing was opened — drop the zero-count entry inserted by
            // operator[] above rather than leaving a stale map node.
            pimpl_->usage_.erase(wheel_id);
            throw;
        }
    }
    ++usage.open_count;
}

void ZWOEFWSDKWrapper::close_wheel(int wheel_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto it = pimpl_->usage_.find(wheel_id);
    if (it == pimpl_->usage_.end() || it->second.open_count <= 0) {
        return;
    }
    --it->second.open_count;
    if (it->second.open_count == 0) {
        // Erase the bookkeeping first: a failing SDK close (e.g. device
        // unplugged) must not leave a zero-count entry that turns every
        // later close into a no-op and leaks the handle. This assumes an
        // SDK close error means the handle is unusable on the SDK side —
        // a later open performs a fresh SDK open instead of reusing
        // half-closed state.
        pimpl_->usage_.erase(it);
        throw_on_error(EFWClose(wheel_id), "EFWClose");
    }
}

int ZWOEFWSDKWrapper::get_position(int wheel_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int position = 0;
    throw_on_error(EFWGetPosition(wheel_id, &position), "EFWGetPosition");
    return position;
}

void ZWOEFWSDKWrapper::set_position(int wheel_id, int position) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(EFWSetPosition(wheel_id, position), "EFWSetPosition");
}

void ZWOEFWSDKWrapper::set_unidirectional(int wheel_id, bool enabled) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(EFWSetDirection(wheel_id, enabled), "EFWSetDirection");
}

bool ZWOEFWSDKWrapper::get_unidirectional(int wheel_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    bool enabled = false;
    throw_on_error(EFWGetDirection(wheel_id, &enabled), "EFWGetDirection");
    return enabled;
}

void ZWOEFWSDKWrapper::calibrate(int wheel_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(EFWCalibrate(wheel_id), "EFWCalibrate");
}

int ZWOEFWSDKWrapper::get_hardware_error_code(int wheel_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int error_code = 0;
    throw_on_error(EFWGetHWErrorCode(wheel_id, &error_code), "EFWGetHWErrorCode");
    return error_code;
}

std::string ZWOEFWSDKWrapper::get_serial_number(int wheel_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    EFW_SN sn{};
    throw_on_error(EFWGetSerialNumber(wheel_id, &sn), "EFWGetSerialNumber");
    std::ostringstream oss;
    for (unsigned char byte : sn.id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::string ZWOEFWSDKWrapper::get_firmware_version(int wheel_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    unsigned char major = 0;
    unsigned char minor = 0;
    unsigned char build = 0;
    throw_on_error(EFWGetFirmwareVersion(wheel_id, &major, &minor, &build), "EFWGetFirmwareVersion");
    std::ostringstream oss;
    oss << static_cast<int>(major) << "." << static_cast<int>(minor) << "." << static_cast<int>(build);
    return oss.str();
}

std::string ZWOEFWSDKWrapper::get_sdk_version() {
    const char* version = EFWGetSDKVersion();
    if (!version) {
        return "unknown";
    }
    return version;
}

} // namespace alpacacore::vendor::zwo
