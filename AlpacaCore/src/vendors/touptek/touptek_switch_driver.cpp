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
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogCategory = "TOUPTEK_STELLAVITA";

}  // namespace

TouptekSwitchConfig default_stellavita_config() {
    TouptekSwitchConfig cfg;
    // StellaVita is a Raspberry Pi CM4 (BCM2711). The 40-pin header bank is
    // /dev/gpiochip0 (pinctrl-bcm2711); the libgpiod line offset equals the BCM
    // GPIO number. Mapping verified on hardware against config.txt
    // (gpio=18,10,17,4,9,11=op,dh,pu) — 9/11 power the USB hub and are excluded.
    cfg.gpio_chip_path = "/dev/gpiochip0";
    cfg.ports = {
        // name,      gpio_line, writable
        {"Port 1", 18u, true},
        {"Port 2", 10u, true},
        {"Port 3", 17u, true},
        {"Port 4", 4u, true},
    };
    return cfg;
}

class TouptekSwitchDriver : public SwitchDriver {
public:
    TouptekSwitchDriver(int device_number, TouptekSwitchConfig config)
        : device_number_(device_number),
          config_(std::move(config)),
          wrapper_(config_.gpio_chip_path, config_.ports, config_.pwm_frequency_hz),
          connecting_(false) {
        switch_names_.reserve(config_.ports.size());
        for (const auto& p : config_.ports) {
            switch_names_.emplace_back(p.name);
        }
    }

    ~TouptekSwitchDriver() override {
        stop_connection_thread();
        try {
            wrapper_.close();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogCategory, std::string("Error during StellaVita switch destruction: ") + e.what());
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "ToupTek " + config_.model_name; }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string get_unique_id() const override { return "ToupTek_StellaVita_" + std::to_string(device_number_); }

    std::string get_description() const override {
        return "ToupTek " + config_.model_name + " DC power switch (" + config_.gpio_chip_path + ")";
    }

    std::string get_driver_info() const override { return "AlpacaCore ToupTek " + config_.model_name + " Switch"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return wrapper_.is_open(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        if (connected) {
            if (!wrapper_.is_open()) {
                wrapper_.open();
            }
        } else {
            if (wrapper_.is_open()) {
                wrapper_.close();
            }
        }
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

    int get_max_switch() const override { return static_cast<int>(config_.ports.size()); }

    bool get_can_write(int id) const override {
        validate_id(id);
        return config_.ports[static_cast<std::size_t>(id)].writable;
    }

    bool get_can_async(int id) const override {
        validate_id(id);
        return false;
    }

    bool get_switch(int id) const override {
        validate_id(id);
        ensure_connected();
        return wrapper_.get_value(static_cast<std::size_t>(id)) != 0;
    }

    void set_switch(int id, bool state) override {
        validate_id(id);
        // "On" maps to the port's maximum (100 for a PWM port, 1 for boolean);
        // "off" maps to 0. set_switch_value enforces writability/connection.
        set_switch_value(id, state ? get_max_switch_value_unchecked(id) : 0.0);
    }

    void set_async(int id, bool /*state*/) override {
        validate_id(id);
        ensure_writable(id);
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    double get_switch_value(int id) const override {
        validate_id(id);
        ensure_connected();
        return static_cast<double>(wrapper_.get_value(static_cast<std::size_t>(id)));
    }

    void set_switch_value(int id, double value) override {
        validate_id(id);
        ensure_writable(id);
        ensure_connected();
        // Guard against NaN/Inf from the HTTP API: std::lround on a non-finite
        // value is undefined behaviour. Reject them as InvalidValue up front.
        if (!std::isfinite(value)) {
            throw AlpacaException("Switch value must be a finite number", AlpacaError::InvalidValue);
        }
        const long max_v = static_cast<long>(get_max_switch_value_unchecked(id));
        const long rounded = std::lround(value);
        if (rounded < 0 || rounded > max_v) {
            throw AlpacaException("Switch value out of range [0," + std::to_string(max_v) + "]",
                                  AlpacaError::InvalidValue);
        }
        wrapper_.set_value(static_cast<std::size_t>(id), static_cast<int>(rounded));
    }

    void set_async_value(int id, double /*value*/) override {
        validate_id(id);
        ensure_writable(id);
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    bool get_state_change_complete(int id) const override {
        validate_id(id);
        ensure_connected();
        return true;
    }

    std::string get_switch_name(int id) const override {
        validate_id(id);
        std::lock_guard<std::mutex> lock(name_mutex_);
        return switch_names_[static_cast<std::size_t>(id)];
    }

    void set_switch_name(int id, const std::string& name) override {
        validate_id(id);
        std::lock_guard<std::mutex> lock(name_mutex_);
        switch_names_[static_cast<std::size_t>(id)] = name;
    }

    std::string get_switch_description(int id) const override {
        validate_id(id);
        const auto& p = config_.ports[static_cast<std::size_t>(id)];
        const std::string mode = p.pwm_enabled ? "PWM 0-100%" : "on/off";
        return "StellaVita DC power port on GPIO " + std::to_string(p.gpio_line) + " (" + mode + ")";
    }

    double get_min_switch_value(int id) const override {
        validate_id(id);
        return 0.0;
    }

    double get_max_switch_value(int id) const override {
        validate_id(id);
        return get_max_switch_value_unchecked(id);
    }

    double get_switch_step(int id) const override {
        validate_id(id);
        return 1.0;
    }

private:
    void ensure_connected() const {
        if (!wrapper_.is_open()) {
            throw AlpacaException("StellaVita not connected", AlpacaError::NotConnected);
        }
    }

    void validate_id(int id) const {
        if (id < 0 || id >= static_cast<int>(config_.ports.size())) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
    }

    // A PWM port is an analog channel [0,100]; a boolean port is [0,1].
    double get_max_switch_value_unchecked(int id) const {
        return config_.ports[static_cast<std::size_t>(id)].pwm_enabled ? 100.0 : 1.0;
    }

    // Read-only ports reject writes with a not-implemented error per the ASCOM
    // Switch spec, independent of the connection state.
    void ensure_writable(int id) const {
        if (!config_.ports[static_cast<std::size_t>(id)].writable) {
            throw AlpacaException("Switch " + std::to_string(id) + " is read-only", AlpacaError::NotImplemented);
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
                ALPACA_LOG_ERROR(kLogCategory, std::string("StellaVita connection task failed: ") + e.what());
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

    const int device_number_;
    const TouptekSwitchConfig config_;
    TouptekPowerboxWrapper wrapper_;

    mutable std::mutex name_mutex_;
    std::vector<std::string> switch_names_;

    std::atomic<bool> connecting_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<SwitchDriver> create_touptek_switch(int device_number, TouptekSwitchConfig config) {
    return std::make_unique<TouptekSwitchDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::touptek
