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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace alpacacore::util {

/**
 * The clock every driver wait and deadline goes through (issue #697,
 * decision record docs/decisions/0005-task-clock.md).
 *
 * Real by default (RealTaskClock, over std::chrono::steady_clock), virtual in
 * tests (alpacacore::test::FakeTaskClock in AlpacaCore/tests/fake_task_clock.h),
 * so a test drives a driver timer with advance() instead of sleeping for it.
 * This clock is for waits and deadlines only: pointing time (LST) and the
 * UTC value of a UTCDate write stay off it (decision 0001, a different
 * contract). The host time elapsed since such a write reads this clock
 * through util::UtcOffset (decision 0014).
 */
class TaskClock {
public:
    using clock = std::chrono::steady_clock;

    /// Monotonic time for deadlines.
    virtual clock::time_point now() const = 0;

    /**
     * Cancellable wait, with the semantics of
     * std::condition_variable::wait_for with a predicate.
     *
     * Called with `lock` held on the caller's mutex; returns with it held.
     * Returns when pred() is true or `timeout` has passed, and returns pred()
     * evaluated under the lock at that moment, so a cancel set before the
     * deadline, or at the same moment, reports true ("cancel wins"). A pred()
     * that is already true returns true at once without blocking. A timeout
     * of zero or less evaluates pred() once and returns. A timeout too large
     * for the clock waits until pred() is true.
     */
    virtual bool wait_for(std::unique_lock<std::mutex>& lock, std::condition_variable& cv,
                          std::chrono::nanoseconds timeout, const std::function<bool()>& pred) = 0;

    /// Plain, uncancellable sleep.
    virtual void sleep_for(std::chrono::nanoseconds duration) = 0;

protected:
    // Protected and non-virtual (AGENTS.md, "Prefer the QHY form for a new
    // seam"): nothing owns a clock through the interface, so `delete` through
    // a TaskClock* must not compile.
    TaskClock() = default;
    TaskClock(const TaskClock&) = default;
    TaskClock& operator=(const TaskClock&) = default;
    ~TaskClock() = default;
};

/// The production clock: steady_clock, condition_variable::wait_for and
/// this_thread::sleep_for, with no other state.
class RealTaskClock final : public TaskClock {
public:
    clock::time_point now() const override { return clock::now(); }

    bool wait_for(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::chrono::nanoseconds timeout,
                  const std::function<bool()>& pred) override {
        // condition_variable::wait_for evaluates pred() a second time after
        // an already-expired wait; the contract above says once.
        if (timeout <= std::chrono::nanoseconds::zero()) {
            return pred();
        }
        // condition_variable::wait_for adds the timeout to clock::now(),
        // which overflows for a timeout near nanoseconds::max().
        const auto current = clock::now();
        if (timeout >= clock::time_point::max() - current) {
            return cv.wait_until(lock, clock::time_point::max(), pred);
        }
        return cv.wait_for(lock, timeout, pred);
    }

    void sleep_for(std::chrono::nanoseconds duration) override { std::this_thread::sleep_for(duration); }
};

/// One process-wide RealTaskClock (a function-local static). Drivers take a
/// TaskClock& that defaults to this; tests pass a FakeTaskClock instead.
inline TaskClock& default_task_clock() {
    static RealTaskClock clock;
    return clock;
}

}  // namespace alpacacore::util
