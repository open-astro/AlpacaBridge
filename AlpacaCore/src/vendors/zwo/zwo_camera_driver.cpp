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

#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace alpacacore::vendor::zwo {

namespace {

std::pair<int, int> bayer_offsets(ZWOBayerPattern pattern) {
    switch (pattern) {
    case ZWOBayerPattern::RG:
        return {0, 0};
    case ZWOBayerPattern::BG:
        return {1, 1};
    case ZWOBayerPattern::GR:
        return {1, 0};
    case ZWOBayerPattern::GB:
        return {0, 1};
    default:
        return {0, 0};
    }
}

bool supports_format(const std::vector<ZWOImageType>& formats, ZWOImageType type) {
    return std::find(formats.begin(), formats.end(), type) != formats.end();
}

bool supports_bin(const std::vector<int>& bins, int bin) {
    return std::find(bins.begin(), bins.end(), bin) != bins.end();
}

} // namespace

class ZWOCameraDriver : public CameraDriver {
public:
    ZWOCameraDriver(int device_number, std::optional<int> camera_id, std::optional<int> camera_index)
        : device_number_(device_number)
        , camera_id_(camera_id)
        , camera_index_(camera_index)
        , serial_number_()
        , camera_info_()
        , camera_info_valid_(false)
        , control_caps_()
        , connected_(false)
        , connecting_(false)
        , image_type_(ZWOImageType::Raw8)
        , bin_x_(1)
        , bin_y_(1)
        , num_x_(0)
        , num_y_(0)
        , start_x_(0)
        , start_y_(0)
        , image_ready_(false)
        , image_cached_(false)
        , last_image_()
        , last_exposure_duration_(0.0)
        , last_exposure_start_()
        , pulse_guiding_(false)
        , pulse_guiding_end_(std::chrono::steady_clock::time_point{})
    {
    }

