// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <string>
#include <exception>
#include <cstdint>

#include <alpacacore/alpaca_errors.h>

namespace alpacahttp::util {

// AlpacaHTTP-facing names for the canonical ASCOM error numbers defined in
// alpacacore::AlpacaError (the single source of truth). Aliased — not
// re-declared — so the two layers can never drift apart.
namespace ErrorCode {
    constexpr std::int32_t SUCCESS = alpacacore::AlpacaError::Success;
    constexpr std::int32_t NOT_IMPLEMENTED = alpacacore::AlpacaError::NotImplemented;
    constexpr std::int32_t INVALID_VALUE = alpacacore::AlpacaError::InvalidValue;
    constexpr std::int32_t VALUE_NOT_SET = alpacacore::AlpacaError::ValueNotSet;
    constexpr std::int32_t NOT_CONNECTED = alpacacore::AlpacaError::NotConnected;
    constexpr std::int32_t INVALID_WHILE_PARKED = alpacacore::AlpacaError::InvalidWhileParked;
    constexpr std::int32_t INVALID_WHILE_SLAVED = alpacacore::AlpacaError::InvalidWhileSlaved;
    constexpr std::int32_t INVALID_OPERATION = alpacacore::AlpacaError::InvalidOperation;
    constexpr std::int32_t ACTION_NOT_IMPLEMENTED = alpacacore::AlpacaError::ActionNotImplemented;
    constexpr std::int32_t DRIVER_ERROR = alpacacore::AlpacaError::DriverException;
}

// Map exception to Alpaca error code and message
std::int32_t exception_to_error_code(const std::exception& e);
std::string exception_to_error_message(const std::exception& e);

// Map generic error to Alpaca error
std::int32_t map_error_code(int error_code);
std::string map_error_message(std::int32_t alpaca_error_code);

} // namespace alpacahttp::util
