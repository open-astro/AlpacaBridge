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

namespace alpacahttp::util {

// Alpaca error codes
namespace ErrorCode {
    constexpr std::int32_t SUCCESS = 0;
    constexpr std::int32_t NOT_IMPLEMENTED = 0x400;  // 1024
    constexpr std::int32_t INVALID_VALUE = 0x401;  // 1025
    constexpr std::int32_t VALUE_NOT_SET = 0x402;  // 1026
    constexpr std::int32_t NOT_CONNECTED = 0x407;  // 1031
    constexpr std::int32_t INVALID_WHILE_PARKED = 0x408; // 1032
    constexpr std::int32_t INVALID_WHILE_SLAVED = 0x409; // 1033
    constexpr std::int32_t INVALID_OPERATION = 0x40B; // 1035
    constexpr std::int32_t ACTION_NOT_IMPLEMENTED = 0x40C; // 1036
    constexpr std::int32_t DRIVER_ERROR = 0x500; // 1280
    constexpr std::int32_t DRIVER_NOT_READY = 0x501; // 1281
    constexpr std::int32_t NOT_SAFE = 0x502; // 1282
}

// Map exception to Alpaca error code and message
std::int32_t exception_to_error_code(const std::exception& e);
std::string exception_to_error_message(const std::exception& e);

// Map generic error to Alpaca error
std::int32_t map_error_code(int error_code);
std::string map_error_message(std::int32_t alpaca_error_code);

} // namespace alpacahttp::util
