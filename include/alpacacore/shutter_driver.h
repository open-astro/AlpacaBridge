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

