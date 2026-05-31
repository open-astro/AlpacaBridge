// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacacore/vendor/zwo/zwo_eaf_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <EAF_focuser.h>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <unordered_map>

namespace alpacacore::vendor::zwo {

namespace {

std::string error_code_to_string(EAF_ERROR_CODE code) {
    switch (code) {
    case EAF_SUCCESS:
        return "EAF_SUCCESS";
    case EAF_ERROR_INVALID_INDEX:
        return "EAF_ERROR_INVALID_INDEX";
    case EAF_ERROR_INVALID_ID:
        return "EAF_ERROR_INVALID_ID";
    case EAF_ERROR_INVALID_VALUE:
        return "EAF_ERROR_INVALID_VALUE";
    case EAF_ERROR_REMOVED:
        return "EAF_ERROR_REMOVED";
    case EAF_ERROR_MOVING:
        return "EAF_ERROR_MOVING";
    case EAF_ERROR_ERROR_STATE:
        return "EAF_ERROR_ERROR_STATE";
    case EAF_ERROR_GENERAL_ERROR:
        return "EAF_ERROR_GENERAL_ERROR";
    case EAF_ERROR_NOT_SUPPORTED:
        return "EAF_ERROR_NOT_SUPPORTED";
    case EAF_ERROR_CLOSED:
        return "EAF_ERROR_CLOSED";
    case EAF_ERROR_BATTER_INFO:
        return "EAF_ERROR_BATTER_INFO";
    case EAF_ERROR_INVALID_LENGTH:
        return "EAF_ERROR_INVALID_LENGTH";
    case EAF_BLE_READ_DATA_FAILED:
        return "EAF_BLE_READ_DATA_FAILED";
    case EAF_BLE_SEND_DATA_FAILED:
        return "EAF_BLE_SEND_DATA_FAILED";
    case EAF_BLE_CONNECT_FAILED:
        return "EAF_BLE_CONNECT_FAILED";
    case EAF_BLE_DISCONNECT:
        return "EAF_BLE_DISCONNECT";
    case EAF_BLE_PAIR_FAILED:
        return "EAF_BLE_PAIR_FAILED";
    case EAF_BLE_CLEAR_PAIR_FAILED:
        return "EAF_BLE_CLEAR_PAIR_FAILED";
    case EAF_BLE_PAIRING_TIMEOUT:
        return "EAF_BLE_PAIRING_TIMEOUT";
    case EAF_BLE_RECEIVE_TIMEOUT:
        return "EAF_BLE_RECEIVE_TIMEOUT";
    case EAF_BLE_DEVICE_NOT_EXISTS:
        return "EAF_BLE_DEVICE_NOT_EXISTS";
    case EAF_BLE_INVALID_CALLBACK:
        return "EAF_BLE_INVALID_CALLBACK";
    case EAF_BLE_NEW_PAIR_REQUEST:
        return "EAF_BLE_NEW_PAIR_REQUEST";
    case EAF_BLE_DATA_BUSY:
        return "EAF_BLE_DATA_BUSY";
    case EAF_BLE_CHECK_SIZE_FAILED:
        return "EAF_BLE_CHECK_SIZE_FAILED";
    default:
        return "EAF_ERROR_UNKNOWN";
    }
}

int map_error_code(EAF_ERROR_CODE code) {
    switch (code) {
    case EAF_ERROR_INVALID_INDEX:
    case EAF_ERROR_INVALID_ID:
    case EAF_ERROR_INVALID_VALUE:
    case EAF_ERROR_INVALID_LENGTH:
        return AlpacaError::InvalidValue;
    case EAF_ERROR_CLOSED:
    case EAF_ERROR_REMOVED:
    case EAF_BLE_DISCONNECT:
        return AlpacaError::NotConnected;
    case EAF_ERROR_MOVING:
        return AlpacaError::InvalidOperation;
    case EAF_ERROR_NOT_SUPPORTED:
        return AlpacaError::NotImplemented;
    default:
        return AlpacaError::DriverException;
    }
}

void throw_on_error(EAF_ERROR_CODE code, const std::string& context) {
    if (code == EAF_SUCCESS) {
        return;
    }
    throw AlpacaException(context + ": " + error_code_to_string(code), map_error_code(code));
}

ZWOEAFFocuserInfo convert_focuser_info(const EAF_INFO& info) {
    ZWOEAFFocuserInfo out;
    out.focuser_id = info.ID;
    out.name = info.Name;
    out.max_step = info.MaxStep;
    return out;
}

} // namespace

class ZWOEAFSDKWrapper::Impl {
public:
    std::mutex mutex_;
    struct FocuserUsage {
        int open_count = 0;
    };
    std::unordered_map<int, FocuserUsage> usage_;
};

ZWOEAFSDKWrapper::ZWOEAFSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ZWOEAFSDKWrapper::~ZWOEAFSDKWrapper() = default;

ZWOEAFSDKWrapper& ZWOEAFSDKWrapper::instance() {
    static ZWOEAFSDKWrapper wrapper;
    return wrapper;
}

std::vector<ZWOEAFFocuserInfo> ZWOEAFSDKWrapper::enumerate_focusers() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int count = EAFGetNum();
    std::vector<ZWOEAFFocuserInfo> result;
    if (count <= 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        int id = 0;
        throw_on_error(EAFGetID(i, &id), "EAFGetID");
        EAF_INFO info{};
        EAF_ERROR_CODE code = EAFGetProperty(id, &info);
        if (code == EAF_SUCCESS) {
            result.push_back(convert_focuser_info(info));
        } else {
            ZWOEAFFocuserInfo fallback;
            fallback.focuser_id = id;
            fallback.max_step = 0;
            result.push_back(std::move(fallback));
        }
    }
    return result;
}

