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
#include <alpacacore/vendor/gemini/gemini_pdh_protocol_wrapper.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::gemini {

namespace {

// Switch id layout. Outputs first, in the vendor driver's own order (so a
// NINA profile built against the Windows driver maps 1:1), then the
// telemetry the vendor only shows in its sensor pop-up window.
enum PdhSwitchId : std::uint8_t {
    kUsbA = 0,
    kUsbB = 1,
    kUsbC = 2,
    kUsbD = 3,
    kUsbE = 4,
    kUsbF = 5,
    kDc1AlwaysOn = 6,
    kDc2 = 7,
    kDc3 = 8,
    kDc4 = 9,
    kDc5 = 10,
    kDew6Output = 11,
    kDew7Output = 12,
    kDew6Mode = 13,
    kDew7Mode = 14,
    kInputVoltage = 15,
    kOutputCurrent = 16,
    kOutputPower = 17,
    kAmbientTemp = 18,
    kAmbientHumidity = 19,
    kDewPoint = 20,
    kLensTemp = 21,
    kAht20Attached = 22,
    kDs18b20Attached = 23,
    kPdhSwitchCount = 24,
};

// Wire channel (>O<n>#/>C<n>#) for the switchable outputs: USB A-F = 6..11,
// DC1 = 1 (never sent: read-only), DC2-DC5 = 2..5.
int output_channel(int id) {
    if (id >= kUsbA && id <= kUsbF) return 6 + (id - kUsbA);
    if (id >= kDc1AlwaysOn && id <= kDc5) return 1 + (id - kDc1AlwaysOn);
    return -1;
}

struct PdhSwitchInfo {
    const char* name;
    const char* description;
    double min;
    double max;  // for the DEW outputs this is the Manual-mode max; see get_max_switch_value()
    double step;
    bool writable;
};

// Sensor ranges are generous bounds so live readings always sit inside
// [Min, Max] (ConformU checks this): the -127 degC absent-sensor sentinel
// and an unpowered 0 V rail are all in range.
constexpr std::array<PdhSwitchInfo, kPdhSwitchCount> kSwitches{{
    {"USB A", "USB A power (USB 3.2 Gen1)", 0.0, 1.0, 1.0, true},
    {"USB B", "USB B power (USB 3.2 Gen1)", 0.0, 1.0, 1.0, true},
    {"USB C", "USB C power (USB 2.0)", 0.0, 1.0, 1.0, true},
    {"USB D", "USB D power (USB 2.0)", 0.0, 1.0, 1.0, true},
    {"USB E", "USB E power (USB 2.0)", 0.0, 1.0, 1.0, true},
    {"USB F", "USB F power (USB 2.0)", 0.0, 1.0, 1.0, true},
    {"DC1", "12V always-on output with ideal diode (read-only)", 0.0, 1.0, 1.0, false},
    {"DC2", "12V switched output DC2", 0.0, 1.0, 1.0, true},
    {"DC3", "12V switched output DC3", 0.0, 1.0, 1.0, true},
    {"DC4", "12V switched output DC4", 0.0, 1.0, 1.0, true},
    {"DC5", "12V switched output DC5", 0.0, 1.0, 1.0, true},
    {"DEW6", "Dew heater DEW6: PWM % in Manual mode, on/off in Auto or Switch mode", 0.0,
     static_cast<double>(kPdhDewPwmMax), 1.0, true},
    {"DEW7", "Dew heater DEW7: PWM % in Manual mode, on/off in Auto or Switch mode", 0.0,
     static_cast<double>(kPdhDewPwmMax), 1.0, true},
    {"DEW6 Mode", "DEW6 mode: 0 Auto (PID, needs both sensors), 1 Manual, 2 Switch", 0.0, 2.0, 1.0, true},
    {"DEW7 Mode", "DEW7 mode: 0 Auto (PID, needs both sensors), 1 Manual, 2 Switch", 0.0, 2.0, 1.0, true},
    {"Input Voltage", "Total input voltage (V, read-only)", 0.0, 30.0, 0.01, false},
    {"Output Current", "DC12V output current (A, read-only)", 0.0, 30.0, 0.01, false},
    {"Output Power", "DC12V output power (W, read-only)", 0.0, 500.0, 0.01, false},
    {"Ambient Temperature", "AHT20 ambient temperature (degC, read-only)", -273.15, 150.0, 0.01, false},
    {"Ambient Humidity", "AHT20 relative humidity (%, read-only)", 0.0, 100.0, 0.01, false},
    {"Dew Point", "Dew point from the AHT20 readings (degC, read-only)", -273.15, 150.0, 0.01, false},
    {"Lens Temperature", "DS18B20 lens surface temperature (degC, read-only)", -273.15, 150.0, 0.01, false},
    {"AHT20 Sensor", "AHT20 temperature/humidity sensor attached (read-only)", 0.0, 1.0, 1.0, false},
    {"DS18B20 Sensor", "DS18B20 lens temperature probe attached (read-only)", 0.0, 1.0, 1.0, false},
}};

PdhDewMode mode_from_value(double value) {
    switch (static_cast<int>(std::lround(value))) {
        case 0:
            return PdhDewMode::Auto;
        case 2:
            return PdhDewMode::Switch;
        default:
            return PdhDewMode::Manual;
    }
}

}  // namespace

