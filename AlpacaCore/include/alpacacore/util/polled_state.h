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

#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace alpacacore::util {

/**
 * @brief The state a poll feeds to a driver's getters, with the poll's cadence,
 *        fault latch and link-loss policy in one value the driver owns
 *        (decision records 0009, 0010 and 0016).
 *
 * It composes StateSnapshot (the frame), PolledLinkHealth (the fault latch) and
 * an AsyncOperation slot (the poll thread). Not a base class and nothing
 * virtual: the two callbacks are std::function (decision 0013, point 5).
 *
 * - start(): one probe inline, a second if nothing published, then the poll
 *   slot. False when no frame is held (the driver fails its connect).
 * - The poll runs start to start every `period`, skipping the ticks a slow
 *   probe ran over (no catch-up). It runs on the poll thread with no driver
 *   lock held: `probe()` does the device I/O and returns the frame (it throws
 *   when the exchange failed), and the module publishes it. A write() that
 *   landed since the probe began drops the frame; the next poll publishes.
 * - `fault_threshold` failed probes in a row latch a fault. A latched fault
 *   refuses the cache (served() throws DriverException) but the probe keeps
 *   running; the next good probe clears it.
 * - A fault that has stood for util::kLinkStalenessBound, or a frame older
 *   than it, sets the loss latch once and calls `on_lost()` on the poll thread
 *   with no driver lock held, then the poll ends (no autonomous reconnect,
 *   decision 0009). reset() clears the latch for the next connect.
 * - lose_link(), from the probe on positive evidence that the transport is
 *   gone (decision 0009 rule 2a), sets the same latch at once; the poll ends
 *   after its current cycle and calls `on_lost()` once, as above.
 * - A frame older than `max_age` is stale. Stale alone never refuses
 *   (decision 0010, point 6): served() returns the reading with `stale` set.
 *
 * Locks: the module's mutexes (the snapshot's and the health mutex) are
 * leaves. Under the driver's own `mutex_` they take no other lock and do no
 * I/O (decision 0016, point 2). `write()` callbacks do no I/O, never throw and
 * never call back into the module. The TaskClock and the OperationGeneration
 * must outlive the module.
 */
template <typename Frame>
class PolledState {
public:
    using Reading = typename StateSnapshot<Frame>::Reading;

    struct Config {
        std::chrono::nanoseconds period{std::chrono::milliseconds(250)};
        int fault_threshold = 3;
        /// Freshness bound; must undercut util::kLinkStalenessBound.
        std::chrono::nanoseconds max_age{std::chrono::milliseconds(500)};
        /// Component name on every log line and in the thrown messages.
        std::string log_tag;
    };

    /// `probe` runs the device exchange and returns the frame, or throws.
    /// `on_lost` releases the transport; it runs once per loss.
    PolledState(TaskClock& clock, OperationGeneration& generation, Config config, std::function<Frame()> probe,
                std::function<void()> on_lost)
        : clock_(clock),
          config_(std::move(config)),
          probe_(std::move(probe)),
          on_lost_(std::move(on_lost)),
          snapshot_(clock, config_.max_age),
          slot_(config_.log_tag + " state poll", generation, clock) {
        // Not a static_assert: the bound is a Config value, not a template argument.
        if (config_.max_age >= kLinkStalenessBound) {
            throw AlpacaException("PolledState max_age must undercut util::kLinkStalenessBound",
                                  AlpacaError::InvalidOperation);
        }
    }

    PolledState(const PolledState&) = delete;
    PolledState& operator=(const PolledState&) = delete;
    ~PolledState() { stop_and_join(); }

    /// At connect, with no driver lock held (AsyncOperation rule 10). False when no frame was
    /// published or the poll did not start.
    bool start() {
        poll_once();
        if (!snapshot_.read().has_value()) {
            poll_once();
        }
        if (!snapshot_.read().has_value()) {
            return false;
        }
        try {
            slot_.start([this](OperationContext& ctx) { run(ctx); });
        } catch (const std::exception&) {
            // Connected with no poll would serve the connect-time frame forever: fail the connect.
            ALPACA_LOG_ERROR(config_.log_tag, "State poll did not start");
            return false;
        }
        return true;
    }

    /// Joins the poll body. Call before the transport the probe uses closes.
    void stop_and_join() { slot_.cancel_all_and_join(); }

    /// At connect and disconnect: drop the frame, the fault and the loss latch.
    void reset() {
        snapshot_.reset();
        std::lock_guard<std::mutex> lock(health_mutex_);
        health_.reset();
        ++epoch_;  // after the snapshot reset: a verdict read from the old frame can no longer latch
        lost_.store(false);
        lost_notified_.store(false);
    }

    /// Write-through to the held frame (StateSnapshot::write): `apply` does no
    /// I/O and does not throw. It also drops a poll in flight.
    template <typename Apply>
    void write(Apply&& apply) {
        snapshot_.write(std::forward<Apply>(apply));
    }

