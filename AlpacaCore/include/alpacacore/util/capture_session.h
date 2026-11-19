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
 * A start reserved with CaptureSession::reserve() (decision 0020 rule 9). Move-only. It locks the camera
 * settings from the driver's settings snapshot until start() publishes Exposing or refuses; destroying it
 * without a start releases it. The session must outlive it.
 */
class CaptureReservation {
public:
    CaptureReservation() = default;
    CaptureReservation(const CaptureReservation&) = delete;
    CaptureReservation& operator=(const CaptureReservation&) = delete;
    CaptureReservation(CaptureReservation&& other) noexcept
        : session_(std::exchange(other.session_, nullptr)), id_(std::exchange(other.id_, 0)) {}
    CaptureReservation& operator=(CaptureReservation&& other) noexcept {
        if (this != &other) {
            release();
            session_ = std::exchange(other.session_, nullptr);
            id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }
    ~CaptureReservation() { release(); }
    /// True while it still holds a reservation (not consumed by start(), released or moved from).
    explicit operator bool() const { return id_ != 0; }

private:
    friend class CaptureSession;
    CaptureReservation(CaptureSession* session, std::uint64_t id) : session_(session), id_(id) {}
    inline void release() noexcept;

    CaptureSession* session_ = nullptr;
    std::uint64_t id_ = 0;
};

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
    /// Waits on the TaskClock for CaptureSession::signal() (an SDK frame event), never past the watchdog
    /// deadline, with no poll step. True when the signal came and the body still owns the session; false when
    /// the body must stop. A signal raised before the wait is kept, so an early event is not lost.
    inline bool wait_for_signal();
    StopReason stop_reason() const { return ctx_.stop_reason(); }
    /// This body's session token: what begin_stop() returns while the body owns the session, so a
    /// driver can tell a stop meant for this exposure from one meant for an earlier one (#2043).
    std::uint64_t token() const { return token_; }
    /// The SDK exposure has begun: stamps LastExposureStartTime and the PercentCompleted base.
    inline void exposure_started();
    /// Moves the watchdog deadline to at least `from_now` after the task clock's now, for a download whose length
    /// the body learns only once the exposure has run (QHY's slow readout modes, #1736). It never shortens the
    /// deadline. False, and nothing changed, when this body no longer owns the session.
    inline bool extend_deadline(std::chrono::nanoseconds from_now);
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
 * without the driver mutex; the getters, fail(), set_stopping(), discard_image(),
 * open(), reserve(), record_abort(), starting(), settings_locked() and
 * ensure_settings_unlocked() may run under it. A body never calls start(),
 * abort(), disconnect_and_join() or open().
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
    /// start changes nothing. `watchdog_margin`, when given, replaces the
    /// constructor's margin for this exposure only (deadline = start +
    /// duration + margin), for a body whose length depends on the exposure
    /// (retries, a bulb file wait; #1979).
    void start(double duration_seconds, Body body,
               std::optional<std::chrono::nanoseconds> watchdog_margin = std::nullopt) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (reserved_ != 0) {
            throw AlpacaException(name_ + ": an exposure is already starting", AlpacaError::InvalidOperation);
        }
        (void)start_locked(lock, 0, duration_seconds, std::move(body), watchdog_margin);
    }

    /// Reserves a start (decision 0020 rule 9): from now until start(reservation, ...) publishes Exposing or
    /// refuses, settings_locked() is true and a second reserve() is refused. Call under the driver mutex, while
    /// the driver reads the settings the exposure will use. Throws NotConnected on a closed session, and
    /// InvalidOperation while exposing or while another start holds a reservation. It does not wait for a body
    /// that is only returning: start() does, within kStartTimeout.
    CaptureReservation reserve() {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);  // an exposure past its deadline is not "in progress", as in start()
        if (!open_) {
            throw AlpacaException(name_ + ": the camera is not connected", AlpacaError::NotConnected);
        }
        if (is_exposing_locked()) {
            throw AlpacaException(name_ + ": an exposure is already in progress", AlpacaError::InvalidOperation);
        }
        if (reserved_ != 0) {
            throw AlpacaException(name_ + ": an exposure is already starting", AlpacaError::InvalidOperation);
        }
        reserved_ = ++last_reservation_;  // abort_pending_ is false: every path that ends a reservation clears it
        return CaptureReservation(this, reserved_);
    }

    /// start() for a reserved start: consumes `reservation` once Exposing is published and releases it on any
    /// refusal or throw. A reservation a disconnect_and_join() dropped is refused with NotConnected, and the body
    /// never runs. Returns true when an AbortExposure arrived while the start was reserved (record_abort()): that
    /// abort is applied at once, so the exposure ends Idle with no frame and the body never runs; the driver has no
    /// SDK exposure to stop. An abort recorded for a start that is refused is cleared with the reservation.
    [[nodiscard]] bool start(CaptureReservation&& reservation, double duration_seconds, Body body,
                             std::optional<std::chrono::nanoseconds> watchdog_margin = std::nullopt) {
        CaptureReservation held = std::move(reservation);
        std::unique_lock<std::mutex> lock(mutex_);
        if (held.session_ != this || held.id_ == 0) {
            throw AlpacaException(name_ + ": start() needs a reservation from this session's reserve()",
                                  AlpacaError::InvalidOperation);
        }
        if (held.id_ != reserved_) {
            held.id_ = 0;  // a disconnect dropped it; nothing to release
            throw AlpacaException(name_ + ": the camera was disconnected while the exposure was starting",
                                  AlpacaError::NotConnected);
        }
        const std::uint64_t id = std::exchange(held.id_, 0);  // released below on a refusal, consumed on success
        try {
            return start_locked(lock, id, duration_seconds, std::move(body), watchdog_margin);
        } catch (...) {
            if (!lock.owns_lock()) lock.lock();
            if (reserved_ == id) {
                reserved_ = 0;
                abort_pending_ = false;
                cv_.notify_all();
            }
            throw;
        }
    }

    /// An AbortExposure that finds no exposure running but a start reserved: records it, and the start that holds
    /// the reservation reports it (start(reservation, ...) returns true). True when recorded. Under the driver
    /// mutex or not; it never waits.
    bool record_abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reserved_ == 0 || is_exposing_locked()) {
            return false;
        }
        abort_pending_ = true;
        return true;
    }

    /// True while a start is reserved and has not yet published Exposing.
    bool starting() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reserved_ != 0;
    }

    /// True while the camera settings must not change: a start is reserved, an exposure is Exposing or
    /// Downloading, or a cancelled or failed body is still returning and no frame is ready. Applies the watchdog,
    /// as the state getters do.
    bool settings_locked() const {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);
        return settings_locked_locked();
    }

    /// Throws InvalidOperation, with one text for every camera, while settings_locked() is true.
    void ensure_settings_unlocked() const {
        if (settings_locked()) {
            throw AlpacaException(name_ + ": cannot change camera settings during an exposure",
                                  AlpacaError::InvalidOperation);
        }
    }