    ~ZWOCameraDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.name.empty()) {
            return camera_info_.name;
        }
        return "ZWO Camera";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Camera;
    }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "ZWO_SN_" + serial_number_;
        }
        return "ZWO_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO ASI Camera Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO Camera Driver";
    }

    std::string get_driver_version() const override {
        return "1.0.0";
    }

    int get_interface_version() const override {
        return 3;
    }

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

        auto& sdk = ZWOSDKWrapper::instance();

        if (connected) {
            int resolved_id = resolve_camera_id_locked();
            sdk.open_camera(resolved_id);
            sdk.init_camera(resolved_id);

            refresh_camera_info_locked(resolved_id);
            load_control_caps_locked(resolved_id);
            select_default_image_type_locked();

            bin_x_ = 1;
            bin_y_ = 1;
            start_x_ = 0;
            start_y_ = 0;
            num_x_ = camera_info_valid_ ? camera_info_.max_width : 0;
            num_y_ = camera_info_valid_ ? camera_info_.max_height : 0;

            if (num_x_ > 0 && num_y_ > 0) {
                sdk.set_roi_format(resolved_id, num_x_, num_y_, bin_x_, image_type_);
                sdk.set_start_pos(resolved_id, start_x_, start_y_);
            }

            serial_number_ = sdk.get_serial_number(resolved_id);
            image_ready_ = false;
            image_cached_ = false;
            last_exposure_duration_ = 0.0;
            last_exposure_start_ = std::chrono::system_clock::time_point{};
            connected_.store(true);
            return;
        }

        if (camera_id_.has_value()) {
            try {
                sdk.stop_exposure(camera_id_.value());
            } catch (const std::exception&) {
            }
            sdk.close_camera(camera_id_.value());
        }
        connected_.store(false);
    }

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        state.push_back({"Connected", connected_.load()});
        if (!connected_.load()) {
            return state;
        }
        state.push_back({"CameraState", static_cast<std::int32_t>(get_camera_state())});
        if (can_get_control(ZWOControlType::Temperature)) {
            state.push_back({"CCDTemperature", get_ccd_temperature()});
        }
        if (can_get_control(ZWOControlType::CoolerOn)) {
            state.push_back({"CoolerOn", get_cooler_on()});
        }
        state.push_back({"ImageReady", get_image_ready()});
        return state;
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

    int get_bayer_offset_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            return 0;
        }
        return bayer_offsets(camera_info_.bayer_pattern).first;
    }

    int get_bayer_offset_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            return 0;
        }
        return bayer_offsets(camera_info_.bayer_pattern).second;
    }

    int get_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_x_;
    }

    void set_bin_x(int bin_x) override {
        set_bin_locked(bin_x, bin_x);
    }

    int get_bin_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_y_;
    }

    void set_bin_y(int bin_y) override {
        set_bin_locked(bin_y, bin_y);
    }

    CameraState get_camera_state() const override {
        if (!connected_.load()) {
            return CameraState::Idle;
        }
        auto status = ZWOSDKWrapper::instance().get_exposure_status(camera_id_value());
        switch (status) {
        case ZWOExposureStatus::Idle:
            return CameraState::Idle;
        case ZWOExposureStatus::Working:
            return CameraState::Exposing;
        case ZWOExposureStatus::Success:
            return CameraState::Reading;
        case ZWOExposureStatus::Failed:
        default:
            return CameraState::Error;
        }
    }

    int get_camera_x_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.max_width : 0;
    }

    int get_camera_y_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.max_height : 0;
    }

    bool get_can_abort_exposure() const override {
        return true;
    }

    bool get_can_asymmetric_bin() const override {
        return false;
    }

    bool get_can_fast_readout() const override {
        return can_get_control(ZWOControlType::HighSpeedMode);
    }

    bool get_can_get_cooler_power() const override {
        return can_get_control(ZWOControlType::CoolerPower);
    }

    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_st4_port;
    }

    bool get_can_set_ccd_temperature() const override {
        return can_get_control(ZWOControlType::TargetTemperature);
    }

    bool get_can_stop_exposure() const override {
        return true;
    }

    double get_ccd_temperature() const override {
        ensure_connected();
        long value = get_control_value_or_throw(ZWOControlType::Temperature);
        return static_cast<double>(value) / 10.0;
    }

    bool get_cooler_on() const override {
        ensure_connected();
        long value = get_control_value_or_throw(ZWOControlType::CoolerOn);
        return value != 0;
    }

    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        set_control_value_or_throw(ZWOControlType::CoolerOn, cooler_on ? 1 : 0);
    }

    double get_cooler_power() const override {
        ensure_connected();
        long value = get_control_value_or_throw(ZWOControlType::CoolerPower);
        return static_cast<double>(value);
    }

    double get_electrons_per_adu() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.electrons_per_adu : 0.0;
    }

    double get_exposure_max() const override {
        auto caps = get_control_caps_or_throw(ZWOControlType::Exposure);
        return static_cast<double>(caps.max_value) / 1'000'000.0;
    }

    double get_exposure_min() const override {
        auto caps = get_control_caps_or_throw(ZWOControlType::Exposure);
        return static_cast<double>(caps.min_value) / 1'000'000.0;
    }

    double get_exposure_resolution() const override {
        return 0.000001;
    }

    bool get_fast_readout() const override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::HighSpeedMode)) {
            throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
        }
        long value = get_control_value_or_throw(ZWOControlType::HighSpeedMode);
        return value != 0;
    }

    void set_fast_readout(bool fast_readout) override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::HighSpeedMode)) {
            throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
        }
        set_control_value_or_throw(ZWOControlType::HighSpeedMode, fast_readout ? 1 : 0);
    }

    double get_full_well_capacity() const override {
        return 0.0;
    }

    int get_gain() const override {
        ensure_connected();
        return static_cast<int>(get_control_value_or_throw(ZWOControlType::Gain));
    }

    void set_gain(int gain) override {
        ensure_connected();
        set_control_value_or_throw(ZWOControlType::Gain, gain);
    }

    int get_gain_max() const override {
        auto caps = get_control_caps_or_throw(ZWOControlType::Gain);
        return static_cast<int>(caps.max_value);
    }

    int get_gain_min() const override {
        auto caps = get_control_caps_or_throw(ZWOControlType::Gain);
        return static_cast<int>(caps.min_value);
    }

    std::vector<std::string> get_gains() const override {
        return {};
    }

    bool get_has_shutter() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_shutter;
    }

    double get_heat_sink_temperature() const override {
        return get_ccd_temperature();
    }

    ImageArray get_image_array() const override {
        ensure_connected();
        if (!get_image_ready()) {
            throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
        }

        int active_camera_id = camera_id_value();
        std::lock_guard<std::mutex> lock(mutex_);
        if (image_cached_) {
            return last_image_;
        }

        const auto bytes = image_buffer_size_locked();
        if (bytes == 0) {
            throw AlpacaException("Image buffer size is invalid", AlpacaError::InvalidOperation);
        }
        std::vector<std::uint8_t> buffer(bytes);
        ZWOSDKWrapper::instance().get_data_after_exposure(active_camera_id, buffer.data(),
                                                          static_cast<long>(buffer.size()));

        last_image_ = build_image_array_locked(buffer);
        image_cached_ = true;
        image_ready_ = true;
        return last_image_;
    }

    std::string get_image_array_variant() const override {
        return "int32";
    }

    bool get_image_ready() const override {
        ensure_connected();
        auto status = ZWOSDKWrapper::instance().get_exposure_status(camera_id_value());
        bool ready = (status == ZWOExposureStatus::Success);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            image_ready_ = ready;
            if (!ready) {
                image_cached_ = false;
            }
        }
        return ready;
    }

    bool get_is_pulse_guiding() const override {
        if (!pulse_guiding_.load()) {
            return false;
        }
        auto now = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point end_time;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            end_time = pulse_guiding_end_;
        }
        if (now >= end_time) {
            pulse_guiding_.store(false);
            return false;
        }
        return true;
    }

    double get_last_exposure_duration() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_exposure_duration_;
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_exposure_start_;
    }

    int get_max_adu() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.bit_depth <= 0) {
            return 0;
        }
        return static_cast<int>((1ULL << camera_info_.bit_depth) - 1ULL);
    }

    int get_max_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_.supported_bins.empty()) {
            return 1;
        }
        return *std::max_element(camera_info_.supported_bins.begin(),
                                 camera_info_.supported_bins.end());
    }

    int get_max_bin_y() const override {
        return get_max_bin_x();
    }

    int get_num_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_x_;
    }

    void set_num_x(int num_x) override {
        set_roi_size_locked(num_x, get_num_y());
    }

    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }

    void set_num_y(int num_y) override {
        set_roi_size_locked(get_num_x(), num_y);
    }

    int get_offset() const override {
        ensure_connected();
        return static_cast<int>(get_control_value_or_throw(ZWOControlType::Offset));
    }

    void set_offset(int offset) override {
        ensure_connected();
        set_control_value_or_throw(ZWOControlType::Offset, offset);
    }

    int get_offset_max() const override {
        auto caps = get_control_caps_or_throw(ZWOControlType::Offset);
        return static_cast<int>(caps.max_value);
    }

    int get_offset_min() const override {
        auto caps = get_control_caps_or_throw(ZWOControlType::Offset);
        return static_cast<int>(caps.min_value);
    }

    std::vector<std::string> get_offsets() const override {
        return {};
    }

    double get_percent_completed() const override {
        if (!connected_.load()) {
            return 0.0;
        }
        auto state = get_camera_state();
        if (state != CameraState::Exposing) {
            std::lock_guard<std::mutex> lock(mutex_);
            return image_ready_ ? 100.0 : 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_exposure_duration_ <= 0.0) {
            return 0.0;
        }
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration<double>(now - last_exposure_start_).count();
        double percent = (elapsed / last_exposure_duration_) * 100.0;
        if (percent < 0.0) {
            return 0.0;
        }
        if (percent > 100.0) {
            return 100.0;
        }
        return percent;
    }

    double get_pixel_size_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_um : 0.0;
    }

    double get_pixel_size_y() const override {
        return get_pixel_size_x();
    }

    int get_readout_mode() const override {
        if (!can_get_control(ZWOControlType::HighSpeedMode)) {
            return 0;
        }
        return get_fast_readout() ? 1 : 0;
    }

    void set_readout_mode(int mode) override {
        if (!can_get_control(ZWOControlType::HighSpeedMode)) {
            if (mode != 0) {
                throw AlpacaException("Readout mode not supported", AlpacaError::NotImplemented);
            }
            return;
        }
        if (mode != 0 && mode != 1) {
            throw AlpacaException("Invalid readout mode", AlpacaError::InvalidValue);
        }
        set_fast_readout(mode == 1);
    }

    std::vector<std::string> get_readout_modes() const override {
        if (can_get_control(ZWOControlType::HighSpeedMode)) {
            return {"Normal", "High Speed"};
        }
        return {"Normal"};
    }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.name : "ZWO Sensor";
    }

    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            return SensorType::Monochrome;
        }
        return SensorType::RGGB;
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        long value = get_control_value_or_throw(ZWOControlType::TargetTemperature);
        return static_cast<double>(value);
    }

    void set_set_ccd_temperature(double temperature) override {
        ensure_connected();
        set_control_value_or_throw(ZWOControlType::TargetTemperature,
                                   static_cast<long>(std::lround(temperature)));
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }

    void set_start_x(int start_x) override {
        set_start_pos_locked(start_x, get_start_y());
    }

    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }

    void set_start_y(int start_y) override {
        set_start_pos_locked(get_start_x(), start_y);
    }

    double get_sub_exposure_duration() const override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void set_sub_exposure_duration(double) override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void abort_exposure() override {
        stop_exposure();
    }

    void pulse_guide(int direction, int duration) override {
        ensure_connected();
        if (!get_can_pulse_guide()) {
            throw AlpacaException("Pulse guide not supported", AlpacaError::NotImplemented);
        }

        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid pulse guide direction", AlpacaError::InvalidValue);
        }
        if (duration <= 0) {
            throw AlpacaException("Invalid pulse guide duration", AlpacaError::InvalidValue);
        }

        ZWOGuideDirection guide_direction = ZWOGuideDirection::North;
        switch (direction) {
        case 0:
            guide_direction = ZWOGuideDirection::North;
            break;
        case 1:
            guide_direction = ZWOGuideDirection::South;
            break;
        case 2:
            guide_direction = ZWOGuideDirection::East;
            break;
        case 3:
            guide_direction = ZWOGuideDirection::West;
            break;
        default:
            break;
        }

        auto& sdk = ZWOSDKWrapper::instance();
        sdk.pulse_guide_on(camera_id_value(), guide_direction);
        pulse_guiding_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guiding_end_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
        }

        std::thread([this, guide_direction, duration]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            try {
                if (connected_.load()) {
                    ZWOSDKWrapper::instance().pulse_guide_off(camera_id_value(), guide_direction);
                }
            } catch (const std::exception&) {
            }
            pulse_guiding_.store(false);
        }).detach();
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        if (duration <= 0.0) {
            throw AlpacaException("Exposure duration must be positive", AlpacaError::InvalidValue);
        }

        auto caps = get_control_caps_or_throw(ZWOControlType::Exposure);
        long exposure_us = static_cast<long>(std::lround(duration * 1'000'000.0));
        if (exposure_us < caps.min_value || exposure_us > caps.max_value) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }

        auto& sdk = ZWOSDKWrapper::instance();
        int active_camera_id = camera_id_value();
        sdk.set_control_value(active_camera_id, ZWOControlType::Exposure, exposure_us, false);
        sdk.start_exposure(active_camera_id, !light);

        std::lock_guard<std::mutex> lock(mutex_);
        last_exposure_duration_ = duration;
        last_exposure_start_ = std::chrono::system_clock::now();
        image_ready_ = false;
        image_cached_ = false;
    }

    void stop_exposure() override {
        ensure_connected();
        ZWOSDKWrapper::instance().stop_exposure(camera_id_value());
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        image_cached_ = false;
    }

