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
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/wandererastro/wandererastro_box_protocol_wrapper.h>
#include <alpacacore/vendor/wandererastro/wandererastro_box_switch_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::wandererastro {

namespace {

// Switch id layout. Outputs first (matching the vendor's own ASCOM driver
// ordering), then the read-only sensor values.
enum BoxSwitchId : std::uint8_t {
    kDc1AlwaysOn = 0,  // 12 V always-on rail (read-only)
    kDc2AlwaysOn = 1,  // 19 V regulated always-on rail (read-only)
    kDc34Power = 2,    // adjustable regulated output, on/off
    kDc34Voltage = 3,  // adjustable regulated output setpoint, 5.0-13.2 V
    kDc5Pwm = 4,       // dew heater PWM 0-255
    kDc6Pwm = 5,       // dew heater PWM 0-255
    kDc7Pwm = 6,       // dew heater PWM 0-255
    kDc89Power = 7,    // 12 V switched pair
    kDc1011Power = 8,  // 12 V switched pair
    kUsb31_1 = 9,      // USB3.1 port 1
    kUsb31_2 = 10,     // USB3.1 port 2
    kUsb31_3 = 11,     // USB3.1 port 3
    kUsb20_13 = 12,    // USB2.0 ports 1-3 (switched together)
    kUsb20_46 = 13,    // USB2.0 ports 4-6 (switched together)
    kInputVoltage = 14,
    kTotalCurrent = 15,
    kV19Current = 16,
    kDc34Current = 17,
    kAmbientTemp = 18,
    kAmbientHumidity = 19,
    kDewPoint = 20,
    kProbe1Temp = 21,
    kProbe2Temp = 22,
    kProbe3Temp = 23,
    kBoxSwitchCount = 24,
};

struct BoxSwitchInfo {
    const char* name;
    const char* description;
    double min;
    double max;
    double step;
    bool writable;
};

// Sensor ranges are generous bounds so live readings always sit inside
// [Min, Max] (ConformU checks this): DS18B20 error sentinels (-127/85 degC)
// and unpowered rails (0 V) are all in range.
constexpr std::array<BoxSwitchInfo, kBoxSwitchCount> kSwitches{{
    {"DC1", "12V always-on output (read-only)", 0.0, 1.0, 1.0, false},
    {"DC2 19V", "19V regulated always-on output (read-only)", 0.0, 1.0, 1.0, false},
    {"DC3-4", "Adjustable regulated output on/off", 0.0, 1.0, 1.0, true},
    {"DC3-4 Voltage", "Adjustable regulated output setpoint (V)", kBoxDc34VoltageMin, kBoxDc34VoltageMax,
     kBoxDc34VoltageStep, true},
    {"DC5 Heater", "Dew heater PWM channel DC5 (0-255)", 0.0, 255.0, 1.0, true},
    {"DC6 Heater", "Dew heater PWM channel DC6 (0-255)", 0.0, 255.0, 1.0, true},
    {"DC7 Heater", "Dew heater PWM channel DC7 (0-255)", 0.0, 255.0, 1.0, true},
    {"DC8-9", "12V switched output pair DC8-9", 0.0, 1.0, 1.0, true},
    {"DC10-11", "12V switched output pair DC10-11", 0.0, 1.0, 1.0, true},
    {"USB3.1-1", "USB3.1 port 1 power", 0.0, 1.0, 1.0, true},
    {"USB3.1-2", "USB3.1 port 2 power", 0.0, 1.0, 1.0, true},
    {"USB3.1-3", "USB3.1 port 3 power", 0.0, 1.0, 1.0, true},
    {"USB2.0 1-3", "USB2.0 ports 1-3 power (switched together)", 0.0, 1.0, 1.0, true},
    {"USB2.0 4-6", "USB2.0 ports 4-6 power (switched together)", 0.0, 1.0, 1.0, true},
    {"Input Voltage", "Input voltage (V, read-only)", 0.0, 30.0, 0.01, false},
    {"Total Current", "Total input current (A, read-only)", 0.0, 30.0, 0.01, false},
    {"DC2 19V Current", "19V rail current (A, read-only)", 0.0, 30.0, 0.01, false},
    {"DC3-4 Current", "Adjustable rail current (A, read-only)", 0.0, 30.0, 0.01, false},
    {"Ambient Temp", "DHT22 ambient temperature (degC, read-only)", -273.15, 150.0, 0.01, false},
    {"Ambient Humidity", "DHT22 relative humidity (%, read-only)", 0.0, 100.0, 0.01, false},
    {"Dew Point", "Computed dew point (degC, read-only)", -273.15, 150.0, 0.01, false},
    {"Probe 1 Temp", "DS18B20 probe 1 temperature (degC, read-only)", -273.15, 150.0, 0.01, false},
    {"Probe 2 Temp", "DS18B20 probe 2 temperature (degC, read-only)", -273.15, 150.0, 0.01, false},
    {"Probe 3 Temp", "DS18B20 probe 3 temperature (degC, read-only)", -273.15, 150.0, 0.01, false},
}};

}  // namespace

