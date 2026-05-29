// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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

#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::zwo {

namespace {

constexpr const char* kLogCategory = "ZWO_ASIAIR";

} // namespace

AsiairSwitchConfig default_asiair_pro_config() {
    AsiairSwitchConfig cfg;
    cfg.gpio_chip_path = "/dev/gpiochip0";
    cfg.pwm_frequency_hz = 1000;
    cfg.ports = {
        {"Port 1", 12u, false},
        {"Port 2", 13u, false},
        {"Port 3", 26u, false},
        {"Port 4", 18u, false},
    };
    return cfg;
}

class ZWOAsiairSwitchDriver : public SwitchDriver {
public:
    ZWOAsiairSwitchDriver(int device_number, AsiairSwitchConfig config)
        : device_number_(device_number)
        , config_(std::move(config))
        , wrapper_(config_.gpio_chip_path, config_.ports, config_.pwm_frequency_hz)
        , connecting_(false)
    {
        switch_names_.reserve(config_.ports.size());
        for (const auto& p : config_.ports) {
            switch_names_.emplace_back(p.name);
        }
    }

    ~ZWOAsiairSwitchDriver() override {
        stop_connection_thread();
        try {
            wrapper_.close();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogCategory,
                            std::string("Error during ASIair switch destruction: ") + e.what());
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "ZWO ASIair Pro Switch"; }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string get_unique_id() const override {
        return "ZWO_ASIAIR_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO ASIair Pro 12V power switch (" + config_.gpio_chip_path + ")";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO ASIair Pro Switch";
    }

    std::string get_driver_version() const override { return "1.0.0"; }

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

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        if (!wrapper_.is_open()) {
            return state;
        }
        for (std::size_t i = 0; i < config_.ports.size(); ++i) {
            const int v = wrapper_.get_value(i);
            const auto idx = std::to_string(i);
            const bool on = config_.ports[i].pwm_enabled ? (v > 0) : (v != 0);
            state.push_back({"GetSwitch" + idx, on});
            state.push_back({"GetSwitchValue" + idx, static_cast<double>(v)});
            state.push_back({"StateChangeComplete" + idx, true});
        }
        return state;
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name),
                              AlpacaError::ActionNotImplemented);
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
        return static_cast<int>(config_.ports.size());
    }

    bool get_can_write(int id) const override {
        validate_id(id);
        return true;
    }

    bool get_can_async(int id) const override {
        validate_id(id);
        return false;
    }

    bool get_switch(int id) const override {
        ensure_connected();
        validate_id(id);
        const double v = wrapper_.get_value(static_cast<std::size_t>(id));
        return v > get_min_switch_value_unchecked(id);
    }

    void set_switch(int id, bool state) override {
        ensure_connected();
        validate_id(id);
        const double v = state ? get_max_switch_value_unchecked(id)
                               : get_min_switch_value_unchecked(id);
        set_switch_value(id, v);
    }

    void set_async(int id, bool /*state*/) override {
        ensure_connected();
        validate_id(id);
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    double get_switch_value(int id) const override {
        ensure_connected();
        validate_id(id);
        return static_cast<double>(wrapper_.get_value(static_cast<std::size_t>(id)));
    }

    void set_switch_value(int id, double value) override {
        ensure_connected();
        validate_id(id);
        const double min_v = get_min_switch_value_unchecked(id);
        const double max_v = get_max_switch_value_unchecked(id);
        const long rounded = std::lround(value);
        if (rounded < static_cast<long>(min_v) || rounded > static_cast<long>(max_v)) {
            throw AlpacaException("Switch value out of range", AlpacaError::InvalidValue);
        }
        wrapper_.set_value(static_cast<std::size_t>(id), static_cast<int>(rounded));
    }

    void set_async_value(int id, double /*value*/) override {
        ensure_connected();
        validate_id(id);
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    bool get_state_change_complete(int id) const override {
        ensure_connected();
        validate_id(id);
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
        return "ASIair power port on GPIO " + std::to_string(p.gpio_line) + " (" + mode + ")";
    }

    double get_min_switch_value(int id) const override {
        validate_id(id);
        return get_min_switch_value_unchecked(id);
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
            throw AlpacaException("ASIair switch not connected", AlpacaError::NotConnected);
        }
    }

    void validate_id(int id) const {
        if (id < 0 || id >= static_cast<int>(config_.ports.size())) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
    }

    double get_min_switch_value_unchecked(int) const { return 0.0; }

    double get_max_switch_value_unchecked(int id) const {
        return config_.ports[static_cast<std::size_t>(id)].pwm_enabled ? 100.0 : 1.0;
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
                ALPACA_LOG_ERROR(kLogCategory,
                                 std::string("ASIair connection task failed: ") + e.what());
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
    const AsiairSwitchConfig config_;
    AsiairProtocolWrapper wrapper_;

    mutable std::mutex name_mutex_;
    std::vector<std::string> switch_names_;

    std::atomic<bool> connecting_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<SwitchDriver> create_zwo_asiair_switch(int device_number, AsiairSwitchConfig config) {
    return std::make_unique<ZWOAsiairSwitchDriver>(device_number, std::move(config));
}

} // namespace alpacacore::vendor::zwo
