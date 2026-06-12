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
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/vendor/zwo/zwo_caa_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <CAA_API.h>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace alpacacore::vendor::zwo {

namespace {

std::string error_code_to_string(CAA_ERROR_CODE code) {
    switch (code) {
    case CAA_SUCCESS:
        return "CAA_SUCCESS";
    case CAA_ERROR_INVALID_INDEX:
        return "CAA_ERROR_INVALID_INDEX";
    case CAA_ERROR_INVALID_ID:
        return "CAA_ERROR_INVALID_ID";
    case CAA_ERROR_INVALID_VALUE:
        return "CAA_ERROR_INVALID_VALUE";
    case CAA_ERROR_REMOVED:
        return "CAA_ERROR_REMOVED";
    case CAA_ERROR_MOVING:
        return "CAA_ERROR_MOVING";
    case CAA_ERROR_ERROR_STATE:
        return "CAA_ERROR_ERROR_STATE";
    case CAA_ERROR_GENERAL_ERROR:
        return "CAA_ERROR_GENERAL_ERROR";
    case CAA_ERROR_NOT_SUPPORTED:
        return "CAA_ERROR_NOT_SUPPORTED";
    case CAA_ERROR_CLOSED:
        return "CAA_ERROR_CLOSED";
    case CAA_ERROR_OUT_RANGE:
        return "CAA_ERROR_OUT_RANGE";
    case CAA_ERROR_OVER_LIMIT:
        return "CAA_ERROR_OVER_LIMIT";
    case CAA_ERROR_STALL:
        return "CAA_ERROR_STALL";
    case CAA_ERROR_TIMEOUT:
        return "CAA_ERROR_TIMEOUT";
    case CAA_ERROR_INVALID_LENGTH:
        return "CAA_ERROR_INVALID_LENGTH";
    default:
        return "CAA_ERROR_UNKNOWN";
    }
}

int map_error_code(CAA_ERROR_CODE code) {
    switch (code) {
    case CAA_ERROR_INVALID_INDEX:
    case CAA_ERROR_INVALID_ID:
    case CAA_ERROR_INVALID_VALUE:
    case CAA_ERROR_INVALID_LENGTH:
    case CAA_ERROR_OUT_RANGE:
    case CAA_ERROR_OVER_LIMIT:
        return AlpacaError::InvalidValue;
    case CAA_ERROR_CLOSED:
    case CAA_ERROR_REMOVED:
        return AlpacaError::NotConnected;
    case CAA_ERROR_MOVING:
        return AlpacaError::InvalidOperation;
    case CAA_ERROR_NOT_SUPPORTED:
        return AlpacaError::NotImplemented;
    default:
        return AlpacaError::DriverException;
    }
}

void throw_on_error(CAA_ERROR_CODE code, const std::string& context) {
    if (code == CAA_SUCCESS) {
        return;
    }
    throw AlpacaException(context + ": " + error_code_to_string(code), map_error_code(code));
}

ZWOCAARotatorInfo convert_rotator_info(const CAA_INFO& info) {
    ZWOCAARotatorInfo out;
    out.rotator_id = info.ID;
    out.name = info.Name;
    out.max_degree = static_cast<double>(info.MaxStep);
    return out;
}

} // namespace

class ZWOCAASDKWrapper::Impl {
public:
    std::mutex mutex_;
    struct RotatorUsage {
        int open_count = 0;
    };
    std::unordered_map<int, RotatorUsage> usage_;
};

ZWOCAASDKWrapper::ZWOCAASDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ZWOCAASDKWrapper::~ZWOCAASDKWrapper() = default;

ZWOCAASDKWrapper& ZWOCAASDKWrapper::instance() {
    static ZWOCAASDKWrapper wrapper;
    return wrapper;
}

std::vector<ZWOCAARotatorInfo> ZWOCAASDKWrapper::enumerate_rotators() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int count = CAAGetNum();
    std::vector<ZWOCAARotatorInfo> result;
    if (count <= 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        int id = 0;
        throw_on_error(CAAGetID(i, &id), "CAAGetID");
        CAA_INFO info{};
        CAA_ERROR_CODE code = CAAGetProperty(id, &info);
        if (code == CAA_SUCCESS) {
            result.push_back(convert_rotator_info(info));
        } else {
            ZWOCAARotatorInfo fallback;
            fallback.rotator_id = id;
            result.push_back(std::move(fallback));
        }
    }
    return result;
}

bool ZWOCAASDKWrapper::get_rotator_info_by_id(int rotator_id, ZWOCAARotatorInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    CAA_INFO sdk_info{};
    CAA_ERROR_CODE code = CAAGetProperty(rotator_id, &sdk_info);
    if (code != CAA_SUCCESS) {
        return false;
    }
    info = convert_rotator_info(sdk_info);
    return true;
}