    /// The frame for a getter. Throws NotConnected when the link is lost (or no
    /// frame is held) and DriverException with the fault text while a fault is
    /// latched; otherwise the reading, stale or not.
    Reading served() const {
        throw_if_lost();
        const auto epoch = current_epoch();
        const auto reading = snapshot_.read();
        if (reading.has_value() && clock_.now() - reading->measured_at >= kLinkStalenessBound) {
            latch_lost(epoch);
            throw_if_lost();
        }
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.faulted()) {
                throw AlpacaException(config_.log_tag + " communications compromised: " + health_.fault(),
                                      AlpacaError::DriverException);
            }
        }
        if (!reading.has_value()) {
            // Before the first frame, or a reset() between the checks dropped it.
            throw_if_lost();
            throw AlpacaException(config_.log_tag + " has no state frame", AlpacaError::NotConnected);
        }
        return *reading;
    }

    /// The held frame, or nullopt; never throws.
    std::optional<Reading> read() const { return snapshot_.read(); }

    /// The latched poll fault, empty when none. Kept after the link is lost.
    std::string fault() const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        return health_.faulted() ? health_.fault() : std::string();
    }

    bool link_lost() const { return lost_.load(); }

    /// Positive evidence that the transport is gone (decision 0009 rule 2a): sets the loss latch now, without
    /// waiting out util::kLinkStalenessBound. Called from the probe, so it belongs to the connection that
    /// reset() last opened; the poll ends after the current cycle and calls `on_lost()` once.
    void lose_link() { (void)latch_lost(current_epoch()); }

    /// How long ago the held frame was measured on the task clock.
    std::optional<std::chrono::nanoseconds> age() const {
        const auto reading = snapshot_.read();
        if (!reading.has_value()) {
            return std::nullopt;
        }
        return clock_.now() - reading->measured_at;
    }

    /// Test seam: while paused the poll thread waits its ticks but probes nothing.
    void set_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        slot_.set_spawn_for_testing(std::move(spawn));
    }
    void pause_for_testing(bool paused) { paused_.store(paused); }

    /// Test seam: one poll cycle on the calling thread, `on_lost()` included.
    void poll_now_for_testing() { cycle(); }

private:
    std::uint64_t current_epoch() const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        return epoch_;
    }

    // Sets the loss latch for the connection `epoch` was read in; a reset() since then
    // made the verdict stale, and the new connection stays up. True when this call latched.
    bool latch_lost(std::uint64_t epoch) const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        return epoch == epoch_ && !lost_.exchange(true);
    }

    void throw_if_lost() const {
        if (lost_.load()) {
            throw AlpacaException(config_.log_tag + " link lost; set Connected false then true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    // One exchange with no driver lock held, then publish. The exception text
    // can carry the device's reply; AlpacaCore has no log escaper (CWE-117), so
    // the lines state the fact only.
    void poll_once() {
        const auto token = snapshot_.begin_poll();
        std::optional<Frame> frame;
        try {
            frame = probe_();
        } catch (const std::exception&) {
            note_failure();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.on_reply()) {
                ALPACA_LOG_INFO(config_.log_tag, "Communications restored");
            }
        }
        snapshot_.publish(token, std::move(*frame));
    }

    void note_failure() {
        const auto epoch = current_epoch();
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.note_failure("no valid reply to the state poll", config_.fault_threshold, clock_.now())) {
                ALPACA_LOG_ERROR(config_.log_tag, "Communications compromised: " + health_.fault());
            } else {
                ALPACA_LOG_DEBUG(config_.log_tag, "State poll failed; the frame keeps its age");
            }
            lost = health_.fault_stale(clock_.now());
        }
        if (!lost) {
            const auto reading = snapshot_.read();
            lost = reading.has_value() && clock_.now() - reading->measured_at >= kLinkStalenessBound;
        }
        if (lost && latch_lost(epoch)) {
            ALPACA_LOG_ERROR(
                config_.log_tag,
                "Link lost: no answer to the state poll for " +
                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kLinkStalenessBound).count()) +
                    " s; set Connected false then true to reconnect");
        }
    }

    // One poll and, once per loss, the transport release. True when lost.
    bool cycle() {
        poll_once();
        if (lost_.load()) {
            if (!lost_notified_.exchange(true)) {
                on_lost_();
            }
            return true;
        }
        return false;
    }

    void run(OperationContext& ctx) {
        if (!ctx.wait_for(config_.period)) {
            return;
        }
        for (;;) {
            const auto started = clock_.now();
            if (!paused_.load() && cycle()) {
                // Lost: the transport is released and the poll ends (decision
                // 0009, point 4: no autonomous reconnect).
                return;
            }
            // Start to start: skip any tick a slow poll ran over (no catch-up).
            const auto spent = clock_.now() - started;
            const auto ticks = spent / config_.period + 1;
            if (!ctx.wait_for(ticks * config_.period - spent)) {
                return;
            }
        }
    }

    TaskClock& clock_;
    Config config_;
    std::function<Frame()> probe_;
    std::function<void()> on_lost_;
    StateSnapshot<Frame> snapshot_;
    // Narrow: held for a counter or a string, never across an exchange.
    mutable std::mutex health_mutex_;
    PolledLinkHealth health_;
    // Bumped by reset() under health_mutex_: a loss verdict latches only in the connection it read.
    std::uint64_t epoch_ = 0;
    mutable std::atomic<bool> lost_{false};
    std::atomic<bool> lost_notified_{false};
    std::atomic<bool> paused_{false};
    // Last: destroyed first, so its body joins while the rest is alive.
    AsyncOperation slot_;
};

}  // namespace alpacacore::util
