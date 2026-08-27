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
#include <chrono>
#include <condition_variable>
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

// --- Astro Automatic FlatPanel v2 (Rev2, motorized cover) ---

class GeminiFlatPanelV2Driver : public CoverCalibratorDriver, protected alpacacore::AsyncConnectable {
public:
    GeminiFlatPanelV2Driver(int device_number, FlatPanelConnectionConfig config)
        : AsyncConnectable("Gemini"),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          protocol_() {}

    ~GeminiFlatPanelV2Driver() override {
        // Reap the cover and calibrator tasks first: both touch protocol_,
        // which shutdown_connection()/disconnect below may tear down
        // concurrently. Bounded by kCoverMoveTimeoutS (30s) in the worst
        // case -- Rev2 has no native abort, so a stuck motor holds up
        // teardown just as it would hold up HaltCover reporting completion
        // (see halt_cover()); a calibrator task waiting behind that same
        // port mutex is bounded by the same limit.
        reap_cover_task(/*wait=*/true);
        reap_calibrator_task(/*wait=*/true);

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

    std::string get_unique_id() const override {
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
                ALPACA_LOG_INFO("Gemini", "Auto-detected flat panel v2 at " + port.port_path);
                effective.serial_port = port.port_path;
            }

            std::string firmware = protocol_.connect(effective);
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_ = firmware;
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
                    commanded_brightness_ = bri;
                }
                // Seed the idle cover-status cache from this same read: ConformU
                // reads DeviceState ~0.2 s and CoverState ~0.4 s after Connect(),
                // inside the connect-adjacent window where even constant
                // properties cost ~0.08 s client-side (see AGENTS.md), so a
                // wire read there always misses the 0.1 s FAST target.
                std::lock_guard<std::mutex> lock(cover_cache_mutex_);
                cover_status_cache_ = status;
                cover_status_read_at_ = std::chrono::steady_clock::now();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Flat panel v2 state seed failed: " + std::string(e.what()));
            }

            connected_.store(true);
            ALPACA_LOG_INFO("Gemini", "Flat panel v2 connected");
        } else {
            protocol_.disconnect();
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
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

    int get_max_brightness() const override { return kMaxBrightness; }

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
        try {
            FlatPanelMotorizedStatus status;
            {
                std::lock_guard<std::mutex> lock(cover_cache_mutex_);
                const auto now = std::chrono::steady_clock::now();
                if (now - cover_status_read_at_ > std::chrono::milliseconds(kCoverStatusCacheMs)) {
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

    bool get_cover_moving() const override {
        ensure_connected();
        return cover_in_flight_.load() && !cover_halted_.load();
    }

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
     * a real state, not NotPresent, and per AGENTS.md/the WandererCover
     * precedent ConformU requires HaltCover to actually function on any
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
    // GeminiFlatPanelDriver's identical comment; the same AGENTS.md rule
    // applies here.

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Gemini flat panel not connected", AlpacaError::NotConnected);
        }
    }

    // Reaps a finished cover task thread. If wait is false and the task is
    // still running, a NEW task is refused (InvalidOperation) rather than
    // blocking the caller -- Rev2's open/close commands block the wire for
    // up to kCoverMoveTimeoutS, and joining an in-flight one here would blow
    // OpenCover/CloseCover's STANDARD (1s) ConformU timing budget. wait=true
    // is only used from the destructor, where blocking is acceptable.
    void reap_cover_task(bool wait) {
        std::lock_guard<std::mutex> lock(cover_task_mutex_);
        if (!cover_task_thread_.joinable()) {
            return;
        }
        if (!wait && cover_in_flight_.load()) {
            return;
        }
        cover_task_thread_.join();
    }

    void start_cover_task(const std::function<void()>& fn) {
        std::lock_guard<std::mutex> lock(cover_task_mutex_);
        if (cover_in_flight_.load()) {
            throw AlpacaException("A cover open/close operation is already in progress", AlpacaError::InvalidOperation);
        }
        if (cover_task_thread_.joinable()) {
            cover_task_thread_.join();  // previous command already finished; reap it
        }
        cover_halted_.store(false);
        cover_in_flight_.store(true);
        try {
            cover_task_thread_ = std::thread([this, fn]() {
                wait_for_calibrator_idle();  // see start_calibrator_task() fast path
                try {
                    fn();
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("Gemini", "Flat panel v2 cover command failed: " + std::string(e.what()));
                }
                // Drop any pre-move cached >S# BEFORE clearing in-flight, so
                // the first idle CoverState read after the move goes to the
                // wire instead of returning the stale pre-move state.
                invalidate_cover_status_cache();
                cover_in_flight_.store(false);
            });
        } catch (...) {
            // std::thread ctor can throw (e.g. OS thread limit). Roll back or
            // cover_in_flight_ stays true forever and every future
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
     * moving. Dispatching to a background thread mirrors start_cover_task()
     * and lets CalibratorOn/Off return immediately per ICoverCalibratorV2's
     * documented async contract, with CalibratorChanging/CalibratorState
     * reporting NotReady until the background command completes.
     *
     * Deliberately asymmetric with start_cover_task(), which REJECTS a new
     * command (InvalidOperation) when one is already in flight rather than
     * joining it: two overlapping Open/Close calls make no physical sense,
     * whereas a calibrator command is routinely re-issued in quick
     * succession (e.g. a client ramping Brightness through several values --
     * ConformU itself does this), so rejecting it would break a legitimate
     * use case. Instead the queued command is serialized behind the
     * in-flight one -- but the join itself is deferred onto the NEW
     * background thread (moving the previous std::thread handle into its
     * capture) rather than run on the caller's thread: an earlier version
     * joined inline here, which reintroduced exactly the bug class this PR
     * fixes elsewhere whenever the previous calibrator command was itself
     * still blocked on protocol_'s port mutex behind a concurrent cover
     * move (up to kCoverMoveTimeoutS = 30s). This way calibrator_on()/off()
     * always return immediately, and successive commands still execute in
     * submission order (each thread joins its predecessor before running
     * its own wire command).
     *
     * calibrator_pending_count_ (not a single bool) tracks in-flight-ness:
     * with commands chained behind each other, an earlier thread finishing
     * and clearing a shared bool would report "not changing" while a
     * later-queued thread is still actually running its own wire command
     * (review finding). Each thread increments the count for its own
     * command and decrements it on exit; NotReady is reported as long as
     * the count is above zero, i.e. until the LAST queued command finishes.
     *
     * previous is held via shared_ptr, not moved directly into the new
     * thread's capture, for exception safety (review finding): std::thread's
     * constructor decay-copies its callable into internal storage before
     * attempting to create the OS thread, and destroys that storage if
     * creation fails (e.g. OS thread-limit exhaustion). If previous were
     * captured by move, that storage's destruction would run ~thread() on a
     * still-joinable previous -> std::terminate(), crashing the process
     * instead of throwing a normal AlpacaException. A shared_ptr copy into
     * the capture is cheap and noexcept, and this function's own `previous`
     * keeps a live reference throughout, so if construction below throws,
     * the failed copy's destruction just drops a refcount rather than
     * destroying the underlying std::thread -- leaving it here for the
     * catch block to join safely on the caller's thread.
     */
    // Drop one calibrator claim and wake a cover task waiting for the port.
    void release_calibrator_claim() {
        {
            std::lock_guard<std::mutex> lock(calibrator_idle_mutex_);
            calibrator_pending_count_.fetch_sub(1);
        }
        calibrator_idle_cv_.notify_all();
    }

    // Let an in-progress calibrator command finish its short wire I/O before
    // a cover move takes the port; bounded so a wedged light command can't
    // hold a cover move hostage (it just falls through to the port mutex).
    void wait_for_calibrator_idle() {
        std::unique_lock<std::mutex> lock(calibrator_idle_mutex_);
        calibrator_idle_cv_.wait_for(lock, kCalibratorIdleWait,
                                     [this] { return calibrator_pending_count_.load() == 0; });
    }

    // Send the light on/off + brightness pair and commit the commanded state.
    // Shared by the inline fast path and the background task.
    void run_calibrator_command(bool on, int brightness) {
        if (on) {
            protocol_.light_on();
            protocol_.set_brightness(brightness);
        } else {
            protocol_.light_off();
            protocol_.set_brightness(0);
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        commanded_brightness_ = on ? brightness : 0;
        calibrator_engaged_ = on;
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
        // the pending count itself: start_cover_task()'s thread waits for it
        // to drop before touching the port, so a cover move issued during
        // the toggle cannot grab the port first and stall this inline call
        // behind its 30 s wire wait, and the caller is released before the
        // I/O starts.
        bool claimed = false;
        {
            std::lock_guard<std::mutex> cover_lock(cover_task_mutex_);
            if (!cover_in_flight_.load() && calibrator_pending_count_.load() == 0) {
                calibrator_pending_count_.fetch_add(1);
                claimed = true;
            }
        }
        if (claimed) {
            lock.unlock();  // both task locks released before the wire I/O
            try {
                run_calibrator_command(on, brightness);  // throws on wire failure
            } catch (...) {
                release_calibrator_claim();
                throw;
            }
            release_calibrator_claim();
            return;
        }

        calibrator_pending_count_.fetch_add(1);
        auto previous = std::make_shared<std::thread>(std::move(calibrator_task_thread_));
        try {
            calibrator_task_thread_ = std::thread([this, on, brightness, previous]() {
                if (previous->joinable()) {
                    previous->join();  // off the caller's thread -- see doc comment above
                }
                try {
                    run_calibrator_command(on, brightness);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("Gemini", "Flat panel v2 calibrator command failed: " + std::string(e.what()));
                }
                release_calibrator_claim();
            });
        } catch (...) {
            // std::thread ctor can throw (e.g. OS thread limit). previous is
            // still safely joinable here -- see the doc comment above -- so
            // join it (this rare failure path already means the system is
            // resource-starved; blocking briefly is an acceptable tradeoff
            // against losing track of it or crashing) before rolling back
            // the pending count and rethrowing.
            if (previous->joinable()) {
                previous->join();
            }
            release_calibrator_claim();
            throw;
        }
    }

    // Mirrors reap_cover_task(): wait=false (not currently used, kept for
    // symmetry) would skip an in-flight task rather than block; wait=true is
    // used from the destructor.
    void reap_calibrator_task(bool wait) {
        std::lock_guard<std::mutex> lock(calibrator_task_mutex_);
        if (!calibrator_task_thread_.joinable()) {
            return;
        }
        if (!wait && calibrator_pending_count_.load() > 0) {
            return;
        }
        calibrator_task_thread_.join();
    }

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
    std::thread cover_task_thread_;
    std::atomic<bool> cover_in_flight_{false};
    std::atomic<bool> cover_halted_{false};

    std::mutex calibrator_task_mutex_;
    std::thread calibrator_task_thread_;
    // Count, not a bool: see start_calibrator_task()'s doc comment for why a
    // single flag misreports "not changing" while queued commands remain.
    std::atomic<int> calibrator_pending_count_{0};
    // Pairs with calibrator_pending_count_ so a cover task can wait for an
    // inline calibrator toggle to release the port (see wait_for_calibrator_idle()).
    std::mutex calibrator_idle_mutex_;
    std::condition_variable calibrator_idle_cv_;
    static constexpr std::chrono::seconds kCalibratorIdleWait{2};

    // Idle-cover >S# TTL cache (see get_cover_state()). Serialized on its own
    // mutex so concurrent readers don't each hit the wire.
    static constexpr int kCoverStatusCacheMs = 1000;
    void invalidate_cover_status_cache() const {
        std::lock_guard<std::mutex> lock(cover_cache_mutex_);
        cover_status_read_at_ = std::chrono::steady_clock::time_point{};
    }
    mutable std::mutex cover_cache_mutex_;
    mutable FlatPanelMotorizedStatus cover_status_cache_{};
    mutable std::chrono::steady_clock::time_point cover_status_read_at_{};
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

}  // namespace alpacacore::vendor::gemini
