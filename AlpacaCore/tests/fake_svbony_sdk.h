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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::test {

// KNOWN PARITY LIMITS:
// - SVBGetVideoData has no written-byte count, so the fake can validate the
//   caller's capacity against its own configured ROI/format, but cannot prove
//   how many bytes the native SDK actually wrote.
// - block_video_data() intentionally models a wedged SDK call that ignores its
//   wait_ms argument; the fake stop/release controls unblock it for lifecycle
//   tests, but do not claim the native SDK stop interrupts an in-flight read.
//   It still has a configurable hard timeout for test cleanup.
// - timeout_video_data() models ordinary finite SDK timeouts at the requested
//   wait_ms cadence, independently of the held-call mode above.
// - The synthetic frame payload is deterministic and is not a sensor model.
// - Fake failures throw immediately; these seam tests do not cover the native
//   wrapper's SVB_ERROR_GENERAL_ERROR retry policy.
// - Timeout results are returned directly by the seam; this fake does not
//   verify the native wrapper's SVB_ERROR_TIMEOUT translation.
class FakeSVBSDK final : public vendor::svbony::SVBSDK {
public:
    // Test-only hook, null by default: runs on the calling thread at the top of get_video_data() and
    // pulse_guide(), before the fake takes its mutex, so it may advance a FakeTaskClock. Set it before the
    // driver exists and never change it while a call is in flight (FakeGPhotoSDK::before_call ownership).
    std::function<void(const std::string&)> before_call;

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
        camera_.supported_formats = {ImageType::Raw16, ImageType::Raw8, ImageType::Rgb24, ImageType::Y8,
                                     ImageType::Y16};
        camera_.pixel_size_um = 3.76;
        camera_.supports_pulse_guide = true;
        camera_.bit_depth = 12;
        controls_ = {
            {Control::Exposure, "Exposure", "Exposure", 1, 10'000'000, 1000, false, true},
            {Control::Gain, "Gain", "Gain", 0, 100, 10, true, true},
            {Control::Offset, "Offset", "Offset", 0, 255, 10, false, true},
            {Control::FrameSpeedMode, "Frame Speed", "Frame speed mode", 0, 2, 0, false, true},
            {Control::CurrentTemperature, "Temperature", "Temperature", -500, 500, 0, false, false},
        };
        values_ = {{Control::Exposure, 1000}, {Control::Gain, 10}, {Control::Offset, 10}, {Control::FrameSpeedMode, 0}};
    }

    std::vector<CameraInfo> enumerate_cameras() override {
        std::lock_guard lock(mutex_);
        record_locked("enumerate_cameras");
        maybe_fail_locked("enumerate_cameras");
        return camera_present_ ? std::vector<CameraInfo>{camera_} : std::vector<CameraInfo>{};
    }
    bool get_camera_info_by_index(int index, CameraInfo& info) override {
        std::lock_guard lock(mutex_);
        record_locked("get_camera_info_by_index");
        maybe_fail_locked("get_camera_info_by_index");
        if (index != 0 || !camera_present_) return false;
        info = camera_;
        return true;
    }
    void open_camera(int camera_id) override {
        // Outside the mutex, so the delay holds only the caller (the driver's connect), not other fake calls.
        const auto delay = std::chrono::milliseconds(open_delay_ms_.load());
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        std::lock_guard lock(mutex_);
        record_locked("open_camera");
        maybe_fail_locked("open_camera");
        require_camera_locked(camera_id);
        ++open_count_;
        opened_ids_.insert(camera_id);
    }
    void close_camera(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("close_camera");
        maybe_fail_locked("close_camera");
        require_open_locked(camera_id);
        if (video_data_held_) {
            throw alpacacore::AlpacaException("Fake SVBONY camera closed during an active frame read",
                                              alpacacore::AlpacaError::DriverException);
        }
        capture_ids_.erase(camera_id);
        release_read_ = true;
        video_data_cv_.notify_all();
        if (--open_count_ == 0) opened_ids_.clear();
    }
    std::vector<ControlCaps> get_control_caps(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("get_control_caps");
        maybe_fail_locked("get_control_caps");
        require_open_locked(camera_id);
        return controls_;
    }
    bool get_control_value(int camera_id, Control type, long& value, bool& is_auto) override {
        std::lock_guard lock(mutex_);
        record_locked("get_control_value");
        maybe_fail_locked("get_control_value");
        require_open_locked(camera_id);
        auto it = values_.find(type);
        if (it == values_.end()) return false;
        value = it->second;
        is_auto = auto_values_[type];
        return true;
    }
    void set_control_value(int camera_id, Control type, long value, bool is_auto) override {
        std::lock_guard lock(mutex_);
        record_locked("set_control_value");
        if (type == Control::Exposure) record_locked("set_control_value.Exposure");
        if (type == Control::FrameSpeedMode) {
            record_locked("set_control_value.FrameSpeedMode");
            maybe_fail_locked("set_control_value.FrameSpeedMode");
        }
        maybe_fail_locked("set_control_value");
        require_open_locked(camera_id);
        if (capture_ids_.contains(camera_id) &&
            (type == Control::Exposure || type == Control::Gain || type == Control::Offset)) {
            throw alpacacore::AlpacaException("Fake SVBONY sensor control changed during video capture",
                                              alpacacore::AlpacaError::DriverException);
        }
        values_[type] = value;
        auto_values_[type] = is_auto;
    }
    ROI get_roi_format(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("get_roi_format");
        maybe_fail_locked("get_roi_format");
        require_open_locked(camera_id);
        return returned_roi_.value_or(roi_);
    }
    void set_roi_format(int camera_id, int x, int y, int width, int height, int bin) override {
        std::lock_guard lock(mutex_);
        record_locked("set_roi_format");
        maybe_fail_locked("set_roi_format");
        require_open_locked(camera_id);
        if (capture_ids_.contains(camera_id)) {
            throw alpacacore::AlpacaException("Fake SVBONY ROI changed during video capture",
                                              alpacacore::AlpacaError::DriverException);
        }
        if (bin <= 0 || width <= 0 || height <= 0 || x < 0 || y < 0 ||
            std::find(camera_.supported_bins.begin(), camera_.supported_bins.end(), bin) ==
                camera_.supported_bins.end()) {
            throw alpacacore::AlpacaException("Fake SVBONY ROI is outside configured sensor bounds",
                                              alpacacore::AlpacaError::InvalidValue);
        }
        const int max_width = camera_.max_width / bin;
        const int max_height = camera_.max_height / bin;
        if (x > max_width || y > max_height || width > max_width - x || height > max_height - y) {
            throw alpacacore::AlpacaException("Fake SVBONY ROI is outside configured sensor bounds",
                                              alpacacore::AlpacaError::InvalidValue);
        }
        roi_ = {x, y, width, height, bin};
    }
    ImageType get_output_image_type(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("get_output_image_type");
        maybe_fail_locked("get_output_image_type");
        require_open_locked(camera_id);
        return returned_type_.value_or(type_);
    }
    void set_output_image_type(int camera_id, ImageType type) override {
        std::lock_guard lock(mutex_);
        record_locked("set_output_image_type");
        maybe_fail_locked("set_output_image_type");
        require_open_locked(camera_id);
        if (capture_ids_.contains(camera_id)) {
            throw alpacacore::AlpacaException("Fake SVBONY format changed during video capture",
                                              alpacacore::AlpacaError::DriverException);
        }
        if (std::find(camera_.supported_formats.begin(), camera_.supported_formats.end(), type) ==
            camera_.supported_formats.end()) {
            throw alpacacore::AlpacaException("Fake SVBONY output format is unsupported",
                                              alpacacore::AlpacaError::DriverException);
        }
        type_ = type;
    }
    void start_video_capture(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("start_video_capture");
        maybe_fail_locked("start_video_capture");
        require_open_locked(camera_id);
        capture_ids_.insert(camera_id);
    }
    void stop_video_capture(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("stop_video_capture");
        maybe_fail_locked("stop_video_capture");
        if (!opened_ids_.contains(camera_id)) return;
        capture_ids_.erase(camera_id);
        // Allow deterministic lifecycle tests to release a held fake read.
        // This is not evidence that native SVBStopVideoCapture interrupts one.
        release_read_ = true;
        block_video_data_ = false;
        video_data_cv_.notify_all();
    }
    vendor::svbony::SVBVideoDataResult get_video_data(int camera_id, std::uint8_t* buffer, long capacity,
                                                      int wait_ms) override {
        if (before_call) before_call("get_video_data");
        std::unique_lock lock(mutex_);
        last_video_wait_ms_ = wait_ms;
        record_locked("get_video_data");
        maybe_fail_locked("get_video_data");
        require_open_locked(camera_id);
        if (!capture_ids_.contains(camera_id)) {
            throw alpacacore::AlpacaException("Fake SVBONY capture is stopped",
                                              alpacacore::AlpacaError::DriverException);
        }
        if (block_video_data_) {
            // In held mode, emulate a stuck native call (which may outlive its
            // nominal SDK timeout) while keeping an absolute test-side bound.
            video_data_held_ = true;
            video_data_cv_.notify_all();
            const auto hold_wait = std::chrono::milliseconds(max_read_wait_ms_);
            const bool released = video_data_cv_.wait_for(
                lock, hold_wait, [this, camera_id] { return release_read_ || !capture_ids_.contains(camera_id); });
            if (!released) {
                require_open_locked(camera_id);
                video_data_held_ = false;
                record_locked("get_video_data_complete");
                ++completed_video_data_reads_;
                video_data_cv_.notify_all();
                return vendor::svbony::SVBVideoDataResult::Timeout;
            }
            if (!capture_ids_.contains(camera_id)) {
                require_open_locked(camera_id);
                video_data_held_ = false;
                record_locked("get_video_data_complete");
                ++completed_video_data_reads_;
                video_data_cv_.notify_all();
                return vendor::svbony::SVBVideoDataResult::Timeout;
            }
            if (fail_after_held_read_) {
                fail_after_held_read_ = false;
                video_data_held_ = false;
                record_locked("get_video_data_complete");
                ++completed_video_data_reads_;
                video_data_cv_.notify_all();
                throw alpacacore::AlpacaException("scripted late SVBONY read failure",
                                                  alpacacore::AlpacaError::DriverException);
            }
        }
        video_data_held_ = false;
        if (timeout_video_data_) {
            video_data_cv_.wait_for(lock, std::chrono::milliseconds(std::max(0, wait_ms)),
                                    [this] { return release_read_; });
            require_open_locked(camera_id);
            record_locked("get_video_data_complete");
            ++completed_video_data_reads_;
            video_data_cv_.notify_all();
            return vendor::svbony::SVBVideoDataResult::Timeout;
        }
        if (timeout_reads_remaining_ > 0) {
            --timeout_reads_remaining_;
            record_locked("get_video_data_complete");
            ++completed_video_data_reads_;
            video_data_cv_.notify_all();
            return vendor::svbony::SVBVideoDataResult::Timeout;
        }
        require_open_locked(camera_id);  // detect accidental close-under-read
        const auto required = required_capacity_locked();
        if (capacity < 0 || static_cast<std::size_t>(capacity) < required || buffer == nullptr) {
            throw alpacacore::AlpacaException("Fake SVBONY buffer capacity is smaller than configured ROI/format",
                                              alpacacore::AlpacaError::DriverException);
        }
        if (frame_data_.size() < required) {
            throw alpacacore::AlpacaException("Fake SVBONY frame source is shorter than configured ROI/format",
                                              alpacacore::AlpacaError::DriverException);
        }
        last_buffer_capacity_ = static_cast<std::size_t>(capacity);
        std::copy_n(frame_data_.begin(), required, buffer);
        record_locked("get_video_data_complete");
        ++completed_video_data_reads_;
        video_data_cv_.notify_all();
        return vendor::svbony::SVBVideoDataResult::Frame;
    }
    void pulse_guide(int camera_id, vendor::svbony::SVBGuideDirection, int) override {
        if (before_call) before_call("pulse_guide");
        std::lock_guard lock(mutex_);
        record_locked("pulse_guide");
        maybe_fail_locked("pulse_guide");
        require_open_locked(camera_id);
    }
    float get_sensor_pixel_size(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("get_sensor_pixel_size");
        maybe_fail_locked("get_sensor_pixel_size");
        require_open_locked(camera_id);
        return 3.76F;
    }
    std::string get_serial_number(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("get_serial_number");
        maybe_fail_locked("get_serial_number");
        require_open_locked(camera_id);
        return "fake-17";
    }
    std::string get_sdk_version() override {
        std::lock_guard lock(mutex_);
        record_locked("get_sdk_version");
        maybe_fail_locked("get_sdk_version");
        return "fake-svbony-sdk";
    }
    std::string get_firmware_version(int camera_id) override {
        std::lock_guard lock(mutex_);
        record_locked("get_firmware_version");
        maybe_fail_locked("get_firmware_version");
        require_open_locked(camera_id);
        return "fake-firmware";
    }
    void set_camera_mode_normal(int camera_id) override { simple_open_call("set_camera_mode_normal", camera_id); }
    void set_auto_save_param(int camera_id, bool) override { simple_open_call("set_auto_save_param", camera_id); }
    void restore_default_param(int camera_id) override { simple_open_call("restore_default_param", camera_id); }

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
    void set_sensor_size(int width, int height) {
        std::lock_guard lock(mutex_);
        camera_.max_width = width;
        camera_.max_height = height;
    }
    void set_color_sensor(bool is_color) {
        std::lock_guard lock(mutex_);
        camera_.is_color = is_color;
    }
    void set_frame_data(std::vector<std::uint8_t> data) {
        std::lock_guard lock(mutex_);
        frame_data_ = std::move(data);
    }
    void set_camera_present(bool present) {
        std::lock_guard lock(mutex_);
        camera_present_ = present;
    }
    void set_auto(Control control, bool is_auto) {
        std::lock_guard lock(mutex_);
        auto_values_[control] = is_auto;
    }
    // Holds each open_camera() for `delay` (bounded, never indefinite), so a test can observe Connecting.
    void set_open_delay(std::chrono::milliseconds delay) {
        open_delay_ms_.store(std::clamp<std::int64_t>(delay.count(), 0, 20000));
    }
    void set_max_read_wait(std::chrono::milliseconds timeout) {
        std::lock_guard lock(mutex_);
        max_read_wait_ms_ = static_cast<int>(std::clamp<std::int64_t>(timeout.count(), 1, 20000));
    }
    void timeout_next_video_data(int count = 1) {
        std::lock_guard lock(mutex_);
        timeout_reads_remaining_ = std::max(0, count);
    }
    void timeout_video_data(bool enabled = true) {
        std::lock_guard lock(mutex_);
        timeout_video_data_ = enabled;
    }
    void fail_next_held_read() {
        std::lock_guard lock(mutex_);
        fail_after_held_read_ = true;
    }
    void block_video_data() {
        std::lock_guard lock(mutex_);
        block_video_data_ = true;
        release_read_ = false;
        video_data_held_ = false;
    }
    bool wait_for_video_data(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return video_data_cv_.wait_for(lock, timeout, [this] { return video_data_held_; });
    }
    bool wait_for_completed_video_data(int count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return video_data_cv_.wait_for(lock, timeout, [this, count] { return completed_video_data_reads_ >= count; });
    }
    void release_video_data() {
        std::lock_guard lock(mutex_);
        release_read_ = true;
        block_video_data_ = false;
        video_data_cv_.notify_all();
    }
    void fail_next(std::string method) {
        std::lock_guard lock(mutex_);
        ++one_shot_failures_[std::move(method)];
    }
    void fail_next_capture_start() { fail_next("start_video_capture"); }
    void set_persistent_failure(std::string method, bool enabled = true) {
        std::lock_guard lock(mutex_);
        if (enabled)
            persistent_failures_.insert(std::move(method));
        else
            persistent_failures_.erase(method);
    }
    int call_count(const std::string& method) const {
        std::lock_guard lock(mutex_);
        auto it = call_counts_.find(method);
        return it == call_counts_.end() ? 0 : it->second;
    }
    std::vector<std::string> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }
    int open_count() const {
        std::lock_guard lock(mutex_);
        return open_count_;
    }
    std::size_t last_buffer_capacity() const {
        std::lock_guard lock(mutex_);
        return last_buffer_capacity_;
    }
    int last_video_wait_ms() const {
        std::lock_guard lock(mutex_);
        return last_video_wait_ms_;
    }
    int completed_video_data_reads() const {
        std::lock_guard lock(mutex_);
        return completed_video_data_reads_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable video_data_cv_;
    CameraInfo camera_{};
    std::vector<ControlCaps> controls_;
    std::map<Control, long> values_;
    std::map<Control, bool> auto_values_;
    ROI roi_{0, 0, 16, 8, 1};
    std::optional<ROI> returned_roi_;
    ImageType type_{ImageType::Raw16};
    std::optional<ImageType> returned_type_;
    std::vector<std::uint8_t> frame_data_ = [] {
        std::vector<std::uint8_t> data(16 * 8 * 4);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
        return data;
    }();
    std::set<int> opened_ids_;
    std::set<int> capture_ids_;
    std::map<std::string, int> call_counts_;
    std::map<std::string, int> one_shot_failures_;
    std::set<std::string> persistent_failures_;
    std::vector<std::string> events_;
    std::size_t last_buffer_capacity_{0};
    int last_video_wait_ms_{0};
    int max_read_wait_ms_{250};
    std::atomic<std::int64_t> open_delay_ms_{0};
    int timeout_reads_remaining_{0};
    int completed_video_data_reads_{0};
    int open_count_{0};
    bool camera_present_{true};
    bool block_video_data_{false};
    bool timeout_video_data_{false};
    bool fail_after_held_read_{false};
    bool release_read_{false};
    bool video_data_held_{false};

    void record_locked(const std::string& method) {
        ++call_counts_[method];
        events_.push_back(method);
    }
    void maybe_fail_locked(const std::string& method) {
        if (persistent_failures_.contains(method)) {
            throw alpacacore::AlpacaException("scripted persistent SVBONY failure: " + method,
                                              alpacacore::AlpacaError::DriverException);
        }
        auto it = one_shot_failures_.find(method);
        if (it != one_shot_failures_.end() && it->second > 0) {
            if (--it->second == 0) one_shot_failures_.erase(it);
            throw alpacacore::AlpacaException("scripted one-shot SVBONY failure: " + method,
                                              alpacacore::AlpacaError::DriverException);
        }
    }
    void require_camera_locked(int camera_id) const {
        if (!camera_present_ || camera_id != camera_.camera_id) {
            throw alpacacore::AlpacaException("Fake SVBONY camera id is invalid",
                                              alpacacore::AlpacaError::DriverException);
        }
    }
    void require_open_locked(int camera_id) const {
        if (!opened_ids_.contains(camera_id)) {
            throw alpacacore::AlpacaException("Fake SVBONY SDK call used a closed camera",
                                              alpacacore::AlpacaError::DriverException);
        }
    }
    std::size_t required_capacity_locked() const {
        if (roi_.width <= 0 || roi_.height <= 0) {
            throw alpacacore::AlpacaException("Fake SVBONY ROI is invalid", alpacacore::AlpacaError::DriverException);
        }
        std::size_t bytes_per_pixel = 0;
        switch (type_) {
            case ImageType::Raw8:
            case ImageType::Y8:
                bytes_per_pixel = 1;
                break;
            case ImageType::Raw16:
            case ImageType::Y16:
                bytes_per_pixel = 2;
                break;
            case ImageType::Rgb24:
                bytes_per_pixel = 3;
                break;
            case ImageType::Rgb32:
            case ImageType::Unknown:
                throw alpacacore::AlpacaException("Fake SVBONY format is unsupported",
                                                  alpacacore::AlpacaError::DriverException);
        }
        const auto width = static_cast<std::size_t>(roi_.width);
        const auto height = static_cast<std::size_t>(roi_.height);
        if (width > static_cast<std::size_t>(-1) / height ||
            width * height > static_cast<std::size_t>(-1) / bytes_per_pixel) {
            throw alpacacore::AlpacaException("Fake SVBONY frame size overflow",
                                              alpacacore::AlpacaError::DriverException);
        }
        return width * height * bytes_per_pixel;
    }
    void simple_open_call(const std::string& method, int camera_id) {
        std::lock_guard lock(mutex_);
        record_locked(method);
        maybe_fail_locked(method);
        require_open_locked(camera_id);
    }
};

}  // namespace alpacacore::test
