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

#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace alpacacore::vendor::qhy {

namespace {

// Map Alpaca pulse guide direction to QHY guide direction.
// Alpaca: 0=North, 1=South, 2=East, 3=West
// QHY:    0=EAST,  1=NORTH, 2=SOUTH, 3=WEST
uint32_t alpaca_to_qhy_guide_direction(int alpaca_direction) {
    switch (alpaca_direction) {
    case 0: return guide_direction::NORTH;
    case 1: return guide_direction::SOUTH;
    case 2: return guide_direction::EAST;
    case 3: return guide_direction::WEST;
    default: return guide_direction::NORTH;
    }
}

// Return true if the bin value is supported (1-4 checked via CAM_BINnXn).
bool bin_is_supported(const std::string& camera_id, int bin) {
    auto& sdk = QHYSDKWrapper::instance();
    switch (bin) {
    case 1: return sdk.is_control_available(camera_id, control::BIN1X1);
    case 2: return sdk.is_control_available(camera_id, control::BIN2X2);
    case 3: return sdk.is_control_available(camera_id, control::BIN3X3);
    case 4: return sdk.is_control_available(camera_id, control::BIN4X4);
    default: return false;
    }
}

int max_supported_bin(const std::string& camera_id) {
    for (int bin = 4; bin >= 1; --bin) {
        if (bin_is_supported(camera_id, bin)) {
            return bin;
        }
    }
    return 1;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
// Exposure state
// ────────────────────────────────────────────────────────────────────────────

enum class QHYExposureStatus {
    Idle,
    Working,
    Success,
    Failed
};

// ────────────────────────────────────────────────────────────────────────────
// QHYCameraDriver
// ────────────────────────────────────────────────────────────────────────────

class QHYCameraDriver : public CameraDriver {
public:
    QHYCameraDriver(int device_number,
                    std::optional<std::string> camera_id,
                    std::optional<int> camera_index)
        : device_number_(device_number)
        , camera_id_(std::move(camera_id))
        , camera_index_(camera_index)
        , camera_info_{}
        , camera_info_valid_(false)
        , readout_modes_{}
        , readout_mode_(0)
        , bin_x_(1)
        , bin_y_(1)
        , num_x_(0)
        , num_y_(0)
        , start_x_(0)
        , start_y_(0)
        , bits_(16)
        , target_temp_(0.0)
        , cooler_on_(false)
        , connected_(false)
        , connecting_(false)
        , exposure_status_(QHYExposureStatus::Idle)
        , image_ready_(false)
        , last_exposure_duration_(0.0)
        , last_exposure_start_{}
        , last_exposure_valid_(false)
        , pulse_guiding_(false)
        , pulse_guiding_end_{}
    {
        try_preload_camera_info();
    }

    ~QHYCameraDriver() override {
        stop_all_threads();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Error during destruction: " + std::string(e.what()));
            }
        }
    }

