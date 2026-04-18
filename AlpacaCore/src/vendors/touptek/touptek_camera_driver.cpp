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

#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::touptek {

namespace {

// FourCC codes reported by Toupcam_get_RawFormat. Bayer layouts here are for
// the raw sensor (not the output after any rotation/flip).
constexpr unsigned kFourCC_RGGB = 0x42474752; // 'R','G','G','B'
constexpr unsigned kFourCC_BGGR = 0x52474742; // 'B','G','G','R'
constexpr unsigned kFourCC_GRBG = 0x47425247; // 'G','R','B','G'
constexpr unsigned kFourCC_GBRG = 0x47524247; // 'G','B','R','G'
constexpr unsigned kFourCC_YYYY = 0x59595959; // mono

ToupBayerPattern four_cc_to_bayer(unsigned four_cc) {
    switch (four_cc) {
    case kFourCC_RGGB: return ToupBayerPattern::RG;
    case kFourCC_BGGR: return ToupBayerPattern::BG;
    case kFourCC_GRBG: return ToupBayerPattern::GR;
    case kFourCC_GBRG: return ToupBayerPattern::GB;
    default:           return ToupBayerPattern::None;
    }
}

std::pair<int, int> bayer_offsets(ToupBayerPattern pattern) {
    switch (pattern) {
    case ToupBayerPattern::RG: return {0, 0};
    case ToupBayerPattern::BG: return {1, 1};
    case ToupBayerPattern::GR: return {1, 0};
    case ToupBayerPattern::GB: return {0, 1};
    default:                   return {0, 0};
    }
}

} // namespace

class ToupTekCameraDriver : public CameraDriver {
public:
    ToupTekCameraDriver(int device_number, int camera_index)
        : device_number_(device_number)
        , camera_index_(camera_index)
        , handle_(nullptr)
        , camera_info_()
        , camera_info_valid_(false)
        , serial_number_()
        , firmware_version_()
        , connected_(false)
        , connecting_(false)
        , bin_(1)
        , start_x_(0)
        , start_y_(0)
        , num_x_(0)
        , num_y_(0)
        , image_ready_(false)
        , image_cached_(false)
        , last_image_()
        , last_exposure_duration_(0.0)
        , last_exposure_start_()
        , last_exposure_valid_(false)
        , exposure_active_(false)
        , pulse_guiding_(false)
    {
        preload_camera_info();
    }

