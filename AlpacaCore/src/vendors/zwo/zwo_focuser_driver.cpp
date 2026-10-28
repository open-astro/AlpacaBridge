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

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_epoch.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_eaf_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace alpacacore::vendor::zwo {

namespace {

// CWE-117: the serial and SDK error texts are device text; control characters never reach the log or a client.
std::string printable(std::string text) {
    std::replace_if(
        text.begin(), text.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f; }, '?');
    return text;
}

}  // namespace

class ZWOEAFFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ZWOEAFFocuserDriver(int device_number, std::optional<int> focuser_id, std::optional<int> focuser_index,
                        ZWOEAFSDK& sdk, std::optional<int> configured_max_step)
        : AsyncConnectable("ZWO"),
          sdk_(sdk),
          device_number_(device_number),
          focuser_id_(focuser_id),
          focuser_index_(focuser_index),
          serial_number_(),
          focuser_info_(),
          focuser_info_valid_(false),
          configured_max_step_(configured_max_step),
          status_cache_("ZWO EAF", kStatusTtl, 3, "ZWO") {}

    ~ZWOEAFFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (link_.is_open()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during EAF destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (focuser_info_valid_ && !focuser_info_.name.empty()) {
            return focuser_info_.name;
        }
        return "ZWO EAF";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Focuser;
    }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "ZWO_EAF_SN_" + serial_number_;
        }
        if (focuser_id_.has_value()) {
            return "ZWO_EAF_ID_" + std::to_string(focuser_id_.value());
        }
        return "ZWO_EAF_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO EAF Focuser Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO EAF Focuser Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only (never in
    // DriverInfo). EAFGetSDKVersion() returns "1, 7, 0, 0"; render as "1.7.0.0".
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = sdk_.get_sdk_version();
        // get_sdk_version() returns the literal "unknown" when EAFGetSDKVersion()
        // yields nullptr — suppress the row rather than show "unknown".
        if (version.empty() || version == "unknown") {
            return std::nullopt;
        }
        return util::normalize_dotted_version(version);
    }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override { return link_.is_open(); }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // transition_mutex_ spans the whole call and no getter takes it: the connect handshake
        // below runs under it and the I/O lock, never under mutex_ (decision 0016).
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(link_.is_open())) {
            return;
        }
        if (connected && consume_pending_disconnect(link_.is_open())) {
            return;
        }
        if (connected == link_.is_open()) {
            return;
        }
        // A new epoch first: a call that captured the old link can no longer drop this one (issue #1500).
        link_.begin_transition();

        if (connected) {
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                release_open_locked();  // an open left from a lost link
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lost_reason_.clear();
                }
                const auto opened = bind_focuser_locked();
                try {
                    sdk_.with_link(open_id_, open_link_, [&] { publish_connect_state(open_id_, opened.serial); });
                } catch (...) {
                    release_open_locked();
                    throw;
                }
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            status_cache_.reset();
            link_.publish_open();
            return;
        }

        // Clear driver state before the SDK close: a close that fails for another reason than a
        // removed unit still surfaces, but the driver must not stay half-connected.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            max_step_.store(0);
            step_range_.store(0);
            if (focuser_index_.has_value()) {
                focuser_id_.reset();
                focuser_info_ = {};
                focuser_info_valid_ = false;
                serial_number_.clear();
            }
        }
        status_cache_.reset();
        // Waits for an exchange in flight: the handle is never closed under one.
        std::lock_guard<std::mutex> io(io_mutex_);
        release_open_locked();
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name),
                              AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override {
        return false;
    }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    bool get_absolute() const override {
        return true;
    }

    bool get_is_moving() const override { return read_status().moving; }

    int get_max_step() const override {
        ensure_connected();
        return max_step_.load();
    }

    int get_max_increment() const override {
        ensure_connected();
        const int max_step = max_step_.load();
        const int step_range = step_range_.load();
        if (step_range > 0) {
            return step_range > max_step ? max_step : step_range;
        }
        return max_step;
    }

    int get_position() const override { return read_status().position; }

    double get_step_size() const override {
        // open-astro#309: connection check before the not-implemented throw,
        // the same order astroasis's get_step_size() already used.
        ensure_connected();
        throw AlpacaException("Step size is not available for this focuser",
                              AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        // open-astro#309: the connection check comes first. driver-lifecycle.instructions.md's ASCOM
        // contract precedence rule is that only parameter/range validation
        // precedes it -- every other property throws NotConnected when
        // disconnected, with no early return that skips it. ensure_connected()
        // is a lock-free atomic read while connected, so this costs a getter nothing.
        ensure_connected();
        return false;
    }

    bool get_temp_comp() const override {
        ensure_connected();
        return false;
    }

    void set_temp_comp(bool) override {
        ensure_connected();
        throw AlpacaException("Temperature compensation not supported", AlpacaError::NotImplemented);
    }

    double get_temperature() const override {
        const auto status = read_status();
        if (!status.temperature.has_value()) {
            throw AlpacaException("Focuser temperature is not available: " + status.temperature_error,
                                  status.temperature_error_code);
        }
        return *status.temperature;
    }

    void halt() override {
        ensure_connected();
        {
            // Waits for a status refill in flight (the I/O lock is not preemptible); the stop follows at once.
            std::lock_guard<std::mutex> io(io_mutex_);
            call_locked([&] { sdk_.stop(open_id_); });
        }
        status_cache_.invalidate();
    }

    void move(int position) override {
        ensure_connected();
        int max_step = get_max_step();
        if (position < 0 || position > max_step) {
            throw AlpacaException("Focuser position out of range", AlpacaError::InvalidValue);
        }
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            call_locked([&] { sdk_.move(open_id_, position); });
        }
        status_cache_.invalidate();
    }

