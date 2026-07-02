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
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>

namespace alpacacore::vendor::zwo {

namespace {

constexpr int kDewHeaterSwitchId = 0;

} // namespace

class ZWODewHeaterSwitchDriver : public SwitchDriver {
public:
    ZWODewHeaterSwitchDriver(int device_number, std::optional<int> camera_id, std::optional<int> camera_index)
        : device_number_(device_number)
        , camera_id_(camera_id)
        , camera_index_(camera_index)
        , serial_number_()
        , camera_name_("ZWO Camera")
        , dew_caps_()
        , connected_(false)
        , connecting_(false)
        , switch_name_("DewHeater")
        , switch_description_("ZWO camera anti-dew heater control")
    {
    }

    ~ZWODewHeaterSwitchDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during dew heater destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        return "ZWO Dew Heater";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Switch;
    }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "ZWO_DEW_SN_" + serial_number_;
        }
        return "ZWO_DEW_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO camera dew heater switch";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO Dew Heater Switch";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

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

        auto& sdk = ZWOSDKWrapper::instance();
        if (connected) {
            int resolved_id = resolve_camera_id_locked();
            sdk.open_camera(resolved_id);
            sdk.init_camera(resolved_id);
            try {
                refresh_camera_name_locked(resolved_id);
                load_dew_caps_locked(resolved_id);
                serial_number_ = sdk.get_serial_number(resolved_id);
            } catch (...) {
                sdk.close_camera(resolved_id);
                throw;
            }
            connected_.store(true);
            return;
        }

        if (camera_id_.has_value()) {
            sdk.close_camera(camera_id_.value());
        }
        if (camera_index_.has_value()) {
            camera_id_.reset();
            camera_name_ = "ZWO Camera";
            serial_number_.clear();
        }
        dew_caps_.reset();
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
        throw AlpacaException("CommandBlind not supported", AlpacaError::NotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("CommandBool not supported", AlpacaError::NotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("CommandString not supported", AlpacaError::NotImplemented);
    }

    int get_max_switch() const override {
        return 1;
    }

    bool get_can_write(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        const auto& caps = dew_caps_or_throw();
        return caps.is_writable;
    }

    bool get_can_async(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return false;
    }

    bool get_switch(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        double value = get_switch_value(id);
        return value > get_min_switch_value(id);
    }

    void set_switch(int id, bool state) override {
        validate_switch_id(id);
        ensure_connected();
        double value = state ? get_max_switch_value(id) : get_min_switch_value(id);
        set_switch_value(id, value);
    }

    void set_async(int id, bool /*state*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    double get_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        dew_caps_or_throw();
        bool is_auto = false;
        long value = 0;
        if (!ZWOSDKWrapper::instance().get_control_value(camera_id_value(), ZWOControlType::AntiDewHeater, value, is_auto)) {
            throw AlpacaException("Failed to read dew heater value", AlpacaError::DriverException);
        }
        return static_cast<double>(value);
    }

    void set_switch_value(int id, double value) override {
        validate_switch_id(id);
        ensure_connected();
        const auto& caps = dew_caps_or_throw();
        if (!caps.is_writable) {
            throw AlpacaException("Dew heater is read-only", AlpacaError::InvalidOperation);
        }
        // std::lround on NaN/Inf is undefined behaviour; reject non-finite
        // values from the HTTP API as InvalidValue first.
        if (!std::isfinite(value)) {
            throw AlpacaException("Dew heater value must be a finite number", AlpacaError::InvalidValue);
        }
        long value_long = static_cast<long>(std::lround(value));
        if (value_long < caps.min_value || value_long > caps.max_value) {
            throw AlpacaException("Dew heater value out of range", AlpacaError::InvalidValue);
        }
        ZWOSDKWrapper::instance().set_control_value(camera_id_value(), ZWOControlType::AntiDewHeater, value_long, false);
    }

    void set_async_value(int id, double /*value*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    bool get_state_change_complete(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return true;
    }

    std::string get_switch_name(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return switch_name_;
    }

    void set_switch_name(int id, const std::string& name) override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        switch_name_ = name;
    }

    std::string get_switch_description(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return switch_description_ + " (" + camera_name_ + ")";
    }

    double get_min_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return static_cast<double>(dew_caps_or_throw().min_value);
    }

    double get_max_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return static_cast<double>(dew_caps_or_throw().max_value);
    }

    double get_switch_step(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return 1.0;
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Dew heater not connected", AlpacaError::NotConnected);
        }
    }

    void validate_switch_id(int id) const {
        if (id != kDewHeaterSwitchId) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
    }

    const ZWOControlCaps& dew_caps_or_throw() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dew_caps_.has_value()) {
            throw AlpacaException("Dew heater not supported", AlpacaError::NotImplemented);
        }
        return dew_caps_.value();
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
                ALPACA_LOG_ERROR("ZWO", "Dew heater connection failed: " + std::string(e.what()));
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

    int resolve_camera_id_locked() {
        if (camera_index_.has_value()) {
            auto cameras = ZWOSDKWrapper::instance().enumerate_cameras();
            if (cameras.empty()) {
                ALPACA_LOG_WARN("ZWO", "No ZWO cameras detected by SDK");
                throw AlpacaException("No ZWO cameras detected", AlpacaError::NotConnected);
            }
            int index = camera_index_.value();
            if (index < 0 || index >= static_cast<int>(cameras.size())) {
                ALPACA_LOG_WARN("ZWO", "Camera index out of range: " + std::to_string(index) + " (count=" + std::to_string(cameras.size()) + ")");
                throw AlpacaException("Camera index not found", AlpacaError::InvalidValue);
            }
            const auto& info = cameras[static_cast<std::size_t>(index)];
            camera_id_ = info.camera_id;
            camera_name_ = info.name;
            return camera_id_.value();
        }

        if (camera_id_.has_value()) {
            return camera_id_.value();
        }

        throw AlpacaException("Camera ID not specified", AlpacaError::InvalidValue);
    }

    void refresh_camera_name_locked(int camera_id) {
        ZWOCameraInfo info;
        if (ZWOSDKWrapper::instance().get_camera_info_by_id(camera_id, info) && !info.name.empty()) {
            camera_name_ = info.name;
        }
    }

    void load_dew_caps_locked(int camera_id) {
        dew_caps_.reset();
        auto caps = ZWOSDKWrapper::instance().get_control_caps(camera_id);
        for (const auto& cap : caps) {
            if (cap.type == ZWOControlType::AntiDewHeater) {
                dew_caps_ = cap;
                return;
            }
        }
        throw AlpacaException("Dew heater not supported", AlpacaError::NotImplemented);
    }

    int camera_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        return camera_id_.value();
    }

    int device_number_;
    std::optional<int> camera_id_;
    std::optional<int> camera_index_;
    std::string serial_number_;
    std::string camera_name_;
    std::optional<ZWOControlCaps> dew_caps_;
    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
    std::string switch_name_;
    std::string switch_description_;
};

std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch(int device_number, int camera_id) {
    return std::make_unique<ZWODewHeaterSwitchDriver>(device_number, camera_id, std::nullopt);
}

std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch_by_index(int device_number, int camera_index) {
    return std::make_unique<ZWODewHeaterSwitchDriver>(device_number, std::nullopt, camera_index);
}

} // namespace alpacacore::vendor::zwo
