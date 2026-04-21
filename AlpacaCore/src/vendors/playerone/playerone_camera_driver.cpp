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

#include <alpacacore/vendor/playerone/playerone_camera_driver.h>
#include <alpacacore/vendor/playerone/playerone_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::playerone {

namespace {

std::pair<int, int> bayer_offsets(PlayerOneBayerPattern pattern) {
    switch (pattern) {
    case PlayerOneBayerPattern::RG: return {0, 0};
    case PlayerOneBayerPattern::BG: return {1, 1};
    case PlayerOneBayerPattern::GR: return {1, 0};
    case PlayerOneBayerPattern::GB: return {0, 1};
    default:                        return {0, 0};
    }
}

SensorType bayer_to_sensor_type(PlayerOneBayerPattern pattern) {
    // AlpacaCore's SensorType enum only names RGGB — CMYG/LRGB don't apply to
    // Player One sensors, and there's no dedicated BGGR/GRBG/GBRG value. Report
    // the mono/color split accurately and fall back to RGGB for any color
    // pattern; the Bayer-offset properties carry the actual layout for clients
    // that need to debayer.
    if (pattern == PlayerOneBayerPattern::None) return SensorType::Monochrome;
    return SensorType::RGGB;
}

bool format_is_16bit(PlayerOneImageFormat f) {
    return f == PlayerOneImageFormat::Raw16;
}

bool format_is_supported(const std::vector<PlayerOneImageFormat>& supported,
                          PlayerOneImageFormat candidate) {
    return std::find(supported.begin(), supported.end(), candidate) != supported.end();
}

PlayerOneImageFormat choose_default_format(const PlayerOneCameraInfo& info) {
    // Prefer RAW16 for anything >8-bit so we keep the sensor's full dynamic
    // range; fall back to RAW8 on 8-bit-only sensors.
    if (info.bit_depth > 8 && format_is_supported(info.supported_formats,
                                                   PlayerOneImageFormat::Raw16)) {
        return PlayerOneImageFormat::Raw16;
    }
    if (format_is_supported(info.supported_formats, PlayerOneImageFormat::Raw8)) {
        return PlayerOneImageFormat::Raw8;
    }
    // Last resort — take whatever the camera claims first.
    if (!info.supported_formats.empty()) return info.supported_formats.front();
    return PlayerOneImageFormat::Raw16;
}

} // namespace

class PlayerOneCameraDriver : public CameraDriver {
public:
    PlayerOneCameraDriver(int device_number, int camera_index)
        : device_number_(device_number)
        , camera_index_(camera_index)
    {
        preload_camera_info();
    }

