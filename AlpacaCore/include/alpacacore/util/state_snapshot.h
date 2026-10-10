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
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace alpacacore::util {

/**
 * @brief One device state frame served to getters from memory, with the time
 *        it was measured (decision record docs/decisions/0010-snapshot-served-state.md).
 *
 * One publisher (a poll) fills it; any number of threads read and write.
 *
 * - read() returns the value, the `measured_at` of the frame and a stale flag
 *   (age on the TaskClock greater than `max_age`). A stale value is still
 *   returned; the driver decides what stale means for each getter.
 * - write() is the write-through: a client write applies its change to the
 *   snapshot at once, so the next read sees it before the next poll runs.
 * - Publish-sequence guard: a poll takes begin_poll() before it samples the
 *   device and passes the token to publish(). A write that landed after the
 *   token was taken makes publish() drop the frame and return false, so a
 *   poll that sampled before a write cannot overwrite it. The next poll
 *   publishes normally.
 *
 * The mutex is held only for the copy in and out, never across device I/O.
 */
template <typename T>
class StateSnapshot {
public:
    using clock = TaskClock::clock;
    using Token = std::uint64_t;

    struct Reading {
        T value;
        clock::time_point measured_at{};
        bool stale = false;
    };

    StateSnapshot(const TaskClock& task_clock, std::chrono::nanoseconds max_age)
        : task_clock_(task_clock), max_age_(max_age) {}

    /// Call before sampling the device; pass the result to publish().
    Token begin_poll() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return write_epoch_;
    }

    /// Stores a polled frame measured now. False (frame dropped) when a write
    /// landed after `token` was taken.
    bool publish(Token token, T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token != write_epoch_) {
            return false;
        }
        value_ = std::move(value);
        measured_at_ = task_clock_.now();
        return true;
    }

    /// Write-through: `apply` changes the held frame in place; `measured_at`
    /// stays at the last publish, because a client write is not a measurement.
    /// With no frame held (before the first publish, after reset()) the write
    /// is not retained and `apply` does not run, but it still drops a poll in
    /// flight: the next poll measures the device. `apply` runs under the
    /// snapshot mutex: it must not block or do device I/O.
    template <typename Apply>
    void write(Apply&& apply) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++write_epoch_;
        if (!value_.has_value()) {
            return;
        }
        apply(*value_);
    }

    /// The held frame, or nullopt before the first publish or after reset().
    std::optional<Reading> read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!value_.has_value()) {
            return std::nullopt;
        }
        return Reading{*value_, measured_at_, (task_clock_.now() - measured_at_) > max_age_};
    }

    /// At connect and disconnect: drop the frame. A poll in flight is dropped too.
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.reset();
        ++write_epoch_;
    }

private:
    const TaskClock& task_clock_;
    std::chrono::nanoseconds max_age_;
    mutable std::mutex mutex_;
    std::optional<T> value_;
    clock::time_point measured_at_{};
    Token write_epoch_ = 0;
};

}  // namespace alpacacore::util
