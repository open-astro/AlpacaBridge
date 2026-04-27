// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::touptek {

class ToupTekFilterWheelDriver : public FilterWheelDriver {
public:
    ToupTekFilterWheelDriver(int device_number, int camera_index)
        : device_number_(device_number)
        , camera_index_(camera_index)
        , handle_(nullptr)
        , camera_name_()
        , serial_number_()
        , slot_count_(0)
        , filter_names_()
        , focus_offsets_()
        , connected_(false)
        , connecting_(false)
    {
        preload_camera_info();
    }

    ~ToupTekFilterWheelDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "Error during FW destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_name_.empty()) {
            return camera_name_ + " FilterWheel";
        }
        return "ToupTek FilterWheel";
    }

    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "TOUPTEK_FW_SN_" + serial_number_;
        }
        return "TOUPTEK_FW_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ToupTek Filter Wheel Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ToupTek Filter Wheel Driver";
    }

    std::string get_driver_version() const override { return "1.0.0"; }

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

        auto& sdk = ToupTekSDKWrapper::instance();

        if (connected) {
            // Enumerate and resolve the target camera.
            auto cameras = sdk.enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No ToupTek cameras detected", AlpacaError::NotConnected);
            }
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range", AlpacaError::InvalidValue);
            }

            const auto& cinfo = cameras[static_cast<std::size_t>(camera_index_)];
            if (!cinfo.supports_filterwheel) {
                throw AlpacaException("Camera does not have a filter wheel: " + cinfo.name,
                                      AlpacaError::NotImplemented);
            }

            camera_name_ = cinfo.name;
            serial_number_.clear();

            ALPACA_LOG_INFO("ToupTek", "Opening camera for filter wheel access: " + cinfo.name);

            handle_ = sdk.open_camera_by_id(cinfo.id);

            try {
                int slots = sdk.get_filterwheel_slot_count(handle_);
                if (slots <= 0) {
                    sdk.close_camera(handle_);
                    handle_ = nullptr;
                    throw AlpacaException("Filter wheel has no slots", AlpacaError::DriverException);
                }
                slot_count_ = slots;
            } catch (const AlpacaException&) {
                sdk.close_camera(handle_);
                handle_ = nullptr;
                throw;
            } catch (const std::exception& e) {
                sdk.close_camera(handle_);
                handle_ = nullptr;
                throw AlpacaException("Failed to query filter wheel slots: " + std::string(e.what()),
                                      AlpacaError::DriverException);
            }

            serial_number_ = sdk.get_serial_number(handle_);

            // Initialize filter names and focus offsets to match slot count.
            filter_names_.assign(static_cast<std::size_t>(slot_count_), std::string());
            focus_offsets_.assign(static_cast<std::size_t>(slot_count_), 0);
            apply_default_names_locked();

            connected_.store(true);
            return;
        }

        // Disconnecting.
        if (handle_) {
            sdk.close_camera(handle_);
            handle_ = nullptr;
        }
        camera_name_.clear();
        serial_number_.clear();
        slot_count_ = 0;
        filter_names_.clear();
        focus_offsets_.clear();
        connected_.store(false);
    }

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        if (!connected_.load()) {
            return state;
        }
        try {
            state.push_back({"Position", get_position()});
        } catch (const std::exception&) {
        }
        return state;
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name),
                              AlpacaError::ActionNotImplemented);
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
        auto& sdk = ToupTekSDKWrapper::instance();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!handle_) {
            throw AlpacaException("Filter wheel disconnected", AlpacaError::NotConnected);
        }
        int pos = sdk.get_filterwheel_position(handle_);
        if (pos < 0) {
            // Filter wheel is in motion; return current known position or 0.
            throw AlpacaException("Filter wheel is moving", AlpacaError::InvalidOperation);
        }
        return pos;
    }

    void set_position(int position) override {
        ensure_connected();
        if (position < 0 || position >= slot_count_locked()) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        auto& sdk = ToupTekSDKWrapper::instance();
        // Issue the move command while holding the mutex so disconnect() cannot
        // close the handle concurrently.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!handle_) {
                throw AlpacaException("Filter wheel disconnected", AlpacaError::NotConnected);
            }
            sdk.set_filterwheel_position(handle_, position, 1); // auto direction spinning
        }

        // The filter wheel moves asynchronously. Wait in a spin-loop for
        // up to 30 seconds for the move to complete (get_position returns >= 0).
        const int max_wait_ms = 30000;
        const int poll_ms = 100;
        int waited = 0;
        while (waited < max_wait_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
            waited += poll_ms;
            int pos = -1;
            bool handle_valid = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (handle_) {
                    pos = sdk.get_filterwheel_position(handle_);
                    handle_valid = true;
                }
            }
            if (!handle_valid) {
                throw AlpacaException("Filter wheel disconnected during move",
                                      AlpacaError::NotConnected);
            }
            if (pos >= 0) {
                if (pos != position) {
                    throw AlpacaException("Filter wheel reached wrong position",
                                          AlpacaError::DriverException);
                }
                return;
            }
            // pos < 0 means still in motion — continue polling.
        }
        // Timeout reached - stop the wheel to prevent endless spinning
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_) {
                sdk.reset_filterwheel(handle_);
            }
        }
        throw AlpacaException("Filter wheel move timed out", AlpacaError::DriverException);
    }

    std::vector<int> get_focus_offsets() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return focus_offsets_;
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only validate slot count when connected; when disconnected,
        // just store the values for later use on connect.
        if (slot_count_ > 0) {
            validate_slot_count_locked(static_cast<int>(offsets.size()));
        }
        focus_offsets_ = offsets;
    }

    std::vector<std::string> get_names() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return filter_names_;
    }

    void set_names(const std::vector<std::string>& names) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only validate slot count when connected; when disconnected,
        // just store the values for later use on connect.
        if (slot_count_ > 0) {
            validate_slot_count_locked(static_cast<int>(names.size()));
        }
        filter_names_ = names;
        apply_default_names_locked();
    }

