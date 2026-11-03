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

#include <CAA_API.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_caa_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_slots.h>

#include <iomanip>
#include <sstream>

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

// Issue #1668, as the EAF (issue #1500): the bridge passes only enumerated ids, so CAA_ERROR_INVALID_ID on a
// per-device call means the id is gone (NotConnected, a lost link). CAAOpen and CAAGetID keep InvalidValue for an id
// or index not found.
void throw_on_error(CAA_ERROR_CODE code, const std::string& context, int invalid_id_code = AlpacaError::NotConnected) {
    if (code == CAA_SUCCESS) {
        return;
    }
    const int mapped = code == CAA_ERROR_INVALID_ID ? invalid_id_code : map_error_code(code);
    throw AlpacaException(context + ": " + error_code_to_string(code), mapped);
}

std::string read_serial_number(int rotator_id) {
    CAA_SN sn{};
    throw_on_error(CAAGetSerialNumber(rotator_id, &sn), "CAAGetSerialNumber");
    std::ostringstream oss;
    for (unsigned char byte : sn.id) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

ZWOCAARotatorInfo convert_rotator_info(const CAA_INFO& info) {
    ZWOCAARotatorInfo out;
    out.rotator_id = info.ID;
    out.name = info.Name;
    out.max_degree = static_cast<double>(info.MaxStep);
    return out;
}

} // namespace

// Every SDK call runs under the table's one mutex: per-device calls in table_.call(), enumeration and info under
// table_.lock_unchecked(). CAAGetSDKVersion returns a static string and takes no lock.
class ZWOCAASDKWrapper::Impl {
public:
    SharedOpenTable table_{SharedOpenTable::Ops{
        [](int id) { throw_on_error(CAAOpen(id), "CAAOpen", AlpacaError::InvalidValue); },
        [](int id) { throw_on_error(CAAClose(id), "CAAClose"); },
        [](int id) { return read_serial_number(id); },
    }};
};

ZWOCAASDKWrapper::ZWOCAASDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ZWOCAASDKWrapper::~ZWOCAASDKWrapper() = default;

ZWOCAASDKWrapper& ZWOCAASDKWrapper::instance() {
    static ZWOCAASDKWrapper wrapper;
    return wrapper;
}

std::vector<ZWOCAARotatorInfo> ZWOCAASDKWrapper::enumerate_rotators() {
    const auto lock = pimpl_->table_.lock_unchecked();
    const auto slots =
        read_zwo_accessory_slots<CAA_INFO>(CAAGetNum(), "CAA", CAAGetID, CAAGetProperty, error_code_to_string);
    std::vector<ZWOCAARotatorInfo> result;
    result.reserve(slots.size());
    for (const auto& slot : slots) {
        result.push_back(convert_rotator_info(slot.info));
        result.back().slot_index = slot.slot;
    }
    return result;
}

bool ZWOCAASDKWrapper::get_rotator_info_by_id(int rotator_id, ZWOCAARotatorInfo& info) {
    const auto lock = pimpl_->table_.lock_unchecked();
    CAA_INFO sdk_info{};
    CAA_ERROR_CODE code = CAAGetProperty(rotator_id, &sdk_info);
    if (code != CAA_SUCCESS) {
        return false;
    }
    info = convert_rotator_info(sdk_info);
    return true;
}

bool ZWOCAASDKWrapper::get_rotator_info_by_index(int rotator_index, ZWOCAARotatorInfo& info) {
    const auto lock = pimpl_->table_.lock_unchecked();
    int rotator_id = 0;
    CAA_ERROR_CODE code = CAAGetID(rotator_index, &rotator_id);
    if (code != CAA_SUCCESS) {
        return false;
    }
    CAA_INFO sdk_info{};
    code = CAAGetProperty(rotator_id, &sdk_info);
    if (code != CAA_SUCCESS) {
        info = ZWOCAARotatorInfo{rotator_id, std::string(), 0.0, rotator_index};
        return true;
    }
    info = convert_rotator_info(sdk_info);
    info.slot_index = rotator_index;
    return true;
}

SharedOpenTable::Opened ZWOCAASDKWrapper::open_rotator_link(int rotator_id) { return pimpl_->table_.open(rotator_id); }

std::optional<SharedOpenTable::Opened> ZWOCAASDKWrapper::open_rotator_candidate(int rotator_id) {
    return pimpl_->table_.open_candidate(rotator_id);
}

