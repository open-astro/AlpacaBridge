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

#include <alpacacore/util/task_clock.h>

#include <chrono>

namespace alpacacore::util {

/**
 * @brief A client-visible flag that stays true until a deadline (decision 0022).
 *
 * Slewing "stays true for N s after a goto" and IsPulseGuiding "stays true
 * until the pulse ends" are the same shape: a deadline armed at dispatch, read
 * as `window.active(now) || <the device reports it>`.
 *
 * Clock-free on purpose: the caller passes `now` (from its TaskClock), as
 * StreamLinkHealth does, and the type has no wait method, so it cannot become
 * a landing wait (open-astro#580). Not thread-safe: the driver guards it with
 * its own state lock.
 */
class HoldWindow {
public:
    using mono = TaskClock::clock::time_point;

    /// until = now + hold, unconditionally: a shorter hold shortens an open window.
    void arm(mono now, std::chrono::nanoseconds hold) { until_ = now + hold; }

    /// True strictly before the deadline.
    bool active(mono now) const { return now < until_; }

    void clear() { until_ = mono::min(); }

private:
    mono until_ = mono::min();
};

}  // namespace alpacacore::util
