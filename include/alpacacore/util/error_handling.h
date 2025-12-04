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

#include <stdexcept>
#include <string>

namespace alpacacore {

/**
 * @brief Base exception class for all AlpacaCore errors.
 *
 * All exceptions thrown by AlpacaCore derive from this class.
 * Higher-level layers (e.g., AlpacaHTTP) map these exceptions to
 * Alpaca error numbers.
 */
class AlpacaException : public std::runtime_error {
public:
    explicit AlpacaException(const std::string& message)
        : std::runtime_error(message) {}
    
    explicit AlpacaException(const char* message)
        : std::runtime_error(message) {}
};

} // namespace alpacacore

