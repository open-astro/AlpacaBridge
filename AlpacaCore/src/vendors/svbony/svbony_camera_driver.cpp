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

#include <alpacacore/vendor/svbony/svbony_camera_driver.h>
#include <alpacacore/vendor/svbony/svbony_sdk_wrapper.h>
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

namespace alpacacore::vendor::svbony {

namespace {

std::pair<int, int> bayer_offsets(SVBBayerPattern pattern) {
    switch (pattern) {
    case SVBBayerPattern::RG:
        return {0, 0};
    case SVBBayerPattern::BG:
        return {1, 1};
    case SVBBayerPattern::GR:
        return {1, 0};
    case SVBBayerPattern::GB:
        return {0, 1};
    default:
        return {0, 0};
    }
}

bool supports_format(const std::vector<SVBImageType>& formats, SVBImageType type) {
    return std::find(formats.begin(), formats.end(), type) != formats.end();
}

bool supports_bin(const std::vector<int>& bins, int bin) {
    return std::find(bins.begin(), bins.end(), bin) != bins.end();
}

} // namespace

class SVBONYCameraDriver : public CameraDriver {
public:
    SVBONYCameraDriver(int device_number, int camera_index)
        : device_number_(device_number)
        , camera_index_(camera_index)
        , camera_id_(-1)
        , serial_number_()
        , camera_info_()
        , camera_info_valid_(false)
        , control_caps_()
        , connected_(false)
        , connecting_(false)
        , image_type_(SVBImageType::Raw8)
        , bin_x_(1)
        , bin_y_(1)
        , num_x_(0)
        , num_y_(0)
        , roi_width_effective_(0)
        , roi_height_effective_(0)
        , start_x_(0)
        , start_y_(0)
        , image_ready_(false)
        , image_cached_(false)
        , last_image_()
        , last_exposure_duration_(0.0)
        , last_exposure_start_()
        , last_exposure_valid_(false)
        , exposure_active_(false)
        , pulse_guiding_(false)
        , pulse_guiding_end_(std::chrono::steady_clock::time_point{})
    {
        preload_camera_info_locked();
    }

