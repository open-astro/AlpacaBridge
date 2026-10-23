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
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/device_location_store.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::gemini {

namespace {
// Maximum brightness per the vendor app's >B<value># range (observed as
// >B255# for full brightness during the ON toggle).
constexpr int kMaxBrightness = 255;
}  // namespace

class GeminiFlatPanelDriver : public CoverCalibratorDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

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

    std::string fallback_unique_id() const override { return "GEMINI_FLATPANEL_" + std::to_string(device_number_); }

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
            // durable intent so a later reconnect resolves the port again, remembered port first (robust to the
            // device moving ports).
            FlatPanelConnectionConfig effective = config_;
            bool auto_detected = false;
            if (effective.serial_port.empty() && effective.auto_detect_index >= 0) {
                // The port that worked last time goes first: one handshake instead of the full
                // search (5-13 s on a rig, past a client's connect timeout).
                if (const auto remembered =
                        util::recall_serial_location(util::location_key(get_unique_id(), effective.auto_detect_index),
                                                     probe_gemini_flatpanel_port)) {
                    ALPACA_LOG_INFO("Gemini", "Flat panel answered on remembered port " + *remembered);
                    effective.serial_port = *remembered;
                } else {
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
                auto_detected = true;
            }

            // I/O lock across the whole handshake + state seed (order:
            // transition_mutex_ -> io_mutex_ -> state_mutex_).
            std::lock_guard<std::mutex> io(io_mutex_);
            std::string firmware = protocol_.connect(effective);
            // Remember the port only after the handshake held: a port the user configured is never stored.
            if (auto_detected) {
                util::DeviceLocationStore::save(util::location_key(get_unique_id(), effective.auto_detect_index),
                                                util::serial_location_for(effective.serial_port));
            }
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
                // The panel remembers its level while the light is off; Brightness is 0 while CalibratorState is
                // Off (ICoverCalibratorV2).
                commanded_brightness_ = on ? bri : 0;
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Flat panel state seed failed: " + std::string(e.what()));
            }

            connected_.store(true);
            ALPACA_LOG_INFO("Gemini", "Flat panel connected");
        } else {
            // Driver state first, SDK close second (driver-lifecycle.instructions.md, issue #387): a
            // throwing close must not leave the driver reporting connected on
            // a closed port. disconnect() cannot throw today; the order is
            // the contract, not the current wrapper's behaviour.
            // The cached firmware goes first, so the store(false) is never
            // observable beside a stale firmware string.
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
            {
                // Published state is cleared first, under the short state
                // lock; the I/O lock then covers the close, so it waits only
                // for an exchange already on the wire.
                std::lock_guard<std::mutex> lock(state_mutex_);
                commanded_brightness_ = 0;
                calibrator_engaged_ = false;
            }
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                protocol_.disconnect();
            }
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

    int get_max_brightness() const override {
        ensure_connected();
        return kMaxBrightness;
    }

    int get_brightness() const override {
        ensure_connected();
        // Report the last commanded brightness synchronously rather than
        // re-querying the panel: ConformU reads Brightness immediately after
        // CalibratorOn and expects the just-set value back (same lesson as
        // WandererCover -- see .github/instructions/gemini.instructions.md), and it also sidesteps the still-
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
        // connection state (ASCOM precedence, per driver-lifecycle.instructions.md).
        if (brightness < 0 || brightness > kMaxBrightness) {
            throw AlpacaException("Brightness " + std::to_string(brightness) + " out of range [0, " +
                                      std::to_string(kMaxBrightness) + "]",
                                  AlpacaError::InvalidValue);
        }
        ensure_connected();
        // io_mutex_ (not just the wrapper's per-command lock) keeps the
        // light_on + set_brightness pair atomic against a concurrent
        // CalibratorOff. state_mutex_ is taken only to publish the result, so
        // Brightness / CalibratorState never wait for the serial reply
        // (decision 0016). The state is published after the commands succeed,
        // so a throwing command leaves the previous state untouched.
        std::lock_guard<std::mutex> io(io_mutex_);
        // Matches the vendor app's own ON sequence (button_toggle_Click):
        // light on, then set the level -- sent back-to-back with no
        // inter-command delay (confirmed from the decompiled IL).
        protocol_.light_on();
        protocol_.set_brightness(brightness);
        publish_calibrator(brightness, true);
    }

    void calibrator_off() override {
        ensure_connected();
        // Same locking as calibrator_on(): I/O lock across the pair, state
        // published after success.
        std::lock_guard<std::mutex> io(io_mutex_);
        // Matches the vendor app's OFF sequence: light off, then zero the
        // level.
        protocol_.light_off();
        protocol_.set_brightness(0);
        publish_calibrator(0, false);
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

    // Deliberately NO per-vendor get_device_state() override: per ascom-contract.instructions.md the
    // CoverCalibratorDriver base builds the DeviceState bag from these same
    // getters, and a single-lock vendor override is explicitly disallowed.

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Gemini flat panel not connected", AlpacaError::NotConnected);
        }
    }

    void publish_calibrator(int brightness, bool engaged) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // set_connected(false) stores false and then clears this state under
        // the same lock, so a command that finished during a disconnect must
        // not write over the cleared state.
        if (!connected_.load()) {
            return;
        }
        commanded_brightness_ = brightness;
        calibrator_engaged_ = engaged;
    }

    int device_number_;
    FlatPanelConnectionConfig config_;
    std::atomic<bool> connected_;
    GeminiFlatPanelProtocolWrapper protocol_;

    std::mutex io_mutex_;             // serialises multi-command wire sequences; never taken by a getter
    mutable std::mutex state_mutex_;  // leaf: published brightness/state only, never held across protocol_ calls
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