    // ── AlpacaDriver ─────────────────────────────────────────────────────────

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.model.empty()) {
            return camera_info_.model;
        }
        return "QHY Camera";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Camera;
    }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_id_.has_value()) {
            return "QHY_" + camera_id_.value();
        }
        return "QHY_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "QHY CCD Camera Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore QHY Camera Driver (SDK " + QHYSDKWrapper::instance().get_sdk_version() + ")";
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

        auto& sdk = QHYSDKWrapper::instance();

        if (connected) {
            const std::string& id = resolve_camera_id_locked();

            sdk.open_camera(id);
            sdk.init_camera(id);

            // Populate full camera info
            QHYCameraInfo info{};
            info.camera_id = id;
            if (!camera_info_valid_ || camera_info_.model.empty()) {
                sdk.get_camera_model(id, info.model);
            } else {
                info.model = camera_info_.model;
            }
            if (sdk.get_chip_info(id, info)) {
                camera_info_ = info;
                camera_info_valid_ = true;
            }

            // Select bit depth (prefer 16-bit)
            bits_ = 8;
            if (sdk.is_control_available(id, control::BITS16)) {
                bits_ = 16;
            }
            sdk.set_bits_mode(id, bits_);

            // Default to full-frame 1x1 binning
            bin_x_ = 1;
            bin_y_ = 1;
            start_x_ = 0;
            start_y_ = 0;
            num_x_ = static_cast<int>(camera_info_.max_width);
            num_y_ = static_cast<int>(camera_info_.max_height);
            sdk.set_bin_mode(id, 1, 1);
            sdk.set_resolution(id, 0, 0,
                               static_cast<uint32_t>(num_x_),
                               static_cast<uint32_t>(num_y_));

            // Enumerate readout modes
            load_readout_modes_locked(id);

            reset_exposure_state_locked();
            connected_.store(true);

            // Start temperature control thread if cooler is on
            if (cooler_on_ && camera_info_.has_cooler) {
                start_temp_control_thread_locked();
            }
            return;
        }

        // Disconnecting
        stop_temp_control_thread_locked();

        const std::string& id = camera_id_.value_or("");
        if (!id.empty()) {
            try {
                sdk.cancel_exposure(id);
            } catch (const std::exception&) {}
            sdk.close_camera(id);
        }

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
        if (camera_info_.has_cooler) {
            state.push_back({"CCDTemperature", get_ccd_temperature()});
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

    // ── CameraDriver ─────────────────────────────────────────────────────────

    int get_bayer_offset_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not applicable to monochrome sensor",
                                  AlpacaError::PropertyNotImplemented);
        }
        // QHY Bayer pattern is RGGB by default; offsets are (0, 0)
        return 0;
    }

    int get_bayer_offset_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not applicable to monochrome sensor",
                                  AlpacaError::PropertyNotImplemented);
        }
        return 0;
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
        std::lock_guard<std::mutex> lock(mutex_);
        switch (exposure_status_) {
        case QHYExposureStatus::Working:
            return CameraState::Exposing;
        case QHYExposureStatus::Failed:
            return CameraState::Error;
        case QHYExposureStatus::Idle:
        case QHYExposureStatus::Success:
        default:
            return CameraState::Idle;
        }
    }

    int get_camera_x_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? static_cast<int>(camera_info_.max_width) : 0;
    }

    int get_camera_y_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? static_cast<int>(camera_info_.max_height) : 0;
    }

    bool get_can_abort_exposure() const override {
        return true;
    }

    bool get_can_asymmetric_bin() const override {
        return false;
    }

    bool get_can_fast_readout() const override {
        return false;
    }

    bool get_can_get_cooler_power() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_cooler;
    }

    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_st4_port;
    }

    bool get_can_set_ccd_temperature() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_cooler;
    }

    bool get_can_stop_exposure() const override {
        return true;
    }

    double get_ccd_temperature() const override {
        ensure_connected();
        return QHYSDKWrapper::instance().get_param(camera_id_value(),
                                                   control::CURTEMP);
    }

    bool get_cooler_on() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.has_cooler) {
            return false;
        }
        return cooler_on_;
    }

    void set_cooler_on(bool cooler_on) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.has_cooler) {
            if (cooler_on) {
                throw AlpacaException("Cooler not available on this camera",
                                      AlpacaError::NotImplemented);
            }
            return;
        }
        cooler_on_ = cooler_on;
        if (!connected_.load()) {
            return;
        }
        if (cooler_on_) {
            start_temp_control_thread_locked();
        } else {
            stop_temp_control_thread_locked();
            // Cut power to cooler
            try {
                QHYSDKWrapper::instance().set_param(camera_id_value(),
                                                    control::MANULPWM, 0.0);
            } catch (const std::exception&) {
                // MANULPWM may not be writable on all cameras — ignore
            }
        }
    }

    double get_cooler_power() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.has_cooler) {
            return 0.0;
        }
        try {
            return QHYSDKWrapper::instance().get_param(camera_id_value(), control::CURPWM);
        } catch (const std::exception&) {
            return 0.0;
        }
    }

    double get_electrons_per_adu() const override {
        // TODO: QHY SDK does not expose e-/ADU directly; return 1.0 as placeholder
        return 1.0;
    }

    double get_exposure_max() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::EXPOSURE);
        if (!range.available) {
            return 3600.0; // 1 hour default
        }
        return range.max / 1'000'000.0; // microseconds → seconds
    }

    double get_exposure_min() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::EXPOSURE);
        if (!range.available) {
            return 0.000001; // 1 µs default
        }
        double min_s = range.min / 1'000'000.0;
        return min_s > 0.0 ? min_s : 0.000001;
    }

    double get_exposure_resolution() const override {
        return 0.000001; // 1 microsecond
    }

    bool get_fast_readout() const override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    void set_fast_readout(bool) override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    double get_full_well_capacity() const override {
        // TODO: QHY SDK exposes this via CAM_CurveFullWell on supported cameras
        return 0.0;
    }

    int get_gain() const override {
        ensure_connected();
        return static_cast<int>(
            QHYSDKWrapper::instance().get_param(camera_id_value(), control::GAIN));
    }

    void set_gain(int gain) override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::GAIN);
        if (range.available && (gain < static_cast<int>(range.min) ||
                                gain > static_cast<int>(range.max))) {
            throw AlpacaException("Gain value out of range", AlpacaError::InvalidValue);
        }
        QHYSDKWrapper::instance().set_param(camera_id_value(), control::GAIN,
                                            static_cast<double>(gain));
    }

    int get_gain_max() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::GAIN);
        if (!range.available) {
            throw AlpacaException("Gain range not available", AlpacaError::NotImplemented);
        }
        return static_cast<int>(range.max);
    }

    int get_gain_min() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::GAIN);
        if (!range.available) {
            throw AlpacaException("Gain range not available", AlpacaError::NotImplemented);
        }
        return static_cast<int>(range.min);
    }

    std::vector<std::string> get_gains() const override {
        throw AlpacaException("Named gain list not supported", AlpacaError::PropertyNotImplemented);
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

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!last_exposure_valid_) {
                throw AlpacaException("No exposure has been taken", AlpacaError::InvalidOperation);
            }
        }

        // Wait until exposure thread completes (poll with short sleep)
        for (int i = 0; i < 600; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (exposure_status_ == QHYExposureStatus::Success) {
                    return build_image_array_locked();
                }
                if (exposure_status_ == QHYExposureStatus::Failed) {
                    throw AlpacaException("Exposure failed", AlpacaError::DriverException);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        throw AlpacaException("Timeout waiting for image data", AlpacaError::DriverException);
    }

    std::string get_image_array_variant() const override {
        return "Int32";
    }

    bool get_image_ready() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            return false;
        }
        return exposure_status_ == QHYExposureStatus::Success;
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
            throw AlpacaException("No exposure has been taken", AlpacaError::ValueNotSet);
        }
        return last_exposure_duration_;
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("No exposure has been taken", AlpacaError::ValueNotSet);
        }
        return last_exposure_start_;
    }

    int get_max_adu() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        int depth = (bits_ > 0) ? bits_ : static_cast<int>(camera_info_.bpp);
        if (depth <= 0) {
            return 65535;
        }
        return static_cast<int>((1ULL << depth) - 1ULL);
    }

    int get_max_bin_x() const override {
        ensure_connected();
        return max_supported_bin(camera_id_value());
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
        return static_cast<int>(
            QHYSDKWrapper::instance().get_param(camera_id_value(), control::OFFSET));
    }

    void set_offset(int offset) override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::OFFSET);
        if (range.available && (offset < static_cast<int>(range.min) ||
                                offset > static_cast<int>(range.max))) {
            throw AlpacaException("Offset value out of range", AlpacaError::InvalidValue);
        }
        QHYSDKWrapper::instance().set_param(camera_id_value(), control::OFFSET,
                                            static_cast<double>(offset));
    }

    int get_offset_max() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::OFFSET);
        if (!range.available) {
            throw AlpacaException("Offset range not available", AlpacaError::NotImplemented);
        }
        return static_cast<int>(range.max);
    }

    int get_offset_min() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::OFFSET);
        if (!range.available) {
            throw AlpacaException("Offset range not available", AlpacaError::NotImplemented);
        }
        return static_cast<int>(range.min);
    }

    std::vector<std::string> get_offsets() const override {
        throw AlpacaException("Named offset list not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        if (!connected_.load()) {
            return 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (exposure_status_ == QHYExposureStatus::Success) {
            return 100.0;
        }
        if (exposure_status_ != QHYExposureStatus::Working) {
            return 0.0;
        }
        if (last_exposure_duration_ <= 0.0) {
            return 0.0;
        }
        auto now = std::chrono::system_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_exposure_start_).count();
        double percent = (elapsed / last_exposure_duration_) * 100.0;
        return std::clamp(percent, 0.0, 100.0);
    }

    double get_pixel_size_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_x_um : 0.0;
    }

    double get_pixel_size_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_y_um : 0.0;
    }

    int get_readout_mode() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return readout_mode_;
    }

    void set_readout_mode(int mode) override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (mode < 0 || mode >= static_cast<int>(readout_modes_.size())) {
            throw AlpacaException("Invalid readout mode index", AlpacaError::InvalidValue);
        }
        QHYSDKWrapper::instance().set_readout_mode(camera_id_value(),
                                                   static_cast<uint32_t>(mode));
        readout_mode_ = mode;

        // After changing readout mode, refresh chip info as dimensions may change
        QHYCameraInfo updated_info = camera_info_;
        if (QHYSDKWrapper::instance().get_chip_info(camera_id_value(), updated_info)) {
            camera_info_ = updated_info;
        }
        // Reset ROI to full frame
        num_x_ = static_cast<int>(camera_info_.max_width);
        num_y_ = static_cast<int>(camera_info_.max_height);
        start_x_ = 0;
        start_y_ = 0;
        try {
            QHYSDKWrapper::instance().set_resolution(camera_id_value(), 0, 0,
                static_cast<uint32_t>(num_x_), static_cast<uint32_t>(num_y_));
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "Failed to reset ROI after readout mode change: " +
                            std::string(e.what()));
        }
    }

    std::vector<std::string> get_readout_modes() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (readout_modes_.empty()) {
            return {"Normal"};
        }
        return readout_modes_;
    }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.model : "QHY Sensor";
    }

    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            return SensorType::Monochrome;
        }
        return SensorType::RGGB;
    }

    double get_set_ccd_temperature() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.has_cooler) {
            throw AlpacaException("Cooler not available", AlpacaError::PropertyNotImplemented);
        }
        return target_temp_;
    }

    void set_set_ccd_temperature(double temperature) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.has_cooler) {
            throw AlpacaException("Cooler not available", AlpacaError::PropertyNotImplemented);
        }
        target_temp_ = temperature;
        // If cooler is on and connected, the temp control thread picks up the new target
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
            throw AlpacaException("Pulse guide not supported on this camera",
                                  AlpacaError::NotImplemented);
        }
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid pulse guide direction", AlpacaError::InvalidValue);
        }
        if (duration <= 0) {
            throw AlpacaException("Invalid pulse guide duration", AlpacaError::InvalidValue);
        }

        uint32_t qhy_dir = alpaca_to_qhy_guide_direction(direction);
        uint16_t dur_ms = static_cast<uint16_t>(std::min(duration, 65535));

        QHYSDKWrapper::instance().guide(camera_id_value(), qhy_dir, dur_ms);
        pulse_guiding_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guiding_end_ = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(duration);
        }

        // Clear guiding flag after duration elapses
        std::thread([this, duration]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            pulse_guiding_.store(false);
        }).detach();
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        if (duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative",
                                  AlpacaError::InvalidValue);
        }

        const std::string& id = camera_id_value();
        auto& sdk = QHYSDKWrapper::instance();

        // Validate ROI
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (num_x_ <= 0 || num_y_ <= 0) {
                throw AlpacaException("ROI dimensions are invalid", AlpacaError::InvalidValue);
            }
        }

        // Apply exposure time (microseconds)
        double exposure_us = duration * 1'000'000.0;
        sdk.set_param(id, control::EXPOSURE, exposure_us);

        // Close mechanical shutter for dark/bias frames if the camera supports it
        if (!light && sdk.is_control_available(id, control::MECHANICALSHUTTER)) {
            sdk.set_param(id, control::MECHANICALSHUTTER, 1.0); // 1 = closed
        }

        // Apply current ROI and binning
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sdk.set_bin_mode(id, static_cast<uint32_t>(bin_x_),
                                 static_cast<uint32_t>(bin_y_));
            sdk.set_resolution(id,
                               static_cast<uint32_t>(start_x_),
                               static_cast<uint32_t>(start_y_),
                               static_cast<uint32_t>(num_x_),
                               static_cast<uint32_t>(num_y_));
            sdk.set_bits_mode(id, static_cast<uint32_t>(bits_));
        }

        // Get buffer size before starting exposure
        uint32_t mem_length = sdk.get_mem_length(id);
        if (mem_length == 0) {
            throw AlpacaException("Invalid memory length from QHY SDK", AlpacaError::DriverException);
        }

        // Start exposure
        bool read_directly = sdk.start_single_frame(id);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_exposure_duration_ = duration;
            last_exposure_start_ = std::chrono::system_clock::now();
            last_exposure_valid_ = true;
            image_ready_ = false;
            exposure_status_ = QHYExposureStatus::Working;
            exposure_buffer_.assign(mem_length, 0);
            exposure_width_ = 0;
            exposure_height_ = 0;
            exposure_bpp_ = 0;
            exposure_channels_ = 0;
        }

        // Launch background thread to complete the blocking GetSingleFrame call
        join_exposure_thread();
        exposure_thread_ = std::thread([this, id, read_directly]() {
            (void)read_directly; // QHYCCD_READ_DIRECTLY: we still call GetSingleFrame
            uint32_t w = 0, h = 0, bpp = 0, channels = 0;
            std::vector<uint8_t> local_buf;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                local_buf = exposure_buffer_;
            }

            bool ok = QHYSDKWrapper::instance().get_single_frame(
                id, local_buf.data(), w, h, bpp, channels);

            std::lock_guard<std::mutex> lk(mutex_);
            if (ok) {
                exposure_buffer_ = std::move(local_buf);
                exposure_width_    = w;
                exposure_height_   = h;
                exposure_bpp_      = bpp;
                exposure_channels_ = channels;
                exposure_status_   = QHYExposureStatus::Success;
                image_ready_       = true;
            } else {
                exposure_status_ = QHYExposureStatus::Failed;
                image_ready_     = false;
            }
        });
    }

    void stop_exposure() override {
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_status_ != QHYExposureStatus::Working) {
                return;
            }
        }
        // Cancel exposure (causes GetSingleFrame to return with an error)
        try {
            QHYSDKWrapper::instance().cancel_exposure(camera_id_value());
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "cancel_exposure error: " + std::string(e.what()));
        }
        join_exposure_thread();

        std::lock_guard<std::mutex> lock(mutex_);
        exposure_status_ = QHYExposureStatus::Idle;
        image_ready_ = false;
        last_exposure_valid_ = false;
    }

