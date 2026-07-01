// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/touptek/touptek_focuser_driver.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogTag = "ToupTek";

} // namespace

class ToupTekFocuserDriver : public FocuserDriver {
public:
    ToupTekFocuserDriver(int device_number,
                          std::optional<int> focuser_index,
                          std::optional<std::string> focuser_id)
        : device_number_(device_number)
        , focuser_index_(focuser_index)
        , focuser_id_(std::move(focuser_id))
    {}

    ~ToupTekFocuserDriver() override {
        {
            // Block any new connection task from spawning a thread that would
            // outlive this object (destructor race -> std::terminate on an
            // unjoined connection_thread_).
            std::lock_guard<std::mutex> lock(connection_mutex_);
            shutting_down_ = true;
        }
        stop_connection_thread();
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

    std::string get_unique_id() const override {
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

    bool get_connecting() const override {
        return connecting_.load();
    }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = ToupTekSDKWrapper::instance();

        if (connected) {
            ToupFocuserInfo info = resolve_focuser_locked(sdk);
            HToupcam handle = sdk.open_focuser_by_id(info.id);
            try {
                int max_step = sdk.aaf_range(handle, ToupAAF::RangeMax,
                                              ToupAAF::GetMaxStep,
                                              "Toupcam_AAF(RANGEMAX,GETMAXSTEP)");
                int current_max = sdk.aaf_get(handle, ToupAAF::GetMaxStep,
                                               "Toupcam_AAF(GETMAXSTEP)");
                int backlash_max = sdk.aaf_range(handle, ToupAAF::RangeMax,
                                                  ToupAAF::GetBacklash,
                                                  "Toupcam_AAF(RANGEMAX,GETBACKLASH)");
                handle_ = handle;
                info_ = info;
                max_step_limit_ = max_step;
                max_step_current_ = current_max > 0 ? current_max : max_step;
                backlash_max_ = backlash_max;
            } catch (...) {
                sdk.close_focuser(handle);
                throw;
            }
            connected_.store(true);
            return;
        }

        if (handle_) {
            sdk.close_focuser(handle_);
            handle_ = nullptr;
        }
        connected_.store(false);
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

    bool get_is_moving() const override {
        ensure_connected();
        return ToupTekSDKWrapper::instance().aaf_get(handle_, ToupAAF::IsMoving,
                                                     "Toupcam_AAF(ISMOVING)") != 0;
    }

    int get_max_step() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return max_step_current_;
    }

    int get_max_increment() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return max_step_current_;
    }

    int get_position() const override {
        ensure_connected();
        return ToupTekSDKWrapper::instance().aaf_get(handle_, ToupAAF::GetPosition,
                                                     "Toupcam_AAF(GETPOSITION)");
    }

    double get_step_size() const override {
        // The AAF firmware reports a step size in some configurations, but
        // the value is unitless across mechanical setups so the driver does
        // not expose it as microns-per-step.
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
        throw AlpacaException("Temperature compensation not supported",
                              AlpacaError::NotImplemented);
    }

    double get_temperature() const override {
        ensure_connected();
        int t = ToupTekSDKWrapper::instance().aaf_get(handle_, ToupAAF::GetTemp,
                                                       "Toupcam_AAF(GETTEMP)");
        return static_cast<double>(t) / 10.0;
    }

    void halt() override {
        ensure_connected();
        ToupTekSDKWrapper::instance().aaf_set(handle_, ToupAAF::Halt, 0,
                                               "Toupcam_AAF(HALT)");
    }

    void move(int position) override {
        ensure_connected();
        int max_step;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            max_step = max_step_current_;
        }
        if (position < 0 || position > max_step) {
            throw AlpacaException("Focuser position out of range",
                                  AlpacaError::InvalidValue);
        }
        ToupTekSDKWrapper::instance().aaf_set(handle_, ToupAAF::SetPosition, position,
                                               "Toupcam_AAF(SETPOSITION)");
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
    }

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (shutting_down_) {
            return;  // Destruction in progress; never spawn a new thread.
        }
        if (connecting_.load()) {
            return;
        }
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
        connecting_.store(true);
        connection_thread_ = std::thread([this, connect]() {
            try {
                set_connected(connect);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR(kLogTag,
                                 std::string("AAF connection failed: ") + e.what());
            }
            connecting_.store(false);
        });
    }

    void stop_connection_thread() {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
    }

    ToupFocuserInfo resolve_focuser_locked(ToupTekSDKWrapper& sdk) {
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

    int device_number_;
    std::optional<int> focuser_index_;
    std::optional<std::string> focuser_id_;

    HToupcam handle_{nullptr};
    ToupFocuserInfo info_{};
    int max_step_limit_{0};
    int max_step_current_{0};
    int backlash_max_{0};

    std::atomic<bool> connected_{false};
    std::atomic<bool> connecting_{false};
    bool shutting_down_ = false;  // guarded by connection_mutex_
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<FocuserDriver> create_touptek_focuser_by_index(int device_number,
                                                                int focuser_index) {
    return std::make_unique<ToupTekFocuserDriver>(device_number, focuser_index, std::nullopt);
}

std::unique_ptr<FocuserDriver> create_touptek_focuser_by_id(int device_number,
                                                             const std::string& focuser_id) {
    return std::make_unique<ToupTekFocuserDriver>(device_number, std::nullopt, focuser_id);
}

} // namespace alpacacore::vendor::touptek
