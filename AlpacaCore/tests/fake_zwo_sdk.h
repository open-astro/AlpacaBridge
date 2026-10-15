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

#pragma once

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::test {

class FakeZWOSDK final : public vendor::zwo::ZWOSDK {
public:
    using CameraInfo = vendor::zwo::ZWOCameraInfo;
    using ControlCaps = vendor::zwo::ZWOControlCaps;
    using ControlType = vendor::zwo::ZWOControlType;
    using ExposureStatus = vendor::zwo::ZWOExposureStatus;
    using ImageType = vendor::zwo::ZWOImageType;
    using ROIDescription = vendor::zwo::ZWOROIFormat;
    using StartPosition = vendor::zwo::ZWOStartPos;
    using GuideDirection = vendor::zwo::ZWOGuideDirection;

    CameraInfo camera = default_camera();
    std::vector<std::uint8_t> frame_bytes;
    std::string download_error;
    std::optional<ROIDescription> roi_readback_override;
    std::optional<StartPosition> start_readback_override;
    std::function<void(const std::string&)> before_call;
    bool fail_open{false};
    bool stop_releases_download{true};
    bool auto_complete_exposure{true};
    bool fail_exposure_register_write{false};
    bool fail_exposure_start{false};
    // Issue #722: the real SDK answers 0 (success) on the first temperature reads after open.
    int zero_temperature_reads_after_open{0};

    static CameraInfo default_camera() {
        CameraInfo info;
        info.camera_id = 17;
        info.name = "Fake ZWO Camera";
        info.max_width = 16;
        info.max_height = 8;
        info.is_color = false;
        info.supported_bins = {1, 2, 4};
        info.supported_formats = {ImageType::Raw16, ImageType::Raw8, ImageType::Y8, ImageType::Rgb24};
        info.pixel_size_um = 3.76;
        info.has_st4_port = true;
        info.has_cooler = true;
        info.bit_depth = 16;
        return info;
    }

    void hold_download() {
        std::lock_guard lock(download_mutex_);
        download_held_ = true;
        download_entered_ = false;
    }

    void release_download() {
        {
            std::lock_guard lock(download_mutex_);
            download_held_ = false;
        }
        download_cv_.notify_all();
    }

    bool wait_for_download(std::chrono::milliseconds timeout) {
        std::unique_lock lock(download_mutex_);
        return download_cv_.wait_for(lock, timeout, [this] { return download_entered_; });
    }

    int call_count(const std::string& name) const {
        std::lock_guard lock(mutex_);
        const auto it = calls_.find(name);
        return it == calls_.end() ? 0 : it->second;
    }

    bool wait_for_call_count(const std::string& name, int count, std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return calls_cv_.wait_for(lock, timeout, [&] {
            const auto it = calls_.find(name);
            return it != calls_.end() && it->second >= count;
        });
    }

    void script_exposure_status(std::vector<ExposureStatus> statuses) {
        std::lock_guard lock(mutex_);
        status_script_.assign(statuses.begin(), statuses.end());
    }

    void set_exposure_status_error(std::string message) {
        std::lock_guard lock(mutex_);
        exposure_status_error_ = std::move(message);
    }

    void set_exposure_status(ExposureStatus status) {
        std::lock_guard lock(mutex_);
        status_ = status;
    }

    int open_count() const {
        std::lock_guard lock(mutex_);
        return open_count_;
    }

    bool closed_during_download() const {
        std::lock_guard lock(download_mutex_);
        return closed_during_download_;
    }

    std::vector<CameraInfo> enumerate_cameras(std::vector<vendor::zwo::ZwoUnreadableSlot>* = nullptr) override {
        hit("enumerate_cameras");
        return {camera};
    }

    std::vector<vendor::zwo::ZwoEnumeratedCamera> enumerate_identified_cameras(
        const std::string& only_model_name, std::vector<vendor::zwo::ZwoUnreadableSlot>*) override {
        hit("enumerate_identified_cameras");
        if (!only_model_name.empty() && only_model_name != camera.name) return {};
        return {{0, camera.camera_id, camera.name, "FAKE-ZWO-17"}};
    }

    bool get_camera_info_by_id(int camera_id, CameraInfo& info) override {
        hit("get_camera_info_by_id");
        if (camera.camera_id != camera_id) return false;
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        info = camera;
        return true;
    }

    bool get_camera_info_by_index(int camera_index, CameraInfo& info) override {
        hit("get_camera_info_by_index");
        if (camera_index != 0) return false;
        info = camera;
        return true;
    }

    void open_camera(int camera_id) override {
        hit("open_camera");
        require_id(camera_id);
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        std::lock_guard lock(mutex_);
        ++open_count_;
        temperature_reads_since_open_ = 0;
    }

    void init_camera(int camera_id) override {
        hit("init_camera");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
    }

    void close_camera(int camera_id) override {
        hit("close_camera");
        require_id(camera_id);
        {
            std::lock_guard lock(download_mutex_);
            closed_during_download_ = closed_during_download_ || download_in_progress_;
        }
        std::lock_guard lock(mutex_);
        if (open_count_ <= 0) throw AlpacaException("fake ZWO camera is not open", AlpacaError::DriverException);
        --open_count_;
    }

    std::vector<ControlCaps> get_control_caps(int camera_id) override {
        hit("get_control_caps");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        return {{ControlType::Exposure, "Exposure", "Exposure time", 1, 3'600'000'000L, 100'000, false, true},
                {ControlType::Gain, "Gain", "Gain", 0, 500, 100, false, true},
                {ControlType::Offset, "Offset", "Offset", 0, 100, 10, false, true},
                {ControlType::Temperature, "Temperature", "Temperature", -500, 500, 0, false, false},
                {ControlType::CoolerOn, "Cooler", "Cooler", 0, 1, 0, false, true},
                {ControlType::CoolerPower, "Cooler Power", "Cooler power", 0, 100, 0, false, false},
                {ControlType::TargetTemperature, "Target Temperature", "Target temperature", -500, 300, 0, false, true},
                {ControlType::HighSpeedMode, "High Speed", "High speed", 0, 1, 0, false, true},
                {ControlType::AntiDewHeater, "Anti Dew", "Anti-dew heater", 0, 100, 0, false, true}};
    }

    bool get_control_value(int camera_id, ControlType type, long& value, bool& is_auto) override {
        hit("get_control_value");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        value = controls_[type];
        if (type == ControlType::Temperature && temperature_reads_since_open_++ < zero_temperature_reads_after_open) {
            value = 0;
        }
        is_auto = false;
        return true;
    }

    void set_control_value(int camera_id, ControlType type, long value, bool) override {
        hit("set_control_value");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        if (type == ControlType::Exposure && fail_exposure_register_write) {
            fail_exposure_register_write = false;
            throw AlpacaException("scripted exposure-register failure", AlpacaError::DriverException);
        }
        controls_[type] = value;
    }

    ROIDescription get_roi_format(int camera_id) override {
        hit("get_roi_format");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        return roi_readback_override.value_or(roi_);
    }

    void set_roi_format(int camera_id, int width, int height, int bin, ImageType type) override {
        hit("set_roi_format");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        roi_ = {width, height, bin, type};
    }

    StartPosition get_start_pos(int camera_id) override {
        hit("get_start_pos");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        return start_readback_override.value_or(start_pos_);
    }

    void set_start_pos(int camera_id, int start_x, int start_y) override {
        hit("set_start_pos");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        start_pos_ = {start_x, start_y};
    }

    void start_exposure(int camera_id, bool) override {
        hit("start_exposure");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        if (fail_exposure_start) {
            fail_exposure_start = false;
            throw AlpacaException("scripted exposure-trigger failure", AlpacaError::DriverException);
        }
        status_ = auto_complete_exposure ? ExposureStatus::Success : ExposureStatus::Working;
    }

    void stop_exposure(int camera_id) override {
        hit("stop_exposure");
        {
            std::lock_guard lock(mutex_);
            require_open_locked(camera_id);
            status_ = ExposureStatus::Idle;
        }
        if (stop_releases_download) release_download();
    }

    ExposureStatus get_exposure_status(int camera_id) override {
        hit("get_exposure_status");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        if (!exposure_status_error_.empty()) {
            const auto message = std::exchange(exposure_status_error_, {});
            throw AlpacaException(message, AlpacaError::DriverException);
        }
        if (!status_script_.empty()) {
            const auto status = status_script_.front();
            status_script_.pop_front();
            return status;
        }
        return status_;
    }

    void get_data_after_exposure(int camera_id, std::uint8_t* buffer, long buffer_size) override {
        hit("get_data_after_exposure");
        {
            std::unique_lock lock(download_mutex_);
            {
                std::lock_guard sdk_lock(mutex_);
                require_open_locked(camera_id);
            }
            download_entered_ = true;
            download_in_progress_ = true;
            download_cv_.notify_all();
            download_cv_.wait(lock, [this] { return !download_held_; });
        }
        std::string error;
        {
            std::lock_guard lock(mutex_);
            error = download_error;
            if (error.empty()) {
                const auto copy_size = std::min(static_cast<std::size_t>(buffer_size), frame_bytes.size());
                if (copy_size > 0) std::memcpy(buffer, frame_bytes.data(), copy_size);
            }
        }
        {
            std::lock_guard lock(download_mutex_);
            download_in_progress_ = false;
        }
        if (!error.empty()) throw AlpacaException(error, AlpacaError::DriverException);
    }

    void pulse_guide_on(int camera_id, GuideDirection) override {
        hit("pulse_guide_on");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
    }

    void pulse_guide_off(int camera_id, GuideDirection) override {
        hit("pulse_guide_off");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
    }

    std::string get_serial_number(int camera_id) override {
        hit("get_serial_number");
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        return "FAKE-ZWO-17";
    }

    std::string get_sdk_version() override {
        hit("get_sdk_version");
        return "fake-zwo-sdk-1.0";
    }

private:
    void hit(const std::string& name) {
        if (before_call) before_call(name);
        std::lock_guard lock(mutex_);
        ++calls_[name];
        calls_cv_.notify_all();
    }

    void require_id(int camera_id) const {
        if (camera_id != camera.camera_id) {
            throw AlpacaException("fake ZWO camera id mismatch", AlpacaError::DriverException);
        }
    }

    void require_open_locked(int camera_id) const {
        require_id(camera_id);
        if (open_count_ <= 0) {
            throw AlpacaException("fake ZWO operation requires an open camera", AlpacaError::DriverException);
        }
    }

    mutable std::mutex mutex_;
    mutable std::mutex download_mutex_;
    std::condition_variable download_cv_;
    std::map<std::string, int> calls_;
    mutable std::condition_variable calls_cv_;
    // Non-zero so a default connect+read takes the plain path; 0 is the #722 placeholder.
    std::map<ControlType, long> controls_{{ControlType::Temperature, 150}};
    ROIDescription roi_{};
    StartPosition start_pos_{};
    ExposureStatus status_{ExposureStatus::Idle};
    std::deque<ExposureStatus> status_script_;
    std::string exposure_status_error_;
    int open_count_{0};
    int temperature_reads_since_open_{0};
    bool download_held_{false};
    bool download_entered_{false};
    bool download_in_progress_{false};
    bool closed_during_download_{false};
};

}  // namespace alpacacore::test
