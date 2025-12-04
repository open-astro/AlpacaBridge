// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacahttp/util/error_mapping.h>
#include <stdexcept>
#include <sstream>

namespace alpacahttp::util {

std::int32_t exception_to_error_code(const std::exception& e) {
    // Try to map common exception types to Alpaca error codes
    const std::string& what = e.what();
    
    // Check for common error patterns
    if (what.find("not connected") != std::string::npos ||
        what.find("NotConnected") != std::string::npos) {
        return ErrorCode::NOT_CONNECTED;
    }
    
    if (what.find("not implemented") != std::string::npos ||
        what.find("NotImplemented") != std::string::npos) {
        return ErrorCode::NOT_IMPLEMENTED;
    }
    
    if (what.find("invalid value") != std::string::npos ||
        what.find("InvalidValue") != std::string::npos) {
        return ErrorCode::INVALID_VALUE;
    }
    
    // Default to driver error
    return ErrorCode::DRIVER_ERROR;
}

std::string exception_to_error_message(const std::exception& e) {
    return e.what();
}

std::int32_t map_error_code(int error_code) {
    // Map system error codes to Alpaca error codes if needed
    // For now, return as-is or map to DRIVER_ERROR
    return ErrorCode::DRIVER_ERROR;
}

std::string map_error_message(std::int32_t alpaca_error_code) {
    switch (alpaca_error_code) {
        case ErrorCode::SUCCESS:
            return "Success";
        case ErrorCode::INVALID_VALUE:
            return "Invalid value";
        case ErrorCode::VALUE_NOT_SET:
            return "Value not set";
        case ErrorCode::NOT_CONNECTED:
            return "Not connected";
        case ErrorCode::NOT_IMPLEMENTED:
            return "Not implemented";
        case ErrorCode::INVALID_WHILE_PARKED:
            return "Invalid while parked";
        case ErrorCode::INVALID_WHILE_SLAVED:
            return "Invalid while slaved";
        case ErrorCode::INVALID_OPERATION:
            return "Invalid operation";
        case ErrorCode::ACTION_NOT_IMPLEMENTED:
            return "Action not implemented";
        case ErrorCode::DRIVER_ERROR:
            return "Driver error";
        case ErrorCode::DRIVER_NOT_READY:
            return "Driver not ready";
        case ErrorCode::NOT_SAFE:
            return "Not safe";
        default:
            return "Unknown error";
    }
}

} // namespace alpacahttp::util