private:
    // ── Members ──────────────────────────────────────────────────────────────

    int device_number_;
    std::optional<std::string> camera_id_;
    std::optional<int> camera_index_;

    QHYCameraInfo camera_info_;
    bool camera_info_valid_;

    std::vector<std::string> readout_modes_;
    int readout_mode_;

    int bin_x_;
    int bin_y_;
    int num_x_;
    int num_y_;
    int start_x_;
    int start_y_;
    int bits_; // active bit depth (8 or 16)

    double target_temp_;
    bool cooler_on_;

    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;

    // Exposure state
    QHYExposureStatus exposure_status_;
    mutable bool image_ready_;
    std::vector<uint8_t> exposure_buffer_;
    uint32_t exposure_width_{};
    uint32_t exposure_height_{};
    uint32_t exposure_bpp_{};
    uint32_t exposure_channels_{};
    double last_exposure_duration_;
    std::chrono::system_clock::time_point last_exposure_start_;
    bool last_exposure_valid_;
    std::thread exposure_thread_;

    // Temperature control
    std::thread temp_thread_;
    std::atomic<bool> temp_thread_stop_{false};

    // Pulse guide
    mutable std::atomic<bool> pulse_guiding_;
    std::chrono::steady_clock::time_point pulse_guiding_end_;

    // ── Helpers ──────────────────────────────────────────────────────────────

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    const std::string& camera_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        return camera_id_.value();
    }

    const std::string& resolve_camera_id_locked() {
        if (camera_index_.has_value() && !camera_id_.has_value()) {
            auto cameras = QHYSDKWrapper::instance().enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No QHY cameras detected", AlpacaError::NotConnected);
            }
            int index = camera_index_.value();
            if (index < 0 || index >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range: " + std::to_string(index),
                                      AlpacaError::InvalidValue);
            }
            const auto& info = cameras[static_cast<std::size_t>(index)];
            ALPACA_LOG_INFO("QHY", "Resolved camera index " + std::to_string(index) +
                            " → ID: " + info.camera_id);
            camera_id_ = info.camera_id;
            if (info.model.empty() == false) {
                camera_info_.model = info.model;
            }
        }
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not specified", AlpacaError::InvalidValue);
        }
        return camera_id_.value();
    }

    void try_preload_camera_info() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            if (camera_id_.has_value()) {
                std::string model;
                if (QHYSDKWrapper::instance().get_camera_model(camera_id_.value(), model)) {
                    camera_info_.camera_id = camera_id_.value();
                    camera_info_.model = model;
                    camera_info_valid_ = false; // chip info still not loaded
                }
            } else if (camera_index_.has_value()) {
                auto cameras = QHYSDKWrapper::instance().enumerate_cameras();
                int index = camera_index_.value();
                if (index >= 0 && index < static_cast<int>(cameras.size())) {
                    const auto& info = cameras[static_cast<std::size_t>(index)];
                    camera_info_ = info;
                    camera_id_   = info.camera_id;
                    camera_info_valid_ = false;
                }
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("QHY", "Pre-load camera info skipped: " + std::string(e.what()));
        }
    }

    void load_readout_modes_locked(const std::string& id) {
        readout_modes_.clear();
        try {
            uint32_t num = QHYSDKWrapper::instance().get_num_readout_modes(id);
            for (uint32_t i = 0; i < num; ++i) {
                readout_modes_.push_back(
                    QHYSDKWrapper::instance().get_readout_mode_name(id, i));
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("QHY", "Readout mode enumeration failed: " + std::string(e.what()));
        }
        if (readout_modes_.empty()) {
            readout_modes_.push_back("Normal");
        }
        readout_mode_ = 0;
    }

    void reset_exposure_state_locked() {
        exposure_status_   = QHYExposureStatus::Idle;
        image_ready_       = false;
        last_exposure_duration_ = 0.0;
        last_exposure_start_    = std::chrono::system_clock::time_point{};
        last_exposure_valid_    = false;
        exposure_buffer_.clear();
        exposure_width_    = 0;
        exposure_height_   = 0;
        exposure_bpp_      = 0;
        exposure_channels_ = 0;
    }

    void join_exposure_thread() {
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
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
                ALPACA_LOG_ERROR("QHY", "Connection task failed: " + std::string(e.what()));
            }
            connecting_.store(false);
        });
    }

    void stop_all_threads() {
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            if (connection_thread_.joinable()) {
                connection_thread_.join();
            }
        }
        stop_temp_control_thread_locked();
        join_exposure_thread();
    }

    // Temperature control thread — must hold mutex_ when calling start/stop
    void start_temp_control_thread_locked() {
        if (temp_thread_.joinable()) {
            return; // Already running
        }
        temp_thread_stop_.store(false);
        std::string id = camera_id_.value_or("");
        if (id.empty()) {
            return;
        }
        temp_thread_ = std::thread([this, id]() {
            while (!temp_thread_stop_.load()) {
                double target = 0.0;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    target = target_temp_;
                }
                if (connected_.load()) {
                    try {
                        QHYSDKWrapper::instance().control_temp(id, target);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_DEBUG("QHY", "Temp control error: " + std::string(e.what()));
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
    }

    void stop_temp_control_thread_locked() {
        temp_thread_stop_.store(true);
        if (temp_thread_.joinable()) {
            temp_thread_.join();
        }
    }

    void set_bin_locked(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
        }
        const std::string& id = camera_id_value();
        if (!bin_is_supported(id, bin_x)) {
            throw AlpacaException("Bin value not supported: " + std::to_string(bin_x),
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        bin_x_ = bin_x;
        bin_y_ = bin_y;
        // Reset to full-frame in new binning
        int max_w = static_cast<int>(camera_info_.max_width)  / bin_x_;
        int max_h = static_cast<int>(camera_info_.max_height) / bin_y_;
        num_x_ = max_w;
        num_y_ = max_h;
        start_x_ = 0;
        start_y_ = 0;
        try {
            QHYSDKWrapper::instance().set_bin_mode(id, static_cast<uint32_t>(bin_x_),
                                                       static_cast<uint32_t>(bin_y_));
            QHYSDKWrapper::instance().set_resolution(id, 0, 0,
                static_cast<uint32_t>(num_x_), static_cast<uint32_t>(num_y_));
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "Failed to apply binning: " + std::string(e.what()));
        }
    }

    void set_roi_size_locked(int width, int height) {
        ensure_connected();
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        const std::string& id = camera_id_value();
        std::lock_guard<std::mutex> lock(mutex_);
        int max_w = static_cast<int>(camera_info_.max_width)  / bin_x_;
        int max_h = static_cast<int>(camera_info_.max_height) / bin_y_;
        if (width > max_w || height > max_h) {
            throw AlpacaException("ROI exceeds sensor area for current binning",
                                  AlpacaError::InvalidValue);
        }
        num_x_ = width;
        num_y_ = height;
        try {
            QHYSDKWrapper::instance().set_resolution(id,
                static_cast<uint32_t>(start_x_), static_cast<uint32_t>(start_y_),
                static_cast<uint32_t>(num_x_),   static_cast<uint32_t>(num_y_));
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "Failed to apply ROI: " + std::string(e.what()));
        }
    }

    void set_start_pos_locked(int start_x, int start_y) {
        ensure_connected();
        if (start_x < 0 || start_y < 0) {
            throw AlpacaException("Start position must be non-negative", AlpacaError::InvalidValue);
        }
        const std::string& id = camera_id_value();
        std::lock_guard<std::mutex> lock(mutex_);
        int max_w = static_cast<int>(camera_info_.max_width)  / bin_x_;
        int max_h = static_cast<int>(camera_info_.max_height) / bin_y_;
        if (start_x + num_x_ > max_w || start_y + num_y_ > max_h) {
            throw AlpacaException("Start position + ROI exceeds sensor area",
                                  AlpacaError::InvalidValue);
        }
        start_x_ = start_x;
        start_y_ = start_y;
        try {
            QHYSDKWrapper::instance().set_resolution(id,
                static_cast<uint32_t>(start_x_), static_cast<uint32_t>(start_y_),
                static_cast<uint32_t>(num_x_),   static_cast<uint32_t>(num_y_));
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "Failed to apply start position: " + std::string(e.what()));
        }
    }

    ImageArray build_image_array_locked() const {
        ImageArray image;
        image.width  = num_x_;
        image.height = num_y_;

        if (image.width <= 0 || image.height <= 0 || exposure_buffer_.empty()) {
            image.rank = 0;
            return image;
        }

        const uint32_t eff_w = exposure_width_  > 0 ? exposure_width_  : static_cast<uint32_t>(num_x_);
        const uint32_t eff_h = exposure_height_ > 0 ? exposure_height_ : static_cast<uint32_t>(num_y_);
        const uint32_t bpp   = exposure_bpp_;
        const uint32_t ch    = exposure_channels_;

        // Color (3-channel) output
        if (ch == 3) {
            image.rank = 3;
            image.data.resize(static_cast<std::size_t>(image.width) *
                              static_cast<std::size_t>(image.height) * 3);
            const std::size_t buf_stride = static_cast<std::size_t>(eff_w) * 3;
            const std::size_t out_stride = static_cast<std::size_t>(image.width) * 3;
            for (int row = 0; row < image.height; ++row) {
                if (static_cast<uint32_t>(row) >= eff_h) break;
                const std::size_t src = static_cast<std::size_t>(row) * buf_stride;
                const std::size_t dst = static_cast<std::size_t>(row) * out_stride;
                const std::size_t copy = std::min(buf_stride, out_stride);
                for (std::size_t i = 0; i < copy && src + i < exposure_buffer_.size(); ++i) {
                    image.data[dst + i] = exposure_buffer_[src + i];
                }
            }
            return image;
        }

        // Monochrome output
        image.rank = 2;
        const std::size_t pixel_count = static_cast<std::size_t>(image.width) *
                                        static_cast<std::size_t>(image.height);
        image.data.resize(pixel_count, 0);

        if (bpp == 16) {
            for (int row = 0; row < image.height; ++row) {
                for (int col = 0; col < image.width; ++col) {
                    const std::size_t out_idx =
                        static_cast<std::size_t>(row) * static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(col);
                    if (static_cast<uint32_t>(row) < eff_h && static_cast<uint32_t>(col) < eff_w) {
                        const std::size_t src =
                            (static_cast<std::size_t>(row) * static_cast<std::size_t>(eff_w) +
                             static_cast<std::size_t>(col)) * 2;
                        if (src + 1 < exposure_buffer_.size()) {
                            // QHY delivers 16-bit data big-endian
                            uint16_t val = (static_cast<uint16_t>(exposure_buffer_[src]) << 8) |
                                            static_cast<uint16_t>(exposure_buffer_[src + 1]);
                            image.data[out_idx] = static_cast<std::int32_t>(val);
                        }
                    }
                }
            }
        } else {
            // 8-bit
            for (int row = 0; row < image.height; ++row) {
                for (int col = 0; col < image.width; ++col) {
                    const std::size_t out_idx =
                        static_cast<std::size_t>(row) * static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(col);
                    if (static_cast<uint32_t>(row) < eff_h && static_cast<uint32_t>(col) < eff_w) {
                        const std::size_t src =
                            static_cast<std::size_t>(row) * static_cast<std::size_t>(eff_w) +
                            static_cast<std::size_t>(col);
                        if (src < exposure_buffer_.size()) {
                            image.data[out_idx] = static_cast<std::int32_t>(exposure_buffer_[src]);
                        }
                    }
                }
            }
        }
        return image;
    }
};

// ────────────────────────────────────────────────────────────────────────────
// Factory functions
// ────────────────────────────────────────────────────────────────────────────

std::unique_ptr<CameraDriver> create_qhy_camera(int device_number,
                                                 const std::string& camera_id) {
    return std::make_unique<QHYCameraDriver>(device_number, camera_id, std::nullopt);
}

std::unique_ptr<CameraDriver> create_qhy_camera_by_index(int device_number, int camera_index) {
    return std::make_unique<QHYCameraDriver>(device_number, std::nullopt, camera_index);
}

} // namespace alpacacore::vendor::qhy
