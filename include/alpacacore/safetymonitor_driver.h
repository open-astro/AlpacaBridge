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
 * @brief Pure virtual interface for Alpaca SafetyMonitor drivers.
 *
 * Follows ASCOM Alpaca SafetyMonitor API specification.
 * All safety monitor drivers must implement this interface.
 */
class SafetyMonitorDriver : public AlpacaDriver {
public:
    virtual ~SafetyMonitorDriver() = default;

    // SafetyMonitor-specific properties

    /**
     * @brief Get whether the safety condition is safe.
     *
     * @return true if safe, false if unsafe
     */
    virtual bool get_is_safe() const = 0;
};

} // namespace alpacacore

