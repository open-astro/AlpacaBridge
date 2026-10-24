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
#include <alpacacore/vendor/touptek/touptek_focuser_driver.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogTag = "ToupTek";

} // namespace

class ToupTekFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ToupTekFocuserDriver(int device_number, std::optional<int> focuser_index, std::optional<std::string> focuser_id,
                         ToupTekSDK& sdk)
        : AsyncConnectable(kLogTag),
          sdk_(sdk),
          device_number_(device_number),
          focuser_index_(focuser_index),
          focuser_id_(std::move(focuser_id)),
          status_cache_("ToupTek AAF", kStatusTtl, 3, kLogTag) {}

    ~ToupTekFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag,
                                std::string("Error during AAF destruction: ") + e.what());
            }
        }
    }

    // AlpacaDriver --------------------------------------------------------

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.name.empty()) {
            return info_.name;
        }
        return "ToupTek AAF";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Focuser;
    }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.id.empty()) {
            return "TOUPTEK_AAF_" + info_.id;
        }
        if (focuser_id_.has_value() && !focuser_id_->empty()) {
            return "TOUPTEK_AAF_" + *focuser_id_;
        }
        return "TOUPTEK_AAF_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ToupTek AAF Focuser Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ToupTek AAF Focuser Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

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
                ToupFocuserInfo info = resolve_focuser(sdk);
                HToupcam handle = sdk.open_focuser_by_id(info.id);
                try {
                    int max_step = sdk.aaf_range(handle, ToupAAF::RangeMax, ToupAAF::GetMaxStep,
                                                 "Toupcam_AAF(RANGEMAX,GETMAXSTEP)");
                    int current_max = sdk.aaf_get(handle, ToupAAF::GetMaxStep, "Toupcam_AAF(GETMAXSTEP)");
                    int backlash_max = sdk.aaf_range(handle, ToupAAF::RangeMax, ToupAAF::GetBacklash,
                                                     "Toupcam_AAF(RANGEMAX,GETBACKLASH)");
                    handle_ = handle;
                    // The short state lock publishes identity and limits at the end of connect.
                    std::lock_guard<std::mutex> lock(mutex_);
                    info_ = info;
                    max_step_limit_ = max_step;
                    max_step_current_.store(current_max > 0 ? current_max : max_step);
                    backlash_max_ = backlash_max;
                } catch (...) {
                    handle_ = nullptr;
                    sdk.close_focuser(handle);
                    throw;
                }
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            status_cache_.reset();
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws the error still surfaces,
        // but the driver must not stay half-connected.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_.store(false);
            max_step_current_.store(0);
        }
        status_cache_.reset();
        // Waits for an exchange in flight: the handle is never closed under one.
        std::lock_guard<std::mutex> io(io_mutex_);
        if (handle_) {
            const HToupcam handle = handle_;
            handle_ = nullptr;
            sdk.close_focuser(handle);
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

    // FocuserDriver -------------------------------------------------------

    bool get_absolute() const override {
        return true;
    }

    bool get_is_moving() const override { return read_status().moving; }

    int get_max_step() const override { return connected_max_step(); }

    int get_max_increment() const override { return connected_max_step(); }

    int get_position() const override { return read_status().position; }

    double get_step_size() const override {
        // The AAF firmware reports a step size in some configurations, but
        // the value is unitless across mechanical setups so the driver does
        // not expose it as microns-per-step.
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
        throw AlpacaException("Temperature compensation not supported",
                              AlpacaError::NotImplemented);
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
            sdk_.aaf_set(handle_, ToupAAF::Halt, 0, "Toupcam_AAF(HALT)");
        }
        status_cache_.invalidate();
    }

    void move(int position) override {
        // Exception ordering is unchanged (NotConnected, then InvalidValue). The write runs under the I/O
        // lock, which a disconnect takes before it closes the handle (no use-after-close).
        const int max_step = connected_max_step();
        if (position < 0 || position > max_step) {
            throw AlpacaException("Focuser position out of range",
                                  AlpacaError::InvalidValue);
        }
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            ensure_connected();  // a disconnect that won the I/O lock has closed the handle
            sdk_.aaf_set(handle_, ToupAAF::SetPosition, position, "Toupcam_AAF(SETPOSITION)");
        }
        status_cache_.invalidate();
    }

