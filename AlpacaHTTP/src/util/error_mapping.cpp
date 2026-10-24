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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacahttp/util/error_mapping.h>

#include <stdexcept>

namespace alpacahttp::util {

std::int32_t exception_to_error_code(const std::exception& e) {
    if (const auto* alpaca_ex = dynamic_cast<const alpacacore::AlpacaException*>(&e)) {
        return map_error_code(alpaca_ex->error_code());
    }
    // Any other exception is a driver failure. The old tail guessed a code from
    // substrings of e.what(); no in-tree throw reached it (#1476).
    return ErrorCode::DRIVER_ERROR;
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