// COMMANDED-VALUE SEMANTICS for the writable switches (WandererBox / ETA
// lesson): set commands are fire-and-forget and the status frame lags a write
// by a poll period, but ConformU reads a value back microseconds after
// writing it -- so writes record the commanded value and report it until
// disconnect. Read-only telemetry always reports the live frame.
class GeminiPdhSwitchDriver : public SwitchDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    GeminiPdhSwitchDriver(int device_number, PdhConnectionConfig config)
        : AsyncConnectable("Gemini"), device_number_(device_number), config_(std::move(config)), connected_(false) {
        for (int i = 0; i < kPdhSwitchCount; ++i) {
            switch_names_[static_cast<std::size_t>(i)] = kSwitches[static_cast<std::size_t>(i)].name;
        }
    }

    ~GeminiPdhSwitchDriver() override {
        // Blocks new connection tasks, then joins the in-flight one -- MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                protocol_.disconnect();
                connected_.store(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Error during power hub destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "Gemini Power & Data Hubs Advanced 3"; }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string get_unique_id() const override { return "GEMINI_PDH_ADV3_" + std::to_string(device_number_); }

    std::string get_description() const override {
        return "Gemini Power & Data Hubs Advanced 3 Power Box Switch Driver";
    }

    std::string get_driver_info() const override { return "AlpacaCore Gemini Power & Data Hub Switch Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware ("3.0.8") captured at connect and cleared on disconnect. Web UI only.
    std::optional<std::string> get_device_firmware() const override { return protocol_.get_firmware(); }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously -- closing the port + joining the reader is
        // quick and ASCOM clients expect Connected to be false immediately.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Gemini", "Power hub disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Base gates BEFORE the idempotency check (see the WandererBox driver
        // for the race this prevents).
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
            // Resolve auto-detect here (on the background connection thread),
            // not at registration. A local copy keeps config_ as the durable
            // intent so a later reconnect re-scans.
            PdhConnectionConfig effective = config_;
            if (effective.serial_port.empty() && effective.auto_detect_index >= 0) {
                auto ports = enumerate_gemini_pdh_ports();
                if (ports.empty()) {
                    throw AlpacaException(
                        util::serial_auto_detect_failed_message("Gemini Power & Data Hubs Advanced 3"),
                        AlpacaError::NotConnected);
                }
                if (effective.auto_detect_index >= static_cast<int>(ports.size())) {
                    throw AlpacaException("Hub index " + std::to_string(effective.auto_detect_index) +
                                              " out of range (detected " + std::to_string(ports.size()) + ")",
                                          AlpacaError::NotConnected);
                }
                const auto& port = ports[static_cast<std::size_t>(effective.auto_detect_index)];
                ALPACA_LOG_INFO("Gemini", "Auto-detected Power & Data Hubs Advanced 3 at " + port.port_path);
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
            ALPACA_LOG_INFO("Gemini", "Power & Data Hubs Advanced 3 connected");
        } else {
            protocol_.disconnect();  // joins the reader, clears cached firmware
            connected_.store(false);
            ALPACA_LOG_INFO("Gemini", "Power & Data Hubs Advanced 3 disconnected");
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

    int get_max_switch() const override { return kPdhSwitchCount; }

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
        ensure_link_up();
        return get_switch_value(id) > kSwitches[static_cast<std::size_t>(id)].min;
    }

    void set_switch(int id, bool state) override {
        validate_switch_id(id);
        ensure_connected();
        ensure_link_up();
        // Resolve the (mode-dependent) max under the write lock so the value
        // is computed, validated and sent against one mode.
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        set_switch_value_locked(id, state ? get_max_switch_value(id) : kSwitches[static_cast<std::size_t>(id)].min);
    }

    void set_async(int id, bool /*state*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::MethodNotImplemented);
    }

    double get_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        // Commanded values are gated too: with the hub unreachable, "what we
        // last asked for" is no more trustworthy than the stale frame (#237).
        ensure_link_up();
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
        ensure_link_up();
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        set_switch_value_locked(id, value);
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

    // Vendor parity: a DEW output is a 0-100 % PWM in Manual mode but a plain
    // 0/1 on/off in Auto or Switch mode, so its Max follows the channel's
    // effective mode (commanded this session, else firmware-reported).
    double get_max_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        if (id == kDew6Output || id == kDew7Output) {
            ensure_link_up();  // mode-dependent: needs a trustworthy frame
            return effective_dew_mode(id == kDew6Output ? kDew6Mode : kDew7Mode) == PdhDewMode::Manual
                       ? static_cast<double>(kPdhDewPwmMax)
                       : 1.0;
        }
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
            throw AlpacaException("Gemini power hub not connected", AlpacaError::NotConnected);
        }
    }

    // Issue #237: reads are served from the reader thread's cache, so a dead
    // serial link would otherwise be invisible to every reader while writes
    // fail with EIO. Once the wrapper latches a link fault, every value read
    // and write throws (iOptron "communications compromised" convention,
    // DriverException rather than NotConnected because Connected is still
    // true) until status frames resume. Static metadata (names, descriptions,
    // ranges, CanWrite) keeps answering: it does not depend on the hub.
    void ensure_link_up() const {
        if (const auto fault = protocol_.link_fault()) {
            throw AlpacaException("Gemini power hub communications compromised: " + *fault,
                                  AlpacaError::DriverException);
        }
    }

    void validate_switch_id(int id) const {
        if (id < 0 || id >= kPdhSwitchCount) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
    }

    PdhDewMode effective_dew_mode(int mode_id) const {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto& commanded = commanded_[static_cast<std::size_t>(mode_id)];
            if (commanded.has_value()) {
                return mode_from_value(commanded.value());
            }
        }
        const PdhState s = protocol_.get_state();
        return (mode_id == kDew6Mode) ? s.dew6.mode : s.dew7.mode;
    }

    static double dew_output_value(const PdhDewChannel& ch) {
        if (ch.mode == PdhDewMode::Manual) {
            return static_cast<double>(ch.manual_pwm);
        }
        return ch.enabled ? 1.0 : 0.0;
    }

    // Read a switch value from the live status frame.
    static double live_value(int id, const PdhState& s) {
        switch (id) {
            case kUsbA:
            case kUsbB:
            case kUsbC:
            case kUsbD:
            case kUsbE:
            case kUsbF:
                return s.usb[static_cast<std::size_t>(id - kUsbA)] ? 1.0 : 0.0;
            case kDc1AlwaysOn:
                return 1.0;  // always-on rail -- on whenever the hub is powered
            case kDc2:
            case kDc3:
            case kDc4:
            case kDc5:
                return s.dc[static_cast<std::size_t>(id - kDc2)] ? 1.0 : 0.0;
            case kDew6Output:
                return dew_output_value(s.dew6);
            case kDew7Output:
                return dew_output_value(s.dew7);
            case kDew6Mode:
                return static_cast<double>(static_cast<int>(s.dew6.mode));
            case kDew7Mode:
                return static_cast<double>(static_cast<int>(s.dew7.mode));
            case kInputVoltage:
                return s.input_voltage;
            case kOutputCurrent:
                return s.output_current;
            case kOutputPower:
                return s.output_power;
            case kAmbientTemp:
                return s.ambient_temp;
            case kAmbientHumidity:
                return s.humidity;
            case kDewPoint:
                return s.dew_point;
            case kLensTemp:
                return s.lens_temp;
            case kAht20Attached:
                return s.aht20_attached ? 1.0 : 0.0;
            case kDs18b20Attached:
                return s.ds18b20_attached ? 1.0 : 0.0;
            default:
                return 0.0;  // unreachable -- validate_switch_id gates ids
        }
    }

    // Validate, quantise, send and record one write. Caller holds write_mutex_,
    // so the DEW outputs' mode-dependent max (effective_dew_mode()) cannot
    // change between the range check and the commanded-value record: a
    // concurrent write to the matching mode switch waits for this one to
    // finish (PR #236 review).
    void set_switch_value_locked(int id, double value) {
        const auto& info = kSwitches[static_cast<std::size_t>(id)];
        if (!info.writable) {
            throw AlpacaException("Switch " + std::to_string(id) + " is read-only", AlpacaError::NotImplemented);
        }
        if (!std::isfinite(value)) {
            throw AlpacaException("Switch value must be a finite number", AlpacaError::InvalidValue);
        }
        const double max = get_max_switch_value(id);
        if (value < info.min || value > max) {
            throw AlpacaException(
                "Switch value out of range [" + std::to_string(info.min) + ", " + std::to_string(max) + "]",
                AlpacaError::InvalidValue);
        }
        // Quantise to the switch step BEFORE commanding and recording, so the
        // reported value is exactly what went on the wire.
        const double quantised =
            std::clamp(info.min + std::round((value - info.min) / info.step) * info.step, info.min, max);
        dispatch_write(id, quantised);
        std::lock_guard<std::mutex> lock(state_mutex_);
        commanded_[static_cast<std::size_t>(id)] = quantised;
    }

    // Send the command for a validated, quantised write.
    void dispatch_write(int id, double value) {
        const bool on = value > kSwitches[static_cast<std::size_t>(id)].min;
        switch (id) {
            case kUsbA:
            case kUsbB:
            case kUsbC:
            case kUsbD:
            case kUsbE:
            case kUsbF:
            case kDc2:
            case kDc3:
            case kDc4:
            case kDc5:
                protocol_.set_output(output_channel(id), on);
                break;
            case kDew6Output:
            case kDew7Output: {
                const int channel = (id == kDew6Output) ? kPdhDewChannelFirst : kPdhDewChannelLast;
                if (effective_dew_mode(id == kDew6Output ? kDew6Mode : kDew7Mode) == PdhDewMode::Manual) {
                    protocol_.set_dew_manual_pwm(channel, static_cast<int>(std::lround(value)));
                } else {
                    protocol_.set_dew_enabled(channel, on);
                }
                break;
            }
            case kDew6Mode:
            case kDew7Mode: {
                const int channel = (id == kDew6Mode) ? kPdhDewChannelFirst : kPdhDewChannelLast;
                const PdhDewMode mode = mode_from_value(value);
                if (mode == PdhDewMode::Auto) {
                    const PdhState s = protocol_.get_state();
                    if (!s.aht20_attached || !s.ds18b20_attached) {
                        // Vendor behaviour: Auto (PID) needs both the ambient
                        // and the lens sensor; the firmware falls back to
                        // Manual on its own. Warn, don't refuse -- the
                        // commanded mode is reported until the user changes it.
                        ALPACA_LOG_WARN("Gemini", "DEW" + std::to_string(channel) +
                                                      " Auto mode requested without both the AHT20 and DS18B20 "
                                                      "sensors attached; the hub will run Manual instead");
                    }
                }
                protocol_.set_dew_mode(channel, mode);
                break;
            }
            default:
                // Read-only ids are rejected in set_switch_value before here.
                throw AlpacaException("Switch is read-only", AlpacaError::NotImplemented);
        }
        // Pull a fresh frame right after the write so the next status read
        // reflects the new state sooner than the periodic poll would.
        try {
            protocol_.request_status();
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("Gemini", "Power hub post-write status request failed: " + std::string(e.what()));
        }
    }

    int device_number_;
    PdhConnectionConfig config_;
    std::atomic<bool> connected_;
    GeminiPdhProtocolWrapper protocol_;

    mutable std::mutex names_mutex_;  // guards switch_names_
    std::array<std::string, kPdhSwitchCount> switch_names_;

    // Serializes every writable-switch write (validate + send + record) so a
    // DEW value and the mode it was validated against can never disagree.
    // Never taken by readers, so the FAST-target read path is unaffected.
    std::mutex write_mutex_;

    mutable std::mutex state_mutex_;  // guards commanded_
    // Per-switch commanded value (writable switches only); unset until the
    // first write of a session, in which case the live frame is reported.
    std::array<std::optional<double>, kPdhSwitchCount> commanded_;

    std::mutex transition_mutex_;  // serializes set_connected() transitions
};

std::unique_ptr<SwitchDriver> create_gemini_pdh_switch(int device_number, const std::string& serial_port,
                                                       int baud_rate) {
    PdhConnectionConfig config;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<GeminiPdhSwitchDriver>(device_number, std::move(config));
}

std::unique_ptr<SwitchDriver> create_gemini_pdh_switch_by_index(int device_number, int hub_index) {
    // Defer the (blocking) port scan to connect time, which runs on the driver's
    // background connection thread -- never on the HTTP registration thread.
    PdhConnectionConfig config;
    config.auto_detect_index = hub_index;
    return std::make_unique<GeminiPdhSwitchDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::gemini