void ZWOCAASDKWrapper::close_rotator_link(int rotator_id, std::uint64_t link) {
    pimpl_->table_.close(rotator_id, link);
}

void ZWOCAASDKWrapper::with_link(int rotator_id, std::uint64_t link, const std::function<void()>& fn) {
    pimpl_->table_.with_link(rotator_id, link, fn);
}

ZWOCAAMotionStatus ZWOCAASDKWrapper::get_motion_status(int rotator_id) {
    bool moving = false;
    bool hand_control = false;
    pimpl_->table_.call(rotator_id,
                        [&] { throw_on_error(CAAIsMoving(rotator_id, &moving, &hand_control), "CAAIsMoving"); });
    return {moving, hand_control};
}

double ZWOCAASDKWrapper::get_degree(int rotator_id) {
    float angle = 0.0f;
    pimpl_->table_.call(rotator_id, [&] { throw_on_error(CAAGetDegree(rotator_id, &angle), "CAAGetDegree"); });
    return static_cast<double>(angle);
}

void ZWOCAASDKWrapper::move_relative(int rotator_id, double angle) {
    pimpl_->table_.call(rotator_id, [&] { throw_on_error(CAAMove(rotator_id, static_cast<float>(angle)), "CAAMove"); });
}

void ZWOCAASDKWrapper::move_absolute(int rotator_id, double angle) {
    pimpl_->table_.call(rotator_id,
                        [&] { throw_on_error(CAAMoveTo(rotator_id, static_cast<float>(angle)), "CAAMoveTo"); });
}

void ZWOCAASDKWrapper::move_mechanical(int rotator_id, double angle) {
    pimpl_->table_.call(rotator_id, [&] {
        throw_on_error(CAAMoveToMechanical(rotator_id, static_cast<float>(angle)), "CAAMoveToMechanical");
    });
}

void ZWOCAASDKWrapper::stop(int rotator_id) {
    pimpl_->table_.call(rotator_id, [&] { throw_on_error(CAAStop(rotator_id), "CAAStop"); });
}

void ZWOCAASDKWrapper::sync_degree(int rotator_id, double angle) {
    pimpl_->table_.call(rotator_id,
                        [&] { throw_on_error(CAACurDegree(rotator_id, static_cast<float>(angle)), "CAACurDegree"); });
}

double ZWOCAASDKWrapper::get_max_degree(int rotator_id) {
    float angle = 0.0f;
    pimpl_->table_.call(rotator_id, [&] { throw_on_error(CAAGetMaxDegree(rotator_id, &angle), "CAAGetMaxDegree"); });
    return static_cast<double>(angle);
}

double ZWOCAASDKWrapper::get_temperature(int rotator_id) {
    float temp = 0.0f;
    pimpl_->table_.call(rotator_id, [&] { throw_on_error(CAAGetTemp(rotator_id, &temp), "CAAGetTemp"); });
    return static_cast<double>(temp);
}

bool ZWOCAASDKWrapper::get_reverse(int rotator_id) {
    bool reverse = false;
    pimpl_->table_.call(rotator_id, [&] { throw_on_error(CAAGetReverse(rotator_id, &reverse), "CAAGetReverse"); });
    return reverse;
}

void ZWOCAASDKWrapper::set_reverse(int rotator_id, bool reverse) {
    pimpl_->table_.call(rotator_id, [&] { throw_on_error(CAASetReverse(rotator_id, reverse), "CAASetReverse"); });
}

std::string ZWOCAASDKWrapper::get_serial_number(int rotator_id) {
    return pimpl_->table_.call(rotator_id, [&] { return read_serial_number(rotator_id); });
}

std::string ZWOCAASDKWrapper::get_firmware_version(int rotator_id) {
    unsigned char major = 0;
    unsigned char minor = 0;
    unsigned char build = 0;
    pimpl_->table_.call(rotator_id, [&] {
        throw_on_error(CAAGetFirmwareVersion(rotator_id, &major, &minor, &build), "CAAGetFirmwareVersion");
    });
    std::ostringstream oss;
    oss << static_cast<int>(major) << "." << static_cast<int>(minor) << "." << static_cast<int>(build);
    return oss.str();
}

std::string ZWOCAASDKWrapper::get_rotator_type(int rotator_id) {
    CAA_TYPE type{};
    pimpl_->table_.call(rotator_id, [&] { throw_on_error(CAAGetType(rotator_id, &type), "CAAGetType"); });
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
