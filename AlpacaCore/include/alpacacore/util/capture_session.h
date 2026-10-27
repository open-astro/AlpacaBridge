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
#include <alpacacore/camera_driver.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/last_exposure.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace alpacacore::util {

class CaptureSession;

/**
 * Handed to a camera's exposure body; valid only inside that body. The body
 * owns the SDK calls (start, the readiness wait, the download); the session
 * owns the state, the time and the failure.
 */
class CaptureControl {
public:
    CaptureControl(const CaptureControl&) = delete;
    CaptureControl& operator=(const CaptureControl&) = delete;

    /// Waits `d` on the TaskClock, never past the watchdog deadline. False when the body must stop.
    inline bool wait_for(std::chrono::nanoseconds d);
    StopReason stop_reason() const { return ctx_.stop_reason(); }
    /// The SDK exposure has begun: stamps LastExposureStartTime and the PercentCompleted base.
    inline void exposure_started();
    /// The frame is being downloaded: CameraState Download.
    inline void reading_out();
    /// Publishes the frame. False, and the frame dropped, when this body no
    /// longer owns the session (aborted, timed out, disconnected).
    inline bool image_ready(std::shared_ptr<const ImageArray> image);

private:
    friend class CaptureSession;
    CaptureControl(CaptureSession& session, OperationContext& ctx, std::uint64_t token)
        : session_(session), ctx_(ctx), token_(token) {}

    CaptureSession& session_;
    OperationContext& ctx_;
    const std::uint64_t token_;
};

/**
 * One camera's exposure: its state, timestamps, watchdog and kept failure,
 * with the vendor body run in one util::AsyncOperation slot (decision 0006)
 * on the injected TaskClock (decision 0005).
 *
 * Locks: mutex_ is a leaf. While it is held the session calls no slot method,
 * body, log call or callback. Joins serialise on join_mutex_, taken without
 * mutex_. Call start(), abort(), disconnect_and_join() and the destructor
 * without the driver mutex; the getters, fail(), set_stopping(), discard_image()
 * and open() may run under it. A body never calls start(), abort(), disconnect_and_join()
 * or open().
 */
class CaptureSession {
public:
    using Body = std::function<void(CaptureControl&)>;
    /// The longest start() waits for the previous body to return.
    static constexpr std::chrono::milliseconds kStartTimeout{2000};

    CaptureSession(std::string name, TaskClock& clock, std::chrono::nanoseconds watchdog_margin)
        : name_(std::move(name)), clock_(clock), margin_(watchdog_margin), slot_(name_, generation_, clock) {}
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;
    ~CaptureSession() { disconnect_and_join(); }

    /// Starts `body` for an exposure of `duration_seconds`. Throws
    /// NotConnected on a closed session (after disconnect_and_join(), until
    /// open()), and InvalidOperation while exposing, or when the previous body
    /// or an abort's join has not finished within kStartTimeout; a refused
    /// start changes nothing.
    void start(double duration_seconds, Body body) {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);
        require_startable_locked();
        if (running_ > 0 || pending_ > 0) {
            // Bounded, with mutex_ released: only a body stranded in the SDK or a
            // join in flight keeps the session busy.
            (void)clock_.wait_for(lock, cv_, kStartTimeout, [this] {
                return !open_ || is_exposing_locked() || (running_ == 0 && pending_ == 0);
            });
            require_startable_locked();
            if (running_ > 0 || pending_ > 0) {
                throw AlpacaException(name_ + ": the previous exposure is still finishing; try again",
                                      AlpacaError::InvalidOperation);
            }
        }
        const Saved saved{state_, image_, failure_};
        const std::uint64_t token = ++last_token_;
        owner_ = token;
        stopping_ = false;
        state_ = State::Exposing;
        image_.reset();
        failure_.reset();
        duration_ = duration_seconds;
        started_ = false;
        start_time_ = std::chrono::system_clock::now();
        deadline_ =
            clock_.now() +
            std::chrono::duration_cast<TaskClock::clock::duration>(std::chrono::duration<double>(duration_seconds)) +
            margin_;
        ++running_;
        lock.unlock();

