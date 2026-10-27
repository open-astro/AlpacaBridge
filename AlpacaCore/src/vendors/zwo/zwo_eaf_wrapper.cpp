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

#include <EAF_focuser.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_eaf_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_slots.h>

#include <iomanip>
#include <sstream>

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

// Issue #1500, decision 7: the bridge passes only enumerated ids, so EAF_ERROR_INVALID_ID on a per-device call means
// the id is gone (NotConnected, a lost link). EAFOpen and EAFGetID keep InvalidValue for an id or index not found.
// TODO(#1500): confirm on the rig that a call on an old id after a replug returns CLOSED or INVALID_ID.
void throw_on_error(EAF_ERROR_CODE code, const std::string& context, int invalid_id_code = AlpacaError::NotConnected) {
    if (code == EAF_SUCCESS) {
        return;
    }
    const int mapped = code == EAF_ERROR_INVALID_ID ? invalid_id_code : map_error_code(code);
    throw AlpacaException(context + ": " + error_code_to_string(code), mapped);
}

std::string read_serial_number(int focuser_id) {
    EAF_SN sn{};
    throw_on_error(EAFGetSerialNumber(focuser_id, &sn), "EAFGetSerialNumber");
    std::ostringstream oss;
    for (unsigned char byte : sn.id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

ZWOEAFFocuserInfo convert_focuser_info(const EAF_INFO& info) {
    ZWOEAFFocuserInfo out;
    out.focuser_id = info.ID;
    out.name = info.Name;
    out.max_step = info.MaxStep;
    return out;
}

} // namespace

// Every SDK call runs under the table's one mutex: per-device calls in table_.call(), enumeration and info under
// table_.lock_unchecked(). EAFGetSDKVersion returns a static string and takes no lock.
class ZWOEAFSDKWrapper::Impl {
public:
    SharedOpenTable table_{SharedOpenTable::Ops{
        [](int id) { throw_on_error(EAFOpen(id), "EAFOpen", AlpacaError::InvalidValue); },
        [](int id) { throw_on_error(EAFClose(id), "EAFClose"); },
        [](int id) { return read_serial_number(id); },
    }};
};

ZWOEAFSDKWrapper::ZWOEAFSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ZWOEAFSDKWrapper::~ZWOEAFSDKWrapper() = default;

ZWOEAFSDKWrapper& ZWOEAFSDKWrapper::instance() {
    static ZWOEAFSDKWrapper wrapper;
    return wrapper;
}

std::vector<ZWOEAFFocuserInfo> ZWOEAFSDKWrapper::enumerate_focusers() {
    const auto lock = pimpl_->table_.lock_unchecked();
    const auto slots =
        read_zwo_accessory_slots<EAF_INFO>(EAFGetNum(), "EAF", EAFGetID, EAFGetProperty, error_code_to_string);
    std::vector<ZWOEAFFocuserInfo> result;
    result.reserve(slots.size());
    for (const auto& slot : slots) {
        result.push_back(convert_focuser_info(slot.info));
        result.back().slot_index = slot.slot;
    }
    return result;
}

bool ZWOEAFSDKWrapper::get_focuser_info_by_id(int focuser_id, ZWOEAFFocuserInfo& info) {
    const auto lock = pimpl_->table_.lock_unchecked();
    EAF_INFO sdk_info{};
    EAF_ERROR_CODE code = EAFGetProperty(focuser_id, &sdk_info);
    if (code != EAF_SUCCESS) {
        return false;
    }
    info = convert_focuser_info(sdk_info);
    return true;
}

bool ZWOEAFSDKWrapper::get_focuser_info_by_index(int focuser_index, ZWOEAFFocuserInfo& info) {
    const auto lock = pimpl_->table_.lock_unchecked();
    int focuser_id = 0;
    EAF_ERROR_CODE code = EAFGetID(focuser_index, &focuser_id);
    if (code != EAF_SUCCESS) {
        return false;
    }
    EAF_INFO sdk_info{};
    code = EAFGetProperty(focuser_id, &sdk_info);
    if (code != EAF_SUCCESS) {
        info = ZWOEAFFocuserInfo{focuser_id, std::string(), 0, focuser_index};
        return true;
    }
    info = convert_focuser_info(sdk_info);
    info.slot_index = focuser_index;
    return true;
}

SharedOpenTable::Opened ZWOEAFSDKWrapper::open_focuser_link(int focuser_id) { return pimpl_->table_.open(focuser_id); }

std::optional<SharedOpenTable::Opened> ZWOEAFSDKWrapper::open_focuser_candidate(int focuser_id) {
    return pimpl_->table_.open_candidate(focuser_id);
}

void ZWOEAFSDKWrapper::close_focuser_link(int focuser_id, std::uint64_t link) {
    pimpl_->table_.close(focuser_id, link);
}

void ZWOEAFSDKWrapper::with_link(int focuser_id, std::uint64_t link, const std::function<void()>& fn) {
    pimpl_->table_.with_link(focuser_id, link, fn);
}

bool ZWOEAFSDKWrapper::is_moving(int focuser_id) {
    bool moving = false;
    bool hand_control = false;
    pimpl_->table_.call(focuser_id,
                        [&] { throw_on_error(EAFIsMoving(focuser_id, &moving, &hand_control), "EAFIsMoving"); });
    return moving;
}

int ZWOEAFSDKWrapper::get_position(int focuser_id) {
    int position = 0;
    pimpl_->table_.call(focuser_id, [&] { throw_on_error(EAFGetPosition(focuser_id, &position), "EAFGetPosition"); });
    return position;
}

void ZWOEAFSDKWrapper::move(int focuser_id, int position) {
    pimpl_->table_.call(focuser_id, [&] { throw_on_error(EAFMove(focuser_id, position), "EAFMove"); });
}

void ZWOEAFSDKWrapper::stop(int focuser_id) {
    pimpl_->table_.call(focuser_id, [&] { throw_on_error(EAFStop(focuser_id), "EAFStop"); });
}

int ZWOEAFSDKWrapper::get_max_step(int focuser_id) {
    int max_step = 0;
    pimpl_->table_.call(focuser_id, [&] { throw_on_error(EAFGetMaxStep(focuser_id, &max_step), "EAFGetMaxStep"); });
    return max_step;
}

void ZWOEAFSDKWrapper::set_max_step(int focuser_id, int max_step) {
    pimpl_->table_.call(focuser_id, [&] { throw_on_error(EAFSetMaxStep(focuser_id, max_step), "EAFSetMaxStep"); });
}

int ZWOEAFSDKWrapper::get_step_range(int focuser_id) {
    int range = 0;
    pimpl_->table_.call(focuser_id, [&] { throw_on_error(EAFStepRange(focuser_id, &range), "EAFStepRange"); });
    return range;
}

double ZWOEAFSDKWrapper::get_temperature(int focuser_id) {
    float temp = 0.0f;
    pimpl_->table_.call(focuser_id, [&] { throw_on_error(EAFGetTemp(focuser_id, &temp), "EAFGetTemp"); });
    return static_cast<double>(temp);
}

std::string ZWOEAFSDKWrapper::get_serial_number(int focuser_id) {
    return pimpl_->table_.call(focuser_id, [&] { return read_serial_number(focuser_id); });
}

std::string ZWOEAFSDKWrapper::get_firmware_version(int focuser_id) {
    unsigned char major = 0;
    unsigned char minor = 0;
    unsigned char build = 0;
    pimpl_->table_.call(focuser_id, [&] {
        throw_on_error(EAFGetFirmwareVersion(focuser_id, &major, &minor, &build), "EAFGetFirmwareVersion");
    });
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
