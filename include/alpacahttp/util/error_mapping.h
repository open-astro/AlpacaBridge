// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#pragma once

#include <string>
#include <exception>
#include <cstdint>

namespace alpacahttp::util {

// Alpaca error codes
namespace ErrorCode {
    constexpr std::int32_t SUCCESS = 0;
    constexpr std::int32_t INVALID_VALUE = 0x400;  // 1024
    constexpr std::int32_t VALUE_NOT_SET = 0x401;  // 1025
    constexpr std::int32_t NOT_CONNECTED = 0x407;  // 1031
    constexpr std::int32_t NOT_IMPLEMENTED = 0x43C; // 1084
    constexpr std::int32_t INVALID_WHILE_PARKED = 0x44C; // 1100
    constexpr std::int32_t INVALID_WHILE_SLAVED = 0x44D; // 1101
    constexpr std::int32_t INVALID_OPERATION = 0x44E; // 1102
    constexpr std::int32_t ACTION_NOT_IMPLEMENTED = 0x44F; // 1103
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

