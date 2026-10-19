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
#include <optional>

namespace alpacacore::util {

/**
 * @brief The UTC instant a client wrote to a mount, plus the host time that has
 *        passed since (decision 0014).
 *
 * The UTC value stays on std::chrono::system_clock: it is what the client (or
 * the mount, or the host) said, and no TaskClock produces it. The elapsed time
 * since that write is a monotonic duration, measured between two
 * TaskClock::clock::time_point values the caller takes from its TaskClock, so a
 * test moves the readback with FakeTaskClock::advance() instead of sleeping.
 *
 * Clock-free, the shape util::StreamLinkHealth uses: no clock member, every
 * method that needs the time takes `now`. Copyable, no virtuals, not a base
 * class. Not thread-safe: the caller guards it with the mutex that guards the
 * rest of its state.
 *
 * Overflow of the system_clock::duration cast in current() is not guarded; the
 * span between set() and current() is the lifetime of one connection.
 */
class UtcOffset {
public:
    using mono = TaskClock::clock::time_point;
    using utc = std::chrono::system_clock::time_point;

    /// A client write, or a value seeded from the mount or the host. Forgets any earlier anchor.
    void set(utc value, mono now) {
        value_ = value;
        anchor_ = now;
        valid_ = true;
    }

    /// `value + (now - anchor)`; nullopt until set() or after clear(). A `now` before the anchor subtracts.
    std::optional<utc> current(mono now) const {
        if (!valid_) {
            return std::nullopt;
        }
        return value_ + std::chrono::duration_cast<utc::duration>(now - anchor_);
    }

    bool valid() const { return valid_; }

    void clear() { valid_ = false; }

private:
    utc value_{};
    mono anchor_{};
    bool valid_ = false;
};

}  // namespace alpacacore::util