// --- Astro Automatic FlatPanel v2 (Rev2, motorized cover) ---

class GeminiFlatPanelV2Driver : public CoverCalibratorDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    GeminiFlatPanelV2Driver(int device_number, FlatPanelConnectionConfig config,
                            util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("Gemini"),
          clock_(clock),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          protocol_() {}

    ~GeminiFlatPanelV2Driver() override {
        // Cancel and join the cover and calibrator bodies first: both touch
        // protocol_, which shutdown_connection()/disconnect below may tear
        // down concurrently. Bounded by kCoverMoveTimeoutS (30s) in the worst
        // case -- Rev2 has no native abort, so a stuck motor holds up
        // teardown just as it would hold up HaltCover reporting completion
        // (see halt_cover()); a calibrator body waiting behind that same
        // port mutex is bounded by the same limit.
        stop_background_operations();

        shutdown_connection();
        if (connected_.load()) {
            try {
                GeminiFlatPanelV2Driver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Error during flat panel v2 destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }

    // Identity strings are model-dependent: this class serves both the Rev2
    // ("Astro Automatic FlatPanel v2") and Pro ("Motorized Flat Panel V3")
    // hardware, which differ only in the protocol wrapper's >S#/ack parsing.
    bool is_pro() const { return config_.model == FlatPanelModel::Pro; }

    std::string get_name() const override {
        return is_pro() ? "Gemini Motorized Flat Panel V3" : "Gemini Astro Automatic FlatPanel v2";
    }

    DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }

    std::string fallback_unique_id() const override {
        return (is_pro() ? "GEMINI_FLATPANEL_PRO_" : "GEMINI_FLATPANEL_V2_") + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return is_pro() ? "Gemini Motorized Flat Panel V3 (Pro) Driver" : "Gemini Astro Automatic FlatPanel v2 Driver";
    }

    std::string get_driver_info() const override {
        return is_pro() ? "AlpacaCore Gemini Flat Panel Pro Driver" : "AlpacaCore Gemini Flat Panel v2 Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    // ICoverCalibratorV2: CoverMoving/CalibratorChanging/Connecting/DeviceState,
    // all implemented (DeviceState via the CoverCalibratorDriver base).
    int get_interface_version() const override { return 2; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    // Async, not synchronous like the Lite driver's disconnect(): this
    // model's protocol_.disconnect() takes the same port mutex that
    // open_cover()/close_cover() (and now calibrator_on()/off()) can hold for
    // up to kCoverMoveTimeoutS while a wire command is in flight, so a
    // synchronous call here could block the HTTP handler for up to 30s.
    // Routing through start_connection_task() (as ZWOTelescopeDriver does)
    // runs set_connected(false) on the base class's background thread
    // instead -- run_connection_task() already catches and logs exceptions.
    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
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
            FlatPanelConnectionConfig effective = config_;
            bool auto_detected = false;
            if (effective.serial_port.empty() && effective.auto_detect_index >= 0) {
                // The port that worked last time goes first: one handshake instead of the full
                // search (5-13 s on a rig, past a client's connect timeout).
                if (const auto remembered =
                        util::recall_serial_location(util::location_key(get_unique_id(), effective.auto_detect_index),
                                                     probe_gemini_flatpanel_port)) {
                    ALPACA_LOG_INFO("Gemini", "Flat panel v2 answered on remembered port " + *remembered);
                    effective.serial_port = *remembered;
                } else {
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
                    ALPACA_LOG_INFO("Gemini", "Auto-detected flat panel v2 at " + port.port_path);
                    effective.serial_port = port.port_path;
                }
                auto_detected = true;
            }

            std::string firmware = protocol_.connect(effective);
            // Remember the port only after the handshake held: a port the user configured is never stored.
            if (auto_detected) {
                util::DeviceLocationStore::save(util::location_key(get_unique_id(), effective.auto_detect_index),
                                                util::serial_location_for(effective.serial_port));
            }
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_ = firmware;
            }

            // A new connection starts with no remembered body failure; the
            // previous disconnect joined every body (stop_background_operations()).
            {
                std::lock_guard<std::mutex> lock(failure_mutex_);
                cover_failure_.reset();
                calibrator_failure_.reset();
            }
            // Seed calibrator state from a live read (cover state is never
            // cached -- see get_cover_state()'s doc comment).
            invalidate_cover_status_cache();
            try {
                auto status = protocol_.get_motorized_status();
                const int bri = protocol_.get_brightness();
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    calibrator_engaged_ = status.light_on;
                    // The panel remembers its level while the light is off; Brightness is 0 while CalibratorState
                    // is Off (ICoverCalibratorV2).
                    commanded_brightness_ = status.light_on ? bri : 0;
                }
                // Seed the idle cover-status cache from this same read: ConformU
                // reads DeviceState ~0.2 s and CoverState ~0.4 s after Connect(),
                // inside the connect-adjacent window where even constant
                // properties cost ~0.08 s client-side (see .github/instructions/gemini.instructions.md), so a
                // wire read there always misses the 0.1 s FAST target.
                std::lock_guard<std::mutex> lock(cover_cache_mutex_);
                cover_status_cache_ = status;
                cover_status_read_at_ = clock_.now();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Flat panel v2 state seed failed: " + std::string(e.what()));
            }

            connected_.store(true);
            ALPACA_LOG_INFO("Gemini", "Flat panel v2 connected");
        } else {
            // Driver state first, SDK close second (driver-lifecycle.instructions.md, issue #387): a
            // throwing close must not leave the driver reporting connected on
            // a closed port. disconnect() cannot throw today; the order is
            // the contract, not the current wrapper's behaviour.
            // The cached firmware goes first, so the store(false) is never
            // observable beside a stale firmware string.
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
            // Cancel and join the bodies before the port closes: new commands
            // already throw NotConnected, queued light commands are dropped,
            // and a cover body mid-transaction finishes it (bounded by the
            // wire timeout) rather than meeting a closed port.
            stop_background_operations();
            protocol_.disconnect();
            ALPACA_LOG_INFO("Gemini", "Flat panel v2 disconnected");
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
    // Same light/brightness commands and instant-apply assumption as the
    // Cover Lite (>L#/>D#/>B#/>J# are shared across models per INDI) --
    // see GeminiFlatPanelDriver::calibrator_on()/calibrator_off() for the
    // vendor-app-derived ON/OFF command ordering this mirrors.

    int get_max_brightness() const override {
        ensure_connected();
        return kMaxBrightness;
    }

    int get_brightness() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(state_mutex_);
        return commanded_brightness_;
    }

    CalibratorState get_calibrator_state() const override {
        ensure_connected();
        if (calibrator_pending_count_.load() > 0) {
            return CalibratorState::NotReady;
        }
        {
            // A command that failed on the wire reports Error until the next
            // calibrator command starts (ICoverCalibratorV2).
            std::lock_guard<std::mutex> lock(failure_mutex_);
            if (calibrator_failure_) {
                return CalibratorState::Error;
            }
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        return calibrator_engaged_ ? CalibratorState::Ready : CalibratorState::Off;
    }

    bool get_calibrator_changing() const override {
        // Unlike the Lite model, this is a real transition signal, not a
        // hardcoded false: see start_calibrator_task() for why CalibratorOn/
        // CalibratorOff must report NotReady/true here while their background
        // task is in flight. A pending COUNT rather than a single bool: with
        // queued commands chained behind each other (see start_calibrator_task()),
        // an earlier one finishing and clearing a shared bool would report
        // "not changing" while a later-queued command is still actually
        // running its own wire command.
        ensure_connected();
        return calibrator_pending_count_.load() > 0;
    }

    void calibrator_on(int brightness) override {
        if (brightness < 0 || brightness > kMaxBrightness) {
            throw AlpacaException("Brightness " + std::to_string(brightness) + " out of range [0, " +
                                      std::to_string(kMaxBrightness) + "]",
                                  AlpacaError::InvalidValue);
        }
        ensure_connected();
        start_calibrator_task(/*on=*/true, brightness);
    }

    void calibrator_off() override {
        ensure_connected();
        start_calibrator_task(/*on=*/false, 0);
    }

    void set_brightness(int brightness) override { calibrator_on(brightness); }

    // --- CoverCalibrator interface: motorized cover ---

    /**
     * @brief Live cover state, queried from hardware on every call.
     *
     * Unlike Brightness/CalibratorState (cached -- see the class-level
     * comment on GeminiFlatPanelDriver::calibrator_on() for why), CoverState
     * genuinely changes on its own over the course of a multi-second motor
     * move that this driver doesn't control tick-by-tick, so there is no
     * "commanded" value to cache: while a cover task is in flight this
     * reports Moving directly (no need to ask the hardware, since we know a
     * command is outstanding, unless halted -- see halt_cover() for why a
     * halted in-flight move reports Unknown instead); once idle, it always
     * queries >S# live, regardless of a stale cover_halted_ latch, so the
     * state self-heals as soon as the physical move actually finishes (or
     * across a disconnect/reconnect) instead of sticking at Unknown until
     * the next OpenCover/CloseCover call.
     *
     * The idle-path >S# read is served from a short TTL cache
     * (kCoverStatusCacheMs): the Pro firmware's status reply is 18 chars
     * ("*S0M0L2C0D76C405O#") and a live round-trip measured 0.13 s on the
     * Pi, over ConformU's 0.1 s FAST target for CoverState (and DeviceState,
     * which reads it too). One second of staleness on an idle cover is
     * invisible to clients (nothing moves it but our own commands, which
     * bypass and then invalidate the cache); the cache is seeded at connect
     * so ConformU's post-Connect property sweep never hits the wire.
     */
    CoverState get_cover_state() const override {
        ensure_connected();
        if (cover_in_flight_.load()) {
            return cover_halted_.load() ? CoverState::Unknown : CoverState::Moving;
        }
        {
            // The body records its failure before it clears cover_in_flight_,
            // so there is no window between "no longer moving" and Error.
            std::lock_guard<std::mutex> lock(failure_mutex_);
            if (cover_failure_) {
                return CoverState::Error;
            }
        }
        try {
            FlatPanelMotorizedStatus status;
            {
                std::lock_guard<std::mutex> lock(cover_cache_mutex_);
                const auto now = clock_.now();
                if (!cover_status_read_at_ ||
                    now - *cover_status_read_at_ > std::chrono::milliseconds(kCoverStatusCacheMs)) {
                    cover_status_cache_ = protocol_.get_motorized_status();
                    cover_status_read_at_ = now;
                }
                status = cover_status_cache_;
            }
            switch (status.cover_state) {
                case FlatPanelCoverState::Closed:
                    return CoverState::Closed;
                case FlatPanelCoverState::Open:
                    return CoverState::Open;
                case FlatPanelCoverState::Moving:
                    return CoverState::Moving;
                case FlatPanelCoverState::TimedOut:
                    return CoverState::Error;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Gemini", "Flat panel v2 cover status read failed: " + std::string(e.what()));
        }
        return CoverState::Unknown;
    }

    // True exactly when CoverState is Moving (ICoverCalibratorV2), including a move the panel reports with no
    // command in flight. It asks get_cover_state(), so it takes the same short TTL-cached status read under the
    // cache lock (decision 0016: a getter that must reach the device takes the I/O lock) and never disagrees.
    bool get_cover_moving() const override { return get_cover_state() == CoverState::Moving; }

    void open_cover() override {
        ensure_connected();
        start_cover_task([this] { protocol_.open_cover(); });
    }

    void close_cover() override {
        ensure_connected();
        start_cover_task([this] { protocol_.close_cover(); });
    }

    /**
     * @brief Stop reporting cover movement (Rev2 has no native abort/stop).
     *
     * Per INDI's adapter (GeminiFlatpanel::AbortCap()), only the separate
     * "Pro" hardware revision supports a real abort command -- Rev2 (this
     * model) does not, so there is no wire command to send here. That would
     * normally mean throwing MethodNotImplemented (as the light-only Cover
     * Lite does for all three cover methods), but this model's CoverState is
     * a real state, not NotPresent, and per the WandererCover precedent
     * (.github/instructions/wandererastro.instructions.md) ConformU requires HaltCover to actually function on any
     * cover-capable device rather than throw. So instead: stop *reporting*
     * Moving immediately (CoverState becomes Unknown, CoverMoving becomes
     * false) while the in-flight open/close command's blocking wire call
     * keeps running until the motor physically reaches its end stop or the
     * command times out -- mirrors WandererCover's HaltCover, which also
     * sends no serial command and just clears the driver's own "moving"
     * signal rather than the hardware's.
     *
     * Only latches cover_halted_ when a move is actually in flight. Setting
     * it unconditionally would permanently pin get_cover_state() to Unknown
     * for a client that calls HaltCover defensively while the cover is
     * already stationary (a common pattern). cover_halted_ only affects
     * get_cover_state()/get_cover_moving() while cover_in_flight_ is still
     * true: once the background task's blocking wire call returns (motor
     * reaches its end stop, or times out) and cover_in_flight_ goes back to
     * false, get_cover_state() falls through to a live >S# read regardless
     * of this latch, so the reported state self-heals on its own -- true to
     * the WandererCover precedent cited above, which also always recovers
     * via a live read rather than requiring an explicit next Open/Close to
     * clear a stale "halted" flag.
     *
     * The check-then-act on cover_in_flight_/cover_halted_ must be atomic
     * with respect to start_cover_task(): without the shared lock, the
     * in-flight move this call meant to halt could finish and a brand-new
     * OpenCover/CloseCover could start (resetting cover_halted_ to false)
     * between the load and the store below, so this call's delayed
     * cover_halted_.store(true) would land on and incorrectly halt that new,
     * unrelated move instead. Taking cover_task_mutex_ -- the same lock
     * start_cover_task() holds across its own in_flight/halted transition --
     * closes that window.
     */
    void halt_cover() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(cover_task_mutex_);
        if (cover_in_flight_.load()) {
            cover_halted_.store(true);
        }
    }

    // Deliberately NO per-vendor get_device_state() override -- see
    // GeminiFlatPanelDriver's identical comment; the same ascom-contract.instructions.md rule
    // applies here.

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Gemini flat panel not connected", AlpacaError::NotConnected);
        }
    }

    // The cover and calibrator background bodies run in operation slots
    // (util::AsyncOperation, decision 0006), one per family. The slot owns
    // the thread; the wire commands, the port-order rules and what the
    // failure means stay here.

    // Cancel and join both families' bodies (disconnect and the destructor).
    // One reap mutex covers the whole join, so a second caller cannot see an
    // empty slot while the first still owns a running body. stopping_ wakes
    // the bodies waiting on calibrator_idle_cv_, which the slots' own cancel
    // does not reach, and makes them skip their wire command; a body already
    // inside a wire transaction finishes it (bounded by the wire timeout).
    // Queued calibrator commands that never ran are dropped with their
    // pending count.
    void stop_background_operations() {
        std::lock_guard<std::mutex> reap(reap_mutex_);
        {
            std::lock_guard<std::mutex> lock(calibrator_idle_mutex_);
            stopping_ = true;
        }
        calibrator_idle_cv_.notify_all();
        cover_op_.cancel_all_and_join();
        calibrator_op_.cancel_all_and_join();
        {
            std::lock_guard<std::mutex> lock(calibrator_idle_mutex_);
            calibrator_pending_count_.fetch_sub(static_cast<int>(calibrator_queue_.size()));
            calibrator_queue_.clear();
            stopping_ = false;
        }
        calibrator_idle_cv_.notify_all();
    }

    // OpenCover/CloseCover: a second command while one is in flight is
    // refused (InvalidOperation) rather than queued -- two overlapping
    // Open/Close calls make no physical sense. The slot's start() never waits
    // for a body, so the caller returns at once whatever the wire is doing.
    void start_cover_task(const std::function<void()>& fn) {
        std::lock_guard<std::mutex> lock(cover_task_mutex_);
        if (cover_in_flight_.load()) {
            throw AlpacaException("A cover open/close operation is already in progress", AlpacaError::InvalidOperation);
        }
        {
            std::lock_guard<std::mutex> failure_lock(failure_mutex_);
            cover_failure_.reset();  // a new start clears the previous failure
        }
        cover_halted_.store(false);
        cover_in_flight_.store(true);
        try {
            cover_op_.start([this, fn](util::OperationContext& ctx) {
                std::optional<std::string> failure;
                if (wait_for_calibrator_idle(ctx)) {  // see start_calibrator_task() fast path
                    try {
                        fn();
                    } catch (const std::exception& e) {
                        failure = e.what();
                    } catch (...) {
                        failure = "unknown error";
                    }
                }  // else cancelled by disconnect/destruction: the command never reaches the wire
                if (failure) {
                    ALPACA_LOG_WARN("Gemini", "Flat panel v2 cover command failed: " + *failure);
                }
                // Drop any pre-move cached >S# BEFORE clearing in-flight, so
                // the first idle CoverState read after the move goes to the
                // wire instead of returning the stale pre-move state.
                invalidate_cover_status_cache();
                if (failure) {
                    // Recorded BEFORE in-flight clears: CoverState goes
                    // Moving -> Error with no live read in between.
                    std::lock_guard<std::mutex> failure_lock(failure_mutex_);
                    cover_failure_ = std::move(failure);
                }
                cover_in_flight_.store(false);
            });
        } catch (...) {
            // The slot refused the thread (OS limit, or its stale bound). Roll
            // back or cover_in_flight_ stays true forever and every future
            // Open/Close/HaltCover sees a phantom in-flight move.
            cover_in_flight_.store(false);
            throw;
        }
    }

    /**
     * @brief Run a CalibratorOn/CalibratorOff command in the background.
     *
     * protocol_'s light_on()/light_off()/set_brightness() share the same
     * port mutex as open_cover()/close_cover(), which can hold it for up to
     * kCoverMoveTimeoutS (30s) while a motor move is in flight -- there is
     * only one physical serial link, so the mutex is a real constraint, not
     * just a software choice. Calling protocol_ directly from calibrator_on()
     * blocked the caller behind an unrelated in-flight cover move: ConformU
     * caught this, timing out CalibratorOn's STANDARD (1s) budget when it
     * fired right after HaltCover on a cover that was still physically
     * moving. Dispatching to a background body lets CalibratorOn/Off return
     * immediately per ICoverCalibratorV2's documented async contract, with
     * CalibratorChanging/CalibratorState reporting NotReady until the
     * background command completes.
     *
     * Deliberately asymmetric with start_cover_task(), which REJECTS a new
     * command (InvalidOperation) when one is already in flight: a calibrator
     * command is routinely re-issued in quick succession (e.g. a client
     * ramping Brightness through several values -- ConformU itself does
     * this), so rejecting it would break a legitimate use case. Every
     * command is appended to calibrator_queue_ and every call starts a body in
     * the calibrator slot. start() replaces the previous body without
     * waiting for it, so the caller returns at once, but a replaced body may
     * still be inside its wire transaction: only the NEWEST body drains the
     * queue, one command at a time, and a replaced body that is not on the
     * wire returns without touching it. The commands therefore still reach
     * the wire in submission order, none is skipped, and at most one
     * replaced body (the one on the wire) outlives its start -- the slot
     * bounds replaced bodies at kMaxStaleBodies and would otherwise make the
     * Nth queued call wait or fail while a cover move holds the port.
     *
     * calibrator_pending_count_ (not a single bool) tracks in-flight-ness:
     * it counts commands, not bodies, so NotReady is reported until the LAST
     * queued command finishes (review finding).
     */
    // Drop one calibrator claim (and, for the inline path, its in-flight
    // flag) and wake anything waiting for the port.
    void release_calibrator_claim(bool inline_done = false) {
        {
            std::lock_guard<std::mutex> lock(calibrator_idle_mutex_);
            if (inline_done) {
                calibrator_inline_active_.store(false);
            }
            calibrator_pending_count_.fetch_sub(1);
        }
        calibrator_idle_cv_.notify_all();
    }
    // Let an in-progress calibrator command finish its short wire I/O before
    // a cover move takes the port; bounded so a wedged light command can't
    // hold a cover move hostage (it just falls through to the port mutex).
    // False when the body was stopped and must not send its command.
    bool wait_for_calibrator_idle(util::OperationContext& ctx) {
        std::unique_lock<std::mutex> lock(calibrator_idle_mutex_);
        clock_.wait_for(lock, calibrator_idle_cv_, kCalibratorIdleWait,
                        [this] { return stopping_ || calibrator_pending_count_.load() == 0; });
        return !stopping_ && ctx.stop_reason() == util::StopReason::None;
    }
    // Send the light on/off + brightness pair and commit the commanded state.
    // Shared by the inline fast path and the background body.
    void run_calibrator_command(bool on, int brightness) {
        // One port transaction for the on/off + brightness pair: as two
        // separate calls a cover move could take the port in between and
        // hold the brightness back for the whole move (PR #226 review).
        protocol_.set_light(on, on ? brightness : 0);
        std::lock_guard<std::mutex> lock(state_mutex_);
        commanded_brightness_ = on ? brightness : 0;
        calibrator_engaged_ = on;
    }
    // The body of every calibrator slot start (see start_calibrator_task()).
    void drain_calibrator_queue(util::OperationContext& ctx) {
        std::unique_lock<std::mutex> lock(calibrator_idle_mutex_);
        // Wait for the port: no other drain on the wire and no inline toggle
        // (submission order behind a fast-path toggle, PR #226 review round
        // 3). A replaced body does not wait; the newest body drains.
        calibrator_idle_cv_.wait(lock, [this, &ctx] {
            return stopping_ || ctx.stop_reason() != util::StopReason::None ||
                   (!calibrator_draining_ && !calibrator_inline_active_.load());
        });
        if (stopping_ || ctx.stop_reason() != util::StopReason::None) {
            return;
        }
        calibrator_draining_ = true;
        while (!calibrator_queue_.empty()) {
            const CalibratorCommand cmd = calibrator_queue_.front();
            calibrator_queue_.pop_front();
            lock.unlock();
            std::optional<std::string> failure;
            try {
                run_calibrator_command(cmd.on, cmd.brightness);
            } catch (const std::exception& e) {
                failure = e.what();
            } catch (...) {
                failure = "unknown error";
            }
            if (failure) {
                ALPACA_LOG_WARN("Gemini", "Flat panel v2 calibrator command failed: " + *failure);
            }
            {
                // Recorded BEFORE the pending count drops, so CalibratorState
                // goes NotReady -> Error (or Ready) with no gap. The outcome
                // of the last command wins.
                std::lock_guard<std::mutex> failure_lock(failure_mutex_);
                calibrator_failure_ = std::move(failure);
            }
            lock.lock();
            calibrator_pending_count_.fetch_sub(1);
            calibrator_idle_cv_.notify_all();
            if (stopping_ || ctx.stop_reason() != util::StopReason::None) {
                break;  // replaced: the newer body takes the rest of the queue
            }
        }
        calibrator_draining_ = false;
        calibrator_idle_cv_.notify_all();
    }
    void start_calibrator_task(bool on, int brightness) {
        std::unique_lock<std::mutex> lock(calibrator_task_mutex_);
        // Fast path: with no cover move and no earlier calibrator command in
        // flight, the serial link is free and the two light commands take
        // ~30 ms on the Pro (~250 ms worst case on Lite/Rev2 with their
        // pre-read settle) -- well inside the 1 s STANDARD target. Run them
        // inline so the call returns with CalibratorState already Ready/Off,
        // instead of NotReady until the client's next poll: NINA polls
        // slowly enough that the background path made a 30 ms light toggle
        // look like a multi-second one.
        //
        // cover_task_mutex_ is held only for the check-and-claim (lock order:
        // calibrator_task_mutex_ -> cover_task_mutex_; nothing takes them the
        // other way), NOT across the wire I/O: holding it for the whole
        // ~250 ms made a concurrent HaltCover/OpenCover/CalibratorOn block
        // behind an unrelated light toggle (PR #226 review). The claim is
        // the pending count itself: start_cover_task()'s body waits for it
        // to drop before touching the port, so a cover move issued during
        // the toggle cannot grab the port first and stall this inline call
        // behind its 30 s wire wait, and the caller is released before the
        // I/O starts.
        bool claimed = false;
        {
            std::lock_guard<std::mutex> cover_lock(cover_task_mutex_);
            if (!cover_in_flight_.load() && calibrator_pending_count_.load() == 0) {
                calibrator_pending_count_.fetch_add(1);
                calibrator_inline_active_.store(true);
                claimed = true;
            }
        }
        {
            std::lock_guard<std::mutex> failure_lock(failure_mutex_);
            calibrator_failure_.reset();  // a new command clears the previous failure
        }
        if (claimed) {
            lock.unlock();  // both task locks released before the wire I/O
            try {
                run_calibrator_command(on, brightness);  // throws on wire failure
            } catch (...) {
                release_calibrator_claim(/*inline_done=*/true);
                throw;
            }
            release_calibrator_claim(/*inline_done=*/true);
            return;
        }
        // Background path. A command that lands here while an INLINE toggle
        // is still on the wire has no body to wait for (the fast path owns
        // none); the drain waits for that toggle explicitly or the two
        // set_light() transactions would race for the port and the later
        // command could finish first. The wait is on the body, off the caller.
        std::uint64_t id = 0;
        {
            std::lock_guard<std::mutex> idle_lock(calibrator_idle_mutex_);
            id = ++calibrator_next_command_id_;
            calibrator_queue_.push_back({id, on, brightness});
            calibrator_pending_count_.fetch_add(1);
        }
        try {
            calibrator_op_.start([this](util::OperationContext& ctx) { drain_calibrator_queue(ctx); });
        } catch (...) {
            // The slot refused the body (OS thread limit, or its stale
            // bound). Take the command back and roll the count back; if a
            // running drain already took it, it is on its way to the wire
            // and the call is a success.
            bool removed = false;
            {
                std::lock_guard<std::mutex> idle_lock(calibrator_idle_mutex_);
                for (auto it = calibrator_queue_.begin(); it != calibrator_queue_.end(); ++it) {
                    if (it->id == id) {
                        calibrator_queue_.erase(it);
                        calibrator_pending_count_.fetch_sub(1);
                        removed = true;
                        break;
                    }
                }
            }
            calibrator_idle_cv_.notify_all();
            if (removed) {
                throw;
            }
            return;
        }
        calibrator_idle_cv_.notify_all();  // a replaced body waiting for the port returns
    }

    struct CalibratorCommand {
        std::uint64_t id;
        bool on;
        int brightness;
    };

    util::TaskClock& clock_;
    int device_number_;
    FlatPanelConnectionConfig config_;
    std::atomic<bool> connected_;
    mutable GeminiFlatPanelProtocolWrapper protocol_;  // mutable: get_cover_state() is const but does live I/O

    mutable std::mutex state_mutex_;
    int commanded_brightness_ = 0;
    bool calibrator_engaged_ = false;

    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    std::mutex transition_mutex_;

    std::mutex cover_task_mutex_;
    std::atomic<bool> cover_in_flight_{false};
    std::atomic<bool> cover_halted_{false};

    // The last body failure per family, kept until the next start of that
    // family (ICoverCalibratorV2: CoverState / CalibratorState report Error).
    // Held here, not in the slot's last_failure(): the inline calibrator fast
    // path starts no body, and the slot records a failure only after the
    // body returned, which is after the in-flight flags clear.
    mutable std::mutex failure_mutex_;
    std::optional<std::string> cover_failure_;
    std::optional<std::string> calibrator_failure_;

    std::mutex calibrator_task_mutex_;  // serializes submissions, so queue order is start order
    // Count, not a bool: see start_calibrator_task()'s doc comment for why a
    // single flag misreports "not changing" while queued commands remain.
    std::atomic<int> calibrator_pending_count_{0};
    // Pairs with calibrator_pending_count_ so a cover body can wait for an
    // inline calibrator toggle to release the port (see wait_for_calibrator_idle()).
    // Also guards calibrator_queue_, calibrator_draining_, stopping_ and
    // calibrator_next_command_id_.
    std::mutex calibrator_idle_mutex_;
    std::condition_variable calibrator_idle_cv_;
    std::deque<CalibratorCommand> calibrator_queue_;
    std::uint64_t calibrator_next_command_id_ = 0;
    bool calibrator_draining_ = false;  // a drain is on the wire
    bool stopping_ = false;             // stop_background_operations() in progress
    // True while the fast path's set_light() is on the wire; the drain waits
    // on it so submission order survives a fast->background transition.
    std::atomic<bool> calibrator_inline_active_{false};
    static constexpr std::chrono::seconds kCalibratorIdleWait{2};

    // Idle-cover >S# TTL cache (see get_cover_state()). Serialized on its own
    // mutex so concurrent readers don't each hit the wire.
    static constexpr int kCoverStatusCacheMs = 1000;
    void invalidate_cover_status_cache() const {
        std::lock_guard<std::mutex> lock(cover_cache_mutex_);
        cover_status_read_at_.reset();
    }
    mutable std::mutex cover_cache_mutex_;
    mutable FlatPanelMotorizedStatus cover_status_cache_{};
    mutable std::optional<util::TaskClock::clock::time_point> cover_status_read_at_;

    std::mutex reap_mutex_;  // one stop_background_operations() at a time, across its whole join

    // Declared last: the bodies touch everything above, so the slots (whose
    // destructors join) are destroyed first. One generation per slot: a
    // shared one would mark the other family's running body Superseded.
    util::OperationGeneration cover_generation_;
    util::OperationGeneration calibrator_generation_;
    util::AsyncOperation cover_op_{"Gemini flat panel cover", cover_generation_, clock_};
    util::AsyncOperation calibrator_op_{"Gemini flat panel calibrator", calibrator_generation_, clock_};
};