bool ZWOCAASDKWrapper::get_rotator_info_by_index(int rotator_index, ZWOCAARotatorInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int rotator_id = 0;
    CAA_ERROR_CODE code = CAAGetID(rotator_index, &rotator_id);
    if (code != CAA_SUCCESS) {
        return false;
    }
    CAA_INFO sdk_info{};
    code = CAAGetProperty(rotator_id, &sdk_info);
    if (code != CAA_SUCCESS) {
        info = ZWOCAARotatorInfo{rotator_id, std::string(), 0.0};
        return true;
    }
    info = convert_rotator_info(sdk_info);
    return true;
}

void ZWOCAASDKWrapper::open_rotator(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[rotator_id];
    if (usage.open_count == 0) {
        try {
            throw_on_error(CAAOpen(rotator_id), "CAAOpen");
        } catch (...) {
            // Nothing was opened — drop the zero-count entry inserted by
            // operator[] above rather than leaving a stale map node.
            pimpl_->usage_.erase(rotator_id);
            throw;
        }
    }
    ++usage.open_count;
}

void ZWOCAASDKWrapper::close_rotator(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto it = pimpl_->usage_.find(rotator_id);
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
        throw_on_error(CAAClose(rotator_id), "CAAClose");
    }
}

ZWOCAAMotionStatus ZWOCAASDKWrapper::get_motion_status(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    bool moving = false;
    bool hand_control = false;
    throw_on_error(CAAIsMoving(rotator_id, &moving, &hand_control), "CAAIsMoving");
    return {moving, hand_control};
}

double ZWOCAASDKWrapper::get_degree(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    float angle = 0.0f;
    throw_on_error(CAAGetDegree(rotator_id, &angle), "CAAGetDegree");
    return static_cast<double>(angle);
}

void ZWOCAASDKWrapper::move_relative(int rotator_id, double angle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(CAAMove(rotator_id, static_cast<float>(angle)), "CAAMove");
}

void ZWOCAASDKWrapper::move_absolute(int rotator_id, double angle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(CAAMoveTo(rotator_id, static_cast<float>(angle)), "CAAMoveTo");
}

void ZWOCAASDKWrapper::move_mechanical(int rotator_id, double angle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(CAAMoveToMechanical(rotator_id, static_cast<float>(angle)), "CAAMoveToMechanical");
}

void ZWOCAASDKWrapper::stop(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(CAAStop(rotator_id), "CAAStop");
}

void ZWOCAASDKWrapper::sync_degree(int rotator_id, double angle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(CAACurDegree(rotator_id, static_cast<float>(angle)), "CAACurDegree");
}

double ZWOCAASDKWrapper::get_max_degree(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    float angle = 0.0f;
    throw_on_error(CAAGetMaxDegree(rotator_id, &angle), "CAAGetMaxDegree");
    return static_cast<double>(angle);
}

double ZWOCAASDKWrapper::get_temperature(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    float temp = 0.0f;
    throw_on_error(CAAGetTemp(rotator_id, &temp), "CAAGetTemp");
    return static_cast<double>(temp);
}

bool ZWOCAASDKWrapper::get_reverse(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    bool reverse = false;
    throw_on_error(CAAGetReverse(rotator_id, &reverse), "CAAGetReverse");
    return reverse;
}

void ZWOCAASDKWrapper::set_reverse(int rotator_id, bool reverse) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(CAASetReverse(rotator_id, reverse), "CAASetReverse");
}

std::string ZWOCAASDKWrapper::get_serial_number(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    CAA_SN sn{};
    throw_on_error(CAAGetSerialNumber(rotator_id, &sn), "CAAGetSerialNumber");
    std::ostringstream oss;
    for (unsigned char byte : sn.id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::string ZWOCAASDKWrapper::get_firmware_version(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    unsigned char major = 0;
    unsigned char minor = 0;
    unsigned char build = 0;
    throw_on_error(CAAGetFirmwareVersion(rotator_id, &major, &minor, &build), "CAAGetFirmwareVersion");
    std::ostringstream oss;
    oss << static_cast<int>(major) << "." << static_cast<int>(minor) << "." << static_cast<int>(build);
    return oss.str();
}

std::string ZWOCAASDKWrapper::get_rotator_type(int rotator_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    CAA_TYPE type{};
    throw_on_error(CAAGetType(rotator_id, &type), "CAAGetType");
    return type.type;
}

std::string ZWOCAASDKWrapper::get_sdk_version() {
    const char* version = CAAGetSDKVersion();
    if (!version) {
        return "unknown";
    }
    return version;
}

} // namespace alpacacore::vendor::zwo
