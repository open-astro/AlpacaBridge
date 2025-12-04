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

