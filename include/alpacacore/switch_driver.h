// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaCore/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

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
     * @brief Get the switch value.
     *
     * @param id Switch ID (0-based)
     */
    virtual bool get_switch_value(int id) const = 0;

    /**
     * @brief Set the switch value.
     *
     * @param id Switch ID (0-based)
     * @param value Switch value
     */
    virtual void set_switch_value(int id, bool value) = 0;

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

