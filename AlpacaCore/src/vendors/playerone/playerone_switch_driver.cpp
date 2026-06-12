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
#include <alpacacore/vendor/playerone/playerone_sdk_wrapper.h>
#include <alpacacore/vendor/playerone/playerone_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::playerone {

namespace {

// A camera exposes at most a dew heater and a fan. Used as the switch-ID
// bound while disconnected, before the per-model element list is probed.
constexpr int kMaxThermalElements = 2;

enum class ThermalElementKind : std::uint8_t { DewHeater, Fan };

struct ThermalElement {
    ThermalElementKind kind{};
    std::string name;
    std::string description;
    bool writable{};
    long min_value{};
    long max_value{};
};

}  // namespace

class PlayerOneSwitchDriver : public SwitchDriver {
public:
    PlayerOneSwitchDriver(int device_number, int camera_index)
        : device_number_(device_number),
          camera_index_(camera_index),
          camera_name_("Player One Camera"),
          connected_(false),
          connecting_(false) {}

    ~PlayerOneSwitchDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                PlayerOneSwitchDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Error during switch destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "Player One Thermal Switch"; }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "PLAYERONE_SW_SN_" + serial_number_;
        }
        return "PLAYERONE_SW_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "Player One camera dew heater and fan switch"; }

    std::string get_driver_info() const override { return "AlpacaCore Player One Thermal Switch"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = PlayerOneSDKWrapper::instance();
        if (connected) {
            auto cameras = sdk.enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No Player One cameras detected", AlpacaError::NotConnected);
            }
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range", AlpacaError::InvalidValue);
            }
            const auto& info = cameras[static_cast<std::size_t>(camera_index_)];
            camera_id_ = info.camera_id;
            camera_name_ = info.name;
            serial_number_ = info.serial_number;

            sdk.open_camera(camera_id_);
            try {
                sdk.init_camera(camera_id_);
                build_elements_locked(sdk.probe_config_caps(camera_id_));
            } catch (...) {
                sdk.close_camera(camera_id_);
                camera_id_ = -1;
                throw;
            }
            connected_.store(true);
            return;
        }

        if (camera_id_ >= 0) {
            sdk.close_camera(camera_id_);
        }
        camera_id_ = -1;
        elements_.clear();
        connected_.store(false);
    }

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        if (!connected_.load()) {
            return state;
        }
        const int count = get_max_switch();
        for (int id = 0; id < count; ++id) {
            try {
                state.push_back({"GetSwitch" + std::to_string(id), get_switch(id)});
                state.push_back({"GetSwitchValue" + std::to_string(id), get_switch_value(id)});
                state.push_back({"StateChangeComplete" + std::to_string(id), get_state_change_complete(id)});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Omit unavailable members per the DeviceState contract.
            }
        }
        return state;
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override { return false; }

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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_.load()) {
            return kMaxThermalElements;
        }
        return static_cast<int>(elements_.size());
    }

    bool get_can_write(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return element_copy(id).writable;
    }

    bool get_can_async(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return false;
    }

    bool get_switch(int id) const override {
        double value = get_switch_value(id);
        return value > get_min_switch_value(id);
    }

    void set_switch(int id, bool state) override {
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
        const auto element = element_copy(id);
        auto& sdk = PlayerOneSDKWrapper::instance();
        const int camera_id = camera_id_copy();
        int value = element.kind == ThermalElementKind::DewHeater ? sdk.get_heater_power_percent(camera_id)
                                                                  : sdk.get_fan_power_percent(camera_id);
        return static_cast<double>(value);
    }

    void set_switch_value(int id, double value) override {
        validate_switch_id(id);
        ensure_connected();
        const auto element = element_copy(id);
        if (!element.writable) {
            throw AlpacaException(element.name + " is read-only", AlpacaError::InvalidOperation);
        }
        // std::lround on NaN/Inf is undefined behaviour; reject non-finite
        // values from the HTTP API as InvalidValue first.
        if (!std::isfinite(value)) {
            throw AlpacaException(element.name + " value must be a finite number", AlpacaError::InvalidValue);
        }
        long value_long = std::lround(value);
        if (value_long < element.min_value || value_long > element.max_value) {
            throw AlpacaException(element.name + " value out of range", AlpacaError::InvalidValue);
        }
        auto& sdk = PlayerOneSDKWrapper::instance();
        const int camera_id = camera_id_copy();
        if (element.kind == ThermalElementKind::DewHeater) {
            sdk.set_heater_power_percent(camera_id, static_cast<int>(value_long));
        } else {
            sdk.set_fan_power_percent(camera_id, static_cast<int>(value_long));
        }
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
        return element_copy(id).name;
    }

    void set_switch_name(int id, const std::string& name) override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
        elements_[static_cast<std::size_t>(id)].name = name;
    }

    std::string get_switch_description(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
        return elements_[static_cast<std::size_t>(id)].description + " (" + camera_name_ + ")";
    }

    double get_min_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return static_cast<double>(element_copy(id).min_value);
    }

    double get_max_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return static_cast<double>(element_copy(id).max_value);
    }

    double get_switch_step(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return 1.0;
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Switch not connected", AlpacaError::NotConnected);
        }
    }

    // ID validation runs before the connection check (ASCOM: out-of-range ID
    // is InvalidValue even while disconnected). Disconnected, the bound is
    // the potential element count; connected, the probed per-model count.
    void validate_switch_id_locked(int id) const {
        const int limit = connected_.load() ? static_cast<int>(elements_.size()) : kMaxThermalElements;
        if (id < 0 || id >= limit) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
    }

    void validate_switch_id(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
    }

    ThermalElement element_copy(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
        return elements_[static_cast<std::size_t>(id)];
    }

    int camera_id_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_id_ < 0) {
            throw AlpacaException("Camera ID not available", AlpacaError::NotConnected);
        }
        return camera_id_;
    }

    void build_elements_locked(const PlayerOneConfigCaps& caps) {
        elements_.clear();
        if (caps.has_heater_power) {
            ThermalElement e;
            e.kind = ThermalElementKind::DewHeater;
            e.name = "DewHeater";
            e.description = "Player One camera anti-dew lens heater power, percent";
            e.writable = caps.heater_power_writable;
            e.min_value = caps.heater_power_min;
            e.max_value = caps.heater_power_max;
            elements_.push_back(std::move(e));
        }
        if (caps.has_fan_power) {
            ThermalElement e;
            e.kind = ThermalElementKind::Fan;
            e.name = "Fan";
            e.description = "Player One camera radiator fan power, percent";
            e.writable = caps.fan_power_writable;
            e.min_value = caps.fan_power_min;
            e.max_value = caps.fan_power_max;
            elements_.push_back(std::move(e));
        }
        if (elements_.empty()) {
            throw AlpacaException("Camera has no dew heater or fan (uncooled model?)", AlpacaError::NotImplemented);
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
                ALPACA_LOG_ERROR("PlayerOne", "Switch connection failed: " + std::string(e.what()));
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

    int device_number_;
    int camera_index_;
    int camera_id_{-1};
    std::string serial_number_;
    std::string camera_name_;
    std::vector<ThermalElement> elements_;
    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<SwitchDriver> create_playerone_switch(int device_number, int camera_index) {
    return std::make_unique<PlayerOneSwitchDriver>(device_number, camera_index);
}

}  // namespace alpacacore::vendor::playerone
