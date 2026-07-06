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
 * @brief Pure virtual interface for Alpaca Rotator drivers.
 *
 * Follows ASCOM Alpaca Rotator API specification.
 * All rotator drivers must implement this interface.
 */
class RotatorDriver : public AlpacaDriver {
public:
    virtual ~RotatorDriver() = default;

    // Platform 7 operational state (IRotatorV4): IsMoving, MechanicalPosition,
    // Position plus a TimeStamp. Inline so the vtable stays weak; values come
    // from the same getters as the GET endpoints.
    std::vector<DeviceState> get_device_state() const override final {
        std::vector<DeviceState> state;
        auto add = [&state](const char* name, auto getter) {
            try {
                state.push_back({name, DeviceStateValue{getter()}});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Not currently known -- or an unwrapped vendor error -- so omit per the DeviceState contract.
            }
        };
        add("IsMoving", [this] { return get_is_moving(); });
        add("MechanicalPosition", [this] { return get_mechanical_position(); });
        add("Position", [this] { return get_position(); });
        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

    // Rotator-specific properties

    /**
     * @brief Get whether the rotator can reverse.
     */
    virtual bool get_can_reverse() const = 0;

    /**
     * @brief Get whether the rotator direction is reversed.
     */
    virtual bool get_reverse() const = 0;

    /**
     * @brief Set whether the rotator direction is reversed.
     */
    virtual void set_reverse(bool reverse) = 0;

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