        try {
            slot_.start(
                [this, token, fn = std::move(body)](OperationContext& ctx) mutable { run_body(token, fn, ctx); });
        } catch (...) {  // AlpacaException, or std::bad_alloc from building the slot's function
            lock.lock();
            --running_;
            if (owner_ == token && open_) {  // still ours: nothing else moved the state
                owner_ = 0;
                state_ = saved.state;
                image_ = saved.image;
                failure_ = saved.failure;
            }
            cv_.notify_all();
            throw;
        }
    }

    /// Forwards to the slot's thread factory, for a test that must hold a body
    /// before it runs or make thread creation fail. Call with nothing running.
    void set_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        slot_.set_spawn_for_testing(std::move(spawn));
    }

    /// Cancels the exposure in flight and joins its body; the frame is
    /// discarded. A no-op when nothing is exposing.
    void abort() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!is_exposing_locked()) {
            return;
        }
        state_ = State::Idle;
        image_.reset();
        owner_ = 0;
        ++pending_;
        lock.unlock();
        join_all();
        lock.lock();
        --pending_;
        cv_.notify_all();
    }

    /// Closes the session, cancels and joins every body, then clears the
    /// image, the failure and the last exposure. A closed session refuses
    /// start() with NotConnected, a start() waiting for an old body included,
    /// so no body runs against a camera the driver is closing.
    void disconnect_and_join() {
        std::unique_lock<std::mutex> lock(mutex_);
        open_ = false;
        state_ = State::Idle;
        owner_ = 0;
        ++pending_;
        ++disconnecting_;
        cv_.notify_all();
        lock.unlock();
        join_all();
        lock.lock();
        image_.reset();
        failure_.reset();
        last_.clear();
        --pending_;
        --disconnecting_;
        cv_.notify_all();
    }

    /// Reopens the session that disconnect_and_join() closed (the driver's
    /// connect); may be called under the driver mutex. Throws
    /// InvalidOperation while a disconnect_and_join() is still running, and
    /// changes nothing then. A no-op when open.
    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (disconnecting_ > 0) {
            throw AlpacaException(name_ + ": cannot reopen while a disconnect is still running",
                                  AlpacaError::InvalidOperation);
        }
        open_ = true;
    }

    /// Drops a ready frame (a geometry change); keeps the last exposure.
    void discard_image() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Ready) {
            state_ = State::Idle;
            image_.reset();
        }
    }

    /// The driver fails the exposure in flight (an SDK stop that failed):
    /// kept like a body's throw, and the body is cancelled. A no-op when
    /// nothing is exposing.
    void fail(std::string message) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!is_exposing_locked()) {
            return;
        }
        fail_locked(lock, std::move(message));
    }

    /// The driver is about to stop the SDK exposure itself (a disconnect, an AbortExposure): a body that ends
    /// while this is set has its failure dropped, not kept, because the stop broke its download. The body
    /// still owns the session (wait_for and the state getters are unchanged). A later abort() or fail() ends
    /// the exposure; set_stopping(false) when the stop failed and the exposure runs on. A no-op when nothing
    /// is exposing; start() clears it.
    void set_stopping(bool stopping) {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = stopping && is_exposing_locked();
    }

    /// Applies the watchdog: an exposure past start + duration + margin fails.
    CameraState camera_state() const {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);
        switch (state_) {
            case State::Exposing:
                return CameraState::Exposing;
            case State::Download:
                return CameraState::Download;
            default:
                return CameraState::Idle;
        }
    }
    /// True while a body has not returned or a join is in flight: the
    /// exposure settings stay locked.
    bool busy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ > 0 || pending_ > 0;
    }
    /// Raises the kept failure as DriverException.
    bool image_ready() const {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);
        raise_failure_locked();
        return state_ == State::Ready;
    }
    /// The frame; DriverException after a failure, else AskedTooEarly (InvalidOperation) before one is ready.
    std::shared_ptr<const ImageArray> image() const {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);
        raise_failure_locked();
        if (state_ != State::Ready) {
            throw AlpacaException::asked_too_early(name_ + ": no image is ready");
        }
        return image_;
    }
    /// 0 to 100 on the TaskClock while exposing, 100 once the frame is being
    /// read out or is ready, else 0. The driver raises InvalidOperation
    /// outside Exposing (ICameraV4 leaves download progress to it).
    double percent_completed() const {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);
        if (state_ == State::Download || state_ == State::Ready) {
            return 100.0;
        }
        if (state_ != State::Exposing || !started_) {
            return 0.0;
        }
        if (duration_ <= 0.0) {
            return 100.0;
        }
        const std::chrono::duration<double> elapsed = clock_.now() - base_;
        return std::clamp(elapsed.count() / duration_ * 100.0, 0.0, 100.0);
    }
    /// InvalidOperation until an image has been acquired (util::LastExposure).
    double last_exposure_duration() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_.duration();
    }
    std::chrono::system_clock::time_point last_exposure_start_time() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_.start_time();
    }
    std::optional<std::string> failure() const {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);
        return failure_;
    }