// COMMANDED-VALUE SEMANTICS for the writable outputs (ETA / WandererCover
// lesson): commands are fire-and-forget and the streamed status frame lags a
// write by up to a frame period, but ConformU reads a value back microseconds
// after writing it — so writes record the commanded value and report it,
// seeded from the live frame at connect. Sensor switches always report live.
class WandererBoxSwitchDriver : public SwitchDriver, protected alpacacore::AsyncConnectable {
public:
    WandererBoxSwitchDriver(int device_number, BoxConnectionConfig config)
        : AsyncConnectable("WandererAstro"),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          protocol_() {
        for (int i = 0; i < kBoxSwitchCount; ++i) {
            switch_names_[static_cast<std::size_t>(i)] = kSwitches[static_cast<std::size_t>(i)].name;
        }
    }

    ~WandererBoxSwitchDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                // Tear down directly rather than via the virtual set_connected(),
                // which must not be dispatched during destruction.
                protocol_.disconnect();
                connected_.store(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("WandererAstro", "Error during WandererBox destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "WandererAstro WandererBox Pro V3"; }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string get_unique_id() const override { return "WANDERERASTRO_BOX_" + std::to_string(device_number_); }

    std::string get_description() const override { return "WandererAstro WandererBox Pro V3 Power Box Switch Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore WandererAstro WandererBox Switch Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware date (YYYY-MM-DD), captured by the protocol wrapper from the
    // status stream and cleared there on disconnect. Web UI only, never DriverInfo.
    std::optional<std::string> get_device_firmware() const override { return protocol_.get_firmware_date(); }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously — closing the port + joining the reader
        // is quick and ASCOM clients expect Connected to be false immediately.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("WandererAstro", "WandererBox disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Serialize the whole connect/disconnect transition (see the cover
        // driver for the race this prevents).
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
        if (connected) {
            // Resolve an auto-detect request here (on the background connection
            // thread), not at registration: enumerate the ports and pick the
            // requested match. A local copy keeps config_ as the durable intent
            // so a later reconnect re-scans (robust to the device moving ports).
            BoxConnectionConfig effective = config_;
            if (effective.serial_port.empty() && effective.auto_detect_index >= 0) {
                auto ports = enumerate_wandererbox_ports();
                if (ports.empty()) {
                    throw AlpacaException(util::serial_auto_detect_failed_message("WandererBox Pro V3"),
                                          AlpacaError::NotConnected);
                }
                if (effective.auto_detect_index >= static_cast<int>(ports.size())) {
                    throw AlpacaException("Box index " + std::to_string(effective.auto_detect_index) +
                                              " out of range (detected " + std::to_string(ports.size()) + ")",
                                          AlpacaError::NotConnected);
                }
                const auto& port = ports[static_cast<std::size_t>(effective.auto_detect_index)];
                ALPACA_LOG_INFO("WandererAstro", "Auto-detected " + port.model + " at " + port.port_path);
                effective.serial_port = port.port_path;
            }
            protocol_.connect(effective);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                for (auto& c : commanded_) {
                    c.reset();
                }
            }
            connected_.store(true);
            ALPACA_LOG_INFO("WandererAstro", "WandererBox connected");
        } else {
            protocol_.disconnect();  // joins the reader, clears cached firmware
            connected_.store(false);
            ALPACA_LOG_INFO("WandererAstro", "WandererBox disconnected");
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override { return false; }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    // --- Switch interface ---

    int get_max_switch() const override { return kBoxSwitchCount; }

    bool get_can_write(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return kSwitches[static_cast<std::size_t>(id)].writable;
    }

    bool get_can_async(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        // Commands apply instantly on the controller; nothing to run async.
        return false;
    }

    bool get_switch(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return get_switch_value(id) > kSwitches[static_cast<std::size_t>(id)].min;
    }

    void set_switch(int id, bool state) override {
        validate_switch_id(id);
        ensure_connected();
        const auto& info = kSwitches[static_cast<std::size_t>(id)];
        set_switch_value(id, state ? info.max : info.min);
    }

    void set_async(int id, bool /*state*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::MethodNotImplemented);
    }

    double get_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        if (kSwitches[static_cast<std::size_t>(id)].writable) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto& commanded = commanded_[static_cast<std::size_t>(id)];
            if (commanded.has_value()) {
                return commanded.value();
            }
        }
        return live_value(id, protocol_.get_state());
    }

    void set_switch_value(int id, double value) override {
        validate_switch_id(id);
        ensure_connected();
        const auto& info = kSwitches[static_cast<std::size_t>(id)];
        if (!info.writable) {
            throw AlpacaException("Switch " + std::to_string(id) + " is read-only", AlpacaError::NotImplemented);
        }
        if (!std::isfinite(value)) {
            throw AlpacaException("Switch value must be a finite number", AlpacaError::InvalidValue);
        }
        if (value < info.min || value > info.max) {
            throw AlpacaException(
                "Switch value out of range [" + std::to_string(info.min) + ", " + std::to_string(info.max) + "]",
                AlpacaError::InvalidValue);
        }
        // Quantise to the switch step BEFORE commanding and recording, so the
        // reported value is exactly what went on the wire (FP-noise lesson
        // from the ETA driver). Clamp after quantising: step accumulation has
        // its own FP error (5.0 + 82*0.1 = 13.200000000000001 > Max), which
        // the wrapper's range check would otherwise reject at exactly Max.
        const double quantised =
            std::clamp(info.min + std::round((value - info.min) / info.step) * info.step, info.min, info.max);
        dispatch_write(id, quantised);
        std::lock_guard<std::mutex> lock(state_mutex_);
        commanded_[static_cast<std::size_t>(id)] = quantised;
    }

    void set_async_value(int id, double /*value*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::MethodNotImplemented);
    }

    bool get_state_change_complete(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return true;
    }

    std::string get_switch_name(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(names_mutex_);
        return switch_names_[static_cast<std::size_t>(id)];
    }

    void set_switch_name(int id, const std::string& name) override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(names_mutex_);
        switch_names_[static_cast<std::size_t>(id)] = name;
    }

    std::string get_switch_description(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return kSwitches[static_cast<std::size_t>(id)].description;
    }

    double get_min_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return kSwitches[static_cast<std::size_t>(id)].min;
    }

    double get_max_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return kSwitches[static_cast<std::size_t>(id)].max;
    }

    double get_switch_step(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return kSwitches[static_cast<std::size_t>(id)].step;
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("WandererBox not connected", AlpacaError::NotConnected);
        }
    }

    void validate_switch_id(int id) const {
        if (id < 0 || id >= kBoxSwitchCount) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
    }

    // Read a switch value from the live streamed state.
    static double live_value(int id, const BoxState& s) {
        switch (id) {
            case kDc1AlwaysOn:
            case kDc2AlwaysOn:
                return 1.0;  // always-on rails — on whenever the box is powered
            case kDc34Power:
                return s.dc3_4 ? 1.0 : 0.0;
            case kDc34Voltage:
                // The streamed setpoint can be 0 before it was ever set; clamp
                // into the switch range so the value always sits in [Min, Max].
                return std::clamp(s.dc3_4_voltage, kBoxDc34VoltageMin, kBoxDc34VoltageMax);
            case kDc5Pwm:
                return static_cast<double>(s.dc5_pwm);
            case kDc6Pwm:
                return static_cast<double>(s.dc6_pwm);
            case kDc7Pwm:
                return static_cast<double>(s.dc7_pwm);
            case kDc89Power:
                return s.dc8_9 ? 1.0 : 0.0;
            case kDc1011Power:
                return s.dc10_11 ? 1.0 : 0.0;
            case kUsb31_1:
                return s.usb31_1 ? 1.0 : 0.0;
            case kUsb31_2:
                return s.usb31_2 ? 1.0 : 0.0;
            case kUsb31_3:
                return s.usb31_3 ? 1.0 : 0.0;
            case kUsb20_13:
                return s.usb2_13 ? 1.0 : 0.0;
            case kUsb20_46:
                return s.usb2_46 ? 1.0 : 0.0;
            case kInputVoltage:
                return s.input_voltage;
            case kTotalCurrent:
                return s.total_current;
            case kV19Current:
                return s.v19_current;
            case kDc34Current:
                return s.adj_current;
            case kAmbientTemp:
                return s.ambient_temp;
            case kAmbientHumidity:
                return s.humidity;
            case kDewPoint:
                return s.dew_point;
            case kProbe1Temp:
                return s.probe_temps[0];
            case kProbe2Temp:
                return s.probe_temps[1];
            case kProbe3Temp:
                return s.probe_temps[2];
            default:
                return 0.0;  // unreachable — validate_switch_id gates ids
        }
    }

    // Send the command for a validated, quantised write.
    void dispatch_write(int id, double value) {
        const bool on = value > kSwitches[static_cast<std::size_t>(id)].min;
        switch (id) {
            case kDc34Power:
                protocol_.set_dc3_4(on);
                break;
            case kDc34Voltage:
                protocol_.set_dc3_4_voltage(value);
                break;
            case kDc5Pwm:
            case kDc6Pwm:
            case kDc7Pwm:
                protocol_.set_pwm(5 + (id - kDc5Pwm), static_cast<int>(std::lround(value)));
                break;
            case kDc89Power:
                protocol_.set_dc8_9(on);
                break;
            case kDc1011Power:
                protocol_.set_dc10_11(on);
                break;
            case kUsb31_1:
            case kUsb31_2:
            case kUsb31_3:
            case kUsb20_13:
            case kUsb20_46:
                protocol_.set_usb(id - kUsb31_1, on);
                break;
            default:
                // Read-only ids are rejected in set_switch_value before here.
                throw AlpacaException("Switch is read-only", AlpacaError::NotImplemented);
        }
    }

    int device_number_;
    BoxConnectionConfig config_;
    std::atomic<bool> connected_;
    WandererBoxProtocolWrapper protocol_;

    mutable std::mutex names_mutex_;  // guards switch_names_
    std::array<std::string, kBoxSwitchCount> switch_names_;

    mutable std::mutex state_mutex_;  // guards commanded_
    // Per-switch commanded value (writable outputs only); unset until the
    // first write of a session, in which case the live frame is reported.
    std::array<std::optional<double>, kBoxSwitchCount> commanded_;

    std::mutex transition_mutex_;  // serializes set_connected() connect/disconnect transitions
};

std::unique_ptr<SwitchDriver> create_wandererastro_box_switch(int device_number, const std::string& serial_port,
                                                              int baud_rate) {
    BoxConnectionConfig config;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<WandererBoxSwitchDriver>(device_number, std::move(config));
}

std::unique_ptr<SwitchDriver> create_wandererastro_box_switch_by_index(int device_number, int box_index) {
    // Defer the (blocking) port scan to connect time, which runs on the driver's
    // background connection thread — never on the HTTP registration thread.
    BoxConnectionConfig config;
    config.auto_detect_index = box_index;
    return std::make_unique<WandererBoxSwitchDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::wandererastro
