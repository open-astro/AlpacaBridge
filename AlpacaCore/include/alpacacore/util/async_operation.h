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
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::util {

/**
 * Why a body should stop (CONTEXT.md, "Cancelled vs superseded").
 *
 * Cancelled: a client asked the running operation to stop (AbortSlew, Halt,
 * MoveAxis(axis, 0), disconnect); the body brings the device to a safe stop
 * and publishes that state. Superseded: a newer operation now owns the same
 * axes or device; the body touches neither hardware nor driver state.
 */
enum class StopReason : std::uint8_t { None, Cancelled, Superseded };

/**
 * Driver-wide generation, shared by every slot that owns the same axes or
 * device (rule 4). Which slots share one is the driver's choice; a driver may
 * also bump() it directly for a motion command that runs without a slot. A
 * bump does not wake a body waiting on another slot (rule 6): the body sees it
 * at its next wait_for() return or stop_reason() call.
 */
class OperationGeneration {
public:
    OperationGeneration() = default;
    OperationGeneration(const OperationGeneration&) = delete;
    OperationGeneration& operator=(const OperationGeneration&) = delete;

    /// Increments and returns the new value.
    std::uint64_t bump() { return value_.fetch_add(1, std::memory_order_acq_rel) + 1; }
    std::uint64_t current() const { return value_.load(std::memory_order_acquire); }

private:
    std::atomic<std::uint64_t> value_{0};
};

class AsyncOperation;

/**
 * Handed to a body; valid only inside that body.
 */
class OperationContext {
public:
    OperationContext(const OperationContext&) = delete;
    OperationContext& operator=(const OperationContext&) = delete;
    ~OperationContext() = default;

    /// Waits `d` through the slot's TaskClock (rule 6). True when the full
    /// time passed and the body may continue; false when it has been stopped,
    /// a stop at the deadline included.
    inline bool wait_for(std::chrono::nanoseconds d);
    inline StopReason stop_reason() const;
    /// The token taken at start (rule 4).
    std::uint64_t generation() const { return token_; }

private:
    friend class AsyncOperation;
    struct Body;
    OperationContext(AsyncOperation& slot, Body& body, std::uint64_t token) : slot_(slot), body_(body), token_(token) {}

    AsyncOperation& slot_;
    Body& body_;
    std::uint64_t token_;
};

// One body's state. Every field but `thread` is guarded by the slot mutex;
// `token` is written by start() under that mutex before the thread's first
// action (it locks and unlocks the mutex) and never changed.
struct OperationContext::Body {
    std::thread thread;
    std::uint64_t token = 0;
    bool superseded = false;  // replaced by a start() on its own slot
    bool cancelled = false;   // reached by cancel() or cancel_all_and_join()
    bool returned = false;    // the body has returned; the thread can be joined at once
};

/**
 * The async operation slot (issue #717, decision record
 * docs/decisions/0006-async-operation-ownership.md): owns one cancellable
 * background body at a time, starts a new body without waiting for the old
 * one, keeps at most kMaxStaleBodies replaced bodies, tells a cancelled body
 * from a superseded one, and keeps the last failure. Every wait goes through
 * the injected TaskClock (decision 0005).
 *
 * Rule 10, lock order and calling rules: driver mutex_, then the slot mutex.
 * The slot never runs a body, a log call or a user callback while holding its
 * mutex. start() and cancel_all_and_join() must be called WITHOUT the driver
 * mutex held, since a stale body may need it to return (the Sky-Watcher
 * slew body takes it to clean up); cancel(), running(), stale_count() and
 * last_failure() may be called with it held.
 *
 * Neither start() nor cancel_all_and_join() may be called from inside a body
 * of the same slot. cancel_all_and_join() from a body takes that body and
 * joins its own thread: join() throws std::system_error, and the unwinding
 * then destroys that body's still-joinable std::thread, so the process ends
 * in std::terminate(). start() from a body counts that body as still
 * running: it replaces it when the stale bound has room, but with
 * kMaxStaleBodies replaced bodies already running it waits up to
 * kStaleReapTimeout for one to return and throws
 * AlpacaException(InvalidOperation) if none does. Use cancel() or
 * ctx.stop_reason() from a body instead.
 *
 * Rule 11: neither copyable nor movable; it owns a mutex and threads that
 * capture `this`.
 */
class AsyncOperation {
public:
    static constexpr std::size_t kMaxStaleBodies = 3;
    static constexpr std::chrono::milliseconds kStaleReapTimeout{2000};

    AsyncOperation(std::string name, OperationGeneration& generation, TaskClock& clock = default_task_clock())
        : name_(std::move(name)), generation_(generation), clock_(clock) {}

    AsyncOperation(const AsyncOperation&) = delete;
    AsyncOperation& operator=(const AsyncOperation&) = delete;
    AsyncOperation(AsyncOperation&&) = delete;
    AsyncOperation& operator=(AsyncOperation&&) = delete;

    /// Rule 8: cancels the current body, wakes every body and joins every
    /// thread with no time bound. Rule 10: destroy without the driver mutex.
    ~AsyncOperation() { cancel_all_and_join(); }

    /**
     * Starts `body` on a new thread and returns its generation token.
     *
     * Rule 10: call without the driver mutex held. Rule 2: never waits for
     * the body it replaces; rule 3 is its only wait, at most
     * kStaleReapTimeout. Throws AlpacaException(InvalidOperation) when the
     * stale bound does not clear in time, and AlpacaException(DriverException)
     * when the OS refuses a thread (EAGAIN); both change nothing.
     */
    std::uint64_t start(std::function<void(OperationContext&)> body) {
        // Rule 1: the capacity check, the move into the stale list, the bump,
        // the spawn and the assignment all run under this one lock, with no
        // joinable() pre-check outside it.
        std::unique_lock<std::mutex> lock(mutex_);

        // Rule 2: join every stale body that has already returned. Joining
        // under the lock is safe: a body publishes `returned` as the last
        // thing its thread does under the lock, and after that the thread
        // only releases the lock and exits.
        reap_returned_locked();

        // Rule 3: at most kMaxStaleBodies replaced bodies still running. If
        // replacing a running current body would make that one more, wait on
        // the clock for a stale body to return (the oldest, in the blocked-
        // link case), at most kStaleReapTimeout. The predicate recounts under
        // the lock, so a concurrent start() or cancel_all_and_join() that
        // changes the lists is seen, and no removed body is referenced.
        if (would_be_running_stale_locked() > kMaxStaleBodies) {
            const bool room = clock_.wait_for(lock, cv_, kStaleReapTimeout,
                                              [this] { return would_be_running_stale_locked() <= kMaxStaleBodies; });
            if (!room) {
                // Changes nothing: the current body is not superseded, the
                // generation is not bumped, last_failure() is not cleared.
                throw AlpacaException(name_ + ": the previous operations are still finishing; try again",
                                      AlpacaError::InvalidOperation);
            }
            reap_returned_locked();
        }

        // The thread is created before anything else changes, so a refused
        // thread (EAGAIN) leaves the current body, the generation and the kept
        // failure as they were. The thread's first action is to lock mutex_,
        // which start() holds until it returns: the body cannot run, and
        // `token` cannot be read, before the bookkeeping below is done.
        auto next = std::make_unique<OperationContext::Body>();
        OperationContext::Body* raw = next.get();
        try {
            next->thread = spawn_([this, raw, fn = std::move(body)]() mutable { run(*raw, fn); });
        } catch (const std::system_error& e) {
            throw AlpacaException(name_ + ": could not start a background thread: " + e.what(),
                                  AlpacaError::DriverException);
        }

        // Rule 2: mark the current body Superseded, wake it and move it to
        // the stale list without joining it (a body that has already
        // returned is joined at once instead).
        if (current_) {
            current_->superseded = true;
            if (current_->returned) {
                current_->thread.join();
            } else {
                stale_.push_back(std::move(current_));
            }
            current_.reset();
            cv_.notify_all();
        }

        // Rule 4: every start bumps the shared generation; the new value is
        // the body's token.
        const std::uint64_t token = generation_.bump();
        // Rule 9: a successful start clears the kept failure, and from now on
        // only the new body's throw may be kept.
        latest_token_ = token;
        last_failure_.reset();
        next->token = token;
        current_ = std::move(next);
        return token;
    }

    /// Replaces the thread factory, for a test that must make thread creation
    /// fail. Call before the first start().
    void set_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) { spawn_ = std::move(spawn); }

    /// Rule 7: marks the current body Cancelled (rule 5 precedence still
    /// applies) and wakes it. Never joins and never blocks on a body, so it
    /// may be called with the driver mutex held. Stale bodies are not touched.
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_) {
            current_->cancelled = true;
        }
        cv_.notify_all();
    }

    /**
     * Rule 8, the disconnect path: marks the current body Cancelled, wakes
     * every body (current and stale) and joins every thread with no time
     * bound. The slot never calls detach(). A body blocked in a call with no
     * timeout of its own is the caller's to bound. Afterwards the slot is
     * empty and a later start() works. Rule 10: call without the driver mutex.
     */
    void cancel_all_and_join() {
        std::vector<std::unique_ptr<OperationContext::Body>> bodies;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_) {
                current_->cancelled = true;
                bodies.push_back(std::move(current_));
                current_.reset();
            }
            for (auto& stale : stale_) {
                bodies.push_back(std::move(stale));
            }
            stale_.clear();
            cv_.notify_all();
        }
        // Joined outside the lock: a body needs the slot mutex to wait,
        // check its reason and publish that it returned.
        for (auto& b : bodies) {
            if (b->thread.joinable()) {
                b->thread.join();
            }
        }
    }

    /// True while the current body has not returned. May be called with the
    /// driver mutex held (rule 10).
    bool running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_ && !current_->returned;
    }

    /// Replaced bodies not yet joined. May be called with the driver mutex
    /// held (rule 10).
    std::size_t stale_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stale_.size();
    }

    /// The throw of the body the last successful start() spawned (rule 9),
    /// until the next successful start(); a stale body's throw is logged
    /// instead. May be called with the driver mutex held (rule 10).
    std::optional<std::string> last_failure() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_failure_;
    }

