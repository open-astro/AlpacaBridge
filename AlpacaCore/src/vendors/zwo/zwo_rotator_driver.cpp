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
#include <alpacacore/vendor/zwo/zwo_caa_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>

namespace alpacacore::vendor::zwo {

class ZWOCAARotatorDriver : public RotatorDriver {
public:
    ZWOCAARotatorDriver(int device_number, std::optional<int> rotator_id, std::optional<int> rotator_index)
        : device_number_(device_number)
        , rotator_id_(rotator_id)
        , rotator_index_(rotator_index)
        , serial_number_()
        , rotator_type_()
        , rotator_info_()
        , rotator_info_valid_(false)
        , min_degree_(0.0)
        , max_degree_(360.0)
        , target_position_(0.0)
        , has_target_position_(false)
        , sync_offset_(0.0)
        , connected_(false)
        , connecting_(false)
    {
    }

    ~ZWOCAARotatorDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during CAA destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rotator_info_valid_ && !rotator_info_.name.empty()) {
            return rotator_info_.name;
        }
        return "ZWO CAA";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Rotator;
    }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "ZWO_CAA_SN_" + serial_number_;
        }
        if (rotator_id_.has_value()) {
            return "ZWO_CAA_ID_" + std::to_string(rotator_id_.value());
        }
        return "ZWO_CAA_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO CAA Rotator Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO CAA Rotator Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override {
        return connected_.load();
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override {
        return connecting_.load();
    }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = ZWOCAASDKWrapper::instance();
        if (connected) {
            int resolved_id = resolve_rotator_id_locked();
            sdk.open_rotator(resolved_id);
            try {
                refresh_rotator_info_locked(resolved_id);
                refresh_limits_locked(resolved_id);
                try {
                    serial_number_ = sdk.get_serial_number(resolved_id);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ZWO", "CAA serial number unavailable: " + std::string(e.what()));
                    serial_number_.clear();
                }
                try {
                    rotator_type_ = sdk.get_rotator_type(resolved_id);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ZWO", "CAA type unavailable: " + std::string(e.what()));
                    rotator_type_.clear();
                }
                sync_offset_ = 0.0;
                target_position_ = 0.0;
                has_target_position_ = false;
            } catch (...) {
                sdk.close_rotator(resolved_id);
                throw;
            }
            connected_.store(true);
            return;
        }

        if (rotator_id_.has_value()) {
            sdk.close_rotator(rotator_id_.value());
        }
        if (rotator_index_.has_value()) {
            rotator_id_.reset();
            rotator_info_ = {};
            rotator_info_valid_ = false;
            serial_number_.clear();
            rotator_type_.clear();
        }
        connected_.store(false);
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

    bool get_can_reverse() const override {
        return true;
    }

    bool get_reverse() const override {
        ensure_connected();
        return ZWOCAASDKWrapper::instance().get_reverse(rotator_id_value());
    }

    void set_reverse(bool reverse) override {
        ensure_connected();
        ZWOCAASDKWrapper::instance().set_reverse(rotator_id_value(), reverse);
    }

    bool get_is_moving() const override {
        ensure_connected();
        return ZWOCAASDKWrapper::instance().get_motion_status(rotator_id_value()).is_moving;
    }

    double get_mechanical_position() const override {
        ensure_connected();
        double degree = ZWOCAASDKWrapper::instance().get_degree(rotator_id_value());
        // CAAGetDegree() returns the logical (reverse-applied) angle. When Reverse is
        // enabled, the hardware inverts the angle: logical = 360 - physical. We must
        // un-apply that inversion to return the true mechanical (physical) position.
        if (ZWOCAASDKWrapper::instance().get_reverse(rotator_id_value())) {
            return normalize_angle(360.0 - degree);
        }
        return normalize_angle(degree);
    }

    double get_position() const override {
        ensure_connected();
        double mechanical = ZWOCAASDKWrapper::instance().get_degree(rotator_id_value());
        return normalize_angle(mechanical + sync_offset_);
    }

    double get_step_size() const override {
        return 0.0;
    }

    double get_target_position() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return has_target_position_ ? target_position_ : 0.0;
    }

    void set_target_position(double position) override {
        ensure_connected();
        validate_angle(position);
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = normalize_angle(position);
        has_target_position_ = true;
    }

    void halt() override {
        ensure_connected();
        ZWOCAASDKWrapper::instance().stop(rotator_id_value());
    }

    void move(double position) override {
        ensure_connected();
        validate_angle(position);
        double current = get_position();
        double target = normalize_angle(current + position);
        double mechanical_target = to_mechanical_angle(target);
        ZWOCAASDKWrapper::instance().move_absolute(rotator_id_value(), mechanical_target);
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = target;
        has_target_position_ = true;
    }

    void move_absolute(double position) override {
        ensure_connected();
        validate_angle(position);
        double target = normalize_angle(position);
        double mechanical_target = to_mechanical_angle(target);
        ZWOCAASDKWrapper::instance().move_absolute(rotator_id_value(), mechanical_target);
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = target;
        has_target_position_ = true;
    }

    void move_mechanical(double position) override {
        ensure_connected();
        validate_angle(position);
        double mechanical_target = normalize_angle(position);
        ZWOCAASDKWrapper::instance().move_mechanical(rotator_id_value(), mechanical_target);
        // CAAGetDegree() returns the logical (reverse-applied) angle. When Reverse is
        // enabled, the logical angle for a given mechanical angle is 360 - mechanical.
        bool is_reversed = ZWOCAASDKWrapper::instance().get_reverse(rotator_id_value());
        std::lock_guard<std::mutex> lock(mutex_);
        double logical = is_reversed ? normalize_angle(360.0 - mechanical_target) : mechanical_target;
        target_position_ = normalize_angle(logical + sync_offset_);
        has_target_position_ = true;
    }

    void sync(double position) override {
        ensure_connected();
        validate_angle(position);
        double mechanical = ZWOCAASDKWrapper::instance().get_degree(rotator_id_value());
        double target = normalize_angle(position);
        sync_offset_ = normalize_angle(target - mechanical);
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = target;
        has_target_position_ = true;
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Rotator not connected", AlpacaError::NotConnected);
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
                ALPACA_LOG_ERROR("ZWO", "CAA connection failed: " + std::string(e.what()));
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

    int resolve_rotator_id_locked() {
        if (rotator_index_.has_value()) {
            auto rotators = ZWOCAASDKWrapper::instance().enumerate_rotators();
            if (rotators.empty()) {
                ALPACA_LOG_WARN("ZWO", "No ZWO CAA rotators detected by SDK");
                throw AlpacaException("No ZWO CAA rotators detected", AlpacaError::NotConnected);
            }
            int index = rotator_index_.value();
            if (index < 0 || index >= static_cast<int>(rotators.size())) {
                ALPACA_LOG_WARN("ZWO", "CAA rotator index out of range: " + std::to_string(index) +
                                         " (count=" + std::to_string(rotators.size()) + ")");
                throw AlpacaException("Rotator index not found", AlpacaError::InvalidValue);
            }
            const auto& info = rotators[static_cast<std::size_t>(index)];
            rotator_id_ = info.rotator_id;
            rotator_info_ = info;
            rotator_info_valid_ = true;
            return rotator_id_.value();
        }

        if (rotator_id_.has_value()) {
            return rotator_id_.value();
        }

        throw AlpacaException("Rotator ID not specified", AlpacaError::InvalidValue);
    }

    void refresh_rotator_info_locked(int rotator_id) {
        ZWOCAARotatorInfo info;
        if (ZWOCAASDKWrapper::instance().get_rotator_info_by_id(rotator_id, info)) {
            rotator_info_ = info;
            rotator_info_valid_ = true;
            if (info.max_degree > 0.0) {
                max_degree_ = info.max_degree;
            }
            return;
        }
        throw AlpacaException("Failed to read rotator info", AlpacaError::DriverException);
    }

    void refresh_limits_locked(int rotator_id) {
        min_degree_ = 0.0;
        try {
            double max_degree = ZWOCAASDKWrapper::instance().get_max_degree(rotator_id);
            if (max_degree > 0.0) {
                max_degree_ = max_degree;
            }
        } catch (const std::exception&) {
            if (!rotator_info_valid_ || rotator_info_.max_degree <= 0.0) {
                max_degree_ = 360.0;
            }
        }
    }

    void validate_angle(double position) const {
        if (!std::isfinite(position)) {
            throw AlpacaException("Rotator position is not finite", AlpacaError::InvalidValue);
        }
    }

    double normalize_angle(double angle) const {
        double wrap = max_degree_ > 0.0 ? max_degree_ : 360.0;
        if (wrap <= 0.0) {
            return angle;
        }
        double normalized = std::fmod(angle, wrap);
        if (normalized < 0.0) {
            normalized += wrap;
        }
        return normalized;
    }

    double to_mechanical_angle(double logical_angle) const {
        return normalize_angle(logical_angle - sync_offset_);
    }

    int rotator_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!rotator_id_.has_value()) {
            throw AlpacaException("Rotator ID not set", AlpacaError::NotConnected);
        }
        return rotator_id_.value();
    }

    int device_number_;
    std::optional<int> rotator_id_;
    std::optional<int> rotator_index_;
    std::string serial_number_;
    std::string rotator_type_;
    ZWOCAARotatorInfo rotator_info_;
    bool rotator_info_valid_;
    double min_degree_;
    double max_degree_;
    double target_position_;
    bool has_target_position_;
    double sync_offset_;
    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator(int device_number, int rotator_id) {
    return std::make_unique<ZWOCAARotatorDriver>(device_number, rotator_id, std::nullopt);
}

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator_by_index(int device_number, int rotator_index) {
    return std::make_unique<ZWOCAARotatorDriver>(device_number, std::nullopt, rotator_index);
}

} // namespace alpacacore::vendor::zwo
