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
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>

#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace alpacacore::util {

/**
 * @brief Short-TTL cache of one device status frame for request/response
 *        drivers (open-astro#294).
 *
 * DeviceState and the individual getters each used to round-trip the device
 * once per property. The driver defines a `Status` that one refill fills from
 * a single burst of device I/O; every getter reads a field of it. Within
 * the TTL no device I/O happens at all.
 *
 * Link health (issue #237): a failed refill drops the cached frame, so a dead
 * link never serves values, and `PolledLinkHealth` latches after
 * `fault_threshold` consecutive failures. While latched the refill is still
 * attempted on every read (those reads are the only traffic that can clear
 * the fault), but a failure throws DriverException "<label> communications
 * compromised: ..." instead of the raw error. A good refill clears the latch
 * with no reconnect.
 *
 * Locking: the cache has its own mutex, held across the refill so concurrent
 * readers share one device round trip. Lock order is driver mutex (if held)
 * -> cache mutex -> SDK mutex; the converted adopters (ZWO EAF, CAA, dew switch, ToupTek AAF, ToupTek thermal switch,
 * ZWO EFW, Player One PW, ToupTek AFW) refill under cache mutex -> io_mutex_ -> mutex_ (leaf) and hold no driver lock
 * when they call the cache. Never call get() from inside the refill. fault() takes only `fault_mutex_`, a leaf under
 * the cache mutex (cache mutex -> fault_mutex_), never held across the refill, so a getter that reads it never waits
 * for device I/O (decision 0016).
 *
 * Clock: the TTL reads the TaskClock passed to the constructor (decision 0005), the real
 * steady clock by default; a driver on an injected clock passes it so a test ages the frame with advance().
 * Writers call invalidate() after the write; connect/disconnect call reset().
 */
template <typename Status>
class TtlStatusCache {
public:
    using clock = TaskClock::clock;

    TtlStatusCache(std::string device_label, std::chrono::milliseconds ttl, int fault_threshold = 3,
                   std::string log_component = "Device", const TaskClock& clock = default_task_clock())
        : label_(std::move(device_label)),
          log_component_(std::move(log_component)),
          clock_(&clock),
          ttl_(ttl),
          fault_threshold_(fault_threshold) {}

    template <typename Fetch>
    Status get(Fetch&& fetch) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = clock_->now();
        if (status_.has_value() && (now - filled_at_) < ttl_) {
            return *status_;
        }
        try {
            Status fresh = fetch();
            const bool restored = health_.on_reply();
            publish_fault();
            if (restored) {
                ALPACA_LOG_INFO(log_component_, label_ + " communications restored");
            }
            status_ = fresh;
            filled_at_ = clock_->now();
            return fresh;
        } catch (const std::exception& e) {
            status_.reset();
            const auto latched = health_.note_failure(e.what(), fault_threshold_);
            publish_fault();
            if (latched) {
                ALPACA_LOG_ERROR(log_component_, label_ + " communications compromised: " + *latched);
            } else if (!health_.faulted()) {
                ALPACA_LOG_WARN(log_component_, label_ + " status read failed: " + e.what() + " (" +
                                                    std::to_string(health_.consecutive_failures()) + " consecutive)");
            }
            if (health_.faulted()) {
                throw AlpacaException(label_ + " communications compromised: " + health_.fault(),
                                      AlpacaError::DriverException);
            }
            throw;
        }
    }

    /// The latched fault reason, empty while the link is healthy. For writable
    /// device settings held in driver memory, which must not be served either.
    /// Never waits on a refill: it can show the state from before a refill that is in flight.
    std::string fault() const {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        return fault_;
    }

    /// After a write that changes device state: the next read goes to the device.
    /// Writes do not check the latch here: an SDK-backed driver's own call fails if
    /// the device is gone.
    void invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.reset();
    }

    /// At connect and disconnect: drop the frame and the link-health record.
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.reset();
        health_.reset();
        publish_fault();
    }

private:
    /// Copies the latched reason for fault(); the caller holds mutex_.
    void publish_fault() {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        fault_ = health_.fault();
    }

    std::string label_;
    std::string log_component_;
    const TaskClock* clock_;
    std::chrono::milliseconds ttl_;
    int fault_threshold_;
    mutable std::mutex mutex_;
    std::optional<Status> status_;
    clock::time_point filled_at_{};
    PolledLinkHealth health_;
    mutable std::mutex fault_mutex_;
    std::string fault_;
};

}  // namespace alpacacore::util
