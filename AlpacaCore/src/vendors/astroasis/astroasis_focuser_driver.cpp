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
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <string>

namespace alpacacore::vendor::astroasis {

class AstroasisFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    AstroasisFocuserDriver(int device_number, std::string hid_path)
        : AsyncConnectable("Astroasis"),
          device_number_(device_number),
          hid_path_(std::move(hid_path)),
          connected_(false),
          protocol_() {}

    ~AstroasisFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Astroasis", "Error during focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "Astroasis Oasis Focuser"; }

    DeviceType get_device_type() const override { return DeviceType::Focuser; }

    std::string get_unique_id() const override { return "ASTROASIS_FOCUSER_" + std::to_string(device_number_); }

    std::string get_description() const override { return "Astroasis Oasis Focuser Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore Astroasis Focuser Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously — closing the HID handle is trivial and
        // ASCOM clients expect Connected to be false immediately after.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Astroasis", "Focuser disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // See AsyncConnectable's class comment for why these gates run before
        // the idempotency check.
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
            protocol_.connect(hid_path_);
            connected_.store(true);
            ALPACA_LOG_INFO("Astroasis", "Focuser connected");
        } else {
            protocol_.disconnect();
            connected_.store(false);
            ALPACA_LOG_INFO("Astroasis", "Focuser disconnected");
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

    // --- Focuser interface ---

    bool get_absolute() const override { return true; }

    bool get_is_moving() const override {
        ensure_connected();
        return const_cast<AstroasisFocuserDriver*>(this)->protocol_.get_status().moving;
    }

    int get_max_step() const override {
        ensure_connected();
        return const_cast<AstroasisFocuserDriver*>(this)->protocol_.get_max_step();
    }

    int get_max_increment() const override {
        ensure_connected();
        return get_max_step();
    }

    int get_position() const override {
        ensure_connected();
        return const_cast<AstroasisFocuserDriver*>(this)->protocol_.get_status().position;
    }

    double get_step_size() const override {
        ensure_connected();
        // TODO: the vendor protocol does not expose step size in microns
        // (mechanical, varies by focuser model).
        throw AlpacaException("Step size not available for this focuser", AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override { return false; }

    bool get_temp_comp() const override { return false; }

    void set_temp_comp(bool) override {
        ensure_connected();
        throw AlpacaException("Temperature compensation not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_temperature() const override {
        ensure_connected();
        auto status = const_cast<AstroasisFocuserDriver*>(this)->protocol_.get_status();
        if (status.temperature_external_valid) {
            return status.temperature_external;
        }
        return status.temperature_internal;
    }

    void halt() override {
        ensure_connected();
        protocol_.stop_move();
    }

    void move(int position) override {
        ensure_connected();
        int max_step = protocol_.get_max_step();
        // ConformU requires graceful clamping, not exceptions.
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

    int device_number_;
    std::string hid_path_;
    std::atomic<bool> connected_;
    AstroasisProtocolWrapper protocol_;
};

std::unique_ptr<FocuserDriver> create_astroasis_focuser(int device_number, const std::string& hid_path) {
    return std::make_unique<AstroasisFocuserDriver>(device_number, hid_path);
}

std::unique_ptr<FocuserDriver> create_astroasis_focuser_by_index(int device_number, int focuser_index) {
    auto ports = enumerate_astroasis_focusers();
    if (ports.empty()) {
        throw AlpacaException("No Astroasis Oasis Focuser detected on the USB bus", AlpacaError::NotConnected);
    }
    if (focuser_index < 0 || focuser_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Focuser index " + std::to_string(focuser_index) + " out of range (detected " +
                                  std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }

    const auto& port = ports[static_cast<std::size_t>(focuser_index)];
    ALPACA_LOG_INFO("Astroasis", "Auto-detected focuser at " + port.hid_path);

    return std::make_unique<AstroasisFocuserDriver>(device_number, port.hid_path);
}

}  // namespace alpacacore::vendor::astroasis
