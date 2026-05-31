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
