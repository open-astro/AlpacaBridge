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
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

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
    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        try {
            state.push_back({"Position", DeviceStateValue{static_cast<std::int32_t>(get_position())}});
        } catch (const AlpacaException&) {
            // Not currently known: omit per the DeviceState contract.
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