bool ZWOEAFSDKWrapper::get_focuser_info_by_id(int focuser_id, ZWOEAFFocuserInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    EAF_INFO sdk_info{};
    EAF_ERROR_CODE code = EAFGetProperty(focuser_id, &sdk_info);
    if (code != EAF_SUCCESS) {
        return false;
    }
    info = convert_focuser_info(sdk_info);
    return true;
}

bool ZWOEAFSDKWrapper::get_focuser_info_by_index(int focuser_index, ZWOEAFFocuserInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int focuser_id = 0;
    EAF_ERROR_CODE code = EAFGetID(focuser_index, &focuser_id);
    if (code != EAF_SUCCESS) {
        return false;
    }
    EAF_INFO sdk_info{};
    code = EAFGetProperty(focuser_id, &sdk_info);
    if (code != EAF_SUCCESS) {
        info = ZWOEAFFocuserInfo{focuser_id, std::string(), 0};
        return true;
    }
    info = convert_focuser_info(sdk_info);
    return true;
}

void ZWOEAFSDKWrapper::open_focuser(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[focuser_id];
    if (usage.open_count == 0) {
        throw_on_error(EAFOpen(focuser_id), "EAFOpen");
    }
    ++usage.open_count;
}

void ZWOEAFSDKWrapper::close_focuser(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto it = pimpl_->usage_.find(focuser_id);
    if (it == pimpl_->usage_.end() || it->second.open_count <= 0) {
        return;
    }
    --it->second.open_count;
    if (it->second.open_count == 0) {
        throw_on_error(EAFClose(focuser_id), "EAFClose");
        pimpl_->usage_.erase(it);
    }
}

bool ZWOEAFSDKWrapper::is_moving(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    bool moving = false;
    bool hand_control = false;
    throw_on_error(EAFIsMoving(focuser_id, &moving, &hand_control), "EAFIsMoving");
    return moving;
}

int ZWOEAFSDKWrapper::get_position(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int position = 0;
    throw_on_error(EAFGetPosition(focuser_id, &position), "EAFGetPosition");
    return position;
}

void ZWOEAFSDKWrapper::move(int focuser_id, int position) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(EAFMove(focuser_id, position), "EAFMove");
}

void ZWOEAFSDKWrapper::stop(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(EAFStop(focuser_id), "EAFStop");
}

int ZWOEAFSDKWrapper::get_max_step(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int max_step = 0;
    throw_on_error(EAFGetMaxStep(focuser_id, &max_step), "EAFGetMaxStep");
    return max_step;
}

int ZWOEAFSDKWrapper::get_step_range(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int range = 0;
    throw_on_error(EAFStepRange(focuser_id, &range), "EAFStepRange");
    return range;
}

double ZWOEAFSDKWrapper::get_temperature(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    float temp = 0.0f;
    throw_on_error(EAFGetTemp(focuser_id, &temp), "EAFGetTemp");
    return static_cast<double>(temp);
}

std::string ZWOEAFSDKWrapper::get_serial_number(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    EAF_SN sn{};
    throw_on_error(EAFGetSerialNumber(focuser_id, &sn), "EAFGetSerialNumber");
    std::ostringstream oss;
    for (unsigned char byte : sn.id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::string ZWOEAFSDKWrapper::get_firmware_version(int focuser_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    unsigned char major = 0;
    unsigned char minor = 0;
    unsigned char build = 0;
    throw_on_error(EAFGetFirmwareVersion(focuser_id, &major, &minor, &build), "EAFGetFirmwareVersion");
    std::ostringstream oss;
    oss << static_cast<int>(major) << "." << static_cast<int>(minor) << "." << static_cast<int>(build);
    return oss.str();
}

std::string ZWOEAFSDKWrapper::get_sdk_version() {
    const char* version = EAFGetSDKVersion();
    if (!version) {
        return "unknown";
    }
    return version;
}

} // namespace alpacacore::vendor::zwo
