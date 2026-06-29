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
#include <alpacacore/vendor/zwo/zwo_efw_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::zwo {

class ZWOEFWFilterWheelDriver : public FilterWheelDriver {
public:
    ZWOEFWFilterWheelDriver(int device_number, std::optional<int> wheel_id, std::optional<int> wheel_index)
        : device_number_(device_number)
        , wheel_id_(wheel_id)
        , wheel_index_(wheel_index)
        , serial_number_()
        , wheel_info_()
        , wheel_info_valid_(false)
        , filter_names_()
        , focus_offsets_()
        , connected_(false)
        , connecting_(false)
    {
    }

    ~ZWOEFWFilterWheelDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during EFW destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && !wheel_info_.name.empty()) {
            return wheel_info_.name;
        }
        return "ZWO EFW";
    }

    DeviceType get_device_type() const override {
        return DeviceType::FilterWheel;
    }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "ZWO_EFW_SN_" + serial_number_;
        }
        if (wheel_id_.has_value()) {
            return "ZWO_EFW_ID_" + std::to_string(wheel_id_.value());
        }
        return "ZWO_EFW_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO EFW Filter Wheel Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO EFW Filter Wheel Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only (never in
    // DriverInfo). EFWGetSDKVersion() returns "1, 7, 0, 0"; render as "1.7.0.0".
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = ZWOEFWSDKWrapper::instance().get_sdk_version();
        if (version.empty()) {
            return std::nullopt;
        }
        std::string normalized;
        normalized.reserve(version.size());
        for (char c : version) {
            if (c == ' ') {
                continue;
            }
            normalized.push_back(c == ',' ? '.' : c);
        }
        return normalized;
    }

    int get_interface_version() const override {
        return 3;
    }

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

        auto& sdk = ZWOEFWSDKWrapper::instance();
        if (connected) {
            int resolved_id = resolve_wheel_id_locked();
            sdk.open_wheel(resolved_id);
            try {
                refresh_wheel_info_locked(resolved_id);
                try {
                    serial_number_ = sdk.get_serial_number(resolved_id);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ZWO", "EFW serial number unavailable: " + std::string(e.what()));
                    serial_number_.clear();
                }
            } catch (...) {
                sdk.close_wheel(resolved_id);
                throw;
            }
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws (e.g.
        // device unplugged) the error still surfaces, but the driver must not
        // stay half-connected.
        const std::optional<int> close_id = wheel_id_;
        if (wheel_index_.has_value()) {
            wheel_id_.reset();
            wheel_info_ = {};
            wheel_info_valid_ = false;
            serial_number_.clear();
        }
        connected_.store(false);
        if (close_id.has_value()) {
            sdk.close_wheel(close_id.value());
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

    int get_position() const override {
        ensure_connected();
        return ZWOEFWSDKWrapper::instance().get_position(wheel_id_value());
    }

    void set_position(int position) override {
        ensure_connected();
        int slot_count = slot_count_value();
        if (position < 0 || position >= slot_count) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ZWOEFWSDKWrapper::instance().set_position(wheel_id_value(), position);
    }

    std::vector<int> get_focus_offsets() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return focus_offsets_;
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && wheel_info_.slot_count > 0) {
            validate_slot_count_locked(static_cast<int>(offsets.size()), "focusOffsets");
        }
        focus_offsets_ = offsets;
    }

    std::vector<std::string> get_names() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return filter_names_;
    }

    void set_names(const std::vector<std::string>& names) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && wheel_info_.slot_count > 0) {
            validate_slot_count_locked(static_cast<int>(names.size()), "names");
        }
        filter_names_ = names;
        apply_default_names_locked();
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
    }

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
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
                ALPACA_LOG_ERROR("ZWO", "EFW connection failed: " + std::string(e.what()));
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

    int resolve_wheel_id_locked() {
        if (wheel_index_.has_value()) {
            auto wheels = ZWOEFWSDKWrapper::instance().enumerate_wheels();
            if (wheels.empty()) {
                ALPACA_LOG_WARN("ZWO", "No ZWO EFW wheels detected by SDK");
                throw AlpacaException("No ZWO EFW wheels detected", AlpacaError::NotConnected);
            }
            int index = wheel_index_.value();
            if (index < 0 || index >= static_cast<int>(wheels.size())) {
                ALPACA_LOG_WARN("ZWO", "Filter wheel index out of range: " + std::to_string(index) +
                                         " (count=" + std::to_string(wheels.size()) + ")");
                throw AlpacaException("Filter wheel index not found", AlpacaError::InvalidValue);
            }
            const auto& info = wheels[static_cast<std::size_t>(index)];
            wheel_id_ = info.wheel_id;
            wheel_info_ = info;
            wheel_info_valid_ = true;
            normalize_slot_data_locked();
            return wheel_id_.value();
        }

        if (wheel_id_.has_value()) {
            return wheel_id_.value();
        }

        throw AlpacaException("Filter wheel ID not specified", AlpacaError::InvalidValue);
    }

    void refresh_wheel_info_locked(int wheel_id) {
        ZWOEFWInfo info;
        if (ZWOEFWSDKWrapper::instance().get_wheel_info_by_id(wheel_id, info)) {
            wheel_info_ = info;
            wheel_info_valid_ = true;
            normalize_slot_data_locked();
            return;
        }
        throw AlpacaException("Failed to read filter wheel info", AlpacaError::DriverException);
    }

    int wheel_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wheel_id_.has_value()) {
            throw AlpacaException("Filter wheel ID not set", AlpacaError::NotConnected);
        }
        return wheel_id_.value();
    }

    int slot_count_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wheel_info_valid_ || wheel_info_.slot_count <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        return wheel_info_.slot_count;
    }

    void normalize_slot_data_locked() {
        if (!wheel_info_valid_ || wheel_info_.slot_count <= 0) {
            return;
        }
        const std::size_t slots = static_cast<std::size_t>(wheel_info_.slot_count);
        if (filter_names_.size() == 1) {
            const std::string& candidate = filter_names_[0];
            if (candidate.size() == slots &&
                candidate.find_first_of(",; \t") == std::string::npos) {
                filter_names_.clear();
                filter_names_.reserve(slots);
                for (char ch : candidate) {
                    filter_names_.emplace_back(1, ch);
                }
            }
        }
        if (filter_names_.empty()) {
            filter_names_.assign(slots, std::string());
        } else if (filter_names_.size() != slots) {
            ALPACA_LOG_WARN("ZWO", "Configured filterNames count (" + std::to_string(filter_names_.size()) +
                                       ") does not match wheel slot count (" + std::to_string(slots) +
                                       "); resizing to match the wheel");
            filter_names_.resize(slots);
        }
        apply_default_names_locked();
        if (focus_offsets_.empty()) {
            focus_offsets_.assign(slots, 0);
        } else if (focus_offsets_.size() != slots) {
            ALPACA_LOG_WARN("ZWO", "Configured focusOffsets count (" + std::to_string(focus_offsets_.size()) +
                                       ") does not match wheel slot count (" + std::to_string(slots) +
                                       "); resizing to match the wheel");
            focus_offsets_.resize(slots);
        }
    }

    void apply_default_names_locked() {
        for (std::size_t i = 0; i < filter_names_.size(); ++i) {
            if (filter_names_[i].empty()) {
                filter_names_[i] = "Filter " + std::to_string(i + 1);
            }
        }
    }

    void validate_slot_count_locked(int provided_size, std::string_view field_name) const {
        if (!wheel_info_valid_ || wheel_info_.slot_count <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (provided_size != wheel_info_.slot_count) {
            throw AlpacaException("Invalid " + std::string(field_name) +
                                  " length: expected " + std::to_string(wheel_info_.slot_count),
                                  AlpacaError::InvalidValue);
        }
    }

    int device_number_;
    std::optional<int> wheel_id_;
    std::optional<int> wheel_index_;
    std::string serial_number_;
    ZWOEFWInfo wheel_info_;
    bool wheel_info_valid_;
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;
    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel(int device_number, int wheel_id) {
    return std::make_unique<ZWOEFWFilterWheelDriver>(device_number, wheel_id, std::nullopt);
}

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel_by_index(int device_number, int wheel_index) {
    return std::make_unique<ZWOEFWFilterWheelDriver>(device_number, std::nullopt, wheel_index);
}

} // namespace alpacacore::vendor::zwo
