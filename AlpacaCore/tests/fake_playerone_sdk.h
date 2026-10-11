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
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::test {

// KNOWN PARITY GAPS: one fake camera only (no shared open refcount or hot-plug
// enumeration); ordinary image-ready is immediate; set_image_size/start/bin
// model the state changes but not additional model-specific SDK alignment.
// hold_image_data intentionally ignores the SDK timeout to model a late return
// and is always released by explicit test release or stop_exposure(). The SDK
// returns only success/timeout, not an actual byte count, so a partial successful
// write cannot be detected by the driver or faithfully represented here.
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
        std::lock_guard<std::mutex> lock(mutex_);
        opened_ = true;
        open_count_.fetch_add(1);
    }
    void init_camera(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (init_failure_) throw std::runtime_error("scripted init failure");
        if (!opened_) throw std::runtime_error("init on a closed fake camera");
    }
    void close_camera(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (downloads_in_flight_ != 0) close_during_download_.store(true);
        opened_ = false;
        close_count_.fetch_add(1);
    }
    CameraInfo get_camera_properties_by_id(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_camera_properties_by_id");
        return camera_;
    }
    ConfigCaps probe_config_caps(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("probe_config_caps");
        return caps_;
    }

    long get_config_int(int, int config_id, bool* is_auto = nullptr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_config_int");
        if (is_auto) *is_auto = false;
        return config_id == 1 ? gain_.load() : 0;
    }
    void set_config_int(int, int config_id, long value, bool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_config_int");
        if (config_id == 1) gain_ = value;
    }
    double get_config_float(int, int, bool* is_auto = nullptr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_config_float");
        if (is_auto) *is_auto = false;
        return 0.0;
    }
    bool get_config_bool(int, int, bool* is_auto = nullptr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_config_bool");
        if (is_auto) *is_auto = false;
        return false;
    }
    void set_config_float(int, int, double, bool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_config_float");
    }
    void set_config_bool(int, int, bool, bool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_config_bool");
    }

    ImageFormat get_image_format(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_image_format");
        return returned_format_.value_or(format_);
    }
    void set_image_format(int, ImageFormat format) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_image_format");
        format_ = format;
    }
    void get_image_size(int, int& width, int& height) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_image_size");
        width = returned_width_.value_or(width_);
        height = returned_height_.value_or(height_);
    }
    void set_image_size(int, int width, int height) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_image_size");
        width_ = width;
        height_ = height;
        image_size_set_count_.fetch_add(1);
    }
    void get_image_start_pos(int, int& start_x, int& start_y) override {
        std::unique_lock<std::mutex> lock(mutex_);
        require_open_locked("get_image_start_pos");
        start_readback_started_.store(true);
        start_readback_cv_.notify_all();
        if (hold_start_readback_) {
            start_readback_cv_.wait(lock, [this] { return !hold_start_readback_; });
        }
        start_x = start_x_;
        start_y = start_y_;
    }
    void set_image_start_pos(int, int start_x, int start_y) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_image_start_pos");
        start_x_ = start_x;
        start_y_ = start_y;
    }
    int get_image_bin(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_image_bin");
        return bin_;
    }
    void set_image_bin(int, int bin) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_image_bin");
        bin_ = bin;
        width_ = camera_.max_width / bin;
        height_ = camera_.max_height / bin;
        start_x_ = 0;
        start_y_ = 0;
    }

    void start_exposure(int, bool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("start_exposure");
        stopped_ = false;
        exposure_started_ = true;
        ready_ = ready_on_start_.load();
        image_data_started_.store(false);
        image_data_finished_.store(false);
        exposure_start_count_.fetch_add(1);
    }
    void stop_exposure(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("stop_exposure");
        stop_count_.fetch_add(1);
        if (stop_failure_.load()) {
            if (ready_on_stop_failure_.load()) ready_ = true;
            throw std::runtime_error("scripted stop-exposure failure");
        }
        if (exposure_started_) {
            stopped_ = true;
            ready_ = true;
            exposure_started_ = false;
            download_cv_.notify_all();
        }
    }
    vendor::playerone::PlayerOneCameraState get_camera_state(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_camera_state");
        return vendor::playerone::PlayerOneCameraState::Opened;
    }
    bool image_ready(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("image_ready");
        if (readiness_error_.load()) throw std::runtime_error("scripted image-ready failure");
        return ready_.load();
    }
    bool get_image_data(int, std::uint8_t* buffer, std::size_t buffer_size, int timeout_ms) override {
        std::unique_lock<std::mutex> lock(mutex_);
        require_open_locked("get_image_data");
        image_data_started_.store(true);
        ++downloads_in_flight_;
        download_cv_.notify_all();
        const auto finish_download = [this] {
            --downloads_in_flight_;
            image_data_finished_.store(true);
            exposure_started_ = false;
        };
        if (hold_image_data_) {
            const auto released = [this] { return !hold_image_data_ || stopped_; };
            const bool completed = ignore_download_timeout_
                                       ? (download_cv_.wait(lock, released), true)
                                       : download_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), released);
            if (!completed) {
                finish_download();
                return false;
            }
        }
        if (download_failure_) {
            finish_download();
            throw std::runtime_error("scripted image download failure");
        }
        if (frame_data_.size() < buffer_size) {
            finish_download();
            return false;
        }
        std::copy_n(frame_data_.begin(), buffer_size, buffer);
        finish_download();
        return true;
    }

    void pulse_guide_on(int, vendor::playerone::PlayerOneGuideDirection) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("pulse_guide_on");
    }
    void pulse_guide_off(int, vendor::playerone::PlayerOneGuideDirection) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("pulse_guide_off");
        pulse_off_count_.fetch_add(1);
        if (pulse_off_failure_.load()) throw std::runtime_error("scripted pulse-off failure");
    }
    double get_temperature_c(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_temperature_c");
        return -5.0;
    }
    bool get_cooler_on(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_cooler_on");
        return false;
    }
    void set_cooler_on(int, bool) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_cooler_on");
    }
    int get_target_temp_c(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_target_temp_c");
        return -10;
    }
    void set_target_temp_c(int, int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_target_temp_c");
    }
    int get_cooler_power_percent(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_cooler_power_percent");
        return 0;
    }
    double get_egain(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_egain");
        return 1.0;
    }
    int get_heater_power_percent(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_heater_power_percent");
        return 0;
    }
    void set_heater_power_percent(int, int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_heater_power_percent");
    }
    int get_fan_power_percent(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("get_fan_power_percent");
        return 0;
    }
    void set_fan_power_percent(int, int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_open_locked("set_fan_power_percent");
    }

    void set_returned_size(int width, int height) {
        std::lock_guard<std::mutex> lock(mutex_);
        returned_width_ = width;
        returned_height_ = height;
    }
    void set_returned_format(ImageFormat format) {
        std::lock_guard<std::mutex> lock(mutex_);
        returned_format_ = format;
    }
    void set_supported_formats(std::vector<ImageFormat> formats) {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.supported_formats = std::move(formats);
        if (std::find(camera_.supported_formats.begin(), camera_.supported_formats.end(), ImageFormat::Rgb24) !=
            camera_.supported_formats.end()) {
            camera_.is_color = true;
            camera_.bayer = vendor::playerone::PlayerOneBayerPattern::RG;
        }
    }
    void set_has_st4_port(bool has_port) {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.has_st4_port = has_port;
    }
    void set_temperature_control_available(bool available) {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.has_cooler = available;
        caps_.has_cooler = available;
        caps_.has_target_temp = available;
        caps_.target_temp_min = -50;
        caps_.target_temp_max = 20;
    }
    void set_no_cameras(bool no_cameras) {
        std::lock_guard<std::mutex> lock(mutex_);
        no_cameras_ = no_cameras;
    }
    void set_open_delay(std::chrono::milliseconds delay) { open_delay_ = delay; }
    void set_hold_image_data(bool hold, bool ignore_timeout = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        hold_image_data_ = hold;
        ignore_download_timeout_ = ignore_timeout;
        if (!hold) download_cv_.notify_all();
    }
    void set_hold_start_readback(bool hold) {
        std::lock_guard<std::mutex> lock(mutex_);
        hold_start_readback_ = hold;
        start_readback_started_.store(false);
        if (!hold) start_readback_cv_.notify_all();
    }
    bool start_readback_started() const { return start_readback_started_.load(); }
    void set_init_failure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        init_failure_ = fail;
    }
    void set_download_failure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        download_failure_ = fail;
    }
    void set_pulse_off_failure(bool fail) { pulse_off_failure_ = fail; }
    void set_stop_failure(bool fail) { stop_failure_ = fail; }
    void set_ready_on_stop_failure(bool ready) { ready_on_stop_failure_ = ready; }
    void set_image_ready(bool ready) {
        ready_on_start_ = ready;
        ready_ = ready;
    }
    void set_readiness_error(bool fail) { readiness_error_ = fail; }
    void set_frame_data(std::vector<std::uint8_t> data) {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_data_ = std::move(data);
    }
    int open_count() const { return open_count_.load(); }
    int close_count() const { return close_count_.load(); }
    int pulse_off_count() const { return pulse_off_count_.load(); }
    int stop_count() const { return stop_count_.load(); }
    int exposure_start_count() const { return exposure_start_count_.load(); }
    bool image_data_started() const { return image_data_started_.load(); }
    bool image_data_finished() const { return image_data_finished_.load(); }
    bool close_during_download() const { return close_during_download_.load(); }
    bool operation_after_close() const { return operation_after_close_.load(); }
    bool opened() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return opened_;
    }
    int image_size_set_count() const { return image_size_set_count_.load(); }
    std::pair<int, int> last_set_image_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {width_, height_};
    }

