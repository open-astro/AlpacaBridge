// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/util/error_handling.h>
#include <string>
#include <string_view>
#include <memory>

namespace alpacacore {

/**
 * @brief Base interface for all Alpaca device drivers.
 *
 * All device drivers must implement this interface.
 * This provides the common Alpaca device API.
 */
class AlpacaDriver {
public:
    virtual ~AlpacaDriver() = default;

    // Common Alpaca device properties and methods

    /**
     * @brief Get the device number.
     */
    virtual int get_device_number() const = 0;

    /**
     * @brief Get the device name.
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief Get the device type.
     */
    virtual DeviceType get_device_type() const = 0;

    /**
     * @brief Get the device unique ID.
     *
     * This is a unique identifier for the device instance.
     */
    virtual std::string get_unique_id() const = 0;

    /**
     * @brief Get the device description.
     */
    virtual std::string get_description() const = 0;

    /**
     * @brief Get the driver info.
     */
    virtual std::string get_driver_info() const = 0;

    /**
     * @brief Get the driver version.
     */
    virtual std::string get_driver_version() const = 0;

    /**
     * @brief Get the ASCOM interface version.
     */
    virtual int get_interface_version() const = 0;

    /**
     * @brief Get whether the device is connected.
     */
    virtual bool get_connected() const = 0;

    /**
     * @brief Set whether the device is connected.
     */
    virtual void set_connected(bool connected) = 0;

    /**
     * @brief Get the supported actions.
     */
    virtual std::string get_supported_actions() const = 0;

    /**
     * @brief Invoke an action.
     *
     * @param action_name Action name
     * @param action_parameters Action parameters (JSON string)
     * @return Action result (JSON string)
     */
    virtual std::string action(std::string_view action_name,
                               std::string_view action_parameters) = 0;

    /**
     * @brief Get whether an action is supported.
     */
    virtual bool can_action(std::string_view action_name) const = 0;

    /**
     * @brief Get the command blind result.
     *
     * @param command Command string
     * @param raw Whether to return raw response
     * @return Command result
     */
    virtual std::string command_blind(std::string_view command, bool raw = false) = 0;

    /**
     * @brief Get the command bool result.
     *
     * @param command Command string
     * @param raw Whether to return raw response
     * @return Command result
     */
    virtual bool command_bool(std::string_view command, bool raw = false) = 0;

    /**
     * @brief Get the command string result.
     *
     * @param command Command string
     * @param raw Whether to return raw response
     * @return Command result
     */
    virtual std::string command_string(std::string_view command, bool raw = false) = 0;
};

} // namespace alpacacore

