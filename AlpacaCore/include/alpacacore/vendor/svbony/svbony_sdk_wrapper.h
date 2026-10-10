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

#include <memory>

namespace alpacacore::vendor::svbony {

class SVBSDKWrapper final : public SVBSDK {
public:
    static SVBSDKWrapper& instance();

    std::vector<SVBCameraInfo> enumerate_cameras() override;
    bool get_camera_info_by_index(int camera_index, SVBCameraInfo& info) override;

    void open_camera(int camera_id) override;
    void close_camera(int camera_id) override;

    std::vector<SVBControlCaps> get_control_caps(int camera_id) override;
    bool get_control_value(int camera_id, SVBControlType type, long& value, bool& is_auto) override;
    void set_control_value(int camera_id, SVBControlType type, long value, bool is_auto) override;

    SVBROIFormat get_roi_format(int camera_id) override;
    void set_roi_format(int camera_id, int start_x, int start_y, int width, int height, int bin) override;

    SVBImageType get_output_image_type(int camera_id) override;
    void set_output_image_type(int camera_id, SVBImageType type) override;

    void start_video_capture(int camera_id) override;
    void stop_video_capture(int camera_id) override;
    void get_video_data(int camera_id, std::uint8_t* buffer, long buffer_size, int wait_ms) override;

    void pulse_guide(int camera_id, SVBGuideDirection direction, int duration_ms) override;

    float get_sensor_pixel_size(int camera_id) override;
    std::string get_serial_number(int camera_id) override;
    std::string get_sdk_version() override;
    std::string get_firmware_version(int camera_id) override;

    void set_camera_mode_normal(int camera_id) override;
    void set_auto_save_param(int camera_id, bool enable) override;
    void restore_default_param(int camera_id) override;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    SVBSDKWrapper();
    ~SVBSDKWrapper();
};

} // namespace alpacacore::vendor::svbony
