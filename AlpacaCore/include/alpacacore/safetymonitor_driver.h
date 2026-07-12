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

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>

namespace alpacacore {

/**
 * @brief Pure virtual interface for Alpaca SafetyMonitor drivers.
 *
 * Follows ASCOM Alpaca SafetyMonitor API specification.
 * All safety monitor drivers must implement this interface.
 */
class SafetyMonitorDriver : public AlpacaDriver {
public:
    virtual ~SafetyMonitorDriver() = default;

    // Platform 7 operational state (ISafetyMonitorV3): IsSafe plus a TimeStamp.
    // Inline so the vtable stays weak; values come from the same getters as the
    // GET endpoints.
    std::vector<DeviceState> get_device_state() const override final {
        std::vector<DeviceState> state;
        try {
            state.push_back({"IsSafe", DeviceStateValue{get_is_safe()}});
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Not currently known -- or an unwrapped vendor error -- so omit per the DeviceState contract.
        }
        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

    // SafetyMonitor-specific properties

    /**
     * @brief Get whether the safety condition is safe.
     *
     * @return true if safe, false if unsafe
     */
    virtual bool get_is_safe() const = 0;
};

} // namespace alpacacore

