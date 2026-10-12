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

#pragma once

#include <alpacacore/util/logging.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

/// One camera as the SDK enumerates it. `serial` is empty when the body
/// reports none (ASI120MM Mini) or the read failed.
struct ZwoEnumeratedCamera {
    int index{};
    int camera_id{};
    std::string name;
    std::string serial;
};

/// A camera slot the SDK listed but would not describe.
struct ZwoUnreadableSlot {
    int slot{};
    std::string error;
};

/// One readable slot: the SDK slot index it was read from, and what it said.
template <typename Info>
struct ZwoSlotRead {
    int slot{};
    Info info;
};

/// Read every SDK slot in [0, count) through `read(slot, info, error)`, which
/// returns false and fills `error` (the SDK error name) for a slot it cannot
/// read. One unreadable slot (a camera that dropped off the bus,
/// ASI_ERROR_CAMERA_REMOVED) is skipped and logged at WARN, so the other
/// cameras still enumerate; each entry keeps its SDK slot index, which is the
/// index a config entry's `cameraIndex` means. Skipped slots are appended to
/// `unreadable` when it is not null.
template <typename Info, typename Reader>
std::vector<ZwoSlotRead<Info>> read_zwo_camera_slots(int count, Reader&& read,
                                                     std::vector<ZwoUnreadableSlot>* unreadable = nullptr) {
    std::vector<ZwoSlotRead<Info>> result;
    for (int slot = 0; slot < count; ++slot) {
        Info info{};
        std::string error;
        if (!read(slot, info, error)) {
            ALPACA_LOG_WARN("ZWO", "Skipping camera slot " + std::to_string(slot) + ": " + error);
            if (unreadable != nullptr) {
                unreadable->push_back({slot, error});
            }
            continue;
        }
        result.push_back({slot, std::move(info)});
    }
    return result;
}

/// Text for a connect refusal when a slot could not be read: names each slot
/// and its SDK error and tells the operator what to do. Empty when none.
std::string describe_unreadable_zwo_slots(const std::vector<ZwoUnreadableSlot>& unreadable);

/// What a config entry records about the camera it is bound to. `camera_id`
/// and `camera_index` are hints; `serial` (or, with none, `camera_name`) is
/// the key.
struct ZwoConfiguredIdentity {
    std::optional<int> camera_id;
    std::optional<int> camera_index;
    std::string serial;
    std::string camera_name;
};

enum class ZwoResolveFailure : std::uint8_t {
    None,
    NoCameras,
    NotFound,
    IndexOutOfRange,
};

struct ZwoResolveResult {
    std::optional<ZwoEnumeratedCamera> camera;
    ZwoResolveFailure failure{ZwoResolveFailure::None};
    std::string message;
};

/// Drop the trailing spaces and NULs the SDK pads a model name with.
std::string trim_zwo_name(const std::string& name);

/// Choose the camera a config entry means.
///
/// With a serial: only the camera carrying that serial, never another one.
/// Without: the camera whose trimmed model name equals `camera_name`, skipping
/// a camera whose serial `serials_claimed_by_other_entries` holds; among
/// same-model bodies the index hint breaks the tie. An entry with neither
/// (written before serial binding) binds by id, then by index.
ZwoResolveResult resolve_zwo_camera(const ZwoConfiguredIdentity& entry, const std::vector<ZwoEnumeratedCamera>& found,
                                    const std::set<std::string>& serials_claimed_by_other_entries);

/// `ZWO_UID_` plus 16 random hex digits, for a camera that reports no serial.
std::string generate_zwo_unique_id();

/// The UniqueID a camera reports: `ZWO_SN_<serial>` when a serial is known,
/// else `stored_unique_id`.
std::string zwo_unique_id(const std::string& serial, const std::string& stored_unique_id);

}  // namespace alpacacore::vendor::zwo
