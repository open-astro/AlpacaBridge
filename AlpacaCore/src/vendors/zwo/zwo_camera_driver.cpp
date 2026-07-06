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
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

class ZWOCameraDriver : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    ZWOCameraDriver(int device_number, std::optional<int> camera_id, std::optional<int> camera_index)
        : AsyncConnectable("ZWO"),
          device_number_(device_number),
          camera_id_(camera_id),
          camera_index_(camera_index),
          serial_number_(),
          camera_info_(),
          camera_info_valid_(false),
          control_caps_(),
          connected_(false),
          image_type_(ZWOImageType::Raw8),
          bin_x_(1),
          bin_y_(1),
          num_x_(0),
          num_y_(0),
          roi_width_effective_(0),
          roi_height_effective_(0),
          start_x_(0),
          start_y_(0),
          image_ready_(false),
          image_cached_(false),
          last_image_(),
          last_exposure_duration_(0.0),
          last_exposure_start_(),
          last_exposure_valid_(false),
          pulse_guiding_(false),
          pulse_guiding_end_(std::chrono::steady_clock::time_point{}) {
        preload_camera_info_locked();
    }

    ~ZWOCameraDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
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
        const_cast<ZWOCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
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

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only (never in
    // DriverInfo). ASIGetSDKVersion() returns "1, 7, 7, 0"; render as "1.7.7.0".
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = ZWOSDKWrapper::instance().get_sdk_version();
        // get_sdk_version() returns the literal "unknown" when ASIGetSDKVersion()
        // yields nullptr — suppress the row rather than show "unknown".
        if (version.empty() || version == "unknown") {
            return std::nullopt;
        }
        return util::normalize_dotted_version(version);
    }

    int get_interface_version() const override {
        return 4;  // ICameraV4 (Platform 7)
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

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            // Idempotent (ASCOM): a redundant Connect/Disconnect is a no-op and must
            // NOT reset exposure state. The Platform-7 `connect` endpoint calls
            // connect() unconditionally, so wiping here would abort an in-flight
            // exposure or discard a just-completed image. Matches the QHY driver.
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
            int max_width = camera_info_valid_ ? camera_info_.max_width : 0;
            int max_height = camera_info_valid_ ? camera_info_.max_height : 0;
            num_x_ = max_width;
            num_y_ = max_height;
            roi_width_effective_ = align_roi_dimension(max_width, 8);
            roi_height_effective_ = align_roi_dimension(max_height, 2);

            if (roi_width_effective_ > 0 && roi_height_effective_ > 0) {
                sdk.set_roi_format(resolved_id,
                                   roi_width_effective_,
                                   roi_height_effective_,
                                   bin_x_,
                                   image_type_);
                sdk.set_start_pos(resolved_id, start_x_, start_y_);
            }

            serial_number_ = sdk.get_serial_number(resolved_id);
            reset_exposure_state_locked();
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
        if (camera_index_.has_value()) {
            camera_id_.reset();
            camera_info_ = {};
            camera_info_valid_ = false;
            serial_number_.clear();
        }
        reset_exposure_state_locked();
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
        auto status = ZWOSDKWrapper::instance().get_exposure_status(camera_id_value());
        switch (status) {
        case ZWOExposureStatus::Idle:
            return CameraState::Idle;
        case ZWOExposureStatus::Working:
            return CameraState::Exposing;
        case ZWOExposureStatus::Success:
            return CameraState::Idle;
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
        if (!can_get_control(ZWOControlType::CoolerOn)) {
            return false;
        }
        long value = get_control_value_or_throw(ZWOControlType::CoolerOn);
        return value != 0;
    }

    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::CoolerOn)) {
            if (cooler_on) {
                throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
            }
            return;
        }
        set_control_value_or_throw(ZWOControlType::CoolerOn, cooler_on ? 1 : 0);
    }

    double get_cooler_power() const override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::CoolerPower)) {
            return 0.0;
        }
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.electrons_per_adu <= 0.0 || camera_info_.bit_depth <= 0) {
            return 0.0;
        }
        double max_adu = static_cast<double>((1ULL << camera_info_.bit_depth) - 1ULL);
        return camera_info_.electrons_per_adu * 1000.0 * max_adu;
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
        throw AlpacaException("Gain descriptions not supported", AlpacaError::PropertyNotImplemented);
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
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!last_exposure_valid_) {
                throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
            }
            if (image_cached_) {
                return last_image_;
            }
            ready = image_ready_;
        }
        if (!ready) {
            auto status = ZWOSDKWrapper::instance().get_exposure_status(camera_id_value());
            if (status == ZWOExposureStatus::Failed) {
                throw AlpacaException("Exposure failed", AlpacaError::DriverException);
            }
            if (status != ZWOExposureStatus::Success) {
                throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            image_ready_ = true;
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
        return "Int32";
    }

    bool get_image_ready() const override {
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!last_exposure_valid_) {
                image_ready_ = false;
                image_cached_ = false;
                return false;
            }
            if (image_cached_ || image_ready_) {
                return true;
            }
        }
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

    void set_num_x(int num_x) override { set_roi_size_locked(num_x, std::nullopt); }

    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }

    void set_num_y(int num_y) override { set_roi_size_locked(std::nullopt, num_y); }

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
        throw AlpacaException("Offset descriptions not supported", AlpacaError::PropertyNotImplemented);
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

    void set_start_x(int start_x) override { set_start_pos_locked(start_x, std::nullopt); }

    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }

    void set_start_y(int start_y) override { set_start_pos_locked(std::nullopt, start_y); }

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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!is_roi_valid_locked(num_x_, num_y_, start_x_, start_y_)) {
                throw AlpacaException("ROI is not valid for exposure", AlpacaError::InvalidValue);
            }
        }
        sdk.set_control_value(active_camera_id, ZWOControlType::Exposure, exposure_us, false);
        sdk.start_exposure(active_camera_id, !light);

        std::lock_guard<std::mutex> lock(mutex_);
        last_exposure_duration_ = duration;
        last_exposure_start_ = std::chrono::system_clock::now();
        last_exposure_valid_ = true;
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
    mutable std::mutex mutex_;

    ZWOImageType image_type_;
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
    }

    bool is_roi_valid_locked(int width, int height, int start_x, int start_y) const {
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
        if (start_x < 0 || start_y < 0) {
            return false;
        }
        if (start_x + width > max_width || start_y + height > max_height) {
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

        int candidate_width = align_roi_dimension(requested_width, 8);
        int candidate_height = align_roi_dimension(requested_height, 2);
        if (candidate_width <= 0 || candidate_height <= 0) {
            return false;
        }

        adjusted_width = candidate_width;
        adjusted_height = candidate_height;
        return true;
    }

    int resolve_camera_id_locked() {
        if (camera_index_.has_value()) {
            auto cameras = ZWOSDKWrapper::instance().enumerate_cameras();
            if (cameras.empty()) {
                ALPACA_LOG_WARN("ZWO", "No ZWO cameras detected by SDK");
                throw AlpacaException("No ZWO cameras detected", AlpacaError::NotConnected);
            }
            int index = camera_index_.value();
            if (index < 0 || index >= static_cast<int>(cameras.size())) {
                ALPACA_LOG_WARN("ZWO", "Camera index out of range: " + std::to_string(index) + " (count=" + std::to_string(cameras.size()) + ")");
                throw AlpacaException("Camera index not found", AlpacaError::InvalidValue);
            }

            const auto& info = cameras[static_cast<std::size_t>(index)];
            ALPACA_LOG_INFO("ZWO", "Using camera index " + std::to_string(index) + ": " + info.name + " (ID " + std::to_string(info.camera_id) + ")");
            camera_id_ = info.camera_id;
            camera_info_ = info;
            camera_info_valid_ = true;
            return camera_id_.value();
        }

        if (camera_id_.has_value()) {
            return camera_id_.value();
        }

        throw AlpacaException("Camera ID not specified", AlpacaError::InvalidValue);
    }

    void refresh_camera_info_locked(int camera_id) {
        ZWOCameraInfo info;
        if (ZWOSDKWrapper::instance().get_camera_info_by_id(camera_id, info)) {
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
            if (camera_id_.has_value()) {
                ZWOCameraInfo info;
                if (ZWOSDKWrapper::instance().get_camera_info_by_id(camera_id_.value(), info)) {
                    camera_info_ = info;
                    camera_info_valid_ = true;
                    return;
                }
            } else if (camera_index_.has_value()) {
                auto cameras = ZWOSDKWrapper::instance().enumerate_cameras();
                int index = camera_index_.value();
                if (index >= 0 && index < static_cast<int>(cameras.size())) {
                    camera_info_ = cameras[static_cast<std::size_t>(index)];
                    camera_info_valid_ = true;
                    camera_id_ = camera_info_.camera_id;
                }
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ZWO", "Unable to preload camera info: " + std::string(e.what()));
        }
    }

    void refresh_cached_camera_info_if_needed() {
        if (connected_.load()) {
            return;
        }

        std::optional<int> camera_id;
        std::optional<int> camera_index;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            camera_id = camera_id_;
            camera_index = camera_index_;
        }

        try {
            bool refreshed = false;
            if (camera_id.has_value()) {
                ZWOCameraInfo info;
                if (ZWOSDKWrapper::instance().get_camera_info_by_id(camera_id.value(), info)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    camera_info_ = info;
                    camera_info_valid_ = true;
                    refreshed = true;
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    camera_id_.reset();
                    serial_number_.clear();
                }
            }

            if (refreshed) {
                return;
            }

            if (camera_index.has_value()) {
                auto cameras = ZWOSDKWrapper::instance().enumerate_cameras();
                int index = camera_index.value();
                if (index >= 0 && index < static_cast<int>(cameras.size())) {
                    const auto& info = cameras[static_cast<std::size_t>(index)];
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!camera_id_.has_value() || camera_id_.value() != info.camera_id) {
                        serial_number_.clear();
                    }
                    camera_info_ = info;
                    camera_info_valid_ = true;
                    camera_id_ = info.camera_id;
                }
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ZWO", "Unable to refresh camera info: " + std::string(e.what()));
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
        if (supports_format(camera_info_.supported_formats, ZWOImageType::Raw16)) {
            image_type_ = ZWOImageType::Raw16;
            return;
        }
        if (supports_format(camera_info_.supported_formats, ZWOImageType::Raw8)) {
            image_type_ = ZWOImageType::Raw8;
            return;
        }
        if (supports_format(camera_info_.supported_formats, ZWOImageType::Rgb24)) {
            image_type_ = ZWOImageType::Rgb24;
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
        bin_x_ = bin_x;
        bin_y_ = bin_y;
        int max_width = camera_info_.max_width / bin_x_;
        int max_height = camera_info_.max_height / bin_y_;
        int width = 0;
        int height = 0;
        if (!adjust_roi_size_locked(max_width, max_height, width, height)) {
            throw AlpacaException("ROI size invalid for binning", AlpacaError::InvalidValue);
        }
        ZWOSDKWrapper::instance().set_roi_format(active_camera_id, width, height, bin_x_, image_type_);
        ZWOSDKWrapper::instance().set_start_pos(active_camera_id, 0, 0);
        num_x_ = max_width;
        num_y_ = max_height;
        roi_width_effective_ = width;
        roi_height_effective_ = height;
        start_x_ = 0;
        start_y_ = 0;
        image_cached_ = false;
    }

    // width/height (or sx/sy) of std::nullopt means "leave that axis unchanged",
    // resolved UNDER mutex_: each public setter passes only its own axis, so a
    // concurrent setter for the other axis can no longer be clobbered by a stale
    // pre-lock get_num_x()/get_num_y() snapshot (lost-update TOCTOU).
    void set_roi_size_locked(std::optional<int> width_opt, std::optional<int> height_opt) {
        ensure_connected();
        int active_camera_id = camera_id_value();
        std::lock_guard<std::mutex> lock(mutex_);
        const int width = width_opt.value_or(num_x_);
        const int height = height_opt.value_or(num_y_);
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        int adjusted_width = 0;
        int adjusted_height = 0;
        bool valid = adjust_roi_size_locked(width, height, adjusted_width, adjusted_height);
        num_x_ = width;
        num_y_ = height;
        if (valid && is_roi_valid_locked(width, height, start_x_, start_y_)) {
            roi_width_effective_ = adjusted_width;
            roi_height_effective_ = adjusted_height;
            ZWOSDKWrapper::instance().set_roi_format(active_camera_id,
                                                     adjusted_width,
                                                     adjusted_height,
                                                     bin_x_,
                                                     image_type_);
        }
        image_cached_ = false;
    }

    void set_start_pos_locked(std::optional<int> start_x_opt, std::optional<int> start_y_opt) {
        ensure_connected();
        int active_camera_id = camera_id_value();
        std::lock_guard<std::mutex> lock(mutex_);
        const int start_x = start_x_opt.value_or(start_x_);
        const int start_y = start_y_opt.value_or(start_y_);
        if (start_x < 0 || start_y < 0) {
            throw AlpacaException("Start position must be non-negative", AlpacaError::InvalidValue);
        }
        bool valid = is_roi_valid_locked(num_x_, num_y_, start_x, start_y);
        start_x_ = start_x;
        start_y_ = start_y;
        if (valid) {
            ZWOSDKWrapper::instance().set_start_pos(active_camera_id, start_x, start_y);
        }
    }

    std::size_t image_buffer_size_locked() const {
        int width = roi_width_effective_ > 0 ? roi_width_effective_ : num_x_;
        int height = roi_height_effective_ > 0 ? roi_height_effective_ : num_y_;
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

        int out_width = image.width;
        int out_height = image.height;
        int eff_width = roi_width_effective_ > 0 ? roi_width_effective_ : out_width;
        int eff_height = roi_height_effective_ > 0 ? roi_height_effective_ : out_height;
        if (eff_width <= 0 || eff_height <= 0) {
            image.rank = 0;
            return image;
        }

        if (image_type_ == ZWOImageType::Rgb24) {
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
        if (image_type_ == ZWOImageType::Raw16) {
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

std::unique_ptr<CameraDriver> create_zwo_camera(int device_number, int camera_id) {
    return std::make_unique<ZWOCameraDriver>(device_number, camera_id, std::nullopt);
}

std::unique_ptr<CameraDriver> create_zwo_camera_by_index(int device_number, int camera_index) {
    return std::make_unique<ZWOCameraDriver>(device_number, std::nullopt, camera_index);
}

} // namespace alpacacore::vendor::zwo