private:
    // The shared body of both start() overloads, with mutex_ held. `reservation` is the id being consumed, 0 for
    // an unreserved start. Returns whether an abort was recorded against that reservation.
    bool start_locked(std::unique_lock<std::mutex>& lock, std::uint64_t reservation, double duration_seconds, Body body,
                      std::optional<std::chrono::nanoseconds> watchdog_margin) {
        apply_watchdog(lock);
        require_startable_locked();
        if (running_ > 0 || pending_ > 0) {
            // Bounded, with mutex_ released: only a body stranded in the SDK or a
            // join in flight keeps the session busy.
            (void)clock_.wait_for(lock, cv_, kStartTimeout, [this, reservation] {
                return !open_ || is_exposing_locked() || (running_ == 0 && pending_ == 0) ||
                       (reservation != 0 && reserved_ != reservation);
            });
            require_startable_locked();
            if (reservation != 0 && reserved_ != reservation) {
                throw AlpacaException(name_ + ": the camera was disconnected while the exposure was starting",
                                      AlpacaError::NotConnected);
            }
            if (running_ > 0 || pending_ > 0) {
                throw AlpacaException(name_ + ": the previous exposure is still finishing; try again",
                                      AlpacaError::InvalidOperation);
            }
        }
        bool aborted = false;
        if (reservation != 0) {
            reserved_ = 0;
            aborted = std::exchange(abort_pending_, false);
        }
        if (aborted) {
            // Rule 9: the abort recorded during the reservation applies the moment Exposing would be published, so
            // the exposure is published and aborted in one step and no body runs. Running the body and leaving the
            // abort to the driver raced it: a body that reached its download first served the aborted frame (#2337).
            state_ = State::Idle;
            image_.reset();
            failure_.reset();
            owner_ = 0;
            cv_.notify_all();
            return true;
        }
        const Saved saved{state_, image_, failure_};
        const std::uint64_t token = ++last_token_;
        owner_ = token;
        stopping_ = false;
        signalled_ = false;
        state_ = State::Exposing;
        image_.reset();
        failure_.reset();
        duration_ = duration_seconds;
        started_ = false;
        start_time_ = std::chrono::system_clock::now();
        deadline_ =
            clock_.now() +
            std::chrono::duration_cast<TaskClock::clock::duration>(std::chrono::duration<double>(duration_seconds)) +
            std::chrono::duration_cast<TaskClock::clock::duration>(watchdog_margin.value_or(margin_));
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
        return aborted;
    }

