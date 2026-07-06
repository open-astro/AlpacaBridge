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

#include <cstdint>
#include <string>
#include <vector>

namespace alpacacore {

/**
 * @brief Pure virtual interface for Alpaca FilterWheel drivers.
 *
 * Follows ASCOM Alpaca FilterWheel API specification.
 * All filter wheel drivers must implement this interface.
 */
class FilterWheelDriver : public AlpacaDriver {
public:
    virtual ~FilterWheelDriver() = default;

    // Platform 7 operational state (IFilterWheelV3): Position (-1 while moving)
    // plus a TimeStamp. Inline so the vtable stays weak; the value comes from
    // the same getter as the GET endpoint.
    std::vector<DeviceState> get_device_state() const override final {
        std::vector<DeviceState> state;
        try {
            state.push_back({"Position", DeviceStateValue{static_cast<std::int32_t>(get_position())}});
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Not currently known -- or an unwrapped vendor error -- so omit per the DeviceState contract.
        }
        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

    // FilterWheel-specific properties

    /**
     * @brief Get the current filter position (0-based).
     */
    virtual int get_position() const = 0;

    /**
     * @brief Set the filter position (0-based).
     */
    virtual void set_position(int position) = 0;

    /**
     * @brief Get the focus offsets for all filter positions.
     */
    virtual std::vector<int> get_focus_offsets() const = 0;

    /**
     * @brief Set the focus offsets for all filter positions.
     */
    virtual void set_focus_offsets(const std::vector<int>& offsets) = 0;

    /**
     * @brief Get the names of all filters.
     */
    virtual std::vector<std::string> get_names() const = 0;

    /**
     * @brief Set the names of all filters.
     */
    virtual void set_names(const std::vector<std::string>& names) = 0;
};

} // namespace alpacacore
