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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>

#include <chrono>

namespace alpacacore::util {

/**
 * The LastExposureDuration / LastExposureStartTime a camera reports (ICameraV4, issue #1477).
 *
 * Both raise InvalidOperation until an image has been successfully acquired. A driver keeps the values of the
 * exposure in flight in its own pending fields (PercentCompleted needs them) and calls commit() only when that
 * exposure's image is acquired, so a failed exposure never publishes values and never erases the ones of an
 * earlier success. clear() is for a disconnect or reconnect. Not thread-safe: the owner calls it under its state
 * lock.
 */
class LastExposure {
public:
    void commit(double duration_seconds, std::chrono::system_clock::time_point start) {
        duration_ = duration_seconds;
        start_ = start;
        valid_ = true;
    }

    void clear() { *this = LastExposure{}; }

    double duration() const {
        require_valid();
        return duration_;
    }

    std::chrono::system_clock::time_point start_time() const {
        require_valid();
        return start_;
    }

private:
    void require_valid() const {
        if (!valid_) {
            throw AlpacaException("No image has been successfully acquired yet", AlpacaError::InvalidOperation);
        }
    }

    double duration_ = 0.0;
    std::chrono::system_clock::time_point start_{};
    bool valid_ = false;
};

}  // namespace alpacacore::util