private:
    // The connection flag and the limit are read in one state-lock hold. Connect publishes the limit under
    // the lock before it sets the flag; disconnect clears both under the lock. A disconnected reading throws
    // NotConnected without a second read: a reconnect and disconnect between two reads would yield a zero
    // limit, which turns NotConnected into an out-of-range InvalidValue.
    int connected_max_step() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
        return max_step_current_.load();
    }

    // One refill reads IsMoving, Position and Temperature in a burst; every
    // getter and DeviceState reads a field of it (open-astro#294). 100 ms as the
    // other cached request/response drivers: a SetPosition write drops the frame,
    // so the TTL only bounds how stale a motion started elsewhere can look.
    struct Status {
        bool moving{};
        int position{};
        std::optional<double> temperature;                         // absent when GetTemp failed
        std::string temperature_error;                             // why, for the Temperature getter
        int temperature_error_code{AlpacaError::DriverException};  // the SDK error's mapped code
        bool closed{};  // a disconnect won the I/O lock first: not a device frame, never a link fault
    };

    static constexpr std::chrono::milliseconds kStatusTtl{100};

    Status read_status() const {
        ensure_connected();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: transition_mutex_, cache
        // mutex, I/O lock, mutex_), so it takes no driver lock and no caller may hold mutex_ or the I/O lock
        // around the cache. set_connected(false) closes handle_ under the I/O lock.
        Status status = status_cache_.get([this] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            if (!connected_.load()) {
                // A disconnect that won the I/O lock has closed the handle. Returning (not throwing) keeps it
                // out of the link-health count; the disconnect's reset() drops this frame.
                fresh.closed = true;
                return fresh;
            }
            const HToupcam handle = handle_;
            fresh.moving = sdk_.aaf_get(handle, ToupAAF::IsMoving, "Toupcam_AAF(ISMOVING)") != 0;
            fresh.position = sdk_.aaf_get(handle, ToupAAF::GetPosition, "Toupcam_AAF(GETPOSITION)");
            try {
                const int t = sdk_.aaf_get(handle, ToupAAF::GetTemp, "Toupcam_AAF(GETTEMP)");
                fresh.temperature = static_cast<double>(t) / 10.0;
            } catch (const AlpacaException& e) {
                // Temperature is optional: moving/position still answer.
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

    // Caller holds io_mutex_ (the enumeration is a device call).
    ToupFocuserInfo resolve_focuser(ToupTekSDK& sdk) {
        auto focusers = sdk.enumerate_focusers();
        if (focusers.empty()) {
            ALPACA_LOG_WARN(kLogTag, "No ToupTek AAF focusers detected by SDK");
            throw AlpacaException("No ToupTek AAF focusers detected",
                                  AlpacaError::NotConnected);
        }
        if (focuser_id_.has_value() && !focuser_id_->empty()) {
            for (const auto& f : focusers) {
                if (f.id == *focuser_id_) {
                    return f;
                }
            }
            throw AlpacaException("ToupTek AAF id not found: " + *focuser_id_,
                                  AlpacaError::NotConnected);
        }
        if (focuser_index_.has_value()) {
            int index = focuser_index_.value();
            if (index < 0 || index >= static_cast<int>(focusers.size())) {
                throw AlpacaException("Focuser index out of range",
                                      AlpacaError::InvalidValue);
            }
            return focusers[static_cast<std::size_t>(index)];
        }
        throw AlpacaException("Focuser identifier not specified",
                              AlpacaError::InvalidValue);
    }

    // Injected SDK seam (issue #104); see touptek_sdk_wrapper.h.
    ToupTekSDK& sdk_;
    int device_number_;
    std::optional<int> focuser_index_;
    std::optional<std::string> focuser_id_;

    // handle_ is guarded by io_mutex_. info_, max_step_limit_ and backlash_max_ are guarded by mutex_.
    HToupcam handle_{nullptr};
    ToupFocuserInfo info_{};
    int max_step_limit_{0};
    std::atomic<int> max_step_current_{0};
    int backlash_max_{0};

    std::atomic<bool> connected_{false};
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf).
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the wire.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<FocuserDriver> create_touptek_focuser_by_index(int device_number,
                                                                int focuser_index) {
    return std::make_unique<ToupTekFocuserDriver>(device_number, focuser_index, std::nullopt,
                                                  ToupTekSDKWrapper::instance());
}

std::unique_ptr<FocuserDriver> create_touptek_focuser_by_id(int device_number,
                                                             const std::string& focuser_id) {
    return std::make_unique<ToupTekFocuserDriver>(device_number, std::nullopt, focuser_id,
                                                  ToupTekSDKWrapper::instance());
}

std::unique_ptr<FocuserDriver> create_touptek_focuser_by_index(int device_number, int focuser_index, ToupTekSDK& sdk) {
    return std::make_unique<ToupTekFocuserDriver>(device_number, focuser_index, std::nullopt, sdk);
}

std::unique_ptr<FocuserDriver> create_touptek_focuser_by_id(int device_number, const std::string& focuser_id,
                                                            ToupTekSDK& sdk) {
    return std::make_unique<ToupTekFocuserDriver>(device_number, std::nullopt, focuser_id, sdk);
}

} // namespace alpacacore::vendor::touptek