    ~SVBONYCameraDriver() override {
        stop_connection_thread();
        stop_exposure_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SVBONY", "Error during destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        const_cast<SVBONYCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.name.empty()) {
            return camera_info_.name;
        }
        return "SVBONY Camera";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Camera;
    }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "SVBONY_SN_" + serial_number_;
        }
        return "SVBONY_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "SVBONY Camera Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore SVBONY Camera Driver";
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
            if (connected) {
                reset_exposure_state_locked();
            }
            return;
        }

        auto& sdk = SVBSDKWrapper::instance();

        if (connected) {
            int resolved_id = resolve_camera_id_locked();
            ALPACA_LOG_INFO("SVBONY", "SDK version: " + sdk.get_sdk_version());
            sdk.open_camera(resolved_id);
            // Reset any leftover state from a previous session before
            // touching mode/controls. Some models (notably SV905C2) start in
            // an inconsistent state where SVBSetControlValue returns
            // SVB_ERROR_GENERAL_ERROR until defaults are restored. Tolerate
            // failure on cameras/SDK builds that don't support the call.
            try {
                sdk.restore_default_param(resolved_id);
                ALPACA_LOG_DEBUG("SVBONY", "SVBRestoreDefaultParam succeeded");
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SVBONY", "SVBRestoreDefaultParam failed: " + std::string(e.what()));
            }
            sdk.set_camera_mode_normal(resolved_id);
            sdk.set_auto_save_param(resolved_id, false);

            refresh_camera_info_locked(resolved_id);
            load_control_caps_locked(resolved_id);
            select_default_image_type_locked();

            bin_x_ = 1;
            bin_y_ = 1;
            start_x_ = 0;
            start_y_ = 0;
            int max_width = camera_info_valid_ ? camera_info_.max_width : 0;
            int max_height = camera_info_valid_ ? camera_info_.max_height : 0;
            num_x_ = max_width;
            num_y_ = max_height;
            roi_width_effective_ = align_roi_dimension(max_width, 8);
            roi_height_effective_ = align_roi_dimension(max_height, 2);

            if (roi_width_effective_ > 0 && roi_height_effective_ > 0) {
                sdk.set_roi_format(resolved_id, start_x_, start_y_,
                                   roi_width_effective_, roi_height_effective_, bin_x_);
                sdk.set_output_image_type(resolved_id, image_type_);
            }
            roi_dirty_ = false;

            // Probe ALL enumerated controls to find which (if any) accept
            // an SVBSetControlValue write. Earlier probes showed Gain fails
            // both cold and during capture, so we need to know whether
            // SVBSetControlValue is broken for the entire camera or just
            // for SVB_GAIN specifically on SV905C2.
            {
                ALPACA_LOG_INFO("SVBONY",
                    "Control enumeration: " + std::to_string(control_caps_.size()) + " controls reported");
                for (const auto& kv : control_caps_) {
                    const auto& c = kv.second;
                    ALPACA_LOG_INFO("SVBONY",
                        " - " + c.name +
                        " range=[" + std::to_string(c.min_value) + "," + std::to_string(c.max_value) + "]" +
                        " default=" + std::to_string(c.default_value) +
                        " writable=" + std::string(c.is_writable ? "yes" : "no") +
                        " autoSupported=" + std::string(c.is_auto_supported ? "yes" : "no"));
                }
                for (const auto& kv : control_caps_) {
                    const auto& c = kv.second;
                    if (!c.is_writable) continue;
                    try {
                        sdk.set_control_value(resolved_id, kv.first, c.default_value, false);
                        ALPACA_LOG_INFO("SVBONY",
                            "Probe write " + c.name + "=" + std::to_string(c.default_value) + " OK");
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("SVBONY",
                            "Probe write " + c.name + "=" + std::to_string(c.default_value) +
                            " FAILED: " + e.what());
                    }
                }
            }

            // Refresh pixel size now that camera is open
            try {
                float px = sdk.get_sensor_pixel_size(resolved_id);
                camera_info_.pixel_size_um = static_cast<double>(px);
            } catch (const std::exception&) {
                // pixel size may have been set during enumeration
            }

            serial_number_ = sdk.get_serial_number(resolved_id);
            reset_exposure_state_locked();
            connected_.store(true);
            return;
        }

        // Disconnecting
        if (camera_id_ >= 0) {
            try {
                sdk.stop_video_capture(camera_id_);
            } catch (const std::exception&) {
            }
            sdk.close_camera(camera_id_);
        }
        camera_id_ = -1;
        camera_info_ = {};
        camera_info_valid_ = false;
        serial_number_.clear();
        reset_exposure_state_locked();
        connected_.store(false);
    }

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        state.push_back({"Connected", connected_.load()});
        if (!connected_.load()) {
            return state;
        }
        state.push_back({"CameraState", static_cast<std::int32_t>(get_camera_state())});
        if (can_get_control(SVBControlType::CurrentTemperature)) {
            state.push_back({"CCDTemperature", get_ccd_temperature()});
        }
        if (can_get_control(SVBControlType::CoolerEnable)) {
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
            throw AlpacaException("Bayer offsets not supported", AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer_pattern).first;
    }

    int get_bayer_offset_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not supported", AlpacaError::PropertyNotImplemented);
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
        if (exposure_active_.load()) {
            return CameraState::Exposing;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (image_ready_) {
            return CameraState::Idle;
        }
        return CameraState::Idle;
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
        return can_get_control(SVBControlType::FrameSpeedMode);
    }

    bool get_can_get_cooler_power() const override {
        return can_get_control(SVBControlType::CoolerPower);
    }

    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_pulse_guide;
    }

    bool get_can_set_ccd_temperature() const override {
        return can_get_control(SVBControlType::TargetTemperature);
    }

    bool get_can_stop_exposure() const override {
        return true;
    }

    double get_ccd_temperature() const override {
        ensure_connected();
        long value = get_control_value_or_throw(SVBControlType::CurrentTemperature);
        // SVBONY temperature is in 0.1C units
        return static_cast<double>(value) / 10.0;
    }

    bool get_cooler_on() const override {
        ensure_connected();
        if (!can_get_control(SVBControlType::CoolerEnable)) {
            return false;
        }
        long value = get_control_value_or_throw(SVBControlType::CoolerEnable);
        return value != 0;
    }

    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (!can_get_control(SVBControlType::CoolerEnable)) {
            if (cooler_on) {
                throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
            }
            return;
        }
        set_control_value_or_throw(SVBControlType::CoolerEnable, cooler_on ? 1 : 0);
    }

    double get_cooler_power() const override {
        ensure_connected();
        if (!can_get_control(SVBControlType::CoolerPower)) {
            return 0.0;
        }
        long value = get_control_value_or_throw(SVBControlType::CoolerPower);
        return static_cast<double>(value);
    }

    double get_electrons_per_adu() const override {
        // SVBONY SDK does not expose electrons per ADU; return 1.0 as a
        // safe default (ConformU rejects 0).
        return 1.0;
    }

    double get_exposure_max() const override {
        auto caps = get_control_caps_or_throw(SVBControlType::Exposure);
        // SVBONY exposure is in microseconds
        return static_cast<double>(caps.max_value) / 1'000'000.0;
    }

    double get_exposure_min() const override {
        auto caps = get_control_caps_or_throw(SVBControlType::Exposure);
        return static_cast<double>(caps.min_value) / 1'000'000.0;
    }

    double get_exposure_resolution() const override {
        return 0.000001;
    }

    bool get_fast_readout() const override {
        ensure_connected();
        if (!can_get_control(SVBControlType::FrameSpeedMode)) {
            throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (frame_speed_dirty_) {
                return pending_frame_speed_ == 2;
            }
        }
        long value = get_control_value_or_throw(SVBControlType::FrameSpeedMode);
        return value == 2; // 0=low, 1=medium, 2=high
    }

    void set_fast_readout(bool fast_readout) override {
        ensure_connected();
        if (!can_get_control(SVBControlType::FrameSpeedMode)) {
            throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
        }
        long desired = fast_readout ? 2 : 0;
        const auto& caps = get_control_caps_or_throw(SVBControlType::FrameSpeedMode);
        if (desired < caps.min_value || desired > caps.max_value) {
            throw AlpacaException("Control value out of range", AlpacaError::InvalidValue);
        }
        // SVBSetControlValue for FrameSpeedMode takes ~1.1s on some cameras.
        // Defer the actual SDK write to start_exposure to stay within ASCOM timing.
        std::lock_guard<std::mutex> lock(mutex_);
        pending_frame_speed_ = desired;
        frame_speed_dirty_ = true;
    }

    double get_full_well_capacity() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.bit_depth <= 0) {
            return 0.0;
        }
        // Without electrons_per_adu, return max ADU as an approximation
        return static_cast<double>((1ULL << camera_info_.bit_depth) - 1ULL);
    }

    int get_gain() const override {
        ensure_connected();
        return static_cast<int>(get_control_value_or_throw(SVBControlType::Gain));
    }

    void set_gain(int gain) override {
        ensure_connected();
        // Diagnostic logging — SV905C2 has reported persistent
        // SVB_ERROR_GENERAL_ERROR on gain writes; capture exact range,
        // current value/auto state, and target value so we can see why.
        try {
            const auto& caps = get_control_caps_or_throw(SVBControlType::Gain);
            long cur_value = 0;
            bool is_auto = false;
            bool got = SVBSDKWrapper::instance().get_control_value(
                camera_id_value(), SVBControlType::Gain, cur_value, is_auto);
            ALPACA_LOG_DEBUG("SVBONY",
                "set_gain target=" + std::to_string(gain) +
                " range=[" + std::to_string(caps.min_value) + "," + std::to_string(caps.max_value) + "]" +
                " writable=" + std::string(caps.is_writable ? "yes" : "no") +
                " autoSupported=" + std::string(caps.is_auto_supported ? "yes" : "no") +
                " current=" + (got ? std::to_string(cur_value) : std::string("?")) +
                " currentAuto=" + (got ? std::string(is_auto ? "yes" : "no") : std::string("?")));
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("SVBONY", "set_gain pre-log failed: " + std::string(e.what()));
        }
        // Disable auto-gain first; some SVBONY models reject manual writes
        // while auto mode is active (SVB_ERROR_GENERAL_ERROR).
        disable_auto_if_needed(SVBControlType::Gain);
        set_control_value_or_throw(SVBControlType::Gain, gain);
    }

    int get_gain_max() const override {
        auto caps = get_control_caps_or_throw(SVBControlType::Gain);
        return static_cast<int>(caps.max_value);
    }

    int get_gain_min() const override {
        auto caps = get_control_caps_or_throw(SVBControlType::Gain);
        return static_cast<int>(caps.min_value);
    }

    std::vector<std::string> get_gains() const override {
        throw AlpacaException("Gain descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_has_shutter() const override {
        return false; // SVBONY cameras do not have mechanical shutters
    }

    double get_heat_sink_temperature() const override {
        return get_ccd_temperature();
    }

    ImageArray get_image_array() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
        }
        if (!image_ready_) {
            throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
        }
        if (!image_cached_) {
            throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
        }
        return last_image_;
    }

    std::string get_image_array_variant() const override {
        return "Int32";
    }

    bool get_image_ready() const override {
        if (!connected_.load()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            return false;
        }
        return image_ready_ && image_cached_;
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
        if (!last_exposure_valid_) {
            throw AlpacaException("Last exposure duration not set", AlpacaError::ValueNotSet);
        }
        return last_exposure_duration_;
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("Last exposure start time not set", AlpacaError::ValueNotSet);
        }
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
        return static_cast<int>(get_control_value_or_throw(SVBControlType::Offset));
    }

    void set_offset(int offset) override {
        ensure_connected();
        set_control_value_or_throw(SVBControlType::Offset, offset);
    }

    int get_offset_max() const override {
        auto caps = get_control_caps_or_throw(SVBControlType::Offset);
        return static_cast<int>(caps.max_value);
    }

    int get_offset_min() const override {
        auto caps = get_control_caps_or_throw(SVBControlType::Offset);
        return static_cast<int>(caps.min_value);
    }

    std::vector<std::string> get_offsets() const override {
        throw AlpacaException("Offset descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        if (!connected_.load()) {
            return 0.0;
        }
        if (!exposure_active_.load()) {
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
        if (!can_get_control(SVBControlType::FrameSpeedMode)) {
            return 0;
        }
        return get_fast_readout() ? 1 : 0;
    }

    void set_readout_mode(int mode) override {
        if (!can_get_control(SVBControlType::FrameSpeedMode)) {
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
        if (can_get_control(SVBControlType::FrameSpeedMode)) {
            return {"Normal", "High Speed"};
        }
        return {"Normal"};
    }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.name : "SVBONY Sensor";
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
        // SVBONY target temperature is in 0.1C units
        long value = get_control_value_or_throw(SVBControlType::TargetTemperature);
        return static_cast<double>(value) / 10.0;
    }

    void set_set_ccd_temperature(double temperature) override {
        ensure_connected();
        // SVBONY target temperature is in 0.1C units
        set_control_value_or_throw(SVBControlType::TargetTemperature,
                                   static_cast<long>(std::lround(temperature * 10.0)));
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

        SVBGuideDirection guide_direction = SVBGuideDirection::North;
        switch (direction) {
        case 0:
            guide_direction = SVBGuideDirection::North;
            break;
        case 1:
            guide_direction = SVBGuideDirection::South;
            break;
        case 2:
            guide_direction = SVBGuideDirection::East;
            break;
        case 3:
            guide_direction = SVBGuideDirection::West;
            break;
        default:
            break;
        }

        // SVBONY SVBPulseGuide takes direction and duration in one call (blocking on camera side)
        SVBSDKWrapper::instance().pulse_guide(camera_id_value(), guide_direction, duration);
        pulse_guiding_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guiding_end_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
        }

        std::thread([this, duration]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            pulse_guiding_.store(false);
        }).detach();
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        (void)light; // SVBONY SDK does not have a dark frame parameter

        if (duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative", AlpacaError::InvalidValue);
        }

        auto caps = get_control_caps_or_throw(SVBControlType::Exposure);
        long exposure_us = static_cast<long>(std::lround(duration * 1'000'000.0));
        // Clamp to SDK minimum (ASCOM allows duration=0 meaning minimum exposure)
        if (exposure_us < caps.min_value) {
            exposure_us = caps.min_value;
        }
        if (exposure_us > caps.max_value) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }

        int active_camera_id = camera_id_value();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (roi_width_effective_ <= 0 || roi_height_effective_ <= 0) {
                throw AlpacaException("ROI is not valid for exposure", AlpacaError::InvalidValue);
            }
            if (camera_info_valid_) {
                int max_w = camera_info_.max_width / bin_x_;
                int max_h = camera_info_.max_height / bin_y_;
                if (num_x_ > max_w || num_y_ > max_h) {
                    throw AlpacaException("ROI size exceeds sensor dimensions", AlpacaError::InvalidValue);
                }
                if (start_x_ < 0 || start_y_ < 0 || start_x_ >= max_w || start_y_ >= max_h) {
                    throw AlpacaException("Start position outside sensor bounds", AlpacaError::InvalidValue);
                }
                if (start_x_ + num_x_ > max_w || start_y_ + num_y_ > max_h) {
                    throw AlpacaException("ROI extends beyond sensor bounds", AlpacaError::InvalidValue);
                }
            }
        }

        auto& sdk = SVBSDKWrapper::instance();
        sdk.set_control_value(active_camera_id, SVBControlType::Exposure, exposure_us, false);

        // Stop any previous exposure thread
        stop_exposure_thread();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_exposure_duration_ = duration;
            last_exposure_start_ = std::chrono::system_clock::now();
            last_exposure_valid_ = true;
            image_ready_ = false;
            image_cached_ = false;
        }

        exposure_active_.store(true);

        // Start video capture and grab one frame in a background thread.
        // Deferred SDK writes (ROI, FrameSpeedMode) happen here so that
        // start_exposure returns quickly and ConformU sees fast API timing.
        exposure_thread_ = std::thread([this, active_camera_id, exposure_us]() {
            auto& sdk = SVBSDKWrapper::instance();
            try {
                // Apply deferred SDK writes outside the mutex so
                // get_image_ready() polls are not blocked.
                {
                    int sx, sy, rw, rh, bx;
                    SVBImageType img_type;
                    bool need_roi_update;
                    bool need_speed_update;
                    long speed_value;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        sx = start_x_;
                        sy = start_y_;
                        rw = roi_width_effective_;
                        rh = roi_height_effective_;
                        bx = bin_x_;
                        img_type = image_type_;
                        need_roi_update = roi_dirty_;
                        roi_dirty_ = false;
                        need_speed_update = frame_speed_dirty_;
                        speed_value = pending_frame_speed_;
                        frame_speed_dirty_ = false;
                    }
                    if (need_roi_update && rw > 0 && rh > 0) {
                        sdk.set_roi_format(active_camera_id, sx, sy, rw, rh, bx);
                        sdk.set_output_image_type(active_camera_id, img_type);
                    }
                    if (need_speed_update) {
                        long cur_speed = 0;
                        bool cur_auto = false;
                        if (!sdk.get_control_value(active_camera_id, SVBControlType::FrameSpeedMode, cur_speed, cur_auto)
                            || cur_speed != speed_value) {
                            sdk.set_control_value(active_camera_id, SVBControlType::FrameSpeedMode, speed_value, false);
                        }
                    }
                }

                sdk.start_video_capture(active_camera_id);

                std::size_t buffer_size = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    buffer_size = image_buffer_size_locked();
                }

                if (buffer_size == 0 || !exposure_active_.load()) {
                    sdk.stop_video_capture(active_camera_id);
                    exposure_active_.store(false);
                    return;
                }

                std::vector<std::uint8_t> buffer(buffer_size);

                // Poll for frame data like the SDK demo does — SVBGetVideoData
                // can fail on the first attempts while the sensor integrates.
                int total_wait_ms = static_cast<int>(exposure_us / 1000) + 10000;
                if (total_wait_ms < 10000) {
                    total_wait_ms = 10000;
                }
                auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(total_wait_ms);
                bool got_frame = false;
                int attempts = 0;
                while (exposure_active_.load()
                       && std::chrono::steady_clock::now() < deadline) {
                    try {
                        sdk.get_video_data(active_camera_id, buffer.data(),
                                           static_cast<long>(buffer.size()), 500);
                        got_frame = true;
                        break;
                    } catch (const std::exception&) {
                        ++attempts;
                    }
                }

                sdk.stop_video_capture(active_camera_id);

                if (!got_frame || !exposure_active_.load()) {
                    ALPACA_LOG_WARN("SVBONY", "Exposure failed: no frame after " +
                        std::to_string(attempts) + " attempts");
                    exposure_active_.store(false);
                    return;
                }

                // Build image array
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_image_ = build_image_array_locked(buffer);
                    image_cached_ = true;
                    image_ready_ = true;
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SVBONY", "Exposure failed: " + std::string(e.what()));
                try {
                    sdk.stop_video_capture(active_camera_id);
                } catch (const std::exception&) {
                }
            }
            exposure_active_.store(false);
        });
    }

    void stop_exposure() override {
        ensure_connected();
        exposure_active_.store(false);
        try {
            SVBSDKWrapper::instance().stop_video_capture(camera_id_value());
        } catch (const std::exception&) {
        }
        // Wait for exposure thread to finish so CameraState returns Idle
        // immediately after stop/abort.
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        image_cached_ = false;
    }