public:
    /// Wakes the body's wait_for_signal(), or marks the signal for its next one. For an SDK event callback:
    /// any thread, the driver mutex held or not (the session mutex is a leaf). start() clears it.
    void signal() {
        std::lock_guard<std::mutex> lock(mutex_);
        signalled_ = true;
        cv_.notify_all();
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
        cv_.notify_all();  // wakes a body in wait_for_signal(), which the SDK stop does not
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
        reserved_ = 0;  // a start holding it is refused with NotConnected (rule 9)
        abort_pending_ = false;
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
    /// The driver is about to stop the SDK exposure itself (an AbortExposure,
    /// a disconnect): marks the exposure in flight as stopping, as
    /// set_stopping(true) does, and returns the token of the body that owns it
    /// (Exposing or Download), 0 when none does. One hold of the session mutex,
    /// so a body that finds this token recorded against its own
    /// CaptureControl::token() always finds the stopping mark set too, and its
    /// early return is dropped, not kept. A start() mints a new token each time,
    /// so a stop marks the exposure it found and never one a later start begins
    /// (#2043). Applies the watchdog first, so like the state getters it can
    /// fail an exposure past its deadline and cancel its body (with the session
    /// mutex released, the slot being a leaf); a caller holding its own lock
    /// above the session, the ZWO driver's exposure_sdk_mutex_, keeps that order.
    std::uint64_t begin_stop() {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_watchdog(lock);
        if (!is_exposing_locked()) {
            return 0;
        }
        stopping_ = true;
        return owner_;
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
    bool settings_locked_locked() const {
        return reserved_ != 0 || is_exposing_locked() || ((running_ > 0 || pending_ > 0) && state_ != State::Ready);
    }
    friend class CaptureReservation;
    void release_reservation(std::uint64_t id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reserved_ == id) {
            reserved_ = 0;
            abort_pending_ = false;
            cv_.notify_all();
        }
    }
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
    bool signalled_ = false;  // signal() since start(), not yet taken by wait_for_signal()
    std::uint64_t reserved_ = 0;          // id of the held reservation (rule 9); 0 = none
    std::uint64_t last_reservation_ = 0;  // reserve() hands out ++last_reservation_
    bool abort_pending_ = false;          // an AbortExposure during the held reservation (record_abort())
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

inline void CaptureReservation::release() noexcept {
    if (session_ != nullptr && id_ != 0) {
        session_->release_reservation(id_);
    }
    session_ = nullptr;
    id_ = 0;
}

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

// Every path that ends the body's ownership (abort, disconnect_and_join, the watchdog's fail_locked) clears
// owner_ and notifies cv_ under mutex_, so the predicate needs no slot call (the session mutex stays a leaf).
inline bool CaptureControl::wait_for_signal() {
    std::unique_lock<std::mutex> lock(session_.mutex_);
    session_.apply_watchdog(lock);
    const bool woke = session_.clock_.wait_for(lock, session_.cv_, session_.deadline_ - session_.clock_.now(),
                                               [this] { return session_.signalled_ || !session_.owns_locked(token_); });
    session_.apply_watchdog(lock);
    if (!woke || !session_.owns_locked(token_)) {
        return false;
    }
    session_.signalled_ = false;
    return true;
}

inline void CaptureControl::exposure_started() {
    std::lock_guard<std::mutex> lock(session_.mutex_);
    if (session_.owns_locked(token_)) {
        session_.started_ = true;
        session_.start_time_ = std::chrono::system_clock::now();
        session_.base_ = session_.clock_.now();
    }
}

inline bool CaptureControl::extend_deadline(std::chrono::nanoseconds from_now) {
    std::unique_lock<std::mutex> lock(session_.mutex_);
    session_.apply_watchdog(lock);
    if (!session_.owns_locked(token_)) {
        return false;
    }
    const auto at_least = session_.clock_.now() + std::chrono::duration_cast<TaskClock::clock::duration>(from_now);
    if (at_least > session_.deadline_) {
        session_.deadline_ = at_least;
    }
    return true;
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
