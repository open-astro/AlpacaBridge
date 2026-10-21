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

#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any ZWOSDK: every call forwards to the inner
 * implementation under one mutex.
 *
 * FakeZWOSDK locks its own state, so the decorator is not needed to keep the
 * fake race-free; it serialises the SDK the way a single-threaded vendor
 * library would, for tests that want that shape. All 22 entry points are
 * wrapped. The mutex is a leaf: the inner SDK never calls back into this
 * interface.
 *
 * Do not use it over a FakeZWOSDK configured to block: after hold_download()
 * get_data_after_exposure() waits for release_download(), and behind this
 * mutex the abort's stop_exposure() would queue behind the very download it
 * is meant to release.
 */
class LockedZWOSDK : public vendor::zwo::ZWOSDK {
public:
    using ZWOCameraInfo = vendor::zwo::ZWOCameraInfo;
    using ZWOControlCaps = vendor::zwo::ZWOControlCaps;
    using ZWOControlType = vendor::zwo::ZWOControlType;
    using ZWOImageType = vendor::zwo::ZWOImageType;
    using ZWOROIFormat = vendor::zwo::ZWOROIFormat;
    using ZWOStartPos = vendor::zwo::ZWOStartPos;
    using ZWOExposureStatus = vendor::zwo::ZWOExposureStatus;
    using ZWOGuideDirection = vendor::zwo::ZWOGuideDirection;
    using ZwoUnreadableSlot = vendor::zwo::ZwoUnreadableSlot;
    using ZwoEnumeratedCamera = vendor::zwo::ZwoEnumeratedCamera;

    explicit LockedZWOSDK(ZWOSDK& inner) : inner_(inner) {}

    std::vector<ZWOCameraInfo> enumerate_cameras(std::vector<ZwoUnreadableSlot>* unreadable = nullptr) override {
        return locked([&] { return inner_.enumerate_cameras(unreadable); });
    }
    std::vector<ZwoEnumeratedCamera> enumerate_identified_cameras(
        const std::string& only_model_name = {}, std::vector<ZwoUnreadableSlot>* unreadable = nullptr) override {
        return locked([&] { return inner_.enumerate_identified_cameras(only_model_name, unreadable); });
    }
    bool get_camera_info_by_id(int camera_id, ZWOCameraInfo& info) override {
        return locked([&] { return inner_.get_camera_info_by_id(camera_id, info); });
    }
    bool get_camera_info_by_index(int camera_index, ZWOCameraInfo& info) override {
        return locked([&] { return inner_.get_camera_info_by_index(camera_index, info); });
    }

    void open_camera(int camera_id) override {
        locked([&] { inner_.open_camera(camera_id); });
    }
    void init_camera(int camera_id) override {
        locked([&] { inner_.init_camera(camera_id); });
    }
    void close_camera(int camera_id) override {
        locked([&] { inner_.close_camera(camera_id); });
    }

    std::vector<ZWOControlCaps> get_control_caps(int camera_id) override {
        return locked([&] { return inner_.get_control_caps(camera_id); });
    }
    bool get_control_value(int camera_id, ZWOControlType type, long& value, bool& is_auto) override {
        return locked([&] { return inner_.get_control_value(camera_id, type, value, is_auto); });
    }
    void set_control_value(int camera_id, ZWOControlType type, long value, bool is_auto) override {
        locked([&] { inner_.set_control_value(camera_id, type, value, is_auto); });
    }

    ZWOROIFormat get_roi_format(int camera_id) override {
        return locked([&] { return inner_.get_roi_format(camera_id); });
    }
    void set_roi_format(int camera_id, int width, int height, int bin, ZWOImageType type) override {
        locked([&] { inner_.set_roi_format(camera_id, width, height, bin, type); });
    }

    ZWOStartPos get_start_pos(int camera_id) override {
        return locked([&] { return inner_.get_start_pos(camera_id); });
    }
    void set_start_pos(int camera_id, int start_x, int start_y) override {
        locked([&] { inner_.set_start_pos(camera_id, start_x, start_y); });
    }

    void start_exposure(int camera_id, bool is_dark) override {
        locked([&] { inner_.start_exposure(camera_id, is_dark); });
    }
    void stop_exposure(int camera_id) override {
        locked([&] { inner_.stop_exposure(camera_id); });
    }
    ZWOExposureStatus get_exposure_status(int camera_id) override {
        return locked([&] { return inner_.get_exposure_status(camera_id); });
    }
    void get_data_after_exposure(int camera_id, std::uint8_t* buffer, long buffer_size) override {
        locked([&] { inner_.get_data_after_exposure(camera_id, buffer, buffer_size); });
    }

    void pulse_guide_on(int camera_id, ZWOGuideDirection direction) override {
        locked([&] { inner_.pulse_guide_on(camera_id, direction); });
    }
    void pulse_guide_off(int camera_id, ZWOGuideDirection direction) override {
        locked([&] { inner_.pulse_guide_off(camera_id, direction); });
    }

    std::string get_serial_number(int camera_id) override {
        return locked([&] { return inner_.get_serial_number(camera_id); });
    }
    std::string get_sdk_version() override {
        return locked([&] { return inner_.get_sdk_version(); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn();
    }

    ZWOSDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
