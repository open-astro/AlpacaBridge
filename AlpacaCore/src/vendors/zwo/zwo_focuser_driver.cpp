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
#include <alpacacore/util/logging.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_eaf_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

namespace alpacacore::vendor::zwo {

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
          connected_(false),
          status_cache_("ZWO EAF", kStatusTtl, 3, "ZWO") {}

    ~ZWOEAFFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);
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

    std::string get_unique_id() const override {
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

    bool get_connected() const override {
        return connected_.load();
    }

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
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = sdk_;
        if (connected) {
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const int resolved_id = resolve_focuser_id();
                sdk.open_focuser(resolved_id);
                try {
                    publish_connect_state(resolved_id);
                } catch (...) {
                    sdk.close_focuser(resolved_id);
                    throw;
                }
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            status_cache_.reset();
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws (e.g.
        // device unplugged) the error still surfaces, but the driver must not
        // stay half-connected.
        std::optional<int> close_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_id = focuser_id_;
            max_step_.store(0);
            step_range_.store(0);
            if (focuser_index_.has_value()) {
                focuser_id_.reset();
                focuser_info_ = {};
                focuser_info_valid_ = false;
                serial_number_.clear();
            }
            connected_.store(false);
        }
        status_cache_.reset();
        if (close_id.has_value()) {
            // Waits for an exchange in flight: the handle is never closed under one.
            std::lock_guard<std::mutex> io(io_mutex_);
            sdk.close_focuser(close_id.value());
        }
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
        // is a lock-free atomic read, so this costs a getter nothing.
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
            ensure_connected();  // a disconnect that won the I/O lock has closed the handle
            sdk_.stop(focuser_id_value());
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
            ensure_connected();  // a disconnect that won the I/O lock has closed the handle
            sdk_.move(focuser_id_value(), position);
        }
        status_cache_.invalidate();
    }

private:
    // Reads the unit's static members once, writes the configured max step when it differs from the
    // stored one, and publishes the connect-time state: info, serial, then the atomics. Caller holds
    // io_mutex_ and not mutex_ (the state lock is taken for the short publish only).
    void publish_connect_state(int focuser_id) {
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
            // Optional: MaxIncrement falls back to MaxStep when the unit does not report a range.
            ALPACA_LOG_DEBUG("ZWO", "EAF step range unavailable: " + std::string(e.what()));
        }
        max_step = apply_configured_max_step(focuser_id, max_step, step_range);
        if (max_step <= 0) {
            // Connects as before the Max Step setting existed: every Move fails the range check.
            ALPACA_LOG_WARN("ZWO", "EAF reports max step 0 and no Max Step is configured");
        }
        std::string serial;
        try {
            serial = sdk_.get_serial_number(focuser_id);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("ZWO", "EAF serial number unavailable: " + std::string(e.what()));
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
        bool closed{};  // a disconnect won the I/O lock: not a frame, not a link failure
        bool moving{};
        int position{};
        std::optional<double> temperature;                         // absent when the SDK would not report it
        std::string temperature_error;                             // why, for the Temperature getter
        int temperature_error_code{AlpacaError::DriverException};  // the wrapper's mapped code
    };

    static constexpr std::chrono::milliseconds kStatusTtl{100};

    Status read_status() const {
        ensure_connected();
        const int id = focuser_id_value();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: cache mutex, I/O lock,
        // mutex_), so it must not take mutex_ and no caller may hold mutex_ or the I/O lock around the cache.
        Status status = status_cache_.get([this, id] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            if (!connected_.load()) {
                // A disconnect that won the I/O lock has closed the handle. Returning (not throwing) keeps it
                // out of the link-health count; the disconnect's reset() drops this frame.
                fresh.closed = true;
                return fresh;
            }
            fresh.moving = sdk_.is_moving(id);
            fresh.position = sdk_.get_position(id);
            try {
                fresh.temperature = sdk_.get_temperature(id);
            } catch (const AlpacaException& e) {
                // Temperature is optional: moving/position still answer. Keep the
                // wrapper's mapped code (NotImplemented, NotConnected, ...).
                fresh.temperature_error = e.what();
                fresh.temperature_error_code = e.error_code();
            } catch (const std::exception& e) {
                fresh.temperature_error = e.what();
            }
            return fresh;
        });
        if (status.closed) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
        return status;
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
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
            if (index < 0 || index >= static_cast<int>(focusers.size())) {
                ALPACA_LOG_WARN("ZWO", "Focuser index out of range: " + std::to_string(index) +
                                         " (count=" + std::to_string(focusers.size()) + ")");
                throw AlpacaException("Focuser index not found", AlpacaError::InvalidValue);
            }
            const auto& info = focusers[static_cast<std::size_t>(index)];
            std::lock_guard<std::mutex> lock(mutex_);
            focuser_id_ = info.focuser_id;
            focuser_info_ = info;
            focuser_info_valid_ = true;
            return info.focuser_id;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (focuser_id_.has_value()) {
            return focuser_id_.value();
        }

        throw AlpacaException("Focuser ID not specified", AlpacaError::InvalidValue);
    }

    int focuser_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!focuser_id_.has_value()) {
            throw AlpacaException("Focuser ID not set", AlpacaError::NotConnected);
        }
        return focuser_id_.value();
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
    std::atomic<bool> connected_;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // short state reads and writes, never held across an SDK call), then the SDK wrapper's own mutex.
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the
    // device transactions (connect, close, move, halt, the status refill). Never take the cache mutex
    // (reset/invalidate/fault) while holding io_mutex_ or mutex_.
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
