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
#include <alpacacore/alpaca_errors.h>

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
    explicit AlpacaException(const std::string& message,
                             int error_code = AlpacaError::DriverException)
        : std::runtime_error(message)
        , error_code_(error_code) {}
    
    explicit AlpacaException(const char* message,
                             int error_code = AlpacaError::DriverException)
        : std::runtime_error(message)
        , error_code_(error_code) {}
    
    int error_code() const noexcept { return error_code_; }

private:
    int error_code_;
};

} // namespace alpacacore
