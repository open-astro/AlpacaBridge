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

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

namespace alpacacore::test {

/**
 * The SDK camera fakes' one timed completion (issue #2693, decision 0005): an exposure the SDK reports done only once
 * its duration has passed on a TaskClock, and a guide call that blocks for its pulse where the SDK's does. A helper,
 * not a driver's connect path: it is listed in HELPER_FAKES in scripts/check_docs_drift.py. FakeQHYSDK,
 * FakePlayerOneSDK and FakeSVBSDK use it.
 *
 * Without a clock, the default, everything completes at once, so a fake's ordinary unit cases keep their instant
 * answers. The ConformU hosts (conformu_camera_factories.h) set util::default_task_clock(); a case sets a
 * FakeTaskClock and advances it.
 *
 * A value type with no lock of its own, so the fakes stay movable: call set_clock(), start(), clear(), ready(),
 * remaining() and wait_ready() under the owning fake's lock, or where the fake has none, where its caller serialises
 * the calls. Set the clock before a driver uses the fake. block_for() reads only the clock and takes no lock: call it
 * with none of the fake's own locks held, so only the caller waits for the pulse.
 */
class FakeTimedCompletion {
public:
    /// The clock completions are timed on; nullptr (the default) completes at once. Clears an armed completion.
    void set_clock(util::TaskClock* clock) {
        clock_ = clock;
        end_.reset();
    }

    /// Arms a completion `duration` from now (a negative duration is zero). No-op without a clock.
    void start(std::chrono::nanoseconds duration) {
        if (clock_ == nullptr) return;
        end_ = clock_->now() + std::max(duration, std::chrono::nanoseconds::zero());
    }

    /// Disarms the completion: ready() is true again.
    void clear() { end_.reset(); }

    /// True when nothing is armed or the armed duration has passed.
    bool ready() const { return remaining() == std::chrono::nanoseconds::zero(); }

    /// The time left before the armed completion; zero when ready().
    std::chrono::nanoseconds remaining() const {
        if (clock_ == nullptr || !end_) return std::chrono::nanoseconds::zero();
        return std::max(std::chrono::nanoseconds(*end_ - clock_->now()), std::chrono::nanoseconds::zero());
    }

    /// An SDK read that waits up to `max_wait` for the completion (SVBGetVideoData's wait_ms): blocks on the clock,
    /// with `lock` on the fake's mutex and `cv` its condition variable, until the completion is ready, `max_wait`
    /// has passed or `cancelled()` turns true (a capture stop that notifies `cv`). Returns ready().
    bool wait_ready(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::chrono::nanoseconds max_wait,
                    const std::function<bool()>& cancelled) const {
        const auto wait = std::min(remaining(), max_wait);
        if (wait > std::chrono::nanoseconds::zero()) {
            static_cast<void>(clock_->wait_for(lock, cv, wait, cancelled));
        }
        return ready();
    }

    /// A blocking SDK call that returns after `duration` on the clock (a guide pulse); at once without a clock.
    void block_for(std::chrono::nanoseconds duration) const {
        if (clock_ != nullptr && duration > std::chrono::nanoseconds::zero()) clock_->sleep_for(duration);
    }

private:
    util::TaskClock* clock_{nullptr};
    std::optional<util::TaskClock::clock::time_point> end_;
};

}  // namespace alpacacore::test