    ~PlayerOneCameraDriver() override {
        stop_connection_thread();
        stop_exposure_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Error during destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        const_cast<PlayerOneCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.name.empty()) {
            return camera_info_.name;
        }
        return "Player One Camera";
    }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.serial_number.empty()) {
            return "PLAYERONE_SN_" + camera_info_.serial_number;
        }
        return "PLAYERONE_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "Player One Camera Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore Player One Camera Driver"; }
    std::string get_driver_version() const override { return "1.0.0"; }
    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected == connected_.load()) {
            if (connected) reset_exposure_state_locked();
            return;
        }

        auto& sdk = PlayerOneSDKWrapper::instance();

        if (connected) {
            auto cameras = sdk.enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No Player One cameras detected",
                                      AlpacaError::NotConnected);
            }
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range", AlpacaError::InvalidValue);
            }
            camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
            camera_info_valid_ = true;

            ALPACA_LOG_INFO("PlayerOne", "SDK version: " + sdk.get_sdk_version());
            ALPACA_LOG_INFO("PlayerOne", "Opening camera index " +
                std::to_string(camera_index_) + " (id=" +
                std::to_string(camera_info_.camera_id) + "): " + camera_info_.name);

            sdk.open_camera(camera_info_.camera_id);

            try {
                sdk.init_camera(camera_info_.camera_id);
            } catch (const std::exception& e) {
                try { sdk.close_camera(camera_info_.camera_id); } catch (...) {}
                throw AlpacaException(std::string("POAInitCamera failed: ") + e.what(),
                                      AlpacaError::DriverException);
            }

            // Probe capabilities from POAGetConfigAttributes — this gives us
            // ranges and writability for every POAConfig this camera exposes.
            try {
                caps_ = sdk.probe_config_caps(camera_info_.camera_id);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "probe_config_caps failed: " + std::string(e.what()));
                caps_ = PlayerOneConfigCaps{};
            }

            // Refresh properties post-init (Player One fills in some fields
            // only after init — most notably localPath).
            try {
                auto fresh = sdk.get_camera_properties_by_id(camera_info_.camera_id);
                // Keep our enumeration index but take the fresh identity fields.
                int idx = camera_info_.index;
                camera_info_ = fresh;
                camera_info_.index = idx;
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("PlayerOne",
                                 "get_camera_properties_by_id failed: " + std::string(e.what()));
            }

            // Select default image format (prefer RAW16 on ≥10-bit sensors).
            active_format_ = choose_default_format(camera_info_);
            try {
                sdk.set_image_format(camera_info_.camera_id, active_format_);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne",
                                "set_image_format failed: " + std::string(e.what()));
            }

            // Default ROI = full sensor at bin 1. POASetImageBin resets the
            // size/start, so do it first then push the full frame.
            try {
                sdk.set_image_bin(camera_info_.camera_id, 1);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "set_image_bin(1) failed: " + std::string(e.what()));
            }
            try {
                sdk.set_image_start_pos(camera_info_.camera_id, 0, 0);
                sdk.set_image_size(camera_info_.camera_id,
                                   camera_info_.max_width, camera_info_.max_height);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne",
                                "initial ROI setup failed: " + std::string(e.what()));
            }

            // Re-query so our cache matches what the SDK actually accepted
            // (width%4==0 / height%2==0 alignment may have trimmed values).
            int w = camera_info_.max_width;
            int h = camera_info_.max_height;
            int sx = 0;
            int sy = 0;
            int bin = 1;
            try { sdk.get_image_size(camera_info_.camera_id, w, h); } catch (...) {}
            try { sdk.get_image_start_pos(camera_info_.camera_id, sx, sy); } catch (...) {}
            try { bin = sdk.get_image_bin(camera_info_.camera_id); } catch (...) {}

            bin_ = bin;
            num_x_ = w;
            num_y_ = h;
            start_x_ = sx;
            start_y_ = sy;
            roi_dirty_ = false;
            format_dirty_ = false;

            reset_exposure_state_locked();
            connected_.store(true);
            return;
        }

        // Disconnecting.
        exposure_active_.store(false);
        if (camera_info_valid_ && camera_info_.camera_id >= 0) {
            try { sdk.stop_exposure(camera_info_.camera_id); } catch (...) {}
            sdk.close_camera(camera_info_.camera_id);
        }
        camera_info_ = {};
        camera_info_valid_ = false;
        caps_ = {};
        reset_exposure_state_locked();
        connected_.store(false);
    }

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        state.push_back({"Connected", connected_.load()});
        if (!connected_.load()) return state;
        state.push_back({"CameraState", static_cast<std::int32_t>(get_camera_state())});
        if (cooler_available_copy()) {
            try { state.push_back({"CCDTemperature", get_ccd_temperature()}); } catch (...) {}
            try { state.push_back({"CoolerOn", get_cooler_on()}); } catch (...) {}
            try { state.push_back({"CoolerPower", get_cooler_power()}); } catch (...) {}
        }
        state.push_back({"ImageReady", get_image_ready()});
        if (exposure_active_.load()) {
            state.push_back({"PercentCompleted", get_percent_completed()});
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

    int get_bayer_offset_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not supported",
                                  AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer).first;
    }
    int get_bayer_offset_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not supported",
                                  AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer).second;
    }

    int get_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_;
    }
    void set_bin_x(int bin_x) override { set_bin_common(bin_x, bin_x); }
    int get_bin_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_;
    }
    void set_bin_y(int bin_y) override { set_bin_common(bin_y, bin_y); }

    CameraState get_camera_state() const override {
        if (!connected_.load()) return CameraState::Idle;
        if (exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_deadline_valid_ &&
                std::chrono::steady_clock::now() >= exposure_deadline_) {
                ALPACA_LOG_WARN("PlayerOne",
                    "Exposure deadline exceeded; forcing CameraState=Idle.");
                exposure_active_.store(false);
                exposure_deadline_valid_ = false;
                return CameraState::Idle;
            }
            return CameraState::Exposing;
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

    bool get_can_abort_exposure() const override { return true; }
    bool get_can_asymmetric_bin() const override { return false; }
    bool get_can_fast_readout() const override { return false; }
    bool get_can_get_cooler_power() const override { return cooler_available_copy(); }
    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_st4_port;
    }
    bool get_can_set_ccd_temperature() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_cooler && caps_.has_target_temp;
    }
    bool get_can_stop_exposure() const override { return true; }

    double get_ccd_temperature() const override {
        ensure_connected();
        int id = camera_id_copy();
        auto& sdk = PlayerOneSDKWrapper::instance();
        return sdk.get_temperature_c(id);
    }

    bool get_cooler_on() const override {
        ensure_connected();
        if (!cooler_available_copy()) return false;
        return PlayerOneSDKWrapper::instance().get_cooler_on(camera_id_copy());
    }
    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (!cooler_available_copy()) {
            if (cooler_on) {
                throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
            }
            return;
        }
        PlayerOneSDKWrapper::instance().set_cooler_on(camera_id_copy(), cooler_on);
    }
    double get_cooler_power() const override {
        ensure_connected();
        if (!cooler_available_copy()) return 0.0;
        try {
            int pct = PlayerOneSDKWrapper::instance()
                          .get_cooler_power_percent(camera_id_copy());
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            return static_cast<double>(pct);
        } catch (const std::exception&) {
            return 0.0;
        }
    }

    double get_electrons_per_adu() const override {
        if (!connected_.load()) return 1.0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_egain) return 1.0;
        try {
            return PlayerOneSDKWrapper::instance().get_egain(camera_info_.camera_id);
        } catch (const std::exception&) {
            return 1.0;
        }
    }

    double get_exposure_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_exposure) {
            throw AlpacaException("Exposure not supported", AlpacaError::NotImplemented);
        }
        return static_cast<double>(caps_.exposure_max_us) / 1'000'000.0;
    }
    double get_exposure_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_exposure) {
            throw AlpacaException("Exposure not supported", AlpacaError::NotImplemented);
        }
        return static_cast<double>(caps_.exposure_min_us) / 1'000'000.0;
    }
    double get_exposure_resolution() const override { return 0.000001; }

    bool get_fast_readout() const override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }
    void set_fast_readout(bool) override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    double get_full_well_capacity() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.bit_depth <= 0) return 0.0;
        return static_cast<double>((1ULL << camera_info_.bit_depth) - 1ULL);
    }

    int get_gain() const override {
        ensure_connected();
        return static_cast<int>(
            PlayerOneSDKWrapper::instance().get_config_int(camera_id_copy(), /*POA_GAIN=*/1));
    }
    void set_gain(int gain) override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_gain) {
            throw AlpacaException("Gain not supported", AlpacaError::PropertyNotImplemented);
        }
        if (!caps_.gain_writable) {
            throw AlpacaException("Gain is read-only", AlpacaError::NotImplemented);
        }
        if (gain < caps_.gain_min || gain > caps_.gain_max) {
            throw AlpacaException("Gain out of range", AlpacaError::InvalidValue);
        }
        PlayerOneSDKWrapper::instance().set_config_int(camera_info_.camera_id, /*POA_GAIN=*/1,
                                                       static_cast<long>(gain), false);
    }
    int get_gain_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_gain) {
            throw AlpacaException("Gain not supported", AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(caps_.gain_max);
    }
    int get_gain_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_gain) {
            throw AlpacaException("Gain not supported", AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(caps_.gain_min);
    }
    std::vector<std::string> get_gains() const override {
        throw AlpacaException("Gain descriptions not supported",
                              AlpacaError::PropertyNotImplemented);
    }

    bool get_has_shutter() const override { return false; }
    double get_heat_sink_temperature() const override {
        throw AlpacaException("Heat sink temperature not supported",
                              AlpacaError::NotImplemented);
    }

    ImageArray get_image_array() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_ || !image_ready_ || !image_cached_) {
            throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
        }
        return last_image_;
    }
    std::string get_image_array_variant() const override { return "Int32"; }

    bool get_image_ready() const override {
        if (!connected_.load()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return last_exposure_valid_ && image_ready_ && image_cached_;
    }

    bool get_is_pulse_guiding() const override { return pulse_guiding_.load(); }

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
        if (!camera_info_valid_ || camera_info_.bit_depth <= 0) return 0;
        return static_cast<int>((1ULL << camera_info_.bit_depth) - 1ULL);
    }

    int get_max_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_.supported_bins.empty()) return 1;
        return *std::max_element(camera_info_.supported_bins.begin(),
                                 camera_info_.supported_bins.end());
    }
    int get_max_bin_y() const override { return get_max_bin_x(); }

    int get_num_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_x_;
    }
    void set_num_x(int num_x) override { set_roi_size_common(num_x, get_num_y()); }
    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }
    void set_num_y(int num_y) override { set_roi_size_common(get_num_x(), num_y); }

    int get_offset() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_offset) {
            throw AlpacaException("Offset not supported",
                                  AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(
            PlayerOneSDKWrapper::instance().get_config_int(camera_info_.camera_id, /*POA_OFFSET=*/7));
    }
    void set_offset(int offset) override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_offset) {
            throw AlpacaException("Offset not supported",
                                  AlpacaError::PropertyNotImplemented);
        }
        if (!caps_.offset_writable) {
            throw AlpacaException("Offset is read-only", AlpacaError::NotImplemented);
        }
        if (offset < caps_.offset_min || offset > caps_.offset_max) {
            throw AlpacaException("Offset out of range", AlpacaError::InvalidValue);
        }
        PlayerOneSDKWrapper::instance().set_config_int(camera_info_.camera_id, /*POA_OFFSET=*/7,
                                                       static_cast<long>(offset), false);
    }
    int get_offset_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_offset) {
            throw AlpacaException("Offset not supported",
                                  AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(caps_.offset_max);
    }
    int get_offset_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!caps_.has_offset) {
            throw AlpacaException("Offset not supported",
                                  AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(caps_.offset_min);
    }
    std::vector<std::string> get_offsets() const override {
        throw AlpacaException("Offset descriptions not supported",
                              AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        if (!connected_.load()) return 0.0;
        if (!exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            return image_ready_ ? 100.0 : 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_exposure_duration_ <= 0.0) return 0.0;
        auto now = std::chrono::system_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_exposure_start_).count();
        double pct = (elapsed / last_exposure_duration_) * 100.0;
        if (pct < 0.0) return 0.0;
        if (pct > 100.0) return 100.0;
        return pct;
    }

    double get_pixel_size_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_um : 0.0;
    }
    double get_pixel_size_y() const override {
        // Player One reports a single square pixel size, so X == Y.
        return get_pixel_size_x();
    }

    int get_readout_mode() const override { return 0; }
    void set_readout_mode(int mode) override {
        if (mode != 0) {
            throw AlpacaException("Readout mode not supported", AlpacaError::NotImplemented);
        }
    }
    std::vector<std::string> get_readout_modes() const override { return {"Normal"}; }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.sensor_model.empty()) {
            return camera_info_.sensor_model;
        }
        return "Player One Sensor";
    }
    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) return SensorType::Monochrome;
        return bayer_to_sensor_type(camera_info_.bayer);
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        if (!get_can_set_ccd_temperature()) {
            throw AlpacaException("Set CCD temperature not supported",
                                  AlpacaError::NotImplemented);
        }
        return static_cast<double>(
            PlayerOneSDKWrapper::instance().get_target_temp_c(camera_id_copy()));
    }
    void set_set_ccd_temperature(double temperature) override {
        ensure_connected();
        if (!get_can_set_ccd_temperature()) {
            throw AlpacaException("Set CCD temperature not supported",
                                  AlpacaError::NotImplemented);
        }
        int target_c = static_cast<int>(std::lround(temperature));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (target_c < caps_.target_temp_min || target_c > caps_.target_temp_max) {
                throw AlpacaException("Target temperature out of range",
                                      AlpacaError::InvalidValue);
            }
        }
        PlayerOneSDKWrapper::instance().set_target_temp_c(camera_id_copy(), target_c);
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }
    void set_start_x(int start_x) override { set_start_pos_common(start_x, get_start_y()); }
    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }
    void set_start_y(int start_y) override { set_start_pos_common(get_start_x(), start_y); }

    double get_sub_exposure_duration() const override {
        throw AlpacaException("Sub-exposure duration not supported",
                              AlpacaError::NotImplemented);
    }
    void set_sub_exposure_duration(double) override {
        throw AlpacaException("Sub-exposure duration not supported",
                              AlpacaError::NotImplemented);
    }

    void abort_exposure() override { stop_exposure(); }

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
        PlayerOneGuideDirection dir = PlayerOneGuideDirection::North;
        switch (direction) {
        case 0: dir = PlayerOneGuideDirection::North; break;
        case 1: dir = PlayerOneGuideDirection::South; break;
        case 2: dir = PlayerOneGuideDirection::East;  break;
        case 3: dir = PlayerOneGuideDirection::West;  break;
        }

        int id = camera_id_copy();
        auto& sdk = PlayerOneSDKWrapper::instance();
        sdk.pulse_guide_on(id, dir);
        pulse_guiding_.store(true);
        std::thread([id, dir, duration]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            try {
                PlayerOneSDKWrapper::instance().pulse_guide_off(id, dir);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne",
                    "pulse_guide_off failed: " + std::string(e.what()));
            }
        }).detach();
        // Clear the "guiding" flag on the same schedule the SDK was told to stop.
        std::thread([this, duration]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            pulse_guiding_.store(false);
        }).detach();
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        (void)light; // Player One has no mechanical shutter.

        if (duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative",
                                  AlpacaError::InvalidValue);
        }

        long exposure_us_long = static_cast<long>(std::lround(duration * 1'000'000.0));
        int id = 0;
        int active_bin = 0;
        int active_start_x = 0;
        int active_start_y = 0;
        int active_num_x = 0;
        int active_num_y = 0;
        bool dirty_format = false;
        bool dirty_roi = false;
        PlayerOneImageFormat active_format = PlayerOneImageFormat::Raw16;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!camera_info_valid_) {
                throw AlpacaException("Camera info not valid", AlpacaError::DriverException);
            }
            if (!caps_.has_exposure) {
                throw AlpacaException("Exposure not supported", AlpacaError::NotImplemented);
            }
            if (exposure_us_long < caps_.exposure_min_us) exposure_us_long = caps_.exposure_min_us;
            if (exposure_us_long > caps_.exposure_max_us) {
                throw AlpacaException("Exposure duration out of range",
                                      AlpacaError::InvalidValue);
            }
            id = camera_info_.camera_id;
            active_bin = bin_;
            active_start_x = start_x_;
            active_start_y = start_y_;
            active_num_x = num_x_;
            active_num_y = num_y_;
            dirty_format = format_dirty_;
            dirty_roi = roi_dirty_;
            active_format = active_format_;

            int max_w = camera_info_.max_width / active_bin;
            int max_h = camera_info_.max_height / active_bin;
            if (active_num_x <= 0 || active_num_y <= 0) {
                throw AlpacaException("ROI not valid", AlpacaError::InvalidValue);
            }
            if (active_num_x > max_w || active_num_y > max_h) {
                throw AlpacaException("ROI size exceeds sensor dimensions",
                                      AlpacaError::InvalidValue);
            }
            if (active_start_x < 0 || active_start_y < 0 ||
                active_start_x + active_num_x > max_w ||
                active_start_y + active_num_y > max_h) {
                throw AlpacaException("ROI extends beyond sensor bounds",
                                      AlpacaError::InvalidValue);
            }
        }

        unsigned exposure_us = static_cast<unsigned>(exposure_us_long);

        stop_exposure_thread();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_exposure_duration_ = duration;
            last_exposure_start_ = std::chrono::system_clock::now();
            last_exposure_valid_ = true;
            image_ready_ = false;
            image_cached_ = false;
            exposure_deadline_ = std::chrono::steady_clock::now() +
                std::chrono::microseconds(exposure_us) +
                std::chrono::seconds(15);
            exposure_deadline_valid_ = true;
        }

        exposure_active_.store(true);

        exposure_thread_ = std::thread([this, id, exposure_us,
                                        active_bin, active_start_x, active_start_y,
                                        active_num_x, active_num_y,
                                        dirty_format, dirty_roi, active_format]() {
            auto& sdk = PlayerOneSDKWrapper::instance();
            try {
                // Reconfiguring bin/format requires the camera be in STATE_OPENED
                // (not exposing). Stop defensively in case a prior exposure was
                // still running.
                if (dirty_format || dirty_roi) {
                    try { sdk.stop_exposure(id); } catch (const std::exception&) {}
                }

                if (dirty_format) {
                    try {
                        sdk.set_image_format(id, active_format);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("PlayerOne",
                            std::string("set_image_format failed: ") + e.what());
                    }
                    sdk.set_image_bin(id, active_bin);
                    std::lock_guard<std::mutex> lock(mutex_);
                    format_dirty_ = false;
                }

                // Player One SDK requires width%4==0, height%2==0. Align DOWN
                // for the SDK call — the returned ImageArray is built at the
                // user's requested dims so clients see what they set.
                int sdk_w = active_num_x - (active_num_x % 4);
                int sdk_h = active_num_y - (active_num_y % 2);
                if (sdk_w <= 0) sdk_w = 4;
                if (sdk_h <= 0) sdk_h = 2;

                if (dirty_roi) {
                    // Order: size first (inside sensor at current bin), then
                    // start. POASetImageBin resets size/start so we always
                    // push both again.
                    sdk.set_image_size(id, sdk_w, sdk_h);
                    sdk.set_image_start_pos(id, active_start_x, active_start_y);
                    // Read back — SDK may have further aligned values.
                    try { sdk.get_image_size(id, sdk_w, sdk_h); } catch (...) {}
                    int rx = active_start_x;
                    int ry = active_start_y;
                    try { sdk.get_image_start_pos(id, rx, ry); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        start_x_ = rx;
                        start_y_ = ry;
                        roi_dirty_ = false;
                    }
                }

                // Set the exposure — POA_EXPOSURE's value is microseconds (long).
                sdk.set_config_int(id, /*POA_EXPOSURE=*/0,
                                   static_cast<long>(exposure_us), false);

                sdk.start_exposure(id, /*single_frame=*/true);

                // Snapshot ROI (after any SDK alignment) so the buffer size
                // matches the image the SDK will deliver.
                int sdk_frame_w = sdk_w;
                int sdk_frame_h = sdk_h;
                try { sdk.get_image_size(id, sdk_frame_w, sdk_frame_h); } catch (...) {}

                const bool is_16 = format_is_16bit(active_format);
                const std::size_t bytes_per_pixel = is_16 ? 2u
                    : (active_format == PlayerOneImageFormat::Rgb24 ? 3u : 1u);
                const std::size_t buffer_bytes =
                    static_cast<std::size_t>(sdk_frame_w) *
                    static_cast<std::size_t>(sdk_frame_h) * bytes_per_pixel;

                // Poll for image readiness so the abort path can short-circuit
                // without a long blocking get_image_data.
                auto poll_deadline = std::chrono::steady_clock::now() +
                    std::chrono::microseconds(exposure_us) +
                    std::chrono::seconds(15);

                bool ready = false;
                while (exposure_active_.load() &&
                       std::chrono::steady_clock::now() < poll_deadline) {
                    try {
                        if (sdk.image_ready(id)) { ready = true; break; }
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("PlayerOne",
                            std::string("image_ready failed: ") + e.what());
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                if (!ready || !exposure_active_.load()) {
                    ALPACA_LOG_WARN("PlayerOne",
                        "Exposure aborted or did not complete before deadline");
                    try { sdk.stop_exposure(id); } catch (...) {}
                    exposure_active_.store(false);
                    return;
                }

                std::vector<std::uint8_t> raw(buffer_bytes, 0);
                // Short timeout since the image is already marked ready.
                bool got = sdk.get_image_data(id, raw.data(), raw.size(), 5000);
                if (!got) {
                    ALPACA_LOG_WARN("PlayerOne", "get_image_data timed out after ready=true");
                    exposure_active_.store(false);
                    return;
                }

                ImageArray img = build_image_array(raw,
                                                   sdk_frame_w, sdk_frame_h,
                                                   active_num_x, active_num_y,
                                                   active_format);

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_image_ = std::move(img);
                    image_cached_ = true;
                    image_ready_ = true;
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Exposure failed: " + std::string(e.what()));
            }
            exposure_active_.store(false);
        });
    }

    void stop_exposure() override {
        ensure_connected();
        exposure_active_.store(false);
        if (exposure_thread_.joinable()) exposure_thread_.join();
        try {
            PlayerOneSDKWrapper::instance().stop_exposure(camera_id_copy());
        } catch (const std::exception&) {}
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        image_cached_ = false;
        exposure_deadline_valid_ = false;
    }

private:
    int device_number_;
    int camera_index_;
    PlayerOneCameraInfo camera_info_{};
    bool camera_info_valid_{false};
    PlayerOneConfigCaps caps_{};

    std::atomic<bool> connected_{false};
    std::atomic<bool> connecting_{false};
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;

    int bin_{1};
    int start_x_{0};
    int start_y_{0};
    int num_x_{0};
    int num_y_{0};
    bool roi_dirty_{false};
    bool format_dirty_{false};
    PlayerOneImageFormat active_format_{PlayerOneImageFormat::Raw16};

    mutable bool image_ready_{false};
    mutable bool image_cached_{false};
    mutable ImageArray last_image_{};
    double last_exposure_duration_{0.0};
    std::chrono::system_clock::time_point last_exposure_start_{};
    bool last_exposure_valid_{false};

    mutable std::atomic<bool> exposure_active_{false};
    std::thread exposure_thread_;
    mutable std::chrono::steady_clock::time_point exposure_deadline_{};
    mutable bool exposure_deadline_valid_{false};

    std::atomic<bool> pulse_guiding_{false};

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    int camera_id_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.camera_id < 0) {
            throw AlpacaException("Camera ID not available", AlpacaError::NotConnected);
        }
        return camera_info_.camera_id;
    }

    bool cooler_available_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_cooler && caps_.has_cooler;
    }

    void reset_exposure_state_locked() {
        image_ready_ = false;
        image_cached_ = false;
        last_exposure_duration_ = 0.0;
        last_exposure_start_ = std::chrono::system_clock::time_point{};
        last_exposure_valid_ = false;
        exposure_active_.store(false);
        exposure_deadline_valid_ = false;
    }

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connecting_.load()) return;
        if (connection_thread_.joinable()) connection_thread_.join();
        connecting_.store(true);
        connection_thread_ = std::thread([this, connect]() {
            try {
                set_connected(connect);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR("PlayerOne",
                                 "Connection task failed: " + std::string(e.what()));
            }
            connecting_.store(false);
        });
    }

    void stop_connection_thread() {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connection_thread_.joinable()) connection_thread_.join();
    }

    void stop_exposure_thread() {
        exposure_active_.store(false);
        if (exposure_thread_.joinable()) exposure_thread_.join();
    }

    void preload_camera_info() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            auto cameras = PlayerOneSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("PlayerOne",
                             "Preload enumerate failed: " + std::string(e.what()));
        }
    }

    void refresh_cached_camera_info_if_needed() {
        if (connected_.load()) return;
        try {
            auto cameras = PlayerOneSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                std::lock_guard<std::mutex> lock(mutex_);
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception&) {}
    }

    void set_bin_common(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_) {
            throw AlpacaException("Camera info not valid", AlpacaError::DriverException);
        }
        if (std::find(camera_info_.supported_bins.begin(),
                      camera_info_.supported_bins.end(),
                      bin_x) == camera_info_.supported_bins.end()) {
            throw AlpacaException("Bin value not supported", AlpacaError::InvalidValue);
        }
        if (bin_ == bin_x) return;
        bin_ = bin_x;
        // Binning in the Player One SDK resets size/start — mirror that in
        // our cache so the driver's next set_num_x/y uses the right base.
        // Align to SDK requirements (width%4==0, height%2==0).
        int w = camera_info_.max_width / bin_;
        int h = camera_info_.max_height / bin_;
        num_x_ = w - (w % 4);
        num_y_ = h - (h % 2);
        start_x_ = 0;
        start_y_ = 0;
        format_dirty_ = true;
        roi_dirty_ = true;
        image_cached_ = false;
    }

    void set_roi_size_common(int width, int height) {
        ensure_connected();
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        // ASCOM convention: setters are lenient, StartExposure does the
        // sensor-bounds validation. ConformU's "Reject Bad XSize/YSize" tests
        // set values beyond sensor dimensions and expect the error at
        // StartExposure rather than here.
        std::lock_guard<std::mutex> lock(mutex_);
        if (num_x_ == width && num_y_ == height) return;
        num_x_ = width;
        num_y_ = height;
        roi_dirty_ = true;
        image_cached_ = false;
    }

    void set_start_pos_common(int sx, int sy) {
        ensure_connected();
        if (sx < 0 || sy < 0) {
            throw AlpacaException("Start position must be non-negative",
                                  AlpacaError::InvalidValue);
        }
        // ASCOM convention: setters are lenient, StartExposure validates.
        std::lock_guard<std::mutex> lock(mutex_);
        if (start_x_ == sx && start_y_ == sy) return;
        start_x_ = sx;
        start_y_ = sy;
        roi_dirty_ = true;
    }

    ImageArray build_image_array(const std::vector<std::uint8_t>& buffer,
                                  int src_w, int src_h,
                                  int out_w, int out_h,
                                  PlayerOneImageFormat format) const {
        ImageArray image;
        image.width = out_w;
        image.height = out_h;
        image.rank = 2;
        if (out_w <= 0 || out_h <= 0 || src_w <= 0 || src_h <= 0) {
            image.rank = 0;
            return image;
        }
        // The SDK buffer is src_w × src_h (aligned). The returned ImageArray is
        // out_w × out_h (what the client requested). Copy the overlapping
        // region; any trailing row/col is left zeroed.
        const int copy_w = std::min(src_w, out_w);
        const int copy_h = std::min(src_h, out_h);
        const std::size_t out_pixels =
            static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h);

        if (format == PlayerOneImageFormat::Raw16) {
            image.data.assign(out_pixels, 0);
            const std::size_t src_stride = static_cast<std::size_t>(src_w) * 2;
            for (int y = 0; y < copy_h; ++y) {
                const std::size_t row_off = static_cast<std::size_t>(y) * src_stride;
                for (int x = 0; x < copy_w; ++x) {
                    const std::size_t j = row_off + static_cast<std::size_t>(x) * 2;
                    if (j + 1 >= buffer.size()) break;
                    std::uint16_t px = static_cast<std::uint16_t>(buffer[j]) |
                                       (static_cast<std::uint16_t>(buffer[j + 1]) << 8);
                    image.data[static_cast<std::size_t>(y) * out_w + x] =
                        static_cast<std::int32_t>(px);
                }
            }
        } else if (format == PlayerOneImageFormat::Raw8 ||
                   format == PlayerOneImageFormat::Mono8) {
            image.data.assign(out_pixels, 0);
            const std::size_t src_stride = static_cast<std::size_t>(src_w);
            for (int y = 0; y < copy_h; ++y) {
                const std::size_t row_off = static_cast<std::size_t>(y) * src_stride;
                for (int x = 0; x < copy_w; ++x) {
                    const std::size_t j = row_off + static_cast<std::size_t>(x);
                    if (j >= buffer.size()) break;
                    image.data[static_cast<std::size_t>(y) * out_w + x] =
                        static_cast<std::int32_t>(buffer[j]);
                }
            }
        } else if (format == PlayerOneImageFormat::Rgb24) {
            // Present RGB24 as rank-3 (H x W x 3). Pack each channel into its
            // plane as ASCOM clients expect.
            image.rank = 3;
            image.data.assign(out_pixels * 3, 0);
            const std::size_t src_stride = static_cast<std::size_t>(src_w) * 3;
            for (int y = 0; y < copy_h; ++y) {
                const std::size_t row_off = static_cast<std::size_t>(y) * src_stride;
                for (int x = 0; x < copy_w; ++x) {
                    const std::size_t j = row_off + static_cast<std::size_t>(x) * 3;
                    if (j + 2 >= buffer.size()) break;
                    // Player One RGB24 is stored as B,G,R per pixel; transpose
                    // to R,G,B for Alpaca clients.
                    std::int32_t b = buffer[j];
                    std::int32_t g = buffer[j + 1];
                    std::int32_t r = buffer[j + 2];
                    const std::size_t out_i =
                        (static_cast<std::size_t>(y) * out_w + x) * 3;
                    image.data[out_i + 0] = r;
                    image.data[out_i + 1] = g;
                    image.data[out_i + 2] = b;
                }
            }
        }
        return image;
    }
};

std::unique_ptr<CameraDriver> create_playerone_camera(int device_number, int camera_index) {
    return std::make_unique<PlayerOneCameraDriver>(device_number, camera_index);
}

} // namespace alpacacore::vendor::playerone