private:
    friend class CaptureControl;
    enum class State : std::uint8_t { Idle, Exposing, Download, Ready, Failed };
    struct Saved {
        State state;
        std::shared_ptr<const ImageArray> image;
        std::optional<std::string> failure;
    };

    bool is_exposing_locked() const { return state_ == State::Exposing || state_ == State::Download; }
    bool owns_locked(std::uint64_t token) const { return owner_ == token && is_exposing_locked(); }

    void require_startable_locked() const {
        if (!open_) {
            throw AlpacaException(name_ + ": the camera is not connected", AlpacaError::NotConnected);
        }
        if (is_exposing_locked()) {
            throw AlpacaException(name_ + ": an exposure is already in progress", AlpacaError::InvalidOperation);
        }
    }

    void raise_failure_locked() const {
        if (failure_) {
            throw AlpacaException(*failure_, AlpacaError::DriverException);
        }
    }

    // Marks Failed and takes ownership away under mutex_, then cancels the
    // body with mutex_ released, counted as pending so a start() waits for it.
    void fail_locked(std::unique_lock<std::mutex>& lock, std::string message) const {
        state_ = State::Failed;
        failure_ = std::move(message);
        owner_ = 0;
        ++pending_;
        lock.unlock();
        slot_.cancel();
        lock.lock();
        --pending_;
        cv_.notify_all();
    }

    // The watchdog (getters, the body's waits and image_ready() all run it).
    void apply_watchdog(std::unique_lock<std::mutex>& lock) const {
        if (is_exposing_locked() && clock_.now() >= deadline_) {
            fail_locked(lock, name_ + ": the exposure did not complete before its deadline");
        }
    }

    void join_all() {
        std::lock_guard<std::mutex> join(join_mutex_);
        slot_.cancel_all_and_join();
    }

    // CWE-117: a late throw comes from the SDK; control characters never reach the log.
    static std::string printable(std::string text) {
        std::replace_if(
            text.begin(), text.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f; }, '?');
        return text;
    }

    // The slot's thread function.
    void run_body(std::uint64_t token, Body& fn, OperationContext& ctx) {
        std::optional<std::string> thrown;
        bool call = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            call = owner_ == token;  // a close or an abort since start(): the vendor body never runs
        }
        if (call) {
            try {
                CaptureControl control(*this, ctx, token);
                fn(control);
            } catch (const std::exception& e) {
                thrown = e.what();
            } catch (...) {
                thrown = "unknown exception";
            }
        }
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (owns_locked(token) && !stopping_) {
                state_ = State::Failed;
                failure_ = thrown ? *thrown : name_ + ": the exposure ended without an image";
                owner_ = 0;
            } else {
                stale = thrown.has_value();
            }
            --running_;
            cv_.notify_all();
        }
        if (stale) {
            ALPACA_LOG_WARN(name_, "stale exposure failed: " + printable(*thrown));
        }
    }

    const std::string name_;
    TaskClock& clock_;
    const std::chrono::nanoseconds margin_;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    mutable State state_ = State::Idle;
    mutable std::optional<std::string> failure_;
    mutable std::uint64_t owner_ = 0;  // token of the body that may publish; 0 = none
    mutable int pending_ = 0;          // cancels and joins in flight
    std::shared_ptr<const ImageArray> image_;
    LastExposure last_;
    std::uint64_t last_token_ = 0;
    int running_ = 0;  // bodies reserved or running
    int disconnecting_ = 0;
    bool open_ = true;
    bool stopping_ = false;  // the driver is stopping the SDK exposure: a body failure is not kept
    bool started_ = false;
    double duration_ = 0.0;
    std::chrono::system_clock::time_point start_time_{};
    TaskClock::clock::time_point base_{};
    TaskClock::clock::time_point deadline_{};

    std::mutex join_mutex_;
    OperationGeneration generation_;
    // Declared last: destroyed first, joining every thread before the members above die.
    mutable AsyncOperation slot_;
};

inline bool CaptureControl::wait_for(std::chrono::nanoseconds d) {
    std::chrono::nanoseconds wait = d;
    {
        std::unique_lock<std::mutex> lock(session_.mutex_);
        session_.apply_watchdog(lock);
        if (!session_.owns_locked(token_)) {
            return false;
        }
        wait = std::min(d, session_.deadline_ - session_.clock_.now());
    }
    if (!ctx_.wait_for(wait)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(session_.mutex_);
    session_.apply_watchdog(lock);
    return session_.owns_locked(token_) && wait == d;
}

inline void CaptureControl::exposure_started() {
    std::lock_guard<std::mutex> lock(session_.mutex_);
    if (session_.owns_locked(token_)) {
        session_.started_ = true;
        session_.start_time_ = std::chrono::system_clock::now();
        session_.base_ = session_.clock_.now();
    }
}

inline void CaptureControl::reading_out() {
    std::lock_guard<std::mutex> lock(session_.mutex_);
    if (session_.owns_locked(token_)) {
        session_.state_ = CaptureSession::State::Download;
    }
}

inline bool CaptureControl::image_ready(std::shared_ptr<const ImageArray> image) {
    std::unique_lock<std::mutex> lock(session_.mutex_);
    session_.apply_watchdog(lock);
    if (!session_.owns_locked(token_)) {
        return false;
    }
    session_.image_ = std::move(image);
    session_.state_ = CaptureSession::State::Ready;
    session_.owner_ = 0;
    session_.last_.commit(session_.duration_, session_.start_time_);
    return true;
}

}  // namespace alpacacore::util
