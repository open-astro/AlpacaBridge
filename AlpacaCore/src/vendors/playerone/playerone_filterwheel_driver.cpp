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
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>
#include <alpacacore/vendor/playerone/playerone_pw_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::playerone {

class PlayerOnePWFilterWheelDriver : public FilterWheelDriver {
public:
    PlayerOnePWFilterWheelDriver(int device_number, int wheel_index)
        : device_number_(device_number),
          wheel_index_(wheel_index),
          handle_(-1),
          wheel_info_(),
          wheel_info_valid_(false),
          filter_names_(),
          focus_offsets_(),
          connected_(false),
          connecting_(false) {}

    ~PlayerOnePWFilterWheelDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                PlayerOnePWFilterWheelDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Error during PW destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && !wheel_info_.name.empty()) {
            return wheel_info_.name;
        }
        return "Player One Phoenix Wheel";
    }

    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && !wheel_info_.serial_number.empty()) {
            return "PlayerOne_PW_SN_" + wheel_info_.serial_number;
        }
        return "PlayerOne_PW_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "Player One Phoenix Filter Wheel Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore Player One Phoenix Filter Wheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = PlayerOnePWSDKWrapper::instance();
        if (connected) {
            int handle = resolve_handle_locked();
            sdk.open_wheel(handle);
            try {
                handle_ = handle;
                seed_slot_data_locked();
            } catch (...) {
                handle_ = -1;
                sdk.close_wheel(handle);
                throw;
            }
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws (e.g.
        // device unplugged) the error still surfaces, but the driver must not
        // stay half-connected.
        const int close_handle = handle_;
        handle_ = -1;
        wheel_info_ = {};
        wheel_info_valid_ = false;
        connected_.store(false);
        if (close_handle >= 0) {
            sdk.close_wheel(close_handle);
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

    int get_position() const override {
        ensure_connected();
        // Hold mutex_ across the SDK call: set_connected(false) closes the wheel
        // under mutex_, so serialising here prevents a concurrent disconnect from
        // invalidating handle_ mid-call (matches the ToupTek/thermal pattern).
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ < 0) {
            throw AlpacaException("Filter wheel handle not set", AlpacaError::NotConnected);
        }
        return PlayerOnePWSDKWrapper::instance().get_position(handle_);
    }

    void set_position(int position) override {
        ensure_connected();
        // Hold mutex_ across validation and the SDK move for the same
        // use-after-close reason as get_position.
        std::lock_guard<std::mutex> lock(mutex_);
        // Check the disconnect sentinel first so a concurrent disconnect yields
        // NotConnected, not a generic DriverException (matches get_position).
        if (handle_ < 0) {
            throw AlpacaException("Filter wheel handle not set", AlpacaError::NotConnected);
        }
        if (!wheel_info_valid_ || wheel_info_.position_count <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (position < 0 || position >= wheel_info_.position_count) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        PlayerOnePWSDKWrapper::instance().goto_position(handle_, position);
    }

    std::vector<int> get_focus_offsets() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return focus_offsets_;
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && wheel_info_.position_count > 0) {
            validate_slot_count_locked(static_cast<int>(offsets.size()), "focusOffsets");
        }
        focus_offsets_ = offsets;
    }

    std::vector<std::string> get_names() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return filter_names_;
    }

    void set_names(const std::vector<std::string>& names) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && wheel_info_.position_count > 0) {
            validate_slot_count_locked(static_cast<int>(names.size()), "names");
        }
        filter_names_ = names;
        apply_default_names_locked();
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
    }

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connecting_.load()) {
            return;
        }
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
        connecting_.store(true);
        connection_thread_ = std::thread([this, connect]() {
            try {
                set_connected(connect);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR("PlayerOne", "PW connection failed: " + std::string(e.what()));
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

    int resolve_handle_locked() {
        auto wheels = PlayerOnePWSDKWrapper::instance().enumerate_wheels();
        if (wheels.empty()) {
            ALPACA_LOG_WARN("PlayerOne", "No Player One filter wheels detected by SDK");
            throw AlpacaException("No Player One filter wheels detected", AlpacaError::NotConnected);
        }
        if (wheel_index_ < 0 || wheel_index_ >= static_cast<int>(wheels.size())) {
            ALPACA_LOG_WARN("PlayerOne", "Filter wheel index out of range: " + std::to_string(wheel_index_) +
                                             " (count=" + std::to_string(wheels.size()) + ")");
            throw AlpacaException("Filter wheel index not found", AlpacaError::InvalidValue);
        }
        const auto& info = wheels[static_cast<std::size_t>(wheel_index_)];
        wheel_info_ = info;
        wheel_info_valid_ = true;
        return info.handle;
    }

    // The Phoenix Wheel stores per-slot filter aliases and focus offsets in the
    // wheel itself. Use them as defaults at connect; names/offsets supplied via
    // config (set_names/set_focus_offsets) take precedence.
    void seed_slot_data_locked() {
        if (!wheel_info_valid_ || wheel_info_.position_count <= 0) {
            throw AlpacaException("Filter wheel reported no filter positions", AlpacaError::DriverException);
        }
        const std::size_t slots = static_cast<std::size_t>(wheel_info_.position_count);
        auto& sdk = PlayerOnePWSDKWrapper::instance();

        if (filter_names_.empty()) {
            filter_names_.assign(slots, std::string());
            for (std::size_t i = 0; i < slots; ++i) {
                try {
                    filter_names_[i] = sdk.get_filter_alias(handle_, static_cast<int>(i));
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("PlayerOne", "PW filter alias unavailable for slot " + std::to_string(i) + ": " +
                                                     std::string(e.what()));
                }
            }
        } else if (filter_names_.size() != slots) {
            ALPACA_LOG_WARN("PlayerOne", "Configured filterNames count (" + std::to_string(filter_names_.size()) +
                                             ") does not match wheel position count (" + std::to_string(slots) +
                                             "); resizing to match the wheel");
            filter_names_.resize(slots);
        }
        apply_default_names_locked();

        if (focus_offsets_.empty()) {
            focus_offsets_.assign(slots, 0);
            for (std::size_t i = 0; i < slots; ++i) {
                try {
                    focus_offsets_[i] = sdk.get_focus_offset(handle_, static_cast<int>(i));
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("PlayerOne", "PW focus offset unavailable for slot " + std::to_string(i) + ": " +
                                                     std::string(e.what()));
                }
            }
        } else if (focus_offsets_.size() != slots) {
            ALPACA_LOG_WARN("PlayerOne", "Configured focusOffsets count (" + std::to_string(focus_offsets_.size()) +
                                             ") does not match wheel position count (" + std::to_string(slots) +
                                             "); resizing to match the wheel");
            focus_offsets_.resize(slots);
        }
    }

    void apply_default_names_locked() {
        for (std::size_t i = 0; i < filter_names_.size(); ++i) {
            if (filter_names_[i].empty()) {
                filter_names_[i] = "Filter " + std::to_string(i + 1);
            }
        }
    }

    void validate_slot_count_locked(int provided_size, std::string_view field_name) const {
        if (provided_size != wheel_info_.position_count) {
            throw AlpacaException("Invalid " + std::string(field_name) + " length: expected " +
                                      std::to_string(wheel_info_.position_count),
                                  AlpacaError::InvalidValue);
        }
    }

    int device_number_;
    int wheel_index_;
    int handle_;
    PlayerOnePWInfo wheel_info_;
    bool wheel_info_valid_;
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;
    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<FilterWheelDriver> create_playerone_filterwheel(int device_number, int wheel_index) {
    return std::make_unique<PlayerOnePWFilterWheelDriver>(device_number, wheel_index);
}

}  // namespace alpacacore::vendor::playerone
