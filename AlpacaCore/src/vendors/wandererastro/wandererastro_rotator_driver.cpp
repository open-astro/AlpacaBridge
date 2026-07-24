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
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::wandererastro {

class WandererRotatorDriver : public RotatorDriver, protected alpacacore::AsyncConnectable {
public:
    WandererRotatorDriver(int device_number, RotatorConnectionConfig config)
        : AsyncConnectable("WandererAstro"),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          protocol_() {}

    ~WandererRotatorDriver() override {
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
                ALPACA_LOG_WARN("WandererAstro", "Error during rotator destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "WandererAstro WandererRotator Mini"; }

    DeviceType get_device_type() const override { return DeviceType::Rotator; }

    std::string get_unique_id() const override { return "WANDERERASTRO_ROTATOR_" + std::to_string(device_number_); }

    std::string get_description() const override { return "WandererAstro WandererRotator Mini Rotator Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore WandererAstro Rotator Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware date (YYYY-MM-DD), captured by the protocol wrapper from the
    // handshake and cleared there on disconnect. Web UI only, never DriverInfo.
    std::optional<std::string> get_device_firmware() const override { return protocol_.get_firmware_date(); }

    // IRotatorV4 (ASCOM Platform 7): MechanicalPosition, MoveMechanical, Sync,
    // Connecting and DeviceState, all of which this driver implements.
    int get_interface_version() const override { return 4; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously — closing the port + joining the move monitor
        // is quick and ASCOM clients expect Connected to be false immediately.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("WandererAstro", "Rotator disconnect error: " + std::string(e.what()));
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
            RotatorConnectionConfig effective = config_;
            if (effective.serial_port.empty() && effective.auto_detect_index >= 0) {
                auto ports = enumerate_wanderer_rotator_ports();
                if (ports.empty()) {
                    throw AlpacaException(util::serial_auto_detect_failed_message("WandererRotator"),
                                          AlpacaError::NotConnected);
                }
                if (effective.auto_detect_index >= static_cast<int>(ports.size())) {
                    throw AlpacaException("Rotator index " + std::to_string(effective.auto_detect_index) +
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
                sync_offset_ = 0.0;
                target_position_ = 0.0;
                has_target_position_ = false;
            }
            connected_.store(true);
            ALPACA_LOG_INFO("WandererAstro", "Rotator connected");
        } else {
            protocol_.disconnect();  // joins the move monitor, clears cached firmware
            connected_.store(false);
            ALPACA_LOG_INFO("WandererAstro", "Rotator disconnected");
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

    // --- Rotator interface ---

    bool get_can_reverse() const override { return true; }

    bool get_reverse() const override {
        ensure_connected();
        return protocol_.get_state().reverse;
    }

    void set_reverse(bool reverse) override {
        ensure_connected();
        protocol_.set_reverse(reverse);
    }

    bool get_is_moving() const override {
        ensure_connected();
        return protocol_.get_state().moving;
    }

    double get_mechanical_position() const override {
        ensure_connected();
        return normalize_angle(protocol_.get_state().mechanical_angle);
    }

    double get_position() const override {
        ensure_connected();
        const double mechanical = protocol_.get_state().mechanical_angle;
        std::lock_guard<std::mutex> lock(state_mutex_);
        return normalize_angle(mechanical + sync_offset_);
    }

    // One full step of the 1142 steps/degree worm drive.
    double get_step_size() const override { return 1.0 / kRotatorMiniStepsPerDegree; }

    double get_target_position() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(state_mutex_);
        return has_target_position_ ? target_position_ : 0.0;
    }

    void set_target_position(double position) override {
        ensure_connected();
        validate_angle(position);
        std::lock_guard<std::mutex> lock(state_mutex_);
        target_position_ = normalize_angle(position);
        has_target_position_ = true;
    }

    void halt() override {
        ensure_connected();
        protocol_.halt();
    }

    void move(double position) override {
        // IRotator.Move: relative offset from the current Position.
        ensure_connected();
        validate_angle(position);
        const double current = get_position();
        start_move_to(normalize_angle(current + position));
    }

    void move_absolute(double position) override {
        ensure_connected();
        validate_angle(position);
        start_move_to(normalize_angle(position));
    }

    void move_mechanical(double position) override {
        ensure_connected();
        validate_angle(position);
        const double mechanical_target = normalize_angle(position);
        const double mechanical_current = protocol_.get_state().mechanical_angle;
        protocol_.move_relative(shortest_delta(mechanical_current, mechanical_target));
        std::lock_guard<std::mutex> lock(state_mutex_);
        target_position_ = normalize_angle(mechanical_target + sync_offset_);
        has_target_position_ = true;
    }

    void sync(double position) override {
        // IRotatorV4 Sync is an explicit driver-side offset between Position and
        // MechanicalPosition — no hardware command is involved (the device-level
        // "set zero" would destroy the mechanical coordinate instead).
        ensure_connected();
        validate_angle(position);
        const double mechanical = protocol_.get_state().mechanical_angle;
        const double target = normalize_angle(position);
        std::lock_guard<std::mutex> lock(state_mutex_);
        sync_offset_ = normalize_angle(target - mechanical);
        target_position_ = target;
        has_target_position_ = true;
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Rotator not connected", AlpacaError::NotConnected);
        }
    }

    static void validate_angle(double position) {
        if (!std::isfinite(position)) {
            throw AlpacaException("Rotator position is not finite", AlpacaError::InvalidValue);
        }
    }

    static double normalize_angle(double angle) {
        double normalized = std::fmod(angle, 360.0);
        if (normalized < 0.0) {
            normalized += 360.0;
        }
        return normalized;
    }

    // Signed shortest rotation from one normalized angle to another, in
    // (-180, 180] degrees — keeps every commanded sweep at most half a turn.
    static double shortest_delta(double from, double to) {
        double delta = std::fmod(to - from, 360.0);
        if (delta > 180.0) {
            delta -= 360.0;
        } else if (delta <= -180.0) {
            delta += 360.0;
        }
        return delta;
    }

    // Issue the relative move that takes Position to logical_target and record
    // the target for GET targetposition.
    void start_move_to(double logical_target) {
        const double current = get_position();
        protocol_.move_relative(shortest_delta(current, logical_target));
        std::lock_guard<std::mutex> lock(state_mutex_);
        target_position_ = logical_target;
        has_target_position_ = true;
    }

    int device_number_;
    RotatorConnectionConfig config_;
    std::atomic<bool> connected_;
    WandererRotatorProtocolWrapper protocol_;

    mutable std::mutex state_mutex_;
    double sync_offset_ = 0.0;
    double target_position_ = 0.0;
    bool has_target_position_ = false;

    std::mutex transition_mutex_;  // serializes set_connected() connect/disconnect transitions
};

std::unique_ptr<RotatorDriver> create_wandererastro_rotator(int device_number, const std::string& serial_port,
                                                            int baud_rate) {
    RotatorConnectionConfig config;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<WandererRotatorDriver>(device_number, std::move(config));
}

std::unique_ptr<RotatorDriver> create_wandererastro_rotator_by_index(int device_number, int rotator_index) {
    // Defer the (blocking) port scan to connect time, which runs on the driver's
    // background connection thread — never on the HTTP registration thread.
    RotatorConnectionConfig config;
    config.auto_detect_index = rotator_index;
    return std::make_unique<WandererRotatorDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::wandererastro
