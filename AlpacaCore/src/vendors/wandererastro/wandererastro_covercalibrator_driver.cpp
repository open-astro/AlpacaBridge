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
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace alpacacore::vendor::wandererastro {

namespace {
// The cover reports no explicit "motion complete" signal; like the INDI driver
// we treat the cover as having reached its target once its angle is within this
// tolerance of the configured open/close angle.
constexpr double kPositionToleranceDeg = 10.0;
// Maximum flat panel PWM level per the WandererCover serial protocol.
constexpr int kMaxBrightness = 255;
} // namespace

class WandererCoverCalibratorDriver : public CoverCalibratorDriver {
public:
    enum class CoverTarget { None, Opening, Closing };

    WandererCoverCalibratorDriver(int device_number, ConnectionConfig config)
        : device_number_(device_number)
        , config_(std::move(config))
        , connected_(false)
        , connecting_(false)
        , protocol_()
    {
    }

    ~WandererCoverCalibratorDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("WandererAstro",
                                "Error during cover destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        return "WandererAstro WandererCover V4";
    }

    DeviceType get_device_type() const override {
        return DeviceType::CoverCalibrator;
    }

    std::string get_unique_id() const override {
        return "WANDERERASTRO_COVERCALIBRATOR_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "WandererAstro WandererCover V4 CoverCalibrator Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore WandererAstro CoverCalibrator Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // ICoverCalibratorV2 (ASCOM Platform 7): adds CoverMoving, CalibratorChanging,
    // Connecting and DeviceState, all of which this driver implements.
    int get_interface_version() const override { return 2; }

    bool get_connected() const override {
        return connected_.load();
    }

    void connect() override {
        start_connection_task(true);
    }

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

    bool get_connecting() const override {
        return connecting_.load();
    }

    void set_connected(bool connected) override {
        if (connected == connected_.load()) {
            return;
        }
        if (connected) {
            protocol_.connect(config_);
            // Seed the calibrator state from the first streamed frame so the
            // initial CalibratorState/Brightness reflect reality if the panel
            // was already lit from a previous session.
            const WandererStatus s = protocol_.get_status();
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                commanded_ = CoverTarget::None;
                commanded_brightness_ = s.valid ? s.brightness : 0;
                calibrator_engaged_ = s.valid && s.brightness > 0;
            }
            connected_.store(true);
            ALPACA_LOG_INFO("WandererAstro", "Cover connected");
        } else {
            protocol_.disconnect();
            connected_.store(false);
            ALPACA_LOG_INFO("WandererAstro", "Cover disconnected");
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name),
                              AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override {
        return false;
    }

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
        return kMaxBrightness;
    }

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
        // Validate the range first so an out-of-range request is rejected with
        // InvalidValue regardless of connection state (ConformU exercises the
        // boundaries while connected; the order is equivalent there).
        if (brightness < 0 || brightness > kMaxBrightness) {
            throw AlpacaException("Brightness " + std::to_string(brightness) +
                                  " out of range [0, " + std::to_string(kMaxBrightness) + "]",
                                  AlpacaError::InvalidValue);
        }
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            commanded_brightness_ = brightness;
            calibrator_engaged_ = true;   // "on" even at brightness 0 (ASCOM: Ready)
        }
        protocol_.set_brightness(brightness);
    }

    void calibrator_off() override {
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            commanded_brightness_ = 0;
            calibrator_engaged_ = false;
        }
        protocol_.turn_off_light();
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
        if (!s.valid) {
            return CoverState::Unknown;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        const bool at_close = std::fabs(s.current_position - s.close_position) <= kPositionToleranceDeg;
        const bool at_open  = std::fabs(s.current_position - s.open_position)  <= kPositionToleranceDeg;

        switch (commanded_) {
            case CoverTarget::Opening:
                if (at_open) {
                    commanded_ = CoverTarget::None;
                    return CoverState::Open;
                }
                return CoverState::Moving;
            case CoverTarget::Closing:
                if (at_close) {
                    commanded_ = CoverTarget::None;
                    return CoverState::Closed;
                }
                return CoverState::Moving;
            case CoverTarget::None:
            default:
                if (at_close) return CoverState::Closed;
                if (at_open)  return CoverState::Open;
                return CoverState::Unknown;
        }
    }

    bool get_cover_moving() const override {
        return get_cover_state() == CoverState::Moving;
    }

    void open_cover() override {
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            commanded_ = CoverTarget::Opening;
        }
        protocol_.open_cover();
    }

    void close_cover() override {
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            commanded_ = CoverTarget::Closing;
        }
        protocol_.close_cover();
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

    void start_connection_task(bool do_connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connecting_.load()) {
            return;
        }
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
        connecting_.store(true);
        connection_thread_ = std::thread([this, do_connect]() {
            try {
                set_connected(do_connect);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR("WandererAstro",
                                 "Cover connection failed: " + std::string(e.what()));
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
    ConnectionConfig config_;
    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    WandererProtocolWrapper protocol_;

    mutable std::mutex state_mutex_;
    mutable CoverTarget commanded_ = CoverTarget::None;
    // Driver-side calibrator state, updated synchronously on CalibratorOn/Off so
    // reads don't wait for the lagging status stream.
    int commanded_brightness_ = 0;
    bool calibrator_engaged_ = false;

    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<CoverCalibratorDriver> create_wandererastro_covercalibrator(
    int device_number,
    const std::string& serial_port,
    int baud_rate) {
    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<WandererCoverCalibratorDriver>(device_number, std::move(config));
}

std::unique_ptr<CoverCalibratorDriver> create_wandererastro_covercalibrator_by_index(
    int device_number,
    int cover_index) {
    auto ports = enumerate_wanderer_ports();
    if (ports.empty()) {
        throw AlpacaException("No WandererCover detected on any serial port",
                              AlpacaError::NotConnected);
    }
    if (cover_index < 0 || cover_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Cover index " + std::to_string(cover_index) +
                              " out of range (detected " + std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }

    const auto& port = ports[static_cast<std::size_t>(cover_index)];
    ALPACA_LOG_INFO("WandererAstro", "Auto-detected " + port.model + " at " + port.port_path);

    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.serial_port = port.port_path;
    return std::make_unique<WandererCoverCalibratorDriver>(device_number, std::move(config));
}

} // namespace alpacacore::vendor::wandererastro
