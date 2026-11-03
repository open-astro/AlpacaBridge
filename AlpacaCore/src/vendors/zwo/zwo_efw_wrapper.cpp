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

#include <EFW_filter.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_efw_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_slots.h>

#include <iomanip>
#include <sstream>

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

// Issue #1668, as the EAF (issue #1500): the bridge passes only enumerated ids, so EFW_ERROR_INVALID_ID on a
// per-device call means the id is gone (NotConnected, a lost link). EFWOpen and EFWGetID keep InvalidValue for an id
// or index not found.
void throw_on_error(EFW_ERROR_CODE code, const std::string& context, int invalid_id_code = AlpacaError::NotConnected) {
    if (code == EFW_SUCCESS) {
        return;
    }
    const int mapped = code == EFW_ERROR_INVALID_ID ? invalid_id_code : map_error_code(code);
    throw AlpacaException(context + ": " + error_code_to_string(code), mapped);
}

std::string read_serial_number(int wheel_id) {
    EFW_SN sn{};
    throw_on_error(EFWGetSerialNumber(wheel_id, &sn), "EFWGetSerialNumber");
    std::ostringstream oss;
    for (unsigned char byte : sn.id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

ZWOEFWInfo convert_wheel_info(const EFW_INFO& info) {
    ZWOEFWInfo out;
    out.wheel_id = info.ID;
    out.name = info.Name;
    out.slot_count = info.slotNum;
    return out;
}

} // namespace

// Every SDK call runs under the table's one mutex: per-device calls in table_.call(), enumeration and info under
// table_.lock_unchecked(). EFWGetSDKVersion returns a static string and takes no lock.
class ZWOEFWSDKWrapper::Impl {
public:
    SharedOpenTable table_{SharedOpenTable::Ops{
        [](int id) { throw_on_error(EFWOpen(id), "EFWOpen", AlpacaError::InvalidValue); },
        [](int id) { throw_on_error(EFWClose(id), "EFWClose"); },
        [](int id) { return read_serial_number(id); },
    }};
};

ZWOEFWSDKWrapper::ZWOEFWSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ZWOEFWSDKWrapper::~ZWOEFWSDKWrapper() = default;

ZWOEFWSDKWrapper& ZWOEFWSDKWrapper::instance() {
    static ZWOEFWSDKWrapper wrapper;
    return wrapper;
}

std::vector<ZWOEFWInfo> ZWOEFWSDKWrapper::enumerate_wheels() {
    const auto lock = pimpl_->table_.lock_unchecked();
    // A wheel that refuses EFWGetProperty before its open is listed by id (issue #1668, zwo_slots.h).
    const auto slots = read_zwo_efw_slots<EFW_INFO>(
        EFWGetNum(), EFWGetID, EFWGetProperty, error_code_to_string,
        [](EFW_ERROR_CODE code) { return code == EFW_ERROR_REMOVED || code == EFW_ERROR_INVALID_ID; });
    std::vector<ZWOEFWInfo> result;
    result.reserve(slots.size());
    for (const auto& slot : slots) {
        result.push_back(convert_wheel_info(slot.info));
        result.back().slot_index = slot.slot;
    }
    return result;
}

bool ZWOEFWSDKWrapper::get_wheel_info_by_id(int wheel_id, ZWOEFWInfo& info) {
    const auto lock = pimpl_->table_.lock_unchecked();
    EFW_INFO sdk_info{};
    EFW_ERROR_CODE code = EFWGetProperty(wheel_id, &sdk_info);
    if (code != EFW_SUCCESS) {
        return false;
    }
    info = convert_wheel_info(sdk_info);
    return true;
}

bool ZWOEFWSDKWrapper::get_wheel_info_by_index(int wheel_index, ZWOEFWInfo& info) {
    const auto lock = pimpl_->table_.lock_unchecked();
    int wheel_id = 0;
    EFW_ERROR_CODE code = EFWGetID(wheel_index, &wheel_id);
    if (code != EFW_SUCCESS) {
        return false;
    }
    EFW_INFO sdk_info{};
    code = EFWGetProperty(wheel_id, &sdk_info);
    if (code != EFW_SUCCESS) {
        info = ZWOEFWInfo{wheel_id, std::string(), 0, wheel_index};
        return true;
    }
    info = convert_wheel_info(sdk_info);
    info.slot_index = wheel_index;
    return true;
}

SharedOpenTable::Opened ZWOEFWSDKWrapper::open_wheel_link(int wheel_id) { return pimpl_->table_.open(wheel_id); }

std::optional<SharedOpenTable::Opened> ZWOEFWSDKWrapper::open_wheel_candidate(int wheel_id) {
    return pimpl_->table_.open_candidate(wheel_id);
}

void ZWOEFWSDKWrapper::close_wheel_link(int wheel_id, std::uint64_t link) { pimpl_->table_.close(wheel_id, link); }

void ZWOEFWSDKWrapper::with_link(int wheel_id, std::uint64_t link, const std::function<void()>& fn) {
    pimpl_->table_.with_link(wheel_id, link, fn);
}

int ZWOEFWSDKWrapper::get_position(int wheel_id) {
    int position = 0;
    pimpl_->table_.call(wheel_id, [&] { throw_on_error(EFWGetPosition(wheel_id, &position), "EFWGetPosition"); });
    return position;
}

void ZWOEFWSDKWrapper::set_position(int wheel_id, int position) {
    pimpl_->table_.call(wheel_id, [&] { throw_on_error(EFWSetPosition(wheel_id, position), "EFWSetPosition"); });
}

void ZWOEFWSDKWrapper::set_unidirectional(int wheel_id, bool enabled) {
    pimpl_->table_.call(wheel_id, [&] { throw_on_error(EFWSetDirection(wheel_id, enabled), "EFWSetDirection"); });
}

bool ZWOEFWSDKWrapper::get_unidirectional(int wheel_id) {
    bool enabled = false;
    pimpl_->table_.call(wheel_id, [&] { throw_on_error(EFWGetDirection(wheel_id, &enabled), "EFWGetDirection"); });
    return enabled;
}

void ZWOEFWSDKWrapper::calibrate(int wheel_id) {
    pimpl_->table_.call(wheel_id, [&] { throw_on_error(EFWCalibrate(wheel_id), "EFWCalibrate"); });
}

int ZWOEFWSDKWrapper::get_hardware_error_code(int wheel_id) {
    int error_code = 0;
    pimpl_->table_.call(wheel_id,
                        [&] { throw_on_error(EFWGetHWErrorCode(wheel_id, &error_code), "EFWGetHWErrorCode"); });
    return error_code;
}

std::string ZWOEFWSDKWrapper::get_serial_number(int wheel_id) {
    return pimpl_->table_.call(wheel_id, [&] { return read_serial_number(wheel_id); });
}

std::string ZWOEFWSDKWrapper::get_firmware_version(int wheel_id) {
    unsigned char major = 0;
    unsigned char minor = 0;
    unsigned char build = 0;
    pimpl_->table_.call(wheel_id, [&] {
        throw_on_error(EFWGetFirmwareVersion(wheel_id, &major, &minor, &build), "EFWGetFirmwareVersion");
    });
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