private:
    friend class OperationContext;

    // Caller holds mutex_.
    void reap_returned_locked() {
        for (auto it = stale_.begin(); it != stale_.end();) {
            if ((*it)->returned) {
                (*it)->thread.join();
                it = stale_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Caller holds mutex_. How many replaced bodies would still be running
    // if the current body were replaced now.
    std::size_t would_be_running_stale_locked() const {
        std::size_t n = 0;
        for (const auto& b : stale_) {
            if (!b->returned) {
                ++n;
            }
        }
        if (current_ && !current_->returned) {
            ++n;
        }
        return n;
    }

    // Rule 5, caller holds mutex_. Superseded (replaced on its own slot, or
    // the shared generation moved past the token) takes precedence over
    // Cancelled whenever both apply: a superseded body must never send a stop
    // to axes a newer operation owns. Both conditions only ever become true,
    // so the reason is sticky.
    StopReason stop_reason_locked(const OperationContext::Body& b) const {
        if (b.superseded || generation_.current() != b.token) {
            return StopReason::Superseded;
        }
        if (b.cancelled) {
            return StopReason::Cancelled;
        }
        return StopReason::None;
    }

    // The thread function.
    void run(OperationContext::Body& b, std::function<void(OperationContext&)>& fn) {
        // Wait for start() to finish its bookkeeping (see start()).
        { std::lock_guard<std::mutex> started(mutex_); }
        std::optional<std::string> failure;
        // Rule 9: a throw never reaches std::terminate.
        try {
            OperationContext ctx(*this, b, b.token);
            fn(ctx);
        } catch (const std::exception& e) {
            failure = e.what();
        } catch (...) {
            failure = "unknown exception";
        }
        // Rule 10: the body's captures are destroyed outside the slot mutex.
        fn = nullptr;

        bool stale_failure = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failure) {
                // Kept only from the body the slot started last. A body
                // replaced by start(), or taken by cancel_all_and_join()
                // before a later start(), is stale: its throw must not
                // overwrite what that later start() cleared.
                if (b.token != latest_token_) {
                    stale_failure = true;
                } else {
                    last_failure_ = failure;
                }
            }
            if (!stale_failure) {
                b.returned = true;
                cv_.notify_all();
                return;
            }
        }
        // Rule 9: a stale body's throw is logged, not kept; rule 10: never
        // while holding the slot mutex.
        ALPACA_LOG_WARN(name_, "stale operation failed: " + *failure);
        std::lock_guard<std::mutex> lock(mutex_);
        b.returned = true;
        cv_.notify_all();
    }

    const std::string name_;
    OperationGeneration& generation_;
    TaskClock& clock_;
    std::function<std::thread(std::function<void()>)> spawn_ = [](std::function<void()> f) {
        return std::thread(std::move(f));
    };

    mutable std::mutex mutex_;
    // One condition variable for every body's wait and for start()'s rule 3
    // wait; notify_all() wakes each waiter to recheck its own predicate.
    std::condition_variable cv_;
    std::unique_ptr<OperationContext::Body> current_;
    std::vector<std::unique_ptr<OperationContext::Body>> stale_;  // oldest first
    std::optional<std::string> last_failure_;
    std::uint64_t latest_token_ = 0;  // token of the body the last successful start() spawned
};

// Rule 6: waits through the slot's TaskClock on the slot mutex and condition
// variable, and returns stop_reason() == None evaluated under that mutex. A
// cancel or a replacement on the body's own slot wakes it at once; a bump of
// the shared generation elsewhere is seen when the wait returns.
inline bool OperationContext::wait_for(std::chrono::nanoseconds d) {
    std::unique_lock<std::mutex> lock(slot_.mutex_);
    return !slot_.clock_.wait_for(lock, slot_.cv_, d,
                                  [this] { return slot_.stop_reason_locked(body_) != StopReason::None; });
}

inline StopReason OperationContext::stop_reason() const {
    std::lock_guard<std::mutex> lock(slot_.mutex_);
    return slot_.stop_reason_locked(body_);
}

}  // namespace alpacacore::util
