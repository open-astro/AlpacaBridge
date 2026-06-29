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

#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::wandererastro {

namespace {
// The cover reports no explicit "motion complete" signal; like the INDI driver
// we treat the cover as having reached its target once its angle is within this
// tolerance of the configured open/close angle.
constexpr double kPositionToleranceDeg = 10.0;
// Maximum flat panel PWM level per the WandererCover serial protocol.
constexpr int kMaxBrightness = 255;
}  // namespace

class WandererCoverCalibratorDriver : public CoverCalibratorDriver {
public:
    enum class CoverTarget : std::uint8_t { None, Opening, Closing };

    WandererCoverCalibratorDriver(int device_number, ConnectionConfig config)
        : device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          connecting_(false),
          protocol_() {}

    ~WandererCoverCalibratorDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                // Tear down directly rather than via the virtual set_connected(),
                // which must not be dispatched during destruction.
                protocol_.disconnect();
                connected_.store(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("WandererAstro", "Error during cover destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "WandererAstro WandererCover V4"; }

    DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }

    std::string get_unique_id() const override {
        return "WANDERERASTRO_COVERCALIBRATOR_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "WandererAstro WandererCover V4 CoverCalibrator Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore WandererAstro CoverCalibrator Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware date from the streamed status frame (cached — no extra I/O).
    // Surfaced in the web UI only, never in DriverInfo. firmware_version is a
    // YYYYMMDD integer; render it as YYYY-MM-DD.
    std::optional<std::string> get_device_firmware() const override {
        if (!connected_.load()) {
            return std::nullopt;
        }
        const WandererStatus s = protocol_.get_status();
        if (!s.valid || s.firmware_version <= 0) {
            return std::nullopt;
        }
        const int fw = s.firmware_version;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", fw / 10000, (fw / 100) % 100, fw % 100);
        return std::string(buf);
    }

    // ICoverCalibratorV2 (ASCOM Platform 7): adds CoverMoving, CalibratorChanging,
    // Connecting and DeviceState, all of which this driver implements.
    int get_interface_version() const override { return 2; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously — closing the port + stopping the reader is
        // trivial and ASCOM clients expect Connected to be false immediately.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("WandererAstro", "Cover disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        // Serialize the whole connect/disconnect transition. Without this, two
        // threads racing to set the same target both pass the load() guard and
        // both run the body — e.g. concurrent disconnects would call
        // protocol_.disconnect() -> reader_thread_.join() twice on one thread
        // object (undefined behavior). Holding this for the duration also makes
        // the guard check-then-act atomic w.r.t. other transitions.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (connected == connected_.load()) {
            return;
        }
        if (connected) {
            // Resolve an auto-detect request here (on the background connection
            // thread), not at registration: enumerate the ports and pick the
            // requested match. A local copy keeps config_ as the durable intent
            // so a later reconnect re-scans (robust to the device moving ports).
            ConnectionConfig effective = config_;
            if (effective.serial_port.empty() && effective.auto_detect_index >= 0) {
                auto ports = enumerate_wanderer_ports();
                if (ports.empty()) {
                    throw AlpacaException(util::serial_auto_detect_failed_message("WandererCover"),
                                          AlpacaError::NotConnected);
                }
                if (effective.auto_detect_index >= static_cast<int>(ports.size())) {
                    throw AlpacaException("Cover index " + std::to_string(effective.auto_detect_index) +
                                              " out of range (detected " + std::to_string(ports.size()) + ")",
                                          AlpacaError::NotConnected);
                }
                const auto& port = ports[static_cast<std::size_t>(effective.auto_detect_index)];
                ALPACA_LOG_INFO("WandererAstro", "Auto-detected " + port.model + " at " + port.port_path);
                effective.serial_port = port.port_path;
            }
            protocol_.connect(effective);
            // Seed the calibrator state from the first streamed frame so the
            // initial CalibratorState/Brightness reflect reality if the panel
            // was already lit from a previous session. If seeding throws, undo
            // the protocol connect so the open port and reader thread aren't
            // leaked while the driver still reports disconnected.
            try {
                const WandererStatus s = protocol_.get_status();
                std::lock_guard<std::mutex> lock(state_mutex_);
                commanded_ = CoverTarget::None;
                if (s.valid) {
                    commanded_brightness_ = s.brightness;
                    // A lit panel (brightness > 0) is unambiguously Ready. At
                    // brightness 0 the panel is physically indistinguishable from
                    // Off, so we deliberately leave calibrator_engaged_ untouched
                    // instead of forcing it false. calibrator_engaged_ is a
                    // persistent member that disconnect() does NOT reset and the
                    // driver instance survives a Connected=false/true cycle, so a
                    // Ready@0 state set via CalibratorOn(0) is retained across a
                    // reconnect of the same device. A freshly-constructed driver
                    // starts Off via the member initialiser (calibrator_engaged_
                    // = false), so the very first connect at brightness 0 is Off.
                    if (s.brightness > 0) {
                        calibrator_engaged_ = true;
                    }
                }
            } catch (...) {
                protocol_.disconnect();
                throw;
            }
            connected_.store(true);
            ALPACA_LOG_INFO("WandererAstro", "Cover connected");
        } else {
            protocol_.disconnect();
            connected_.store(false);
            ALPACA_LOG_INFO("WandererAstro", "Cover disconnected");
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
        // Report the last commanded brightness synchronously. The streamed
        // status lags a command by up to ~1s, but ASCOM clients (and ConformU)
        // read Brightness immediately after CalibratorOn and expect the value
        // just set.
        std::lock_guard<std::mutex> lock(state_mutex_);
        return commanded_brightness_;
    }

    CalibratorState get_calibrator_state() const override {
        ensure_connected();
        // Synchronous: the panel applies brightness instantly, so the calibrator
        // is Ready whenever it has been turned on (including at brightness 0) and
        // Off after CalibratorOff. Derived from the commanded state rather than
        // the lagging status stream so the value is correct the instant a client
        // reads it after CalibratorOn/Off.
        std::lock_guard<std::mutex> lock(state_mutex_);
        return calibrator_engaged_ ? CalibratorState::Ready : CalibratorState::Off;
    }

    bool get_calibrator_changing() const override {
        // Brightness changes take effect immediately; the calibrator is never
        // in a transient NotReady state.
        ensure_connected();
        return false;
    }

    void calibrator_on(int brightness) override {
        // Brightness 0 is a VALID "on at zero brightness" request, NOT off:
        // ASCOM ICoverCalibratorV2 and ConformU require CalibratorOn(0) to
        // leave CalibratorState == Ready (ConformU's CoverCalibratorTester calls
        // TestCalibratorOn(0) and flags an issue if the state is anything but
        // Ready, and a separate issue if 0 throws InvalidValue). So 0 must
        // neither be rejected nor routed through CalibratorOff(). On this EL
        // panel brightness 0 is physically dark (PWM 0 == the off command 9999),
        // so CalibratorOn(0) and CalibratorOff() look identical at the hardware
        // but are intentionally distinct driver states (Ready@0 vs Off) per the
        // spec — a caller that wants illumination requests a non-zero brightness.
        // Only values outside [0, MaxBrightness] are invalid.
        //
        // Validate the range first so an out-of-range request is rejected with
        // InvalidValue regardless of connection state (ConformU exercises the
        // boundaries while connected; the order is equivalent there).
        if (brightness < 0 || brightness > kMaxBrightness) {
            throw AlpacaException("Brightness " + std::to_string(brightness) + " out of range [0, " +
                                      std::to_string(kMaxBrightness) + "]",
                                  AlpacaError::InvalidValue);
        }
        ensure_connected();
        // Hold state_mutex_ across the write so a concurrent calibrator_off()
        // can't interleave between the state update and the command (which would
        // leave the panel on while the driver reports Off, or vice versa).
        std::lock_guard<std::mutex> lock(state_mutex_);
        const int prev_brightness = commanded_brightness_;
        const bool prev_engaged = calibrator_engaged_;
        commanded_brightness_ = brightness;
        calibrator_engaged_ = true;  // "on" even at brightness 0 (ASCOM: Ready)
        try {
            protocol_.set_brightness(brightness);
        } catch (...) {
            // The command never reached the panel — restore the prior reported
            // state so the driver doesn't claim Ready at a brightness the panel
            // never applied.
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
            protocol_.turn_off_light();
        } catch (...) {
            commanded_brightness_ = prev_brightness;
            calibrator_engaged_ = prev_engaged;
            throw;
        }
    }

    void set_brightness(int brightness) override {
        // Not part of the ASCOM HTTP surface (brightness is set via CalibratorOn),
        // but provided for completeness; mirrors calibrator_on validation.
        calibrator_on(brightness);
    }

    // --- CoverCalibrator interface: cover ---

    CoverState get_cover_state() const override {
        ensure_connected();
        const WandererStatus s = protocol_.get_status();
        std::lock_guard<std::mutex> lock(state_mutex_);
        return cover_state_locked(s);
    }

    bool get_cover_moving() const override {
        ensure_connected();
        // Derive from the same single status snapshot + the active target, the
        // way get_cover_state() does, rather than calling get_cover_state()
        // again — so a single caller gets a self-consistent CoverState /
        // CoverMoving pair. (Two *separate* HTTP reads of a moving cover are
        // still non-atomic by nature; DeviceState is the atomic snapshot.)
        const WandererStatus s = protocol_.get_status();
        std::lock_guard<std::mutex> lock(state_mutex_);
        return cover_state_locked(s) == CoverState::Moving;
    }

    // Deliberately NO per-vendor get_device_state() override: per AGENTS.md the
    // CoverCalibratorDriver base builds the DeviceState bag from these same
    // getters, DeviceState is intentionally non-atomic, and a single-lock vendor
    // override is explicitly disallowed (ASCOM doesn't require cross-property
    // atomicity; ConformU only checks per-property DeviceState↔GET consistency).

    void open_cover() override {
        ensure_connected();
        // Hold state_mutex_ across the write so the target and the command can't
        // be separated by a concurrent halt_cover()/close_cover(): otherwise that
        // could reset commanded_ in the gap, the write would still start the
        // cover moving, and CoverState/CoverMoving would never report Moving.
        // (Lock order is always state_mutex_ -> protocol mutex; the protocol
        // never calls back into the driver, so there's no deadlock. The write is
        // a fire-and-forget 5-byte send.)
        std::lock_guard<std::mutex> lock(state_mutex_);
        const CoverTarget prev = commanded_;
        commanded_ = CoverTarget::Opening;
        try {
            protocol_.open_cover();
        } catch (...) {
            commanded_ = prev;  // the command never reached the controller
            throw;
        }
    }

    void close_cover() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(state_mutex_);
        const CoverTarget prev = commanded_;
        commanded_ = CoverTarget::Closing;
        try {
            protocol_.close_cover();
        } catch (...) {
            commanded_ = prev;
            throw;
        }
    }

    void halt_cover() override {
        ensure_connected();
        // ASCOM requires HaltCover to function on a cover-capable device, but the
        // WandererCover serial protocol has no halt command. Stop tracking the
        // in-progress move so CoverState/CoverMoving immediately stop reporting
        // Moving; the cover then completes its current travel mechanically (the
        // controller stops the motor at the configured end stop). See AGENTS.md.
        std::lock_guard<std::mutex> lock(state_mutex_);
        commanded_ = CoverTarget::None;
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("WandererCover not connected", AlpacaError::NotConnected);
        }
    }