private:
    int device_number_;
    int camera_index_;
    int camera_id_;
    std::string serial_number_;
    SVBCameraInfo camera_info_;
    bool camera_info_valid_;

    std::unordered_map<SVBControlType, SVBControlCaps> control_caps_;

    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;

    SVBImageType image_type_;
    int bin_x_;
    int bin_y_;
    int num_x_;
    int num_y_;
    int roi_width_effective_;
    int roi_height_effective_;
    int start_x_;
    int start_y_;

    mutable bool image_ready_;
    mutable bool image_cached_;
    mutable ImageArray last_image_;
    double last_exposure_duration_;
    std::chrono::system_clock::time_point last_exposure_start_;
    bool last_exposure_valid_;

    bool roi_dirty_{true};
    bool frame_speed_dirty_{false};
    long pending_frame_speed_{0};

    std::atomic<bool> exposure_active_;
    std::thread exposure_thread_;

    mutable std::atomic<bool> pulse_guiding_;
    std::chrono::steady_clock::time_point pulse_guiding_end_;

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    void reset_exposure_state_locked() {
        image_ready_ = false;
        image_cached_ = false;
        last_exposure_duration_ = 0.0;
        last_exposure_start_ = std::chrono::system_clock::time_point{};
        last_exposure_valid_ = false;
        exposure_active_.store(false);
    }

    bool is_roi_valid_locked(int width, int height, int sx, int sy) const {
        if (!camera_info_valid_) {
            return false;
        }
        if (width <= 0 || height <= 0) {
            return false;
        }
        int max_width = camera_info_.max_width / bin_x_;
        int max_height = camera_info_.max_height / bin_y_;
        if (width > max_width || height > max_height) {
            return false;
        }
        if (sx < 0 || sy < 0) {
            return false;
        }
        if (sx + width > max_width || sy + height > max_height) {
            return false;
        }
        int adjusted_width = 0;
        int adjusted_height = 0;
        if (!adjust_roi_size_locked(width, height, adjusted_width, adjusted_height)) {
            return false;
        }
        return true;
    }

    int align_roi_dimension(int value, int multiple) const {
        if (multiple <= 1) {
            return value;
        }
        return value - (value % multiple);
    }

    bool adjust_roi_size_locked(int requested_width,
                                int requested_height,
                                int& adjusted_width,
                                int& adjusted_height) const {
        if (!camera_info_valid_) {
            return false;
        }
        if (requested_width <= 0 || requested_height <= 0) {
            return false;
        }

        int max_width = camera_info_.max_width / bin_x_;
        int max_height = camera_info_.max_height / bin_y_;
        if (requested_width > max_width || requested_height > max_height) {
            return false;
        }

        // SVBONY SDK: width must be multiple of 8, height must be multiple of 2
        int candidate_width = align_roi_dimension(requested_width, 8);
        int candidate_height = align_roi_dimension(requested_height, 2);
        if (candidate_width <= 0 || candidate_height <= 0) {
            return false;
        }

        adjusted_width = candidate_width;
        adjusted_height = candidate_height;
        return true;
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
                ALPACA_LOG_ERROR("SVBONY", "Connection task failed: " + std::string(e.what()));
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

    void stop_exposure_thread() {
        exposure_active_.store(false);
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
    }

    int resolve_camera_id_locked() {
        auto cameras = SVBSDKWrapper::instance().enumerate_cameras();
        if (cameras.empty()) {
            ALPACA_LOG_WARN("SVBONY", "No SVBONY cameras detected by SDK");
            throw AlpacaException("No SVBONY cameras detected", AlpacaError::NotConnected);
        }
        if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
            ALPACA_LOG_WARN("SVBONY", "Camera index out of range: " + std::to_string(camera_index_) +
                           " (count=" + std::to_string(cameras.size()) + ")");
            throw AlpacaException("Camera index not found", AlpacaError::InvalidValue);
        }

        const auto& info = cameras[static_cast<std::size_t>(camera_index_)];
        ALPACA_LOG_INFO("SVBONY", "Using camera index " + std::to_string(camera_index_) +
                       ": " + info.name + " (ID " + std::to_string(info.camera_id) + ")");
        camera_id_ = info.camera_id;
        camera_info_ = info;
        camera_info_valid_ = true;
        return camera_id_;
    }

    void refresh_camera_info_locked(int camera_id) {
        // Re-read properties now that the camera is open
        SVBCameraInfo info;
        if (SVBSDKWrapper::instance().get_camera_info_by_index(camera_index_, info)) {
            // Preserve camera_id from open
            info.camera_id = camera_id;
            camera_info_ = info;
            camera_info_valid_ = true;
        }
    }

    void preload_camera_info_locked() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_) {
            return;
        }

        try {
            auto cameras = SVBSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
                camera_id_ = camera_info_.camera_id;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("SVBONY", "Unable to preload camera info: " + std::string(e.what()));
        }
    }

    void refresh_cached_camera_info_if_needed() {
        if (connected_.load()) {
            return;
        }

        try {
            auto cameras = SVBSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                const auto& info = cameras[static_cast<std::size_t>(camera_index_)];
                std::lock_guard<std::mutex> lock(mutex_);
                if (camera_id_ != info.camera_id) {
                    serial_number_.clear();
                }
                camera_info_ = info;
                camera_info_valid_ = true;
                camera_id_ = info.camera_id;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("SVBONY", "Unable to refresh camera info: " + std::string(e.what()));
        }
    }

    void load_control_caps_locked(int camera_id) {
        control_caps_.clear();
        auto caps = SVBSDKWrapper::instance().get_control_caps(camera_id);
        for (const auto& cap : caps) {
            control_caps_[cap.type] = cap;
        }
    }

    void select_default_image_type_locked() {
        if (!camera_info_valid_) {
            image_type_ = SVBImageType::Raw8;
            return;
        }
        if (supports_format(camera_info_.supported_formats, SVBImageType::Raw16)) {
            image_type_ = SVBImageType::Raw16;
            return;
        }
        if (supports_format(camera_info_.supported_formats, SVBImageType::Raw8)) {
            image_type_ = SVBImageType::Raw8;
            return;
        }
        if (supports_format(camera_info_.supported_formats, SVBImageType::Rgb24)) {
            image_type_ = SVBImageType::Rgb24;
            return;
        }
        image_type_ = SVBImageType::Raw8;
    }

    bool can_get_control(SVBControlType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return control_caps_.find(type) != control_caps_.end();
    }

    const SVBControlCaps& get_control_caps_or_throw(SVBControlType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = control_caps_.find(type);
        if (it == control_caps_.end()) {
            throw AlpacaException("Control not supported", AlpacaError::NotImplemented);
        }
        return it->second;
    }

    long get_control_value_or_throw(SVBControlType type) const {
        get_control_caps_or_throw(type);
        bool is_auto = false;
        long value = 0;
        if (!SVBSDKWrapper::instance().get_control_value(camera_id_value(), type, value, is_auto)) {
            throw AlpacaException("Failed to get control value", AlpacaError::DriverException);
        }
        return value;
    }

    void disable_auto_if_needed(SVBControlType type) const {
        long cur_value = 0;
        bool is_auto = false;
        if (SVBSDKWrapper::instance().get_control_value(camera_id_value(), type, cur_value, is_auto)) {
            if (is_auto) {
                SVBSDKWrapper::instance().set_control_value(camera_id_value(), type, cur_value, false);
            }
        }
    }

    void set_control_value_or_throw(SVBControlType type, long value) const {
        const auto& caps = get_control_caps_or_throw(type);
        if (!caps.is_writable) {
            throw AlpacaException("Control is read-only", AlpacaError::InvalidOperation);
        }
        if (value < caps.min_value || value > caps.max_value) {
            throw AlpacaException("Control value out of range", AlpacaError::InvalidValue);
        }
        SVBSDKWrapper::instance().set_control_value(camera_id_value(), type, value, false);
    }

    int camera_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_id_ < 0) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        return camera_id_;
    }

    void set_bin_locked(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !supports_bin(camera_info_.supported_bins, bin_x)) {
            throw AlpacaException("Bin value not supported", AlpacaError::InvalidValue);
        }
        if (bin_x_ == bin_x && bin_y_ == bin_y) {
            return;
        }
        bin_x_ = bin_x;
        bin_y_ = bin_y;
        int max_width = camera_info_.max_width / bin_x_;
        int max_height = camera_info_.max_height / bin_y_;
        int width = 0;
        int height = 0;
        if (!adjust_roi_size_locked(max_width, max_height, width, height)) {
            throw AlpacaException("ROI size invalid for binning", AlpacaError::InvalidValue);
        }
        // Defer SVBSetROIFormat to exposure start for fast response time.
        num_x_ = max_width;
        num_y_ = max_height;
        roi_width_effective_ = width;
        roi_height_effective_ = height;
        start_x_ = 0;
        start_y_ = 0;
        roi_dirty_ = true;
        image_cached_ = false;
    }

    void set_roi_size_locked(int width, int height) {
        ensure_connected();
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (num_x_ == width && num_y_ == height) {
            return;
        }
        num_x_ = width;
        num_y_ = height;
        // Update effective ROI; align to SDK requirements.
        int adjusted_width = align_roi_dimension(width, 8);
        int adjusted_height = align_roi_dimension(height, 2);
        if (adjusted_width > 0 && adjusted_height > 0) {
            roi_width_effective_ = adjusted_width;
            roi_height_effective_ = adjusted_height;
        }
        roi_dirty_ = true;
        image_cached_ = false;
    }

    void set_start_pos_locked(int sx, int sy) {
        ensure_connected();
        if (sx < 0 || sy < 0) {
            throw AlpacaException("Start position must be non-negative", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (start_x_ == sx && start_y_ == sy) {
            return;
        }
        start_x_ = sx;
        start_y_ = sy;
        roi_dirty_ = true;
    }

    std::size_t image_buffer_size_locked() const {
        int width = roi_width_effective_ > 0 ? roi_width_effective_ : num_x_;
        int height = roi_height_effective_ > 0 ? roi_height_effective_ : num_y_;
        if (width <= 0 || height <= 0) {
            return 0;
        }
        switch (image_type_) {
        case SVBImageType::Raw16:
        case SVBImageType::Y16:
            return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2;
        case SVBImageType::Rgb24:
            return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
        case SVBImageType::Rgb32:
            return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        case SVBImageType::Raw8:
        case SVBImageType::Y8:
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

        int out_width = image.width;
        int out_height = image.height;
        int eff_width = roi_width_effective_ > 0 ? roi_width_effective_ : out_width;
        int eff_height = roi_height_effective_ > 0 ? roi_height_effective_ : out_height;
        if (eff_width <= 0 || eff_height <= 0) {
            image.rank = 0;
            return image;
        }

        if (image_type_ == SVBImageType::Rgb24) {
            image.rank = 3;
            image.data.resize(static_cast<std::size_t>(out_width) *
                              static_cast<std::size_t>(out_height) * 3);
            std::size_t buffer_stride = static_cast<std::size_t>(eff_width) * 3;
            std::size_t output_stride = static_cast<std::size_t>(out_width) * 3;
            for (int row = 0; row < out_height; ++row) {
                std::size_t output_row = static_cast<std::size_t>(row) * output_stride;
                if (row < eff_height) {
                    std::size_t buffer_row = static_cast<std::size_t>(row) * buffer_stride;
                    std::size_t copy_bytes = std::min(buffer_stride, output_stride);
                    for (std::size_t i = 0; i < copy_bytes && buffer_row + i < buffer.size(); ++i) {
                        image.data[output_row + i] = buffer[buffer_row + i];
                    }
                }
            }
            return image;
        }

        image.rank = 2;
        const std::size_t pixel_count = static_cast<std::size_t>(out_width) *
                                        static_cast<std::size_t>(out_height);
        image.data.resize(pixel_count);

        bool is_16bit = (image_type_ == SVBImageType::Raw16 || image_type_ == SVBImageType::Y16);
        if (is_16bit) {
            for (int row = 0; row < out_height; ++row) {
                for (int col = 0; col < out_width; ++col) {
                    std::size_t out_index = static_cast<std::size_t>(row) *
                                            static_cast<std::size_t>(out_width) +
                                            static_cast<std::size_t>(col);
                    if (row < eff_height && col < eff_width) {
                        std::size_t offset = (static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(eff_width) +
                                              static_cast<std::size_t>(col)) * 2;
                        if (offset + 1 < buffer.size()) {
                            std::uint16_t value = static_cast<std::uint16_t>(buffer[offset]) |
                                                  static_cast<std::uint16_t>(buffer[offset + 1] << 8);
                            image.data[out_index] = static_cast<std::int32_t>(value);
                            continue;
                        }
                    }
                    image.data[out_index] = 0;
                }
            }
            return image;
        }

        // 8-bit modes (Raw8, Y8)
        for (int row = 0; row < out_height; ++row) {
            for (int col = 0; col < out_width; ++col) {
                std::size_t out_index = static_cast<std::size_t>(row) *
                                        static_cast<std::size_t>(out_width) +
                                        static_cast<std::size_t>(col);
                if (row < eff_height && col < eff_width) {
                    std::size_t buffer_index = static_cast<std::size_t>(row) *
                                               static_cast<std::size_t>(eff_width) +
                                               static_cast<std::size_t>(col);
                    if (buffer_index < buffer.size()) {
                        image.data[out_index] = buffer[buffer_index];
                        continue;
                    }
                }
                image.data[out_index] = 0;
            }
        }
        return image;
    }
};

std::unique_ptr<CameraDriver> create_svbony_camera(int device_number, int camera_index) {
    return std::make_unique<SVBONYCameraDriver>(device_number, camera_index);
}

} // namespace alpacacore::vendor::svbony