    ~ToupTekCameraDriver() override {
        stop_connection_thread();
        stop_exposure_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "Error during destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        const_cast<ToupTekCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.name.empty()) {
            return camera_info_.name;
        }
        return "ToupTek Camera";
    }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "TOUPTEK_SN_" + serial_number_;
        }
        return "TOUPTEK_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "ToupTek Camera Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore ToupTek Camera Driver"; }
    std::string get_driver_version() const override { return "1.0.0"; }
    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected == connected_.load()) {
            if (connected) {
                reset_exposure_state_locked();
            }
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
            camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
            camera_info_valid_ = true;

            ALPACA_LOG_INFO("ToupTek", "SDK version: " + sdk.get_sdk_version());
            ALPACA_LOG_INFO("ToupTek", "Opening camera index " +
                std::to_string(camera_index_) + ": " + camera_info_.name);

            handle_ = sdk.open_camera_by_id(camera_info_.id);

            // Configure: disable auto-exposure, enable RAW output, select
            // 16-bit bitdepth when the sensor supports it, and arm software
            // trigger mode before starting pull-mode streaming.
            try {
                sdk.put_auto_exposure(handle_, false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "put_AutoExpoEnable failed: " + std::string(e.what()));
            }

            try {
                sdk.put_raw(handle_, 1);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "put_Option(RAW) failed: " + std::string(e.what()));
            }

            if (camera_info_.bit_depth_max > 8) {
                try {
                    sdk.put_bitdepth(handle_, 1);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ToupTek", "put_Option(BITDEPTH=1) failed: " +
                                               std::string(e.what()));
                }
            }

            sdk.put_trigger_mode(handle_, 1); // software trigger

            // Refresh frame dimensions and Bayer pattern from the open camera.
            int width = 0;
            int height = 0;
            try {
                sdk.get_size(handle_, width, height);
            } catch (const std::exception&) {}
            if (width > 0 && height > 0) {
                camera_info_.max_width = width;
                camera_info_.max_height = height;
            }

            try {
                unsigned four_cc = 0;
                unsigned bpp = 0;
                sdk.get_raw_format(handle_, four_cc, bpp);
                if (four_cc == kFourCC_YYYY || (camera_info_.flags & 0x10) /* FLAG_MONO */) {
                    camera_info_.is_color = false;
                    camera_info_.bayer = ToupBayerPattern::None;
                } else {
                    camera_info_.is_color = true;
                    camera_info_.bayer = four_cc_to_bayer(four_cc);
                }
                if (bpp > 0) {
                    camera_info_.bit_depth_max = static_cast<int>(bpp);
                }
            } catch (const std::exception&) {}

            try {
                float px = 0.0f;
                float py = 0.0f;
                sdk.get_pixel_size(handle_, 0xffffffffu, px, py);
                if (px > 0.0f) camera_info_.pixel_size_um_x = px;
                if (py > 0.0f) camera_info_.pixel_size_um_y = py;
            } catch (const std::exception&) {}

            serial_number_ = sdk.get_serial_number(handle_);
            firmware_version_ = sdk.get_firmware_version(handle_);

            bin_ = 1;
            start_x_ = 0;
            start_y_ = 0;
            num_x_ = camera_info_.max_width;
            num_y_ = camera_info_.max_height;
            roi_dirty_ = false;
            format_dirty_ = false;

            try {
                sdk.start_pull_mode(handle_, &ToupTekCameraDriver::on_event_static, this);
            } catch (const std::exception& e) {
                sdk.close_camera(handle_);
                handle_ = nullptr;
                throw AlpacaException(std::string("Failed to start pull mode: ") + e.what(),
                                      AlpacaError::DriverException);
            }

            reset_exposure_state_locked();
            connected_.store(true);
            return;
        }

        // Disconnecting.
        exposure_active_.store(false);
        if (handle_) {
            try { sdk.stop(handle_); } catch (const std::exception&) {}
            sdk.close_camera(handle_);
            handle_ = nullptr;
        }
        camera_info_ = {};
        camera_info_valid_ = false;
        serial_number_.clear();
        firmware_version_.clear();
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
        if (supports_cooler_locked_copy()) {
            state.push_back({"CCDTemperature", get_ccd_temperature()});
            state.push_back({"CoolerOn", get_cooler_on()});
        }
        state.push_back({"ImageReady", get_image_ready()});
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
            throw AlpacaException("Bayer offsets not supported", AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer).first;
    }
    int get_bayer_offset_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not supported", AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer).second;
    }

    int get_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_;
    }
    void set_bin_x(int bin_x) override { set_bin_locked(bin_x, bin_x); }
    int get_bin_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_;
    }
    void set_bin_y(int bin_y) override { set_bin_locked(bin_y, bin_y); }

    CameraState get_camera_state() const override {
        if (!connected_.load()) return CameraState::Idle;
        if (exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_deadline_valid_ &&
                std::chrono::steady_clock::now() >= exposure_deadline_) {
                ALPACA_LOG_WARN("ToupTek",
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
    bool get_can_get_cooler_power() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_cooler;
    }
    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_pulse_guide;
    }
    bool get_can_set_ccd_temperature() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_tec_onoff;
    }
    bool get_can_stop_exposure() const override { return true; }

    double get_ccd_temperature() const override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
        int deci_c = sdk.get_temperature_deciC(handle_copy());
        return static_cast<double>(deci_c) / 10.0;
    }

    bool get_cooler_on() const override {
        ensure_connected();
        if (!get_can_get_cooler_power()) return false;
        return ToupTekSDKWrapper::instance().get_tec_enable(handle_copy());
    }
    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (!get_can_get_cooler_power()) {
            if (cooler_on) {
                throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
            }
            return;
        }
        ToupTekSDKWrapper::instance().put_tec_enable(handle_copy(), cooler_on);
    }
    double get_cooler_power() const override {
        ensure_connected();
        if (!get_can_get_cooler_power()) return 0.0;
        auto& sdk = ToupTekSDKWrapper::instance();
        try {
            int v = sdk.get_tec_voltage_deciV(handle_copy());
            int vmax = sdk.get_tec_voltage_max_deciV(handle_copy());
            if (vmax <= 0) return 0.0;
            double pct = (static_cast<double>(v) / static_cast<double>(vmax)) * 100.0;
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            return pct;
        } catch (const std::exception&) {
            return 0.0;
        }
    }

    double get_electrons_per_adu() const override { return 1.0; }

    double get_exposure_max() const override {
        ensure_connected();
        auto r = ToupTekSDKWrapper::instance().get_exposure_range(handle_copy());
        return static_cast<double>(r.max_us) / 1'000'000.0;
    }
    double get_exposure_min() const override {
        ensure_connected();
        auto r = ToupTekSDKWrapper::instance().get_exposure_range(handle_copy());
        return static_cast<double>(r.min_us) / 1'000'000.0;
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
        if (!camera_info_valid_ || camera_info_.bit_depth_max <= 0) return 0.0;
        return static_cast<double>((1ULL << camera_info_.bit_depth_max) - 1ULL);
    }

    int get_gain() const override {
        ensure_connected();
        return static_cast<int>(ToupTekSDKWrapper::instance().get_gain(handle_copy()));
    }
    void set_gain(int gain) override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
        auto range = sdk.get_gain_range(handle_copy());
        if (gain < range.min || gain > range.max) {
            throw AlpacaException("Gain out of range", AlpacaError::InvalidValue);
        }
        sdk.put_gain(handle_copy(), static_cast<unsigned short>(gain));
    }
    int get_gain_max() const override {
        ensure_connected();
        return static_cast<int>(ToupTekSDKWrapper::instance().get_gain_range(handle_copy()).max);
    }
    int get_gain_min() const override {
        ensure_connected();
        return static_cast<int>(ToupTekSDKWrapper::instance().get_gain_range(handle_copy()).min);
    }
    std::vector<std::string> get_gains() const override {
        throw AlpacaException("Gain descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_has_shutter() const override { return false; }

    double get_heat_sink_temperature() const override { return get_ccd_temperature(); }

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

    bool get_is_pulse_guiding() const override {
        if (!connected_.load()) return false;
        if (!pulse_guiding_.load()) return false;
        try {
            bool guiding = ToupTekSDKWrapper::instance().is_guiding(handle_copy());
            if (!guiding) pulse_guiding_.store(false);
            return guiding;
        } catch (const std::exception&) {
            return false;
        }
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
        if (!camera_info_valid_ || camera_info_.bit_depth_max <= 0) return 0;
        return static_cast<int>((1ULL << camera_info_.bit_depth_max) - 1ULL);
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
    void set_num_x(int num_x) override { set_roi_size_locked(num_x, get_num_y()); }
    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }
    void set_num_y(int num_y) override { set_roi_size_locked(get_num_x(), num_y); }

    int get_offset() const override {
        throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
    }
    void set_offset(int) override {
        throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
    }
    int get_offset_max() const override {
        throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
    }
    int get_offset_min() const override {
        throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
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
        return camera_info_valid_ ? camera_info_.pixel_size_um_x : 0.0;
    }
    double get_pixel_size_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_um_y : 0.0;
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
        if (camera_info_valid_ && !camera_info_.model_name.empty()) {
            return camera_info_.model_name;
        }
        return "ToupTek Sensor";
    }
    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) return SensorType::Monochrome;
        return SensorType::RGGB;
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        int t = ToupTekSDKWrapper::instance().get_tec_target_deciC(handle_copy());
        return static_cast<double>(t) / 10.0;
    }
    void set_set_ccd_temperature(double temperature) override {
        ensure_connected();
        int deci = static_cast<int>(std::lround(temperature * 10.0));
        ToupTekSDKWrapper::instance().put_tec_target_deciC(handle_copy(), deci);
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }
    void set_start_x(int start_x) override { set_start_pos_locked(start_x, get_start_y()); }
    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }
    void set_start_y(int start_y) override { set_start_pos_locked(get_start_x(), start_y); }

    double get_sub_exposure_duration() const override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }
    void set_sub_exposure_duration(double) override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
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
        ToupGuideDirection dir = ToupGuideDirection::North;
        switch (direction) {
        case 0: dir = ToupGuideDirection::North; break;
        case 1: dir = ToupGuideDirection::South; break;
        case 2: dir = ToupGuideDirection::East;  break;
        case 3: dir = ToupGuideDirection::West;  break;
        }
        ToupTekSDKWrapper::instance().pulse_guide(handle_copy(), dir,
                                                   static_cast<unsigned>(duration));
        pulse_guiding_.store(true);
        std::thread([this, duration]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            pulse_guiding_.store(false);
        }).detach();
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        (void)light;

        if (duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative", AlpacaError::InvalidValue);
        }

        auto& sdk = ToupTekSDKWrapper::instance();
        HToupcam handle = handle_copy();
        auto range = sdk.get_exposure_range(handle);

        long exposure_us_long = static_cast<long>(std::lround(duration * 1'000'000.0));
        if (exposure_us_long < static_cast<long>(range.min_us)) exposure_us_long = range.min_us;
        if (exposure_us_long > static_cast<long>(range.max_us)) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }
        unsigned exposure_us = static_cast<unsigned>(exposure_us_long);

        int active_bin = 0;
        int active_start_x = 0;
        int active_start_y = 0;
        int active_num_x = 0;
        int active_num_y = 0;
        bool dirty_format = false;
        bool dirty_roi = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!camera_info_valid_) {
                throw AlpacaException("Camera info not valid", AlpacaError::DriverException);
            }
            active_bin = bin_;
            active_start_x = start_x_;
            active_start_y = start_y_;
            active_num_x = num_x_;
            active_num_y = num_y_;
            dirty_format = format_dirty_;
            dirty_roi = roi_dirty_;

            int max_w = camera_info_.max_width / active_bin;
            int max_h = camera_info_.max_height / active_bin;
            if (active_num_x <= 0 || active_num_y <= 0) {
                throw AlpacaException("ROI not valid", AlpacaError::InvalidValue);
            }
            if (active_num_x > max_w || active_num_y > max_h) {
                throw AlpacaException("ROI size exceeds sensor dimensions", AlpacaError::InvalidValue);
            }
            if (active_start_x < 0 || active_start_y < 0 ||
                active_start_x + active_num_x > max_w ||
                active_start_y + active_num_y > max_h) {
                throw AlpacaException("ROI extends beyond sensor bounds", AlpacaError::InvalidValue);
            }
        }

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

        exposure_thread_ = std::thread([this, handle, exposure_us,
                                        active_bin, active_start_x, active_start_y,
                                        active_num_x, active_num_y,
                                        dirty_format, dirty_roi]() {
            auto& sdk_local = ToupTekSDKWrapper::instance();
            try {
                // Reconfiguring bitdepth / pixel format / binning requires the
                // stream to be stopped (SDK returns E_WRONG_THREAD otherwise).
                if (dirty_format) {
                    try { sdk_local.stop(handle); } catch (const std::exception&) {}
                    sdk_local.put_binning(handle, active_bin);
                    sdk_local.put_trigger_mode(handle, 1);
                    sdk_local.start_pull_mode(handle, &ToupTekCameraDriver::on_event_static, this);
                    std::lock_guard<std::mutex> lock(mutex_);
                    format_dirty_ = false;
                }

                if (dirty_roi) {
                    sdk_local.put_roi(handle,
                                      static_cast<unsigned>(active_start_x * active_bin),
                                      static_cast<unsigned>(active_start_y * active_bin),
                                      static_cast<unsigned>(active_num_x * active_bin),
                                      static_cast<unsigned>(active_num_y * active_bin));
                    std::lock_guard<std::mutex> lock(mutex_);
                    roi_dirty_ = false;
                }

                sdk_local.put_exposure_us(handle, exposure_us);
                sdk_local.trigger(handle, 1);

                unsigned timeout_ms = (exposure_us / 1000) + 10000;
                std::vector<std::uint16_t> pixel_buffer(
                    static_cast<std::size_t>(active_num_x) *
                    static_cast<std::size_t>(active_num_y));

                unsigned got_w = 0;
                unsigned got_h = 0;
                bool got = sdk_local.wait_image(handle, timeout_ms,
                                                 pixel_buffer.data(),
                                                 16,
                                                 static_cast<int>(active_num_x * 2),
                                                 got_w, got_h);

                if (!got || !exposure_active_.load()) {
                    ALPACA_LOG_WARN("ToupTek",
                        "Exposure failed or aborted before frame arrived");
                    exposure_active_.store(false);
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_image_ = build_image_array_16bit(pixel_buffer,
                                                          active_num_x, active_num_y,
                                                          static_cast<int>(got_w),
                                                          static_cast<int>(got_h));
                    image_cached_ = true;
                    image_ready_ = true;
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "Exposure failed: " + std::string(e.what()));
            }
            exposure_active_.store(false);
        });
    }

    void stop_exposure() override {
        ensure_connected();
        exposure_active_.store(false);
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        image_cached_ = false;
        exposure_deadline_valid_ = false;
    }