std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel_v2(int device_number, const std::string& serial_port,
                                                                  int baud_rate) {
    FlatPanelConnectionConfig config;
    config.type = FlatPanelConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    config.model = FlatPanelModel::Rev2;
    return std::make_unique<GeminiFlatPanelV2Driver>(device_number, std::move(config));
}

std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel_v2_by_index(int device_number, int panel_index) {
    FlatPanelConnectionConfig config;
    config.type = FlatPanelConnectionType::Serial;
    config.auto_detect_index = panel_index;
    config.model = FlatPanelModel::Rev2;
    return std::make_unique<GeminiFlatPanelV2Driver>(device_number, std::move(config));
}

std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel_pro(int device_number, const std::string& serial_port,
                                                                   int baud_rate) {
    FlatPanelConnectionConfig config;
    config.type = FlatPanelConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    config.model = FlatPanelModel::Pro;
    return std::make_unique<GeminiFlatPanelV2Driver>(device_number, std::move(config));
}

std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel_pro_by_index(int device_number, int panel_index) {
    FlatPanelConnectionConfig config;
    config.type = FlatPanelConnectionType::Serial;
    config.auto_detect_index = panel_index;
    config.model = FlatPanelModel::Pro;
    return std::make_unique<GeminiFlatPanelV2Driver>(device_number, std::move(config));
}

std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel_v2(int device_number, const std::string& serial_port,
                                                                  int baud_rate, util::TaskClock& clock) {
    FlatPanelConnectionConfig config;
    config.type = FlatPanelConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    config.model = FlatPanelModel::Rev2;
    return std::make_unique<GeminiFlatPanelV2Driver>(device_number, std::move(config), clock);
}

std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel_pro(int device_number, const std::string& serial_port,
                                                                   int baud_rate, util::TaskClock& clock) {
    FlatPanelConnectionConfig config;
    config.type = FlatPanelConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    config.model = FlatPanelModel::Pro;
    return std::make_unique<GeminiFlatPanelV2Driver>(device_number, std::move(config), clock);
}

}  // namespace alpacacore::vendor::gemini