private:
    int device_number_;
    std::optional<int> camera_id_;
    std::optional<int> camera_index_;
    std::string serial_number_;
    ZWOCameraInfo camera_info_;
    bool camera_info_valid_;

    std::unordered_map<ZWOControlType, ZWOControlCaps> control_caps_;

    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;

    ZWOImageType image_type_;
    int bin_x_;
    int bin_y_;
    int num_x_;
    int num_y_;
    int start_x_;
    int start_y_;

    mutable bool image_ready_;
    mutable bool image_cached_;
    mutable ImageArray last_image_;
    double last_exposure_duration_;
    std::chrono::system_clock::time_point last_exposure_start_;

    mutable std::atomic<bool> pulse_guiding_;
    std::chrono::steady_clock::time_point pulse_guiding_end_;

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
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
                ALPACA_LOG_ERROR("ZWO", "Connection task failed: " + std::string(e.what()));
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

    int resolve_camera_id_locked() {
        if (camera_id_.has_value()) {
            return camera_id_.value();
        }
        if (!camera_index_.has_value()) {
            throw AlpacaException("Camera ID not specified", AlpacaError::InvalidValue);
        }
        ZWOCameraInfo info;
        if (!ZWOSDKWrapper::instance().get_camera_info_by_index(camera_index_.value(), info)) {
            throw AlpacaException("Camera index not found", AlpacaError::InvalidValue);
        }
        camera_id_ = info.camera_id;
        camera_info_ = info;
        camera_info_valid_ = true;
        return camera_id_.value();
    }

    void refresh_camera_info_locked(int camera_id) {
        ZWOCameraInfo info;
        if (ZWOSDKWrapper::instance().get_camera_info_by_id(camera_id, info)) {
            camera_info_ = info;
            camera_info_valid_ = true;
        }
    }

    void load_control_caps_locked(int camera_id) {
        control_caps_.clear();
        auto caps = ZWOSDKWrapper::instance().get_control_caps(camera_id);
        for (const auto& cap : caps) {
            control_caps_[cap.type] = cap;
        }
    }

    void select_default_image_type_locked() {
        if (!camera_info_valid_) {
            image_type_ = ZWOImageType::Raw8;
            return;
        }
        if (camera_info_.is_color && supports_format(camera_info_.supported_formats, ZWOImageType::Rgb24)) {
            image_type_ = ZWOImageType::Rgb24;
            return;
        }
        if (supports_format(camera_info_.supported_formats, ZWOImageType::Raw16)) {
            image_type_ = ZWOImageType::Raw16;
            return;
        }
        if (supports_format(camera_info_.supported_formats, ZWOImageType::Raw8)) {
            image_type_ = ZWOImageType::Raw8;
            return;
        }
        image_type_ = ZWOImageType::Raw8;
    }

    bool can_get_control(ZWOControlType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return control_caps_.find(type) != control_caps_.end();
    }

    const ZWOControlCaps& get_control_caps_or_throw(ZWOControlType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = control_caps_.find(type);
        if (it == control_caps_.end()) {
            throw AlpacaException("Control not supported", AlpacaError::NotImplemented);
        }
        return it->second;
    }

    long get_control_value_or_throw(ZWOControlType type) const {
        get_control_caps_or_throw(type);
        bool is_auto = false;
        long value = 0;
        if (!ZWOSDKWrapper::instance().get_control_value(camera_id_value(), type, value, is_auto)) {
            throw AlpacaException("Failed to get control value", AlpacaError::DriverException);
        }
        return value;
    }

    void set_control_value_or_throw(ZWOControlType type, long value) const {
        const auto& caps = get_control_caps_or_throw(type);
        if (!caps.is_writable) {
            throw AlpacaException("Control is read-only", AlpacaError::InvalidOperation);
        }
        if (value < caps.min_value || value > caps.max_value) {
            throw AlpacaException("Control value out of range", AlpacaError::InvalidValue);
        }
        ZWOSDKWrapper::instance().set_control_value(camera_id_value(), type, value, false);
    }

    int camera_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        return camera_id_.value();
    }

    void set_bin_locked(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
        }
        int active_camera_id = camera_id_value();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !supports_bin(camera_info_.supported_bins, bin_x)) {
            throw AlpacaException("Bin value not supported", AlpacaError::InvalidValue);
        }
        if (num_x_ <= 0 || num_y_ <= 0) {
            throw AlpacaException("ROI not initialized", AlpacaError::InvalidOperation);
        }
        ZWOSDKWrapper::instance().set_roi_format(active_camera_id, num_x_, num_y_, bin_x, image_type_);
        bin_x_ = bin_x;
        bin_y_ = bin_y;
    }

    void set_roi_size_locked(int width, int height) {
        ensure_connected();
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        int active_camera_id = camera_id_value();
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_) {
            if (width > camera_info_.max_width || height > camera_info_.max_height) {
                throw AlpacaException("ROI size exceeds sensor size", AlpacaError::InvalidValue);
            }
            if (start_x_ + width > camera_info_.max_width || start_y_ + height > camera_info_.max_height) {
                throw AlpacaException("ROI exceeds sensor bounds", AlpacaError::InvalidValue);
            }
        }
        ZWOSDKWrapper::instance().set_roi_format(active_camera_id, width, height, bin_x_, image_type_);
        num_x_ = width;
        num_y_ = height;
        image_cached_ = false;
    }

    void set_start_pos_locked(int start_x, int start_y) {
        ensure_connected();
        if (start_x < 0 || start_y < 0) {
            throw AlpacaException("Start position must be non-negative", AlpacaError::InvalidValue);
        }
        int active_camera_id = camera_id_value();
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_) {
            if (start_x + num_x_ > camera_info_.max_width || start_y + num_y_ > camera_info_.max_height) {
                throw AlpacaException("Start position out of bounds", AlpacaError::InvalidValue);
            }
        }
        ZWOSDKWrapper::instance().set_start_pos(active_camera_id, start_x, start_y);
        start_x_ = start_x;
        start_y_ = start_y;
    }

    std::size_t image_buffer_size_locked() const {
        int width = num_x_;
        int height = num_y_;
        if (width <= 0 || height <= 0) {
            return 0;
        }
        switch (image_type_) {
        case ZWOImageType::Raw16:
            return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2;
        case ZWOImageType::Rgb24:
            return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
        case ZWOImageType::Raw8:
        case ZWOImageType::Y8:
        default:
            return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        }
    }

    ImageArray build_image_array_locked(const std::vector<std::uint8_t>& buffer) const {
        ImageArray image;
        image.width = num_x_;
        image.height = num_y_;
        if (image.width <= 0 || image.height <= 0) {
            image.rank = 0;
            return image;
        }

        if (image_type_ == ZWOImageType::Rgb24) {
            image.rank = 3;
            image.data.resize(static_cast<std::size_t>(image.width) *
                              static_cast<std::size_t>(image.height) * 3);
            std::size_t idx = 0;
            for (std::size_t i = 0; i + 2 < buffer.size(); i += 3) {
                image.data[idx++] = buffer[i];
                image.data[idx++] = buffer[i + 1];
                image.data[idx++] = buffer[i + 2];
            }
            return image;
        }

        image.rank = 2;
        const std::size_t pixel_count = static_cast<std::size_t>(image.width) *
                                        static_cast<std::size_t>(image.height);
        image.data.resize(pixel_count);
        if (image_type_ == ZWOImageType::Raw16) {
            for (std::size_t i = 0; i < pixel_count; ++i) {
                std::size_t offset = i * 2;
                if (offset + 1 >= buffer.size()) {
                    break;
                }
                std::uint16_t value = static_cast<std::uint16_t>(buffer[offset]) |
                                      static_cast<std::uint16_t>(buffer[offset + 1] << 8);
                image.data[i] = static_cast<std::int32_t>(value);
            }
            return image;
        }

        for (std::size_t i = 0; i < pixel_count && i < buffer.size(); ++i) {
            image.data[i] = buffer[i];
        }
        return image;
    }
};

std::unique_ptr<CameraDriver> create_zwo_camera(int device_number, int camera_id) {
    return std::make_unique<ZWOCameraDriver>(device_number, camera_id, std::nullopt);
}

std::unique_ptr<CameraDriver> create_zwo_camera_by_index(int device_number, int camera_index) {
    return std::make_unique<ZWOCameraDriver>(device_number, std::nullopt, camera_index);
}

} // namespace alpacacore::vendor::zwo