    // Pure cover-state computation from a status snapshot + the active command
    // target. Caller must hold state_mutex_. No side effects: the move target is
    // cleared by an explicit command (HaltCover) or overwritten by the next
    // Open/Close, never as a side effect of a read — so repeated reads of an
    // idle cover always return the same value and get_cover_moving() agrees
    // with get_cover_state().
    CoverState cover_state_locked(const WandererStatus& s) const {
        if (!s.valid) {
            return CoverState::Unknown;
        }
        const bool at_close = std::fabs(s.current_position - s.close_position) <= kPositionToleranceDeg;
        const bool at_open = std::fabs(s.current_position - s.open_position) <= kPositionToleranceDeg;
        switch (commanded_) {
            case CoverTarget::Opening:
                return at_open ? CoverState::Open : CoverState::Moving;
            case CoverTarget::Closing:
                return at_close ? CoverState::Closed : CoverState::Moving;
            case CoverTarget::None:
            default:
                // If the configured open/close angles are within tolerance of
                // each other (misconfigured hardware) a single position can match
                // both — report Unknown rather than silently favouring Closed.
                if (at_close && at_open) return CoverState::Unknown;
                if (at_close) return CoverState::Closed;
                if (at_open) return CoverState::Open;
                return CoverState::Unknown;
        }
    }