private:
    int device_number_;
    int camera_index_;
    HToupcam handle_;
    ToupCameraInfo camera_info_;
    bool camera_info_valid_;
    std::string serial_number_;
    std::string firmware_version_;

    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;

    int bin_;
    int start_x_;
    int start_y_;
    int num_x_;
    int num_y_;
    bool roi_dirty_{false};
    bool format_dirty_{false};

    mutable bool image_ready_;
    mutable bool image_cached_;
    mutable ImageArray last_image_;
    double last_exposure_duration_;
    std::chrono::system_clock::time_point last_exposure_start_;
    bool last_exposure_valid_;

    mutable std::atomic<bool> exposure_active_;
    std::thread exposure_thread_;
    mutable std::chrono::steady_clock::time_point exposure_deadline_{};
    mutable bool exposure_deadline_valid_{false};

    mutable std::atomic<bool> pulse_guiding_;

    static void on_event_static(unsigned /*event*/, void* /*ctx*/) {
        // Pull-mode callbacks fire on an SDK-owned thread. Do not touch the
        // handle here — close/stop from this context deadlocks (SDK contract).
        // Frame delivery is handled by Toupcam_WaitImageV4 on the exposure
        // thread, so this callback is intentionally a no-op.
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    HToupcam handle_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!handle_) {
            throw AlpacaException("Camera handle is null", AlpacaError::NotConnected);
        }
        return handle_;
    }

    bool supports_cooler_locked_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_cooler;
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
                ALPACA_LOG_ERROR("ToupTek", "Connection task failed: " + std::string(e.what()));
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
            auto cameras = ToupTekSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ToupTek",
                             "Preload enumerate failed: " + std::string(e.what()));
        }
    }

    void refresh_cached_camera_info_if_needed() {
        if (connected_.load()) return;
        try {
            auto cameras = ToupTekSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                std::lock_guard<std::mutex> lock(mutex_);
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception&) {}
    }

    void set_bin_locked(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
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
        num_x_ = camera_info_.max_width / bin_;
        num_y_ = camera_info_.max_height / bin_;
        start_x_ = 0;
        start_y_ = 0;
        format_dirty_ = true;
        roi_dirty_ = true;
        image_cached_ = false;
    }

    void set_roi_size_locked(int width, int height) {
        ensure_connected();
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (num_x_ == width && num_y_ == height) return;
        num_x_ = width;
        num_y_ = height;
        roi_dirty_ = true;
        image_cached_ = false;
    }

    void set_start_pos_locked(int sx, int sy) {
        ensure_connected();
        if (sx < 0 || sy < 0) {
            throw AlpacaException("Start position must be non-negative",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (start_x_ == sx && start_y_ == sy) return;
        start_x_ = sx;
        start_y_ = sy;
        roi_dirty_ = true;
    }

    ImageArray build_image_array_16bit(const std::vector<std::uint16_t>& buffer,
                                        int out_width, int out_height,
                                        int actual_width, int actual_height) const {
        ImageArray image;
        image.width = out_width;
        image.height = out_height;
        image.rank = 2;
        if (out_width <= 0 || out_height <= 0) {
            image.rank = 0;
            return image;
        }
        const int copy_w = std::min(out_width, actual_width > 0 ? actual_width : out_width);
        const int copy_h = std::min(out_height, actual_height > 0 ? actual_height : out_height);
        image.data.assign(static_cast<std::size_t>(out_width) *
                          static_cast<std::size_t>(out_height), 0);
        for (int row = 0; row < copy_h; ++row) {
            for (int col = 0; col < copy_w; ++col) {
                std::size_t src_idx = static_cast<std::size_t>(row) *
                                      static_cast<std::size_t>(out_width) +
                                      static_cast<std::size_t>(col);
                if (src_idx < buffer.size()) {
                    image.data[src_idx] = static_cast<std::int32_t>(buffer[src_idx]);
                }
            }
        }
        return image;
    }
};

std::unique_ptr<CameraDriver> create_touptek_camera(int device_number, int camera_index) {
    return std::make_unique<ToupTekCameraDriver>(device_number, camera_index);
}

} // namespace alpacacore::vendor::touptek
