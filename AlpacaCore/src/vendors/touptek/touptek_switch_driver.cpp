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
#include <alpacacore/util/power_port_state_store.h>
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
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

class TouptekSwitchDriver : public SwitchDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    TouptekSwitchDriver(int device_number, TouptekSwitchConfig config)
        : AsyncConnectable(kLogCategory),
          device_number_(device_number),
          config_(std::move(config)),
          owned_(std::make_unique<TouptekPowerboxWrapper>(config_.gpio_chip_path, config_.ports,
                                                          config_.pwm_frequency_hz)),
          wrapper_(*owned_) {
        init_names();
    }

    // Test seam: the power box is injected and outlives the driver.
    TouptekSwitchDriver(int device_number, TouptekSwitchConfig config, TouptekPowerboxSDK& powerbox)
        : AsyncConnectable(kLogCategory),
          device_number_(device_number),
          config_(std::move(config)),
          wrapper_(powerbox) {
        if (powerbox.port_count() != config_.ports.size()) {
            throw AlpacaException("StellaVita power box has " + std::to_string(powerbox.port_count()) +
                                      " ports but the configuration lists " + std::to_string(config_.ports.size()),
                                  AlpacaError::InvalidValue);
        }
        init_names();
    }

    void init_names() {
        switch_names_.reserve(config_.ports.size());
        for (const auto& p : config_.ports) {
            switch_names_.emplace_back(p.name);
        }
    }

    ~TouptekSwitchDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        try {
            wrapper_.close();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogCategory, std::string("Error during StellaVita switch destruction: ") + e.what());
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "ToupTek " + config_.model_name; }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string fallback_unique_id() const override { return "ToupTek_StellaVita_" + std::to_string(device_number_); }

    std::string get_description() const override {
        return "ToupTek " + config_.model_name + " DC power switch (" + config_.gpio_chip_path + ")";
    }

    std::string get_driver_info() const override { return "AlpacaCore ToupTek " + config_.model_name + " Switch"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }

    // Lock-free (issue #382): an atomic, so this never waits behind a connect
    // or disconnect and never nests a lock with AsyncConnectable's
    // pending_mutex_. Not the power box's is_open(): the lines may be held
    // since the server start without a client connected (decision 0021).
    bool get_connected() const override { return connected_.load(std::memory_order_acquire); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Serialize whole transitions against each other (issue #528): the
        // base's record/consume gates only see an ASYNC connect. With a sync
        // connect in flight on thread A, a sync disconnect on thread B saw
        // "not connected" twice (nothing in flight, idempotent) and returned
        // as a no-op while A went on to store true -- the client was told
        // the disconnect succeeded and the device ended up Connected. Held
        // only for set_connected() against set_connected(); getters stay free.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Base gates first: a sync disconnect during an in-flight connect
        // looks idempotent (wrapper still closed) and would be silently
        // dropped without the record; a connect must honor a newer pending
        // disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(get_connected())) {
            return;
        }
        if (connected && consume_pending_disconnect(get_connected())) {
            return;
        }
        if (connected) {
            if (get_connected()) {
                return;
            }
            // Lines held since the server start (decision 0021) are taken over as they are.
            if (!wrapper_.is_open()) {
                wrapper_.open();
            }
            connected_.store(true, std::memory_order_release);
        } else {
            // A Disconnect with no client connected keeps lines held since the
            // server start: releasing them would drop a PWM duty to full on.
            if (!get_connected()) {
                return;
            }
            connected_.store(false, std::memory_order_release);
            if (wrapper_.is_open()) {
                wrapper_.close();
            }
        }
    }

    void apply_power_on_state(bool new_boot) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (config_.power_on_state == util::PowerOnState::Leave || get_connected()) {
            return;
        }
        std::vector<util::PowerOnPort> ports;
        ports.reserve(config_.ports.size());
        for (const auto& p : config_.ports) {
            ports.push_back({p.writable, p.pwm_enabled});
        }
        const auto plan = util::plan_power_on(config_.power_on_state, new_boot, ports,
                                              util::PowerPortStateStore::load(get_unique_id()));
        try {
            for (std::size_t i = 0; i < plan.size(); ++i) {
                if (!plan[i]) {
                    continue;
                }
                // Keep the lines claimed after the start: a released line loses
                // a PWM duty, and the first Connect takes them over.
                if (!wrapper_.is_open()) {
                    wrapper_.open();
                }
                wrapper_.set_value(i, *plan[i]);
                save_port_state(static_cast<int>(i), *plan[i]);
                ALPACA_LOG_INFO(kLogCategory,
                                config_.ports[i].name + " set to " + std::to_string(*plan[i]) + " at server start");
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogCategory, std::string("Could not apply the power-on state: ") + e.what());
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
        ensure_connected();
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
        ensure_connected();
        ensure_writable(id);
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
        save_port_state(id, static_cast<int>(rounded));
    }

    void set_async_value(int id, double /*value*/) override {
        validate_id(id);
        ensure_connected();
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
    // Remembers what a client set, for a start that restores it (decision 0021).
    // A failed save is logged by the store; the port was still set.
    void save_port_state(int id, int value) {
        util::PowerPortStateStore::save(get_unique_id(), id, value * 100.0 / get_max_switch_value_unchecked(id));
    }

    void ensure_connected() const {
        if (!get_connected()) {
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
    // Switch spec, after the connection check.
    void ensure_writable(int id) const {
        if (!config_.ports[static_cast<std::size_t>(id)].writable) {
            throw AlpacaException("Switch " + std::to_string(id) + " is read-only", AlpacaError::NotImplemented);
        }
    }

    const int device_number_;
    const TouptekSwitchConfig config_;
    // Set only by the production factory; declared before wrapper_, which refers to it.
    std::unique_ptr<TouptekPowerboxWrapper> owned_;
    TouptekPowerboxSDK& wrapper_;

    mutable std::mutex name_mutex_;
    // Guards set_connected() against a concurrent set_connected() only (#528),
    // and against apply_power_on_state().
    std::mutex transition_mutex_;
    std::atomic<bool> connected_{false};
    std::vector<std::string> switch_names_;
};

std::unique_ptr<SwitchDriver> create_touptek_switch(int device_number, TouptekSwitchConfig config) {
    return std::make_unique<TouptekSwitchDriver>(device_number, std::move(config));
}

std::unique_ptr<SwitchDriver> create_touptek_switch(int device_number, TouptekSwitchConfig config,
                                                    TouptekPowerboxSDK& powerbox) {
    return std::make_unique<TouptekSwitchDriver>(device_number, std::move(config), powerbox);
}

}  // namespace alpacacore::vendor::touptek
