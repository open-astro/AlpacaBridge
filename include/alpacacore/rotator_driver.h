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
 * @brief Pure virtual interface for Alpaca Rotator drivers.
 *
 * Follows ASCOM Alpaca Rotator API specification.
 * All rotator drivers must implement this interface.
 */
class RotatorDriver : public AlpacaDriver {
public:
    virtual ~RotatorDriver() = default;

    // Rotator-specific properties

    /**
     * @brief Get whether the rotator can reverse.
     */
    virtual bool get_can_reverse() const = 0;

    /**
     * @brief Get whether the rotator is moving.
     */
    virtual bool get_is_moving() const = 0;

    /**
     * @brief Get the mechanical position in degrees.
     */
    virtual double get_mechanical_position() const = 0;

    /**
     * @brief Get the current position in degrees.
     */
    virtual double get_position() const = 0;

    /**
     * @brief Get the step size in degrees.
     */
    virtual double get_step_size() const = 0;

    /**
     * @brief Get the target position in degrees.
     */
    virtual double get_target_position() const = 0;

    /**
     * @brief Set the target position in degrees.
     */
    virtual void set_target_position(double position) = 0;

    // Rotator-specific methods

    /**
     * @brief Halt the rotator movement.
     */
    virtual void halt() = 0;

    /**
     * @brief Move the rotator to an absolute position.
     *
     * @param position Target position in degrees
     */
    virtual void move(double position) = 0;

    /**
     * @brief Move the rotator to an absolute position asynchronously.
     *
     * @param position Target position in degrees
     */
    virtual void move_absolute(double position) = 0;

    /**
     * @brief Move the rotator by a relative amount.
     *
     * @param position Relative position in degrees
     */
    virtual void move_mechanical(double position) = 0;

    /**
     * @brief Sync the rotator to a position.
     *
     * @param position Position in degrees
     */
    virtual void sync(double position) = 0;
};

} // namespace alpacacore