    void start_connection_task(bool do_connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connecting_.load()) {
            return;
        }
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
        connecting_.store(true);
        try {
            connection_thread_ = std::thread([this, do_connect]() {
                try {
                    set_connected(do_connect);
                } catch (const std::exception& e) {
                    ALPACA_LOG_ERROR("WandererAstro", "Cover connection failed: " + std::string(e.what()));
                }
                connecting_.store(false);
            });
        } catch (...) {
            // std::thread ctor can throw (e.g. OS thread limit). Reset the flag
            // so the driver stays connectable instead of being wedged with
            // connecting_ == true forever.
            connecting_.store(false);
            throw;
        }
    }

    void stop_connection_thread() {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
    }

    int device_number_;
    ConnectionConfig config_;
    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    WandererProtocolWrapper protocol_;

    mutable std::mutex state_mutex_;
    CoverTarget commanded_ = CoverTarget::None;
    // Driver-side calibrator state, updated synchronously on CalibratorOn/Off so
    // reads don't wait for the lagging status stream.
    int commanded_brightness_ = 0;
    bool calibrator_engaged_ = false;

    std::mutex connection_mutex_;
    std::thread connection_thread_;
    std::mutex transition_mutex_;  // serializes set_connected() connect/disconnect transitions
};

std::unique_ptr<CoverCalibratorDriver> create_wandererastro_covercalibrator(int device_number,
                                                                            const std::string& serial_port,
                                                                            int baud_rate) {
    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<WandererCoverCalibratorDriver>(device_number, std::move(config));
}

std::unique_ptr<CoverCalibratorDriver> create_wandererastro_covercalibrator_by_index(int device_number,
                                                                                     int cover_index) {
    // Defer the (blocking) port scan to connect time, which runs on the driver's
    // background connection thread. Doing it here would block the caller — the
    // HTTP handler thread at device registration — for up to ~2.5s per candidate
    // port while every already-registered device is unreachable.
    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.auto_detect_index = cover_index;
    return std::make_unique<WandererCoverCalibratorDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::wandererastro
