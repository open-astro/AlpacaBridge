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
#include <alpacacore/vendor/svbony/svbony_sdk.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Scripted fake for the SVBSDK seam.
 *
 * - Fault injection: add a method name to `throw_from` and that call throws
 *   AlpacaException(DriverException), e.g. "open_camera" or "get_serial_number".
 * - `before_call` runs before every call (persistent, null in ordinary tests);
 *   the contract sweep sets it to hold a connect open.
 * - Canned camera: `default_camera()` plus `default_caps()` model a mono
 *   camera with Gain and Exposure controls; `get_video_data` fills the buffer
 *   with a constant pattern and never blocks.
 * - Open/close bookkeeping: `open_count`/`close_count` per camera id so a test
 *   can assert the open is balanced after a failed connect.
 *
 * Not thread-hardened: wrap it in LockedSVBSDK for any test that drives the
 * driver from more than one thread. KNOWN PARITY GAP: the real SDK fails
 * set_control_value on a closed camera; this fake accepts it.
 *
 * LIFETIME: drivers hold a plain SVBSDK&; the fake MUST outlive every driver
 * built on it.
 */
class FakeSVBSDK : public vendor::svbony::SVBSDK {
public:
    using SVBCameraInfo = vendor::svbony::SVBCameraInfo;
    using SVBControlCaps = vendor::svbony::SVBControlCaps;
    using SVBControlType = vendor::svbony::SVBControlType;
    using SVBImageType = vendor::svbony::SVBImageType;
    using SVBROIFormat = vendor::svbony::SVBROIFormat;
    using SVBGuideDirection = vendor::svbony::SVBGuideDirection;

    std::set<std::string> throw_from;
    std::function<void(const std::string&)> before_call;
    std::vector<SVBCameraInfo> cameras;
    std::vector<SVBControlCaps> caps = default_caps();
    std::map<std::string, int> calls;
    std::map<int, int> open_count;
    std::map<int, int> close_count;
    std::string serial = "SVB-FAKE-0001";
    std::string firmware = "1.2.3";
    std::string sdk_version = "1.13.0-fake";
    std::uint8_t fill_byte = 0x40;

    FakeSVBSDK() { cameras.push_back(default_camera()); }

    static SVBCameraInfo default_camera(int id = 7, const std::string& name = "SVBONY Fake Cam") {
        SVBCameraInfo info;
        info.camera_id = id;
        info.name = name;
        info.serial_number = "SVB-FAKE-0001";
        info.max_width = 64;
        info.max_height = 48;
        info.is_color = false;
        info.supported_bins = {1, 2};
        info.supported_formats = {SVBImageType::Raw8, SVBImageType::Raw16};
        info.pixel_size_um = 3.75;
        info.supports_pulse_guide = true;
        info.bit_depth = 16;
        return info;
    }

    static std::vector<SVBControlCaps> default_caps() {
        std::vector<SVBControlCaps> out;
        SVBControlCaps gain;
        gain.type = SVBControlType::Gain;
        gain.name = "Gain";
        gain.min_value = 0;
        gain.max_value = 500;
        gain.default_value = 100;
        gain.is_writable = true;
        out.push_back(gain);
        SVBControlCaps exposure;
        exposure.type = SVBControlType::Exposure;
        exposure.name = "Exposure";
        exposure.min_value = 32;
        exposure.max_value = 2000000000;
        exposure.default_value = 10000;
        exposure.is_writable = true;
        out.push_back(exposure);
        return out;
    }

    int call_count(const char* fn) const {
        auto it = calls.find(fn);
        return it == calls.end() ? 0 : it->second;
    }

    std::vector<SVBCameraInfo> enumerate_cameras() override {
        enter("enumerate_cameras");
        return cameras;
    }
    bool get_camera_info_by_index(int camera_index, SVBCameraInfo& info) override {
        enter("get_camera_info_by_index");
        if (camera_index < 0 || camera_index >= static_cast<int>(cameras.size())) return false;
        info = cameras[static_cast<std::size_t>(camera_index)];
        return true;
    }

    void open_camera(int camera_id) override {
        enter("open_camera");
        ++open_count[camera_id];
    }
    void close_camera(int camera_id) override {
        enter("close_camera");
        ++close_count[camera_id];
    }

    std::vector<SVBControlCaps> get_control_caps(int) override {
        enter("get_control_caps");
        return caps;
    }
    bool get_control_value(int, SVBControlType type, long& value, bool& is_auto) override {
        enter("get_control_value");
        auto it = values_.find(type);
        if (it == values_.end()) return false;
        value = it->second;
        is_auto = false;
        return true;
    }
    void set_control_value(int, SVBControlType type, long value, bool) override {
        enter("set_control_value");
        values_[type] = value;
    }

    SVBROIFormat get_roi_format(int) override {
        enter("get_roi_format");
        return roi_;
    }
    void set_roi_format(int, int start_x, int start_y, int width, int height, int bin) override {
        enter("set_roi_format");
        roi_ = SVBROIFormat{start_x, start_y, width, height, bin};
    }

    SVBImageType get_output_image_type(int) override {
        enter("get_output_image_type");
        return image_type_;
    }
    void set_output_image_type(int, SVBImageType type) override {
        enter("set_output_image_type");
        image_type_ = type;
    }

    void start_video_capture(int) override { enter("start_video_capture"); }
    void stop_video_capture(int) override { enter("stop_video_capture"); }
    void get_video_data(int, std::uint8_t* buffer, long buffer_size, int) override {
        enter("get_video_data");
        if (buffer != nullptr && buffer_size > 0) {
            std::fill(buffer, buffer + buffer_size, fill_byte);
        }
    }

    void pulse_guide(int, SVBGuideDirection, int) override { enter("pulse_guide"); }

    float get_sensor_pixel_size(int) override {
        enter("get_sensor_pixel_size");
        return 3.75F;
    }
    std::string get_serial_number(int) override {
        enter("get_serial_number");
        return serial;
    }
    std::string get_sdk_version() override {
        enter("get_sdk_version");
        return sdk_version;
    }
    std::string get_firmware_version(int) override {
        enter("get_firmware_version");
        return firmware;
    }

    void set_camera_mode_normal(int) override { enter("set_camera_mode_normal"); }
    void set_auto_save_param(int, bool) override { enter("set_auto_save_param"); }
    void restore_default_param(int) override { enter("restore_default_param"); }

private:
    void enter(const char* fn) {
        ++calls[fn];
        if (before_call) before_call(fn);
        if (throw_from.count(fn) != 0) {
            throw AlpacaException(std::string("FakeSVBSDK: injected failure in ") + fn, AlpacaError::DriverException);
        }
    }

    std::map<SVBControlType, long> values_;
    SVBROIFormat roi_{0, 0, 64, 48, 1};
    SVBImageType image_type_ = SVBImageType::Raw16;
};

}  // namespace alpacacore::test
