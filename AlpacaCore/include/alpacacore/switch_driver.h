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
#include <vector>
#include <string>

namespace alpacacore {

/**
 * @brief Pure virtual interface for Alpaca Switch drivers.
 *
 * Follows ASCOM Alpaca Switch API specification.
 * All switch drivers must implement this interface.
 */
class SwitchDriver : public AlpacaDriver {
public:
    virtual ~SwitchDriver() = default;

    /**
     * @brief Platform 7 DeviceState snapshot for Switch devices.
     *
     * Reports the per-switch operational properties (GetSwitchN, GetSwitchValueN,
     * StateChangeCompleteN for every id below MaxSwitch) plus a TimeStamp by
     * calling this device's own public getters — the same ones the GET endpoints
     * use, which is what guarantees the DeviceState↔GET consistency ConformU
     * checks. A getter that throws (NotConnected, or an unwrapped vendor error)
     * causes that id's members to be omitted rather than failing the whole call.
     *
     * Per AGENTS.md, DeviceState is intentionally NOT an atomic snapshot (each
     * getter locks separately) and vendors must NOT override this with a
     * single-lock or wrapper-direct per-vendor version: reading through anything
     * but the public getters is exactly what desyncs DeviceState from the GETs.
     */
    std::vector<DeviceState> get_device_state() const override final {
        std::vector<DeviceState> state;
        int count = 0;
        try {
            count = get_max_switch();
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // MaxSwitch itself unavailable -- report TimeStamp only.
        }
        for (int id = 0; id < count; ++id) {
            try {
                // Read all three before pushing: the getters acquire the driver
                // mutex independently, so a disconnect racing between them can
                // throw mid-id -- computing first keeps the push all-or-nothing
                // (no orphaned GetSwitchN without its siblings).
                const bool on = get_switch(id);
                const double value = get_switch_value(id);
                const bool complete = get_state_change_complete(id);
                state.push_back({"GetSwitch" + std::to_string(id), on});
                state.push_back({"GetSwitchValue" + std::to_string(id), value});
                state.push_back({"StateChangeComplete" + std::to_string(id), complete});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Omit this id's members per the DeviceState contract.
            }
        }
        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

    // Switch-specific properties

    /**
     * @brief Get the number of switches.
     */
    virtual int get_max_switch() const = 0;

    /**
     * @brief Get whether a switch can be written.
     *
     * @param id Switch ID (0-based)
     */
    virtual bool get_can_write(int id) const = 0;

    /**
     * @brief Get whether a switch can operate asynchronously.
     *
     * @param id Switch ID (0-based)
     */
    virtual bool get_can_async(int id) const = 0;

    /**
     * @brief Get the switch state (boolean).
     *
     * @param id Switch ID (0-based)
     */
    virtual bool get_switch(int id) const = 0;

    /**
     * @brief Set the switch state (boolean).
     *
     * @param id Switch ID (0-based)
     * @param state Switch state
     */
    virtual void set_switch(int id, bool state) = 0;

    /**
     * @brief Set the switch state asynchronously (boolean).
     *
     * @param id Switch ID (0-based)
     * @param state Switch state
     */
    virtual void set_async(int id, bool state) = 0;

    /**
     * @brief Get the switch value.
     *
     * @param id Switch ID (0-based)
     */
    virtual double get_switch_value(int id) const = 0;

    /**
     * @brief Set the switch value.
     *
     * @param id Switch ID (0-based)
     * @param value Switch value
     */
    virtual void set_switch_value(int id, double value) = 0;

    /**
     * @brief Set the switch value asynchronously.
     *
     * @param id Switch ID (0-based)
     * @param value Switch value
     */
    virtual void set_async_value(int id, double value) = 0;

    /**
     * @brief Check whether an asynchronous state change is complete.
     *
     * @param id Switch ID (0-based)
     */
    virtual bool get_state_change_complete(int id) const = 0;

    /**
     * @brief Get the switch name.
     *
     * @param id Switch ID (0-based)
     */
    virtual std::string get_switch_name(int id) const = 0;

    /**
     * @brief Set the switch name.
     *
     * @param id Switch ID (0-based)
     * @param name Switch name
     */
    virtual void set_switch_name(int id, const std::string& name) = 0;

    /**
     * @brief Get the switch description.
     *
     * @param id Switch ID (0-based)
     */
    virtual std::string get_switch_description(int id) const = 0;

    /**
     * @brief Get the minimum switch value.
     *
     * @param id Switch ID (0-based)
     */
    virtual double get_min_switch_value(int id) const = 0;

    /**
     * @brief Get the maximum switch value.
     *
     * @param id Switch ID (0-based)
     */
    virtual double get_max_switch_value(int id) const = 0;

    /**
     * @brief Get the switch step size.
     *
     * @param id Switch ID (0-based)
     */
    virtual double get_switch_step(int id) const = 0;
};

} // namespace alpacacore
