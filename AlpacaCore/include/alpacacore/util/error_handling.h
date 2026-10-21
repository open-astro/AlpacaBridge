// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

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

    /**
     * @brief An InvalidOperation answer to a client that asked before the
     * device had one (issue #1167): ImageArray before any exposure, or while
     * one is still running.
     *
     * The ASCOM contract requires the error, so the router logs it at DEBUG
     * rather than ERROR. The code and message sent to the client are those of
     * any other InvalidOperation. Do not use it for an InvalidOperation that
     * reports a fault (a failed or stopped exposure): that one stays at ERROR.
     */
    static AlpacaException asked_too_early(const std::string& message) {
        AlpacaException e(message, AlpacaError::InvalidOperation);
        e.asked_too_early_ = true;
        return e;
    }

    bool is_asked_too_early() const noexcept { return asked_too_early_; }

private:
    int error_code_;
    bool asked_too_early_ = false;
};

} // namespace alpacacore
