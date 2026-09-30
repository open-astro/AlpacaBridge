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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace alpacacore::test {

/**
 * Virtual-time TaskClock for driver wait tests (issue #697, decision record
 * docs/decisions/0005-task-clock.md). A helper fake, not a driver's connect
 * path: it is listed in HELPER_FAKES in scripts/check_docs_drift.py.
 *
 * Virtual time starts at clock::time_point{} and moves only through
 * advance(). A test starts the code under test, rendezvous with it through
 * wait_for_waiters(n, bound), then advance()s instead of sleeping.
 *
 * The thread that calls advance() must hold no lock of the code under test
 * (decision 0005): advance() takes each due waiter's mutex.
 *
 * Lifetime: the caller's mutex and condition variable passed to wait_for()
 * must outlive any advance() running concurrently with that wait, since
 * advance() may lock and notify them just after the wait returned. The
 * test thread that calls advance() also owns the code under test, so this
 * holds whenever it destroys that code only after advance() returns.
 */
class FakeTaskClock final : public util::TaskClock {
public:
    /// Test-only hook, null by default. Called inside wait_for() after the
    /// predicate and deadline checks and before each block, with the
    /// caller's mutex held. It exists only for the lost-wakeup case in
    /// test_task_clock.cpp, the same shape as FakeQHYSDK::before_call. Read
    /// with no synchronisation: set it before the waiter starts, and clear it
    /// after the waiter has returned and before the clock is destroyed.
    std::function<void()> before_block;

    FakeTaskClock() = default;
    FakeTaskClock(const FakeTaskClock&) = delete;
    FakeTaskClock& operator=(const FakeTaskClock&) = delete;
    ~FakeTaskClock() = default;

    /// The virtual time. Each call is counted (now_calls()) under the same
    /// lock that reads the time, so a call counted after an advance() has
    /// returned saw the advanced time.
    clock::time_point now() const override {
        std::lock_guard<std::mutex> guard(mutex_);
        ++now_calls_;
        count_cv_.notify_all();
        return now_;
    }

    /// The number of now() calls so far.
    std::uint64_t now_calls() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return now_calls_;
    }

    /// Blocks the test thread until now() has been called at least `n` times
    /// in total, or until `real_timeout` of real time passes; returns whether
    /// `n` was reached. A driver loop that reads the clock once per pass is
    /// observed through this: take now_calls() after an advance() and wait
    /// for one more to know the loop has run once on the advanced time.
    bool wait_for_now_calls(std::uint64_t n, std::chrono::milliseconds real_timeout) {
        std::unique_lock<std::mutex> guard(mutex_);
        return count_cv_.wait_for(guard, real_timeout, [this, n] { return now_calls_ >= n; });
    }

    /**
     * If pred() is true, return true without registering. Otherwise register
     * a waiter {deadline = now() + timeout, the caller's mutex, &cv} while
     * the caller's mutex is held, and block on the caller's cv (a real
     * cv.wait, no real-time timeout) until pred() is true or the virtual
     * time has reached the deadline; then unregister, still under the
     * caller's mutex, and return pred(). A timeout of zero or less evaluates
     * pred() once and returns without registering.
     */
    bool wait_for(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::chrono::nanoseconds timeout,
                  const std::function<bool()>& pred) override {
        if (pred()) {
            return true;
        }
        if (timeout <= std::chrono::nanoseconds::zero()) {
            return false;
        }
        // Lock order: the caller's mutex (held), then the fake's mutex_.
        std::uint64_t id = 0;
        clock::time_point deadline;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            deadline = saturating_deadline(timeout);
            id = next_id_++;
            waiters_.emplace(id, Entry{deadline, lock.mutex(), &cv});
            count_cv_.notify_all();
        }
        while (!pred() && !reached(deadline)) {
            if (before_block) {
                before_block();
            }
            // Nothing can fall between the checks above and this block:
            // advance() notifies a wait_for waiter only while holding the
            // caller's mutex, which is held from the checks until cv.wait
            // releases it.
            cv.wait(lock);
        }
        {
            // Removed under the caller's mutex (still held) before return, so
            // advance(), which re-checks registration under that mutex, never
            // touches the entry after this wait has returned.
            std::lock_guard<std::mutex> guard(mutex_);
            waiters_.erase(id);
        }
        return pred();
    }

    /// Blocks until the virtual time reaches now() + duration, on the fake's
    /// own mutex and condition variable. It counts as a waiter.
    void sleep_for(std::chrono::nanoseconds duration) override {
        std::unique_lock<std::mutex> guard(mutex_);
        if (duration <= std::chrono::nanoseconds::zero()) {
            return;
        }
        const auto deadline = saturating_deadline(duration);
        const auto id = next_id_++;
        waiters_.emplace(id, Entry{deadline, nullptr, nullptr});
        count_cv_.notify_all();
        sleep_cv_.wait(guard, [this, deadline] { return now_ >= deadline; });
        waiters_.erase(id);
    }

    /**
     * Moves virtual time forward by `d` (zero or positive; a negative `d`
     * throws std::invalid_argument), then wakes every waiter whose deadline
     * is at or before the new time (due when now() >= deadline), and only
     * those.
     *
     * For each due wait_for waiter it takes that waiter's mutex, calls
     * notify_all() on its cv, and releases the mutex, so the notify cannot
     * fall between the waiter's predicate check and its block (decision
     * 0005). Lock order: a waiter holds its own mutex and then takes
     * mutex_, so advance() never holds mutex_ while taking a waiter's mutex:
     * it collects the due waiters under mutex_, releases it, then visits
     * each one, re-checking under the waiter's mutex that the entry is still
     * registered.
     */
    void advance(std::chrono::nanoseconds d) {
        if (d < std::chrono::nanoseconds::zero()) {
            throw std::invalid_argument("FakeTaskClock::advance: negative duration");
        }
        std::vector<std::pair<std::uint64_t, Entry>> due;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            now_ += d;
            for (const auto& [id, entry] : waiters_) {
                if (entry.mutex != nullptr && now_ >= entry.deadline) {
                    due.emplace_back(id, entry);
                }
            }
            // Sleepers wait on mutex_ itself, so this notify cannot be lost.
            sleep_cv_.notify_all();
        }
        for (const auto& [id, entry] : due) {
            std::lock_guard<std::mutex> waiter_lock(*entry.mutex);
            bool registered = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                registered = waiters_.count(id) != 0;
            }
            if (registered) {
                entry.cv->notify_all();
            }
        }
    }

    /// Blocks the test thread until at least `n` waiters (wait_for and
    /// sleep_for together) are registered, or until `real_timeout` of real
    /// time passes; returns whether `n` was reached. The rendezvous that
    /// replaces sleep-and-hope in tests.
    bool wait_for_waiters(std::size_t n, std::chrono::milliseconds real_timeout) {
        std::unique_lock<std::mutex> guard(mutex_);
        return count_cv_.wait_for(guard, real_timeout, [this, n] { return waiters_.size() >= n; });
    }

    /// The number of waiters (wait_for and sleep_for) registered now.
    std::size_t waiter_count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return waiters_.size();
    }

private:
    struct Entry {
        clock::time_point deadline;
        std::mutex* mutex;            // the caller's mutex; null for a sleep_for waiter
        std::condition_variable* cv;  // the caller's cv; null for a sleep_for waiter
    };

    // Caller holds mutex_. Clamps instead of overflowing on a huge timeout.
    clock::time_point saturating_deadline(std::chrono::nanoseconds timeout) const {
        const auto headroom = clock::time_point::max() - now_;
        if (timeout >= headroom) {
            return clock::time_point::max();
        }
        return now_ + std::chrono::duration_cast<clock::duration>(timeout);
    }

    bool reached(clock::time_point deadline) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return now_ >= deadline;
    }

    mutable std::mutex mutex_;
    std::condition_variable sleep_cv_;
    mutable std::condition_variable count_cv_;  // notified from now(), which is const
    clock::time_point now_{};
    mutable std::uint64_t now_calls_ = 0;
    std::uint64_t next_id_ = 0;
    std::map<std::uint64_t, Entry> waiters_;
};

}  // namespace alpacacore::test
