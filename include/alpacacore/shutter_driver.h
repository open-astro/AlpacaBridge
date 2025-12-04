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

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>

namespace alpacacore {

/**
 * @brief Shutter state.
 */
enum class ShutterState {
    Open,
    Closed,
    Opening,
    Closing,
    Error
};

/**
 * @brief Pure virtual interface for Alpaca Shutter drivers.
 *
 * Follows ASCOM Alpaca Shutter API specification.
 * All shutter drivers must implement this interface.
 */
class ShutterDriver : public AlpacaDriver {
public:
    virtual ~ShutterDriver() = default;

    // Shutter-specific properties

    /**
     * @brief Get the shutter state.
     */
    virtual ShutterState get_shutter_state() const = 0;

    // Shutter-specific methods

    /**
     * @brief Open the shutter.
     */
    virtual void open() = 0;

    /**
     * @brief Close the shutter.
     */
    virtual void close() = 0;
};

} // namespace alpacacore

