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
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_eaf_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

namespace alpacacore::vendor::zwo {

class ZWOEAFFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    ZWOEAFFocuserDriver(int device_number, std::optional<int> focuser_id, std::optional<int> focuser_index)
        : AsyncConnectable("ZWO"),
          device_number_(device_number),
          focuser_id_(focuser_id),
          focuser_index_(focuser_index),
          serial_number_(),
          focuser_info_(),
          focuser_info_valid_(false),
          connected_(false) {}

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
        auto version = ZWOEAFSDKWrapper::instance().get_sdk_version();
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
        std::lock_guard<std::mutex> lock(mutex_);
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

        auto& sdk = ZWOEAFSDKWrapper::instance();
        if (connected) {
            int resolved_id = resolve_focuser_id_locked();
            sdk.open_focuser(resolved_id);
            try {
                refresh_focuser_info_locked(resolved_id);
                try {
                    serial_number_ = sdk.get_serial_number(resolved_id);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ZWO", "EAF serial number unavailable: " + std::string(e.what()));
                    serial_number_.clear();
                }
            } catch (...) {
                sdk.close_focuser(resolved_id);
                throw;
            }
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws (e.g.
        // device unplugged) the error still surfaces, but the driver must not
        // stay half-connected.
        const std::optional<int> close_id = focuser_id_;
        if (focuser_index_.has_value()) {
            focuser_id_.reset();
            focuser_info_ = {};
            focuser_info_valid_ = false;
            serial_number_.clear();
        }
        connected_.store(false);
        if (close_id.has_value()) {
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

    bool get_is_moving() const override {
        ensure_connected();
        return ZWOEAFSDKWrapper::instance().is_moving(focuser_id_value());
    }

    int get_max_step() const override {
        ensure_connected();
        return ZWOEAFSDKWrapper::instance().get_max_step(focuser_id_value());
    }

    int get_max_increment() const override {
        ensure_connected();
        int max_step = get_max_step();
        try {
            int range = ZWOEAFSDKWrapper::instance().get_step_range(focuser_id_value());
            if (range > 0) {
                return range > max_step ? max_step : range;
            }
        } catch (const std::exception&) {
        }
        return max_step;
    }

    int get_position() const override {
        ensure_connected();
        return ZWOEAFSDKWrapper::instance().get_position(focuser_id_value());
    }

    double get_step_size() const override {
        throw AlpacaException("Step size is not available for this focuser",
                              AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        return false;
    }

    bool get_temp_comp() const override {
        return false;
    }

    void set_temp_comp(bool) override {
        throw AlpacaException("Temperature compensation not supported", AlpacaError::NotImplemented);
    }

    double get_temperature() const override {
        ensure_connected();
        return ZWOEAFSDKWrapper::instance().get_temperature(focuser_id_value());
    }

    void halt() override {
        ensure_connected();
        ZWOEAFSDKWrapper::instance().stop(focuser_id_value());
    }

    void move(int position) override {
        ensure_connected();
        int max_step = get_max_step();
        if (position < 0 || position > max_step) {
            throw AlpacaException("Focuser position out of range", AlpacaError::InvalidValue);
        }
        ZWOEAFSDKWrapper::instance().move(focuser_id_value(), position);
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
    }

    int resolve_focuser_id_locked() {
        if (focuser_index_.has_value()) {
            auto focusers = ZWOEAFSDKWrapper::instance().enumerate_focusers();
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
            focuser_id_ = info.focuser_id;
            focuser_info_ = info;
            focuser_info_valid_ = true;
            return focuser_id_.value();
        }

        if (focuser_id_.has_value()) {
            return focuser_id_.value();
        }

        throw AlpacaException("Focuser ID not specified", AlpacaError::InvalidValue);
    }

    void refresh_focuser_info_locked(int focuser_id) {
        ZWOEAFFocuserInfo info;
        if (ZWOEAFSDKWrapper::instance().get_focuser_info_by_id(focuser_id, info)) {
            focuser_info_ = info;
            focuser_info_valid_ = true;
            return;
        }
        throw AlpacaException("Failed to read focuser info", AlpacaError::DriverException);
    }

    int focuser_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!focuser_id_.has_value()) {
            throw AlpacaException("Focuser ID not set", AlpacaError::NotConnected);
        }
        return focuser_id_.value();
    }

    int device_number_;
    std::optional<int> focuser_id_;
    std::optional<int> focuser_index_;
    std::string serial_number_;
    ZWOEAFFocuserInfo focuser_info_;
    bool focuser_info_valid_;
    std::atomic<bool> connected_;
    mutable std::mutex mutex_;
};

std::unique_ptr<FocuserDriver> create_zwo_eaf_focuser(int device_number, int focuser_id) {
    return std::make_unique<ZWOEAFFocuserDriver>(device_number, focuser_id, std::nullopt);
}

std::unique_ptr<FocuserDriver> create_zwo_eaf_focuser_by_index(int device_number, int focuser_index) {
    return std::make_unique<ZWOEAFFocuserDriver>(device_number, std::nullopt, focuser_index);
}

} // namespace alpacacore::vendor::zwo
