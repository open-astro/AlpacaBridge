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

#include <alpacahttp/util/error_mapping.h>
#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace alpacahttp::util {

std::int32_t exception_to_error_code(const std::exception& e) {
    if (const auto* alpaca_ex = dynamic_cast<const alpacacore::AlpacaException*>(&e)) {
        return map_error_code(alpaca_ex->error_code());
    }

    // Try to map common exception types to Alpaca error codes
    const std::string& what = e.what();
    std::string what_lower = what;
    std::transform(what_lower.begin(), what_lower.end(), what_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    
    // Check for common error patterns
    if (what_lower.find("not connected") != std::string::npos) {
        return ErrorCode::NOT_CONNECTED;
    }
    
    if (what_lower.find("not implemented") != std::string::npos) {
        return ErrorCode::NOT_IMPLEMENTED;
    }

    if (what_lower.find("not supported") != std::string::npos) {
        return ErrorCode::NOT_IMPLEMENTED;
    }

    if (what_lower.find("value not set") != std::string::npos ||
        what_lower.find("not been set") != std::string::npos ||
        what_lower.find("not set") != std::string::npos) {
        return ErrorCode::VALUE_NOT_SET;
    }
    
    if (what_lower.find("invalid value") != std::string::npos ||
        what_lower.find("invalid form value") != std::string::npos ||
        what_lower.find("invalid json value") != std::string::npos ||
        what_lower.find("missing parameter") != std::string::npos ||
        what_lower.find("missing '") != std::string::npos ||
        what_lower.find("missing \"") != std::string::npos ||
        what_lower.find("invalid utc date format") != std::string::npos) {
        return ErrorCode::INVALID_VALUE;
    }
    
    // Default to driver error
    return ErrorCode::DRIVER_ERROR;
}

std::string exception_to_error_message(const std::exception& e) {
    return e.what();
}

std::int32_t map_error_code(int error_code) {
    namespace AE = alpacacore::AlpacaError;
    switch (error_code) {
        case AE::InvalidValue:
            return ErrorCode::INVALID_VALUE;
        case AE::ValueNotSet:
            return ErrorCode::VALUE_NOT_SET;
        case AE::NotConnected:
            return ErrorCode::NOT_CONNECTED;
        case AE::NotImplemented:
            return ErrorCode::NOT_IMPLEMENTED;
        case AE::ActionNotImplemented:
            return ErrorCode::ACTION_NOT_IMPLEMENTED;
        case AE::InvalidOperation:
            return ErrorCode::INVALID_OPERATION;
        case AE::InvalidWhileParked:
            return ErrorCode::INVALID_WHILE_PARKED;
        case AE::InvalidWhileSlaved:
            return ErrorCode::INVALID_WHILE_SLAVED;
        default:
            return ErrorCode::DRIVER_ERROR;
    }
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
        default:
            return "Unknown error";
    }
}

} // namespace alpacahttp::util
