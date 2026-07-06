// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <alpacacore/alpaca_errors.h>

#include <cstdint>
#include <exception>
#include <string>

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
