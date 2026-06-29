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
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::gemini {

class GeminiFocuserDriver : public FocuserDriver {
public:
    GeminiFocuserDriver(int device_number, ConnectionConfig config)
        : device_number_(device_number)
        , config_(std::move(config))
        , connected_(false)
        , connecting_(false)
        , protocol_()
    {
    }

    ~GeminiFocuserDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Error during focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        return "Gemini Automatic Astro Focuser Pro";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Focuser;
    }

    std::string get_unique_id() const override {
        return "GEMINI_FOCUSER_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "Gemini Automatic Astro Focuser Pro Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore Gemini Focuser Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Focuser firmware captured at connect; surfaced in the web UI only.
    std::optional<std::string> get_device_firmware() const override {
        if (!connected_.load()) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override {
        return connected_.load();
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        // Disconnect synchronously — the operation is trivial (close fd + set flag)
        // and ASCOM clients expect Connected to be false immediately after.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Gemini", "Focuser disconnect error: " + std::string(e.what()));
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
            // Cache the focuser firmware once (web UI only — never DriverInfo)
            // BEFORE marking connected, so a concurrent get_device_firmware()
            // can never observe connected==true with an empty cache (which would
            // be indistinguishable from "no firmware"). A failed query must not
            // fail the connect.
            try {
                int fw = protocol_.get_firmware_version();
                std::lock_guard<std::mutex> lock(connection_mutex_);
                firmware_ = fw > 0 ? std::to_string(fw) : std::string();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Focuser firmware query failed: " + std::string(e.what()));
                std::lock_guard<std::mutex> lock(connection_mutex_);
                firmware_.clear();
            }
            connected_.store(true);
            ALPACA_LOG_INFO("Gemini", "Focuser connected");
        } else {
            protocol_.disconnect();
            connected_.store(false);
            {
                std::lock_guard<std::mutex> lock(connection_mutex_);
                firmware_.clear();
            }
            ALPACA_LOG_INFO("Gemini", "Focuser disconnected");
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

    // --- Focuser interface ---

    bool get_absolute() const override {
        return true;
    }

    bool get_is_moving() const override {
        ensure_connected();
        return const_cast<GeminiFocuserDriver*>(this)->protocol_.is_moving();
    }

    int get_max_step() const override {
        ensure_connected();
        return const_cast<GeminiFocuserDriver*>(this)->protocol_.get_max_position();
    }

    int get_max_increment() const override {
        ensure_connected();
        return get_max_step();
    }

    int get_position() const override {
        ensure_connected();
        return const_cast<GeminiFocuserDriver*>(this)->protocol_.get_position();
    }

    double get_step_size() const override {
        ensure_connected();
        // TODO: The MyFocuserPro2 protocol does not expose step size in microns.
        // This may vary by mechanical configuration. Return a reasonable default
        // or throw if the focuser cannot report this.
        throw AlpacaException("Step size not available for this focuser",
                              AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        return true;
    }

    bool get_temp_comp() const override {
        if (!connected_.load()) {
            return false;
        }
        try {
            return const_cast<GeminiFocuserDriver*>(this)->protocol_.get_temp_comp_enabled();
        } catch (const std::exception&) {
            return false;
        }
    }

    void set_temp_comp(bool temp_comp) override {
        ensure_connected();
        protocol_.set_temp_comp_enabled(temp_comp);
    }

    double get_temperature() const override {
        ensure_connected();
        return const_cast<GeminiFocuserDriver*>(this)->protocol_.get_temperature();
    }

    void halt() override {
        ensure_connected();
        protocol_.halt();
    }

    void move(int position) override {
        ensure_connected();
        int max_step = protocol_.get_max_position();
        // ConformU requires graceful clamping, not exceptions
        if (position < 0) {
            position = 0;
        } else if (position > max_step) {
            position = max_step;
        }
        protocol_.move_to(position);
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
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
                ALPACA_LOG_ERROR("Gemini", "Focuser connection failed: " + std::string(e.what()));
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
    GeminiProtocolWrapper protocol_;
    mutable std::mutex connection_mutex_;
    std::thread connection_thread_;
    std::string firmware_;  // captured at connect; web-UI only (guarded by connection_mutex_)
};

std::unique_ptr<FocuserDriver> create_gemini_focuser(int device_number,
                                                      const std::string& serial_port,
                                                      int baud_rate) {
    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<GeminiFocuserDriver>(device_number, std::move(config));
}

std::unique_ptr<FocuserDriver> create_gemini_focuser_by_index(int device_number,
                                                               int focuser_index) {
    auto ports = enumerate_gemini_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("Gemini/MyFocuserPro2 focuser"),
                              AlpacaError::NotConnected);
    }
    if (focuser_index < 0 || focuser_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Focuser index " + std::to_string(focuser_index) +
                              " out of range (detected " + std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }

    const auto& port = ports[static_cast<std::size_t>(focuser_index)];
    ALPACA_LOG_INFO("Gemini", "Auto-detected focuser at " + port.port_path +
                    " (firmware " + std::to_string(port.firmware_version) + ")");

    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.serial_port = port.port_path;
    return std::make_unique<GeminiFocuserDriver>(device_number, std::move(config));
}

} // namespace alpacacore::vendor::gemini
