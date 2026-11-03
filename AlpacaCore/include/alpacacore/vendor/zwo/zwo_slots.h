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

#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::zwo {

struct ZwoUnreadableSlot {
    int slot{};
    std::string error;
};

template <typename Info>
struct ZwoSlotRead {
    int slot{};
    Info info;
};

// A skipped slot never renumbers the survivors: configured indexes refer to SDK slots.
template <typename Info, typename Reader>
std::vector<ZwoSlotRead<Info>> read_zwo_slots(int count, const std::string& kind, Reader&& read,
                                              std::vector<ZwoUnreadableSlot>* unreadable = nullptr) {
    std::vector<ZwoSlotRead<Info>> result;
    for (int slot = 0; slot < count; ++slot) {
        Info info{};
        std::string error;
        if (!read(slot, info, error)) {
            std::string message = "Skipping ";
            message.append(kind).append(" slot ").append(std::to_string(slot)).append(": ").append(error);
            ALPACA_LOG_WARN("ZWO", message);
            if (unreadable != nullptr) unreadable->push_back({slot, error});
            continue;
        }
        result.push_back({slot, std::move(info)});
    }
    return result;
}

// EFW/EAF/CAA all describe a slot in two steps and define SDK success as zero.
template <typename Info, typename GetID, typename GetProperty, typename ErrorName>
std::vector<ZwoSlotRead<Info>> read_zwo_accessory_slots(int count, const std::string& kind, GetID&& get_id,
                                                        GetProperty&& get_property, ErrorName&& error_name) {
    return read_zwo_slots<Info>(count, kind, [&](int slot, Info& info, std::string& error) {
        int id = 0;
        const auto id_code = get_id(slot, &id);
        if (id_code != 0) {
            error = kind + "GetID: " + error_name(id_code);
            return false;
        }
        const auto property_code = get_property(id, &info);
        if (property_code != 0) {
            error = kind + "GetProperty: " + error_name(property_code);
            return false;
        }
        return true;
    });
}

// The EFW variant (issue #1668). ZWO's EFW procedure opens a wheel before EFWGetProperty ("SlotNum is 0 if not
// opened"), and an EFW on the rig refused the read before its open with a code outside the enum, though it opened and
// read its property after. So a property failure keeps the slot, by its id alone (the rest of @p Info value-
// initialised), unless @p gone says the code means the wheel is not there; the connect reads the property after its
// open. An id that cannot be read is still skipped and logged (#1703). The EAF and CAA headers name no such order, so
// they keep read_zwo_accessory_slots().
template <typename Info, typename GetID, typename GetProperty, typename ErrorName, typename Gone>
std::vector<ZwoSlotRead<Info>> read_zwo_efw_slots(int count, GetID&& get_id, GetProperty&& get_property,
                                                  ErrorName&& error_name, Gone&& gone) {
    return read_zwo_slots<Info>(count, "EFW", [&](int slot, Info& info, std::string& error) {
        int id = 0;
        const auto id_code = get_id(slot, &id);
        if (id_code != 0) {
            error = "EFWGetID: " + error_name(id_code);
            return false;
        }
        const auto property_code = get_property(id, &info);
        if (property_code == 0) {
            return true;
        }
        if (gone(property_code)) {
            error = "EFWGetProperty: " + error_name(property_code);
            return false;
        }
        ALPACA_LOG_DEBUG("ZWO", "EFW slot " + std::to_string(slot) + " property unreadable before open (" +
                                    error_name(property_code) + "); listed by id " + std::to_string(id));
        info = Info{};
        info.ID = id;
        return true;
    });
}

}  // namespace alpacacore::vendor::zwo
