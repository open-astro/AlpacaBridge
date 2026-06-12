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

#include <PlayerOnePW.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/playerone/playerone_pw_wrapper.h>

#include <mutex>
#include <unordered_map>

namespace alpacacore::vendor::playerone {

namespace {

int map_error_code(PWErrors code) {
    switch (code) {
        case PW_ERROR_INVALID_INDEX:
        case PW_ERROR_INVALID_HANDLE:
        case PW_ERROR_INVALID_ARGU:
            return AlpacaError::InvalidValue;
        case PW_ERROR_NOT_OPENED:
        case PW_ERROR_NOT_FOUND:
            return AlpacaError::NotConnected;
        case PW_ERROR_IS_MOVING:
            return AlpacaError::InvalidOperation;
        default:
            return AlpacaError::DriverException;
    }
}

void throw_on_error(PWErrors code, const std::string& context) {
    if (code == PW_OK) {
        return;
    }
    const char* error_string = POAGetPWErrorString(code);
    throw AlpacaException(context + ": " + (error_string ? error_string : "unknown error"), map_error_code(code));
}

PlayerOnePWInfo convert_wheel_info(const PWProperties& props) {
    PlayerOnePWInfo out;
    out.handle = props.Handle;
    out.name = props.Name;
    out.position_count = props.PositionCount;
    out.serial_number = props.SN;
    return out;
}

}  // namespace

class PlayerOnePWSDKWrapper::Impl {
public:
    std::mutex mutex_;
    struct WheelUsage {
        int open_count = 0;
    };
    std::unordered_map<int, WheelUsage> usage_;
};

PlayerOnePWSDKWrapper::PlayerOnePWSDKWrapper() : pimpl_(std::make_unique<Impl>()) {}

PlayerOnePWSDKWrapper::~PlayerOnePWSDKWrapper() = default;

PlayerOnePWSDKWrapper& PlayerOnePWSDKWrapper::instance() {
    static PlayerOnePWSDKWrapper wrapper;
    return wrapper;
}

std::vector<PlayerOnePWInfo> PlayerOnePWSDKWrapper::enumerate_wheels() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int count = POAGetPWCount();
    std::vector<PlayerOnePWInfo> result;
    if (count <= 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        PWProperties props{};
        throw_on_error(POAGetPWProperties(i, &props), "POAGetPWProperties");
        result.push_back(convert_wheel_info(props));
    }
    return result;
}

bool PlayerOnePWSDKWrapper::get_wheel_info_by_index(int wheel_index, PlayerOnePWInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    PWProperties props{};
    if (POAGetPWProperties(wheel_index, &props) != PW_OK) {
        return false;
    }
    info = convert_wheel_info(props);
    return true;
}

bool PlayerOnePWSDKWrapper::get_wheel_info_by_handle(int handle, PlayerOnePWInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    PWProperties props{};
    if (POAGetPWPropertiesByHandle(handle, &props) != PW_OK) {
        return false;
    }
    info = convert_wheel_info(props);
    return true;
}

void PlayerOnePWSDKWrapper::open_wheel(int handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto& usage = pimpl_->usage_[handle];
    if (usage.open_count == 0) {
        throw_on_error(POAOpenPW(handle), "POAOpenPW");
    }
    ++usage.open_count;
}

void PlayerOnePWSDKWrapper::close_wheel(int handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    auto it = pimpl_->usage_.find(handle);
    if (it == pimpl_->usage_.end() || it->second.open_count <= 0) {
        return;
    }
    --it->second.open_count;
    if (it->second.open_count == 0) {
        // Erase the bookkeeping first: a failing SDK close (e.g. device
        // unplugged) must not leave a zero-count entry that turns every
        // later close into a no-op and leaks the handle.
        pimpl_->usage_.erase(it);
        throw_on_error(POAClosePW(handle), "POAClosePW");
    }
}

int PlayerOnePWSDKWrapper::get_position(int handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    PWState state = PW_STATE_CLOSED;
    throw_on_error(POAGetPWState(handle, &state), "POAGetPWState");
    if (state == PW_STATE_MOVING) {
        return -1;
    }
    int position = 0;
    PWErrors code = POAGetCurrentPosition(handle, &position);
    if (code == PW_ERROR_IS_MOVING) {
        // The wheel started moving between the state read and the position
        // read — still maps to the ASCOM "moving" value.
        return -1;
    }
    throw_on_error(code, "POAGetCurrentPosition");
    return position;
}

void PlayerOnePWSDKWrapper::goto_position(int handle, int position) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(POAGotoPosition(handle, position), "POAGotoPosition");
}

bool PlayerOnePWSDKWrapper::is_moving(int handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    PWState state = PW_STATE_CLOSED;
    throw_on_error(POAGetPWState(handle, &state), "POAGetPWState");
    return state == PW_STATE_MOVING;
}

bool PlayerOnePWSDKWrapper::get_one_way(int handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int one_way = 0;
    throw_on_error(POAGetOneWay(handle, &one_way), "POAGetOneWay");
    return one_way != 0;
}

void PlayerOnePWSDKWrapper::set_one_way(int handle, bool one_way) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(POASetOneWay(handle, one_way ? 1 : 0), "POASetOneWay");
}

std::string PlayerOnePWSDKWrapper::get_filter_alias(int handle, int position) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    char alias[MAX_NAME_LEN + 1] = {};
    throw_on_error(POAGetPWFilterAlias(handle, position, alias, MAX_NAME_LEN), "POAGetPWFilterAlias");
    return alias;
}

int PlayerOnePWSDKWrapper::get_focus_offset(int handle, int position) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int focus_offset = 0;
    throw_on_error(POAGetPWFocusOffset(handle, position, &focus_offset), "POAGetPWFocusOffset");
    return focus_offset;
}

void PlayerOnePWSDKWrapper::reset_wheel(int handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(POAResetPW(handle), "POAResetPW");
}

std::string PlayerOnePWSDKWrapper::get_sdk_version() {
    const char* version = POAGetPWSDKVer();
    if (!version) {
        return "unknown";
    }
    return version;
}

int PlayerOnePWSDKWrapper::get_api_version() { return POAGetPWAPIVer(); }

}  // namespace alpacacore::vendor::playerone