private:
    // Reads the unit's static members once, writes the configured max step when it differs from the
    // stored one, and publishes the connect-time state: info, the serial read at the open, then the atomics.
    // Caller holds io_mutex_ and not mutex_ (the state lock is taken for the short publish only), and runs it
    // under the open's link.
    void publish_connect_state(int focuser_id, const std::string& serial) {
        ZWOEAFFocuserInfo info;
        if (!sdk_.get_focuser_info_by_id(focuser_id, info)) {
            throw AlpacaException("Failed to read focuser info", AlpacaError::DriverException);
        }
        // Static members are read once here, so the getters never go back to the device.
        int max_step = sdk_.get_max_step(focuser_id);
        int step_range = 0;  // unavailable: MaxIncrement falls back to MaxStep
        try {
            step_range = sdk_.get_step_range(focuser_id);
        } catch (const std::exception& e) {
            if (util::is_link_loss(e)) {
                throw;  // the unit is gone: the connect closes what it opened and fails
            }
            // Optional: MaxIncrement falls back to MaxStep when the unit does not report a range.
            ALPACA_LOG_DEBUG("ZWO", "EAF step range unavailable: " + std::string(e.what()));
        }
        max_step = apply_configured_max_step(focuser_id, max_step, step_range);
        if (max_step <= 0) {
            // Connects as before the Max Step setting existed: every Move fails the range check.
            ALPACA_LOG_WARN("ZWO", "EAF reports max step 0 and no Max Step is configured");
        }
        if (serial.empty()) {
            ALPACA_LOG_WARN("ZWO",
                            "EAF serial number unavailable: Connect cannot tell this unit from another after a "
                            "replug");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            focuser_info_ = info;
            focuser_info_valid_ = true;
            serial_number_ = serial;
        }
        max_step_.store(max_step);
        step_range_.store(step_range);
    }

    // Writes the configured max step to the unit when it differs from the stored one and returns the
    // value to adopt. A value above the unit's step range is clamped to it; an unavailable range (0) is
    // no bound. Caller holds io_mutex_.
    int apply_configured_max_step(int focuser_id, int stored_max_step, int step_range) {
        if (!configured_max_step_.has_value()) {
            return stored_max_step;
        }
        int wanted = *configured_max_step_;
        if (step_range > 0 && wanted > step_range) {
            ALPACA_LOG_WARN("ZWO", "EAF configured Max Step " + std::to_string(wanted) + " is above the step range " +
                                       std::to_string(step_range) + "; using " + std::to_string(step_range));
            wanted = step_range;
        }
        if (wanted == stored_max_step) {
            return stored_max_step;
        }
        sdk_.set_max_step(focuser_id, wanted);
        ALPACA_LOG_INFO("ZWO", "EAF max step set to " + std::to_string(wanted));
        return wanted;
    }

    // One frame of the live readings; a single refill fills all of it (open-astro#294).
    struct Status {
        bool closed{};  // a disconnect won the I/O lock, or the link was lost: not a frame, not a failed poll
        bool moving{};
        int position{};
        std::optional<double> temperature;                         // absent when the SDK would not report it
        std::string temperature_error;                             // why, for the Temperature getter
        int temperature_error_code{AlpacaError::DriverException};  // the wrapper's mapped code
    };

    static constexpr std::chrono::milliseconds kStatusTtl{100};

    Status read_status() const {
        ensure_connected();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: cache mutex, I/O lock,
        // mutex_), so no caller may hold mutex_ or the I/O lock around the cache.
        Status status = status_cache_.get([this] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            try {
                call_locked([&] {
                    fresh.moving = sdk_.is_moving(open_id_);
                    fresh.position = sdk_.get_position(open_id_);
                    try {
                        fresh.temperature = sdk_.get_temperature(open_id_);
                    } catch (const AlpacaException& e) {
                        if (util::is_link_loss(e)) {
                            throw;  // the unit is gone even though Position answered
                        }
                        // Temperature is optional: moving/position still answer. Keep the
                        // wrapper's mapped code (NotImplemented, ...).
                        fresh.temperature_error = e.what();
                        fresh.temperature_error_code = e.error_code();
                    } catch (const std::exception& e) {
                        fresh.temperature_error = e.what();
                    }
                });
            } catch (const std::exception& e) {
                if (!util::is_link_loss(e)) {
                    throw;  // a fault: the cache counts it toward the latch
                }
                // A disconnect that won the I/O lock, or a lost link. Returning (not throwing) keeps it out of the
                // link-health count; the reader throws NotConnected, and the next connect's reset() drops this frame.
                fresh = Status{};
                fresh.closed = true;
                return fresh;
            }
            last_position_ = fresh.position;
            return fresh;
        });
        if (status.closed) {
            throw AlpacaException(closed_reason(), AlpacaError::NotConnected);
        }
        return status;
    }

    void ensure_connected() const {
        if (!link_.is_open()) {
            throw AlpacaException(closed_reason(), AlpacaError::NotConnected);
        }
    }

    std::string closed_reason() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lost_reason_.empty() ? "Focuser not connected" : lost_reason_;
    }

    // Caller holds io_mutex_. Runs @p fn under this driver's open; the link word is read first, so a disconnect
    // that won the I/O lock refuses here. Loss evidence from the SDK drops the link at once (decision 0009
    // rule 2a, issue #1500) and surfaces as NotConnected; any other error surfaces as itself.
    void call_locked(const std::function<void()>& fn) const {
        const auto captured = link_.load();
        if (!util::LinkEpoch::is_open(captured)) {
            throw AlpacaException(closed_reason(), AlpacaError::NotConnected);
        }
        try {
            sdk_.with_link(open_id_, open_link_, fn);
        } catch (const std::exception& e) {
            if (!util::is_link_loss(e)) {
                throw;
            }
            const std::string reason = unit_label_locked() + " was removed from USB; Connect again when it is back";
            if (link_.mark_lost(captured)) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lost_reason_ = reason;
                }
                // Decision 4: nothing is re-applied at the next Connect; the last-known values go to the log once.
                ALPACA_LOG_WARN("ZWO", unit_label_locked() + " lost (" + printable(e.what()) + "); last position " +
                                           std::to_string(last_position_) + ", max step " +
                                           std::to_string(max_step_.load()));
            }
            throw AlpacaException(reason, AlpacaError::NotConnected);
        }
    }

    // Caller holds io_mutex_.
    std::string unit_label_locked() const {
        return "ZWO EAF " + (bound_serial_.empty() ? "id " + std::to_string(open_id_) : printable(bound_serial_));
    }

    // Caller holds io_mutex_. Opens the unit this driver is bound to and records the open. Once a serial is known
    // (an earlier connect in this process) every Connect scans for it, so a replug that moved the unit to another
    // SDK id, or swapped two units' ids, still opens the same unit and never another one (issue #1500).
    SharedOpenTable::Opened bind_focuser_locked() {
        if (bound_serial_.empty()) {
            const int id = resolve_focuser_id();
            auto opened = sdk_.open_focuser_link(id);
            open_id_ = id;
            open_link_ = opened.link;
            bound_serial_ = opened.serial;
            return opened;
        }
        const auto focusers = sdk_.enumerate_focusers();
        // TODO(#1500): confirm on the rig that EAFOpen of an unheld candidate does not move the focuser.
        for (const auto& info : focusers) {
            std::optional<SharedOpenTable::Opened> candidate;
            try {
                candidate = sdk_.open_focuser_candidate(info.focuser_id);
            } catch (const std::exception& e) {
                // Another process holds it, or its serial read failed: nothing proves which unit it is.
                ALPACA_LOG_DEBUG("ZWO",
                                 "EAF scan skips id " + std::to_string(info.focuser_id) + ": " + printable(e.what()));
                continue;
            }
            if (!candidate.has_value()) {
                ALPACA_LOG_DEBUG("ZWO", "EAF scan skips id " + std::to_string(info.focuser_id) +
                                            ": held by a unit that reports no serial");
                continue;
            }
            if (candidate->serial == bound_serial_) {
                open_id_ = info.focuser_id;
                open_link_ = candidate->link;
                if (focuser_index_.has_value()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    focuser_id_ = info.focuser_id;
                    focuser_info_ = info;
                    focuser_info_valid_ = true;
                }
                return *candidate;
            }
            try {
                sdk_.close_focuser_link(info.focuser_id, candidate->link);
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("ZWO", "EAF scan close of id " + std::to_string(info.focuser_id) +
                                            " failed: " + printable(e.what()));
            }
        }
        throw AlpacaException("ZWO EAF " + printable(bound_serial_) + " not attached (" +
                                  std::to_string(focusers.size()) +
                                  " found); plug it in and Connect again, or re-save the device in the web UI or "
                                  "restart the service to bind another unit",
                              AlpacaError::NotConnected);
    }

    // Caller holds io_mutex_. Drops this driver's open, if any. A NotConnected close (the unit is gone, for example
    // a Connected toggle across a replug) is logged and swallowed; any other close error surfaces.
    void release_open_locked() {
        if (open_link_ == 0) {
            return;
        }
        const int id = open_id_;
        const std::uint64_t link = open_link_;
        open_link_ = 0;
        try {
            sdk_.close_focuser_link(id, link);
        } catch (const std::exception& e) {
            if (!util::is_link_loss(e)) {
                throw;
            }
            ALPACA_LOG_WARN("ZWO", "EAF close of a removed unit: " + printable(e.what()));
        }
    }

    // Caller holds io_mutex_ (the enumeration is a device call); the id and info publish under mutex_.
    int resolve_focuser_id() {
        if (focuser_index_.has_value()) {
            auto focusers = sdk_.enumerate_focusers();
            if (focusers.empty()) {
                ALPACA_LOG_WARN("ZWO", "No ZWO EAF focusers detected by SDK");
                throw AlpacaException("No ZWO EAF focusers detected", AlpacaError::NotConnected);
            }
            int index = focuser_index_.value();
            for (const auto& info : focusers) {
                if (info.slot_index == index) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    focuser_id_ = info.focuser_id;
                    focuser_info_ = info;
                    focuser_info_valid_ = true;
                    return info.focuser_id;
                }
            }
            ALPACA_LOG_WARN("ZWO", "Focuser SDK slot not found: " + std::to_string(index));
            throw AlpacaException("Focuser index not found", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (focuser_id_.has_value()) {
            return focuser_id_.value();
        }

        throw AlpacaException("Focuser ID not specified", AlpacaError::InvalidValue);
    }

    ZWOEAFSDK& sdk_;
    int device_number_;
    std::optional<int> focuser_id_;
    std::optional<int> focuser_index_;
    std::string serial_number_;
    ZWOEAFFocuserInfo focuser_info_;
    bool focuser_info_valid_;
    const std::optional<int> configured_max_step_;  // config "maxStep"; unset = leave the stored value
    // Published at the end of connect and read lock-free by MaxStep/MaxIncrement (decision 0016).
    std::atomic<int> max_step_{0};
    std::atomic<int> step_range_{0};
    // Connected, as epoch << 1 | open bit: begin_transition() and publish_open() in set_connected(), mark_lost() on
    // loss evidence (issue #1500).
    mutable util::LinkEpoch link_;
    // Under io_mutex_: the open this driver holds (open_link_ 0 = none), the serial every Connect binds once one is
    // known (kept across Disconnect, in memory only: decision 0018), and the last position read, for the loss log.
    int open_id_{-1};
    std::uint64_t open_link_{0};
    std::string bound_serial_;
    mutable int last_position_{0};
    mutable std::string lost_reason_;  // under mutex_: why a lost link refuses, cleared by the next Connect
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // short state reads and writes, never held across an SDK call), then the SDK's SharedOpenTable mutex.
    // link_.mark_lost() takes no lock and the SDK's with_link() holds none.
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the
    // device transactions (connect, close, move, halt, the status refill). Never take the cache mutex
    // (reset/invalidate) while holding io_mutex_ or mutex_; fault() takes only the leaf fault_mutex_.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<FocuserDriver> create_zwo_eaf_focuser(int device_number, int focuser_id, ZWOEAFSDK& sdk,
                                                      std::optional<int> max_step) {
    return std::make_unique<ZWOEAFFocuserDriver>(device_number, focuser_id, std::nullopt, sdk, max_step);
}

std::unique_ptr<FocuserDriver> create_zwo_eaf_focuser_by_index(int device_number, int focuser_index, ZWOEAFSDK& sdk,
                                                               std::optional<int> max_step) {
    return std::make_unique<ZWOEAFFocuserDriver>(device_number, std::nullopt, focuser_index, sdk, max_step);
}

std::unique_ptr<FocuserDriver> create_zwo_eaf_focuser(int device_number, int focuser_id, std::optional<int> max_step) {
    return create_zwo_eaf_focuser(device_number, focuser_id, ZWOEAFSDKWrapper::instance(), max_step);
}

std::unique_ptr<FocuserDriver> create_zwo_eaf_focuser_by_index(int device_number, int focuser_index,
                                                               std::optional<int> max_step) {
    return create_zwo_eaf_focuser_by_index(device_number, focuser_index, ZWOEAFSDKWrapper::instance(), max_step);
}

} // namespace alpacacore::vendor::zwo
