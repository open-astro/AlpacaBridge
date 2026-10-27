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

}  // namespace alpacacore::vendor::zwo
