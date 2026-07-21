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
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::gemini {

namespace {
// Maximum brightness per the vendor app's >B<value># range (observed as
// >B255# for full brightness during the ON toggle).
constexpr int kMaxBrightness = 255;
}  // namespace

class GeminiFlatPanelDriver : public CoverCalibratorDriver, protected alpacacore::AsyncConnectable {
public:
    GeminiFlatPanelDriver(int device_number, FlatPanelConnectionConfig config)
        : AsyncConnectable("Gemini"),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          protocol_() {}

    ~GeminiFlatPanelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                GeminiFlatPanelDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Error during flat panel destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "Gemini Astro Flat Panel Cover Lite"; }

    DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }

    std::string get_unique_id() const override { return "GEMINI_FLATPANEL_" + std::to_string(device_number_); }

    std::string get_description() const override { return "Gemini Astro Flat Panel Cover Lite Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore Gemini Flat Panel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware captured at connect (from >V#); surfaced in the web UI only.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    // ICoverCalibratorV2 (ASCOM Platform 7): adds CoverMoving, CalibratorChanging,
    // Connecting and DeviceState, all of which this driver implements (via the
    // CoverCalibratorDriver base's get_device_state()).
    int get_interface_version() const override { return 2; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously -- closing the port is trivial and ASCOM
        // clients expect Connected to be false immediately after.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Gemini", "Flat panel disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
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
            // thread), not at registration. A local copy keeps config_ as the
            // durable intent so a later reconnect re-scans (robust to the
            // device moving ports).
            FlatPanelConnectionConfig effective = config_;
            if (effective.serial_port.empty() && effective.auto_detect_index >= 0) {
                auto ports = enumerate_gemini_flatpanel_ports();
                if (ports.empty()) {
                    throw AlpacaException(util::serial_auto_detect_failed_message("Gemini flat panel"),
                                          AlpacaError::NotConnected);
                }
                if (effective.auto_detect_index >= static_cast<int>(ports.size())) {
                    throw AlpacaException("Flat panel index " + std::to_string(effective.auto_detect_index) +
                                              " out of range (detected " + std::to_string(ports.size()) + ")",
                                          AlpacaError::NotConnected);
                }
                const auto& port = ports[static_cast<std::size_t>(effective.auto_detect_index)];
                ALPACA_LOG_INFO("Gemini", "Auto-detected flat panel at " + port.port_path);
                effective.serial_port = port.port_path;
            }

            std::string firmware = protocol_.connect(effective);
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_ = firmware;
            }

            // Seed the calibrator state from a live read so a driver that
            // reconnects to an already-lit panel reports the truth, rather
            // than always starting at Off/0 (which a freshly constructed
            // driver correctly does via the member initializers below).
            try {
                const bool on = protocol_.get_light_on();
                const int bri = protocol_.get_brightness();
                std::lock_guard<std::mutex> lock(state_mutex_);
                calibrator_engaged_ = on;
                commanded_brightness_ = bri;
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Flat panel state seed failed: " + std::string(e.what()));
            }

            connected_.store(true);
            ALPACA_LOG_INFO("Gemini", "Flat panel connected");
        } else {
            protocol_.disconnect();
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
            ALPACA_LOG_INFO("Gemini", "Flat panel disconnected");
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

    // --- CoverCalibrator interface: calibrator ---

    int get_max_brightness() const override { return kMaxBrightness; }

    int get_brightness() const override {
        ensure_connected();
        // Report the last commanded brightness synchronously rather than
        // re-querying the panel: ConformU reads Brightness immediately after
        // CalibratorOn and expects the just-set value back (same lesson as
        // WandererCover -- see AGENTS.md), and it also sidesteps the still-
        // unconfirmed exact wire format of the >J# reply on a cold read.
        std::lock_guard<std::mutex> lock(state_mutex_);
        return commanded_brightness_;
    }

    CalibratorState get_calibrator_state() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(state_mutex_);
        return calibrator_engaged_ ? CalibratorState::Ready : CalibratorState::Off;
    }

    bool get_calibrator_changing() const override {
        // The panel applies brightness instantly (no fade/ramp observed in the
        // vendor app); never a transient NotReady state.
        ensure_connected();
        return false;
    }

    void calibrator_on(int brightness) override {
        // Brightness 0 is a VALID "on at zero brightness" request, NOT off --
        // ASCOM ICoverCalibratorV2 requires CalibratorOn(0) to leave
        // CalibratorState == Ready. Validate the range first so an
        // out-of-range request is rejected with InvalidValue regardless of
        // connection state (ASCOM precedence, per AGENTS.md).
        if (brightness < 0 || brightness > kMaxBrightness) {
            throw AlpacaException("Brightness " + std::to_string(brightness) + " out of range [0, " +
                                      std::to_string(kMaxBrightness) + "]",
                                  AlpacaError::InvalidValue);
        }
        ensure_connected();
        std::lock_guard<std::mutex> lock(state_mutex_);
        const int prev_brightness = commanded_brightness_;
        const bool prev_engaged = calibrator_engaged_;
        commanded_brightness_ = brightness;
        calibrator_engaged_ = true;
        try {
            // Matches the vendor app's own ON sequence (button_toggle_Click):
            // light on, then set the level -- sent back-to-back with no
            // inter-command delay (confirmed from the decompiled IL).
            protocol_.light_on();
            protocol_.set_brightness(brightness);
        } catch (...) {
            commanded_brightness_ = prev_brightness;
            calibrator_engaged_ = prev_engaged;
            throw;
        }
    }

    void calibrator_off() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(state_mutex_);
        const int prev_brightness = commanded_brightness_;
        const bool prev_engaged = calibrator_engaged_;
        commanded_brightness_ = 0;
        calibrator_engaged_ = false;
        try {
            // Matches the vendor app's OFF sequence: light off, then zero the
            // level.
            protocol_.light_off();
            protocol_.set_brightness(0);
        } catch (...) {
            commanded_brightness_ = prev_brightness;
            calibrator_engaged_ = prev_engaged;
            throw;
        }
    }

    void set_brightness(int brightness) override {
        // Not part of the ASCOM HTTP surface (brightness is set via
        // CalibratorOn), but provided for completeness.
        calibrator_on(brightness);
    }

    // --- CoverCalibrator interface: cover (not present on this model) ---

    CoverState get_cover_state() const override {
        ensure_connected();
        return CoverState::NotPresent;
    }

    bool get_cover_moving() const override {
        ensure_connected();
        return false;
    }

    // No motorized cover on this model -- a structurally absent capability,
    // not a connection-state matter, so these throw unconditionally (matches
    // the same pattern as e.g. Celestron/Bisque's SlewToAltAz on mounts that
    // don't support it: MethodNotImplemented regardless of Connected).
    void open_cover() override {
        throw AlpacaException("This Gemini Flat Panel has no motorized cover", AlpacaError::MethodNotImplemented);
    }

    void close_cover() override {
        throw AlpacaException("This Gemini Flat Panel has no motorized cover", AlpacaError::MethodNotImplemented);
    }

    void halt_cover() override {
        throw AlpacaException("This Gemini Flat Panel has no motorized cover", AlpacaError::MethodNotImplemented);
    }

    // Deliberately NO per-vendor get_device_state() override: per AGENTS.md the
    // CoverCalibratorDriver base builds the DeviceState bag from these same
    // getters, and a single-lock vendor override is explicitly disallowed.

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Gemini flat panel not connected", AlpacaError::NotConnected);
        }
    }

    int device_number_;
    FlatPanelConnectionConfig config_;
    std::atomic<bool> connected_;
    GeminiFlatPanelProtocolWrapper protocol_;

    mutable std::mutex state_mutex_;
    int commanded_brightness_ = 0;
    bool calibrator_engaged_ = false;

    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    std::mutex transition_mutex_;  // serializes set_connected() connect/disconnect transitions
};

std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel(int device_number, const std::string& serial_port,
                                                               int baud_rate) {
    FlatPanelConnectionConfig config;
    config.type = FlatPanelConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<GeminiFlatPanelDriver>(device_number, std::move(config));
}

std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel_by_index(int device_number, int panel_index) {
    // Defer the (blocking) port scan to connect time, which runs on the
    // driver's background connection thread -- doing it here would block the
    // HTTP handler thread at device registration.
    FlatPanelConnectionConfig config;
    config.type = FlatPanelConnectionType::Serial;
    config.auto_detect_index = panel_index;
    return std::make_unique<GeminiFlatPanelDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::gemini
