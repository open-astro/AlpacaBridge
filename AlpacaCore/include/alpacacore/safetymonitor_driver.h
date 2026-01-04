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

