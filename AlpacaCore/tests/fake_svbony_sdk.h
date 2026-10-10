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
#include <alpacacore/vendor/svbony/svbony_sdk_wrapper.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::test {

class FakeSVBSDK final : public vendor::svbony::SVBSDK {
public:
    using CameraInfo = vendor::svbony::SVBCameraInfo;
    using ControlCaps = vendor::svbony::SVBControlCaps;
    using Control = vendor::svbony::SVBControlType;
    using ImageType = vendor::svbony::SVBImageType;
    using ROI = vendor::svbony::SVBROIFormat;

    FakeSVBSDK() {
        camera_.camera_id = 17;
        camera_.name = "Fake SVBONY Camera";
        camera_.serial_number = "fake-17";
        camera_.max_width = 16;
        camera_.max_height = 8;
        camera_.is_color = true;
        camera_.bayer_pattern = vendor::svbony::SVBBayerPattern::RG;
        camera_.supported_bins = {1, 2};
        camera_.supported_formats = {ImageType::Raw16, ImageType::Raw8, ImageType::Rgb24};
        camera_.pixel_size_um = 3.76;
        camera_.supports_pulse_guide = true;
        camera_.bit_depth = 12;
        controls_ = {
            {Control::Exposure, "Exposure", "Exposure", 1, 10'000'000, 1000, false, true},
            {Control::Gain, "Gain", "Gain", 0, 100, 10, true, true},
            {Control::Offset, "Offset", "Offset", 0, 255, 10, false, true},
            {Control::CurrentTemperature, "Temperature", "Temperature", -500, 500, 0, false, false},
        };
        values_ = {{Control::Exposure, 1000}, {Control::Gain, 10}, {Control::Offset, 10}};
        roi_ = {0, 0, 16, 8, 1};
        frame_data_.resize(16 * 8 * 2);
        for (std::size_t i = 0; i < frame_data_.size(); ++i) {
            frame_data_[i] = static_cast<std::uint8_t>(i);
        }
    }

    std::vector<CameraInfo> enumerate_cameras() override {
        std::lock_guard lock(mutex_);
        return camera_present_ ? std::vector<CameraInfo>{camera_} : std::vector<CameraInfo>{};
    }
    bool get_camera_info_by_index(int index, CameraInfo& info) override {
        std::lock_guard lock(mutex_);
        if (index != 0 || !camera_present_) return false;
        info = camera_;
        return true;
    }
    void open_camera(int) override {
        std::lock_guard lock(mutex_);
        ++open_count_;
    }
    void close_camera(int) override {
        std::lock_guard lock(mutex_);
        --open_count_;
    }
    std::vector<ControlCaps> get_control_caps(int) override {
        std::lock_guard lock(mutex_);
        return controls_;
    }
    bool get_control_value(int, Control type, long& value, bool& is_auto) override {
        std::lock_guard lock(mutex_);
        auto it = values_.find(type);
        if (it == values_.end()) return false;
        value = it->second;
        is_auto = false;
        return true;
    }
    void set_control_value(int, Control type, long value, bool) override {
        std::lock_guard lock(mutex_);
        values_[type] = value;
    }
    ROI get_roi_format(int) override {
        std::lock_guard lock(mutex_);
        return returned_roi_.value_or(roi_);
    }
    void set_roi_format(int, int x, int y, int width, int height, int bin) override {
        std::lock_guard lock(mutex_);
        roi_ = {x, y, width, height, bin};
    }
    ImageType get_output_image_type(int) override {
        std::lock_guard lock(mutex_);
        return returned_type_.value_or(type_);
    }
    void set_output_image_type(int, ImageType type) override {
        std::lock_guard lock(mutex_);
        type_ = type;
    }
    void start_video_capture(int) override {
        std::lock_guard lock(mutex_);
        if (fail_next_capture_start_) {
            fail_next_capture_start_ = false;
            throw alpacacore::AlpacaException("scripted capture-start failure",
                                              alpacacore::AlpacaError::DriverException);
        }
    }
    void stop_video_capture(int) override {}
    void get_video_data(int, std::uint8_t* buffer, long size, int) override {
        std::lock_guard lock(mutex_);
        if (size < 0 || frame_data_.size() < static_cast<std::size_t>(size)) {
            throw alpacacore::AlpacaException("scripted short SVBONY frame", alpacacore::AlpacaError::DriverException);
        }
        std::copy_n(frame_data_.begin(), static_cast<std::size_t>(size), buffer);
    }
    void pulse_guide(int, vendor::svbony::SVBGuideDirection, int) override {}
    float get_sensor_pixel_size(int) override { return 3.76F; }
    std::string get_serial_number(int) override { return "fake-17"; }
    std::string get_sdk_version() override { return "fake-svbony-sdk"; }
    std::string get_firmware_version(int) override { return "fake-firmware"; }
    void set_camera_mode_normal(int) override {}
    void set_auto_save_param(int, bool) override {}
    void restore_default_param(int) override {}

    void return_roi(ROI roi) {
        std::lock_guard lock(mutex_);
        returned_roi_ = roi;
    }
    void return_image_type(ImageType type) {
        std::lock_guard lock(mutex_);
        returned_type_ = type;
    }
    void set_supported_formats(std::vector<ImageType> formats) {
        std::lock_guard lock(mutex_);
        camera_.supported_formats = std::move(formats);
    }
    void set_frame_data(std::vector<std::uint8_t> data) {
        std::lock_guard lock(mutex_);
        frame_data_ = std::move(data);
    }
    void fail_next_capture_start() {
        std::lock_guard lock(mutex_);
        fail_next_capture_start_ = true;
    }
    void set_camera_present(bool present) {
        std::lock_guard lock(mutex_);
        camera_present_ = present;
    }
    int open_count() const {
        std::lock_guard lock(mutex_);
        return open_count_;
    }

private:
    mutable std::mutex mutex_;
    CameraInfo camera_{};
    std::vector<ControlCaps> controls_;
    std::map<Control, long> values_;
    ROI roi_{};
    std::optional<ROI> returned_roi_;
    ImageType type_{ImageType::Raw16};
    std::optional<ImageType> returned_type_;
    std::vector<std::uint8_t> frame_data_;
    bool fail_next_capture_start_{false};
    int open_count_{0};
    bool camera_present_{true};
};

}  // namespace alpacacore::test
