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

#include <alpacacore/vendor/svbony/svbony_sdk.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any SVBSDK: every call forwards to the inner
 * implementation under one mutex.
 *
 * Exists for the concurrency stress harness: FakeSVBSDK is deliberately not
 * thread-hardened, but the stress tests hammer one driver from many threads, so
 * unguarded fake state would report races in test code and hide the driver
 * races the harness exists to catch. The real wrapper serialises internally,
 * so production drivers never need this. All 22 entry points are wrapped, since
 * the driver's exposure and pulse-guide workers call the SDK from their own
 * threads. The mutex is a leaf: the inner SDK never calls back into this
 * interface, and no FakeSVBSDK method blocks.
 */
class LockedSVBSDK : public vendor::svbony::SVBSDK {
public:
    using SVBCameraInfo = vendor::svbony::SVBCameraInfo;
    using SVBControlCaps = vendor::svbony::SVBControlCaps;
    using SVBControlType = vendor::svbony::SVBControlType;
    using SVBImageType = vendor::svbony::SVBImageType;
    using SVBROIFormat = vendor::svbony::SVBROIFormat;
    using SVBGuideDirection = vendor::svbony::SVBGuideDirection;

    explicit LockedSVBSDK(SVBSDK& inner) : inner_(inner) {}

    std::vector<SVBCameraInfo> enumerate_cameras() override {
        return locked([&] { return inner_.enumerate_cameras(); });
    }
    bool get_camera_info_by_index(int camera_index, SVBCameraInfo& info) override {
        return locked([&] { return inner_.get_camera_info_by_index(camera_index, info); });
    }

    void open_camera(int camera_id) override {
        locked([&] { inner_.open_camera(camera_id); });
    }
    void close_camera(int camera_id) override {
        locked([&] { inner_.close_camera(camera_id); });
    }

    std::vector<SVBControlCaps> get_control_caps(int camera_id) override {
        return locked([&] { return inner_.get_control_caps(camera_id); });
    }
    bool get_control_value(int camera_id, SVBControlType type, long& value, bool& is_auto) override {
        return locked([&] { return inner_.get_control_value(camera_id, type, value, is_auto); });
    }
    void set_control_value(int camera_id, SVBControlType type, long value, bool is_auto) override {
        locked([&] { inner_.set_control_value(camera_id, type, value, is_auto); });
    }

    SVBROIFormat get_roi_format(int camera_id) override {
        return locked([&] { return inner_.get_roi_format(camera_id); });
    }
    void set_roi_format(int camera_id, int start_x, int start_y, int width, int height, int bin) override {
        locked([&] { inner_.set_roi_format(camera_id, start_x, start_y, width, height, bin); });
    }

    SVBImageType get_output_image_type(int camera_id) override {
        return locked([&] { return inner_.get_output_image_type(camera_id); });
    }
    void set_output_image_type(int camera_id, SVBImageType type) override {
        locked([&] { inner_.set_output_image_type(camera_id, type); });
    }

    void start_video_capture(int camera_id) override {
        locked([&] { inner_.start_video_capture(camera_id); });
    }
    void stop_video_capture(int camera_id) override {
        locked([&] { inner_.stop_video_capture(camera_id); });
    }
    void get_video_data(int camera_id, std::uint8_t* buffer, long buffer_size, int wait_ms) override {
        locked([&] { inner_.get_video_data(camera_id, buffer, buffer_size, wait_ms); });
    }

    void pulse_guide(int camera_id, SVBGuideDirection direction, int duration_ms) override {
        locked([&] { inner_.pulse_guide(camera_id, direction, duration_ms); });
    }

    float get_sensor_pixel_size(int camera_id) override {
        return locked([&] { return inner_.get_sensor_pixel_size(camera_id); });
    }
    std::string get_serial_number(int camera_id) override {
        return locked([&] { return inner_.get_serial_number(camera_id); });
    }
    std::string get_sdk_version() override {
        return locked([&] { return inner_.get_sdk_version(); });
    }
    std::string get_firmware_version(int camera_id) override {
        return locked([&] { return inner_.get_firmware_version(camera_id); });
    }

    void set_camera_mode_normal(int camera_id) override {
        locked([&] { inner_.set_camera_mode_normal(camera_id); });
    }
    void set_auto_save_param(int camera_id, bool enable) override {
        locked([&] { inner_.set_auto_save_param(camera_id, enable); });
    }
    void restore_default_param(int camera_id) override {
        locked([&] { inner_.restore_default_param(camera_id); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn();
    }

    SVBSDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