private:
    int device_number_;
    int camera_index_;
    HToupcam handle_;
    std::string camera_name_;
    std::string serial_number_;
    int slot_count_;
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;

    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
    }

    int slot_count_locked() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_count_ <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        return slot_count_;
    }

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        // If a connection transition is already in flight, wait for it to
        // complete before starting the new one so that state changes are
        // never silently dropped.
        if (connecting_.load()) {
            if (connection_thread_.joinable()) {
                connection_thread_.join();
            }
        }
        connecting_.store(true);
        connection_thread_ = std::thread([this, connect]() {
            try {
                set_connected(connect);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR("ToupTek", "FW connection failed: " + std::string(e.what()));
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

    void preload_camera_info() {
        try {
            auto cameras = ToupTekSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                const auto& ci = cameras[static_cast<std::size_t>(camera_index_)];
                if (ci.supports_filterwheel) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    camera_name_ = ci.name;
                }
            }
        } catch (const std::exception&) {
        }
    }

    void apply_default_names_locked() {
        for (std::size_t i = 0; i < filter_names_.size(); ++i) {
            if (filter_names_[i].empty()) {
                filter_names_[i] = "Filter " + std::to_string(i + 1);
            }
        }
    }

    void validate_slot_count_locked(int provided_size) const {
        if (slot_count_ <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (provided_size != slot_count_) {
            throw AlpacaException("Invalid size: expected " + std::to_string(slot_count_) +
                                  " elements, got " + std::to_string(provided_size),
                                  AlpacaError::InvalidValue);
        }
    }
};

std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel(int device_number, int camera_index) {
    return std::make_unique<ToupTekFilterWheelDriver>(device_number, camera_index);
}

} // namespace alpacacore::vendor::touptek
