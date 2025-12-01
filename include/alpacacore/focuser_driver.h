// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://www.mongodb.com/licensing/server-side-public-license
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>

namespace alpacacore {

/**
 * @brief Pure virtual interface for Alpaca Focuser drivers.
 *
 * Follows ASCOM Alpaca Focuser API specification.
 * All focuser drivers must implement this interface.
 */
class FocuserDriver : public AlpacaDriver {
public:
    virtual ~FocuserDriver() = default;

    // Focuser-specific properties

    /**
     * @brief Get whether the focuser is absolute.
     */
    virtual bool get_absolute() const = 0;

    /**
     * @brief Get whether the focuser is moving.
     */
    virtual bool get_is_moving() const = 0;

    /**
     * @brief Get the maximum step position.
     */
    virtual int get_max_step() const = 0;

    /**
     * @brief Get the maximum increment.
     */
    virtual int get_max_increment() const = 0;

    /**
     * @brief Get the current position.
     */
    virtual int get_position() const = 0;

    /**
     * @brief Get the step size in microns.
     */
    virtual double get_step_size() const = 0;

    /**
     * @brief Get whether the focuser supports temperature compensation.
     */
    virtual bool get_temp_comp_available() const = 0;

    /**
     * @brief Get whether temperature compensation is active.
     */
    virtual bool get_temp_comp() const = 0;

    /**
     * @brief Set whether temperature compensation is active.
     */
    virtual void set_temp_comp(bool temp_comp) = 0;

    /**
     * @brief Get the temperature.
     */
    virtual double get_temperature() const = 0;

    // Focuser-specific methods

    /**
     * @brief Halt the focuser movement.
     */
    virtual void halt() = 0;

    /**
     * @brief Move the focuser to an absolute position.
     *
     * @param position Target position
     */
    virtual void move(int position) = 0;
};

} // namespace alpacacore