private:
    void require_open_locked(const char* operation) {
        if (!opened_) {
            operation_after_close_.store(true);
            throw std::runtime_error(std::string(operation) + " on a closed fake camera");
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable start_readback_cv_;
    CameraInfo camera_{};
    ConfigCaps caps_{};
    std::condition_variable download_cv_;
    bool opened_{false};
    bool init_failure_{false};
    bool download_failure_{false};
    bool hold_image_data_{false};
    bool ignore_download_timeout_{false};
    bool hold_start_readback_{false};
    bool stopped_{false};
    bool exposure_started_{false};
    int downloads_in_flight_{0};
    bool no_cameras_{false};
    std::chrono::milliseconds open_delay_{0};
    ImageFormat format_{ImageFormat::Raw16};
    std::optional<ImageFormat> returned_format_;
    int width_{16};
    int height_{8};
    int start_x_{0};
    int start_y_{0};
    int bin_{1};
    std::optional<int> returned_width_;
    std::optional<int> returned_height_;
    std::vector<std::uint8_t> frame_data_ = std::vector<std::uint8_t>(16 * 8 * 2, 0);
    std::atomic<bool> ready_{true};
    std::atomic<bool> ready_on_start_{true};
    std::atomic<bool> readiness_error_{false};
    std::atomic<bool> image_data_started_{false};
    std::atomic<bool> image_data_finished_{false};
    std::atomic<long> gain_{10};
    std::atomic<int> open_count_{0};
    std::atomic<int> close_count_{0};
    std::atomic<int> pulse_off_count_{0};
    std::atomic<bool> pulse_off_failure_{false};
    std::atomic<bool> stop_failure_{false};
    std::atomic<bool> ready_on_stop_failure_{false};
    std::atomic<int> stop_count_{0};
    std::atomic<int> exposure_start_count_{0};
    std::atomic<int> image_size_set_count_{0};
    std::atomic<bool> close_during_download_{false};
    std::atomic<bool> operation_after_close_{false};
    std::atomic<bool> start_readback_started_{false};
};

}  // namespace alpacacore::test
