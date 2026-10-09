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

#include <alpacacore/vendor/playerone/playerone_sdk_wrapper.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::test {

class FakePlayerOneSDK final : public vendor::playerone::PlayerOneSDK {
public:
    using CameraInfo = vendor::playerone::PlayerOneCameraInfo;
    using ConfigCaps = vendor::playerone::PlayerOneConfigCaps;
    using ImageFormat = vendor::playerone::PlayerOneImageFormat;

    FakePlayerOneSDK() {
        camera_.index = 0;
        camera_.camera_id = 17;
        camera_.name = "Fake Player One Camera";
        camera_.sensor_model = "FakeSensor";
        camera_.serial_number = "fake-17";
        camera_.max_width = 16;
        camera_.max_height = 8;
        camera_.bit_depth = 16;
        camera_.supported_bins = {1, 2};
        camera_.supported_formats = {ImageFormat::Raw16, ImageFormat::Raw8, ImageFormat::Rgb24};
        caps_.has_gain = true;
        caps_.gain_writable = true;
        caps_.gain_min = 0;
        caps_.gain_max = 100;
        caps_.has_offset = true;
        caps_.offset_writable = true;
        caps_.offset_min = 0;
        caps_.offset_max = 255;
        caps_.has_exposure = true;
        caps_.exposure_min_us = 1;
        caps_.exposure_max_us = 10'000'000;
    }

    std::string get_sdk_version() override { return "fake-playerone-sdk"; }
    std::vector<CameraInfo> enumerate_cameras() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return no_cameras_ ? std::vector<CameraInfo>{} : std::vector<CameraInfo>{camera_};
    }
    void open_camera(int) override {
        std::this_thread::sleep_for(open_delay_);
        open_count_.fetch_add(1);
    }
    void init_camera(int) override {}
    void close_camera(int) override { close_count_.fetch_add(1); }
    CameraInfo get_camera_properties_by_id(int) override { return camera_copy(); }
    ConfigCaps probe_config_caps(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return caps_;
    }

    long get_config_int(int, int config_id, bool* is_auto = nullptr) override {
        if (is_auto) *is_auto = false;
        return config_id == 1 ? gain_.load() : 0;
    }
    void set_config_int(int, int config_id, long value, bool) override {
        if (config_id == 1) gain_ = value;
    }
    double get_config_float(int, int, bool* is_auto = nullptr) override {
        if (is_auto) *is_auto = false;
        return 0.0;
    }
    bool get_config_bool(int, int, bool* is_auto = nullptr) override {
        if (is_auto) *is_auto = false;
        return false;
    }
    void set_config_float(int, int, double, bool) override {}
    void set_config_bool(int, int, bool, bool) override {}

    ImageFormat get_image_format(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return format_;
    }
    void set_image_format(int, ImageFormat format) override {
        std::lock_guard<std::mutex> lock(mutex_);
        format_ = format;
    }
    void get_image_size(int, int& width, int& height) override {
        std::lock_guard<std::mutex> lock(mutex_);
        width = returned_width_.value_or(width_);
        height = returned_height_.value_or(height_);
    }
    void set_image_size(int, int width, int height) override {
        std::lock_guard<std::mutex> lock(mutex_);
        width_ = width;
        height_ = height;
        returned_width_.reset();
        returned_height_.reset();
    }
    void get_image_start_pos(int, int& start_x, int& start_y) override {
        start_x = 0;
        start_y = 0;
    }
    void set_image_start_pos(int, int, int) override {}
    int get_image_bin(int) override { return 1; }
    void set_image_bin(int, int) override {}

    void start_exposure(int, bool) override {}
    void stop_exposure(int) override {}
    vendor::playerone::PlayerOneCameraState get_camera_state(int) override {
        return vendor::playerone::PlayerOneCameraState::Opened;
    }
    bool image_ready(int) override { return ready_; }
    bool get_image_data(int, std::uint8_t* buffer, std::size_t buffer_size, int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame_data_.size() < buffer_size) return false;
        std::copy_n(frame_data_.begin(), buffer_size, buffer);
        return true;
    }

    void pulse_guide_on(int, vendor::playerone::PlayerOneGuideDirection) override {}
    void pulse_guide_off(int, vendor::playerone::PlayerOneGuideDirection) override { pulse_off_count_.fetch_add(1); }
    double get_temperature_c(int) override { return -5.0; }
    bool get_cooler_on(int) override { return false; }
    void set_cooler_on(int, bool) override {}
    int get_target_temp_c(int) override { return -10; }
    void set_target_temp_c(int, int) override {}
    int get_cooler_power_percent(int) override { return 0; }
    double get_egain(int) override { return 1.0; }
    int get_heater_power_percent(int) override { return 0; }
    void set_heater_power_percent(int, int) override {}
    int get_fan_power_percent(int) override { return 0; }
    void set_fan_power_percent(int, int) override {}

    void set_returned_size(int width, int height) {
        std::lock_guard<std::mutex> lock(mutex_);
        returned_width_ = width;
        returned_height_ = height;
    }
    void set_supported_formats(std::vector<ImageFormat> formats) {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.supported_formats = std::move(formats);
    }
    void set_has_st4_port(bool has_port) {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.has_st4_port = has_port;
    }
    void set_no_cameras(bool no_cameras) {
        std::lock_guard<std::mutex> lock(mutex_);
        no_cameras_ = no_cameras;
    }
    void set_open_delay(std::chrono::milliseconds delay) { open_delay_ = delay; }
    void set_frame_data(std::vector<std::uint8_t> data) {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_data_ = std::move(data);
    }
    int open_count() const { return open_count_.load(); }
    int close_count() const { return close_count_.load(); }
    int pulse_off_count() const { return pulse_off_count_.load(); }

private:
    CameraInfo camera_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_;
    }

    mutable std::mutex mutex_;
    CameraInfo camera_{};
    ConfigCaps caps_{};
    bool no_cameras_{false};
    std::chrono::milliseconds open_delay_{0};
    ImageFormat format_{ImageFormat::Raw16};
    int width_{16};
    int height_{8};
    std::optional<int> returned_width_;
    std::optional<int> returned_height_;
    std::vector<std::uint8_t> frame_data_ = std::vector<std::uint8_t>(16 * 8 * 2, 0);
    std::atomic<bool> ready_{true};
    std::atomic<long> gain_{10};
    std::atomic<int> open_count_{0};
    std::atomic<int> close_count_{0};
    std::atomic<int> pulse_off_count_{0};
};

}  // namespace alpacacore::test
