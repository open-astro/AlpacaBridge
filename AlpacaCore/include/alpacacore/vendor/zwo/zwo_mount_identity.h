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

#include <alpacacore/util/error_handling.h>

#include <map>
#include <mutex>
#include <string>

namespace alpacacore::vendor::zwo {

// Prevent Auto from sharing a physical mount through different endpoints.
// The caller supplies a fresh, validated, uppercase 12-hex-digit :GMA# reply.
// Own before the protocol member so this claim outlives the link; reset under
// the driver's state lock on disconnect, failed connect or link loss.
// The registry mutex is a leaf: no I/O, other locks or joins run under it.
class MountIdentityClaim {
public:
    MountIdentityClaim() = default;
    ~MountIdentityClaim() { reset(); }
    MountIdentityClaim(const MountIdentityClaim&) = delete;
    MountIdentityClaim& operator=(const MountIdentityClaim&) = delete;

    void claim(const std::string& serial, bool automatic, int device_number) {
        auto& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (const auto& [owner, holder] : reg.holders) {
            if (owner != this && holder.serial == serial && (automatic || holder.automatic)) {
                throw AlpacaException("ZWO mount is already claimed by ZWO telescope device number " +
                                          std::to_string(holder.device_number) +
                                          ". Select explicit, distinct endpoints for each mount instead of Auto.",
                                      AlpacaError::DriverException);
            }
        }
        reg.holders.insert_or_assign(this, Holder{serial, automatic, device_number});
    }

    void reset() const {
        auto& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.holders.erase(this);
    }

private:
    struct Holder {
        std::string serial;
        bool automatic;
        int device_number;
    };
    struct Registry {
        std::mutex mutex;
        std::map<const MountIdentityClaim*, Holder> holders;
    };

    static Registry& registry() {
        // Never destroyed: static drivers may release their claims at process exit.
        static auto* reg = new Registry();  // NOLINT(cppcoreguidelines-owning-memory)
        return *reg;
    }
};

}  // namespace alpacacore::vendor::zwo
