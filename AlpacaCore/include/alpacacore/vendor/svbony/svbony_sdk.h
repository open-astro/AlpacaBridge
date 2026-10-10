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

#include <cstdint>
#include <string>
#include <vector>

namespace alpacacore::vendor::svbony {

enum class SVBImageType { Raw8, Raw16, Y8, Y16, Rgb24, Rgb32 };

enum class SVBBayerPattern { None, RG, BG, GR, GB };

enum class SVBExposureStatus { Idle, Working, Success, Failed };

enum class SVBGuideDirection { North, South, East, West };

enum class SVBControlType {
    Gain,
    Exposure,
    Gamma,
    Offset,
    Flip,
    FrameSpeedMode,
    Contrast,
    Sharpness,
    Saturation,
    AutoTargetBrightness,
    BlackLevel,
    CoolerEnable,
    TargetTemperature,
    CurrentTemperature,
    CoolerPower,
    BadPixelCorrectionEnable,
    BadPixelCorrectionThreshold
};

struct SVBControlCaps {
    SVBControlType type;
    std::string name;
    std::string description;
    long min_value{};
    long max_value{};
    long default_value{};
    bool is_auto_supported{};
    bool is_writable{};
};

struct SVBCameraInfo {
    int camera_id{};
    std::string name;
    std::string serial_number;
    int max_width{};
    int max_height{};
    bool is_color{};
    SVBBayerPattern bayer_pattern{SVBBayerPattern::None};
    std::vector<int> supported_bins;
    std::vector<SVBImageType> supported_formats;
    double pixel_size_um{};
    bool supports_pulse_guide{};
    bool supports_cooler{};
    int bit_depth{};
};

struct SVBROIFormat {
    int start_x{};
    int start_y{};
    int width{};
    int height{};
    int bin{};
};

/**
 * @brief Abstract SVBONY camera SDK surface used by the camera driver.
 *
 * Production passes the SVBSDKWrapper singleton; tests inject a scripted fake
 * through the factory overload in svbony_camera_driver.h, so the driver runs
 * without the vendor SDK. The destructor is protected and non-virtual: nothing
 * owns an SVBSDK*, the driver holds a reference.
 */
class SVBSDK {
public:
    virtual std::vector<SVBCameraInfo> enumerate_cameras() = 0;
    virtual bool get_camera_info_by_index(int camera_index, SVBCameraInfo& info) = 0;

    virtual void open_camera(int camera_id) = 0;
    virtual void close_camera(int camera_id) = 0;

    virtual std::vector<SVBControlCaps> get_control_caps(int camera_id) = 0;
    virtual bool get_control_value(int camera_id, SVBControlType type, long& value, bool& is_auto) = 0;
    virtual void set_control_value(int camera_id, SVBControlType type, long value, bool is_auto) = 0;

    virtual SVBROIFormat get_roi_format(int camera_id) = 0;
    virtual void set_roi_format(int camera_id, int start_x, int start_y, int width, int height, int bin) = 0;

    virtual SVBImageType get_output_image_type(int camera_id) = 0;
    virtual void set_output_image_type(int camera_id, SVBImageType type) = 0;

    virtual void start_video_capture(int camera_id) = 0;
    virtual void stop_video_capture(int camera_id) = 0;
    virtual void get_video_data(int camera_id, std::uint8_t* buffer, long buffer_size, int wait_ms) = 0;

    virtual void pulse_guide(int camera_id, SVBGuideDirection direction, int duration_ms) = 0;

    virtual float get_sensor_pixel_size(int camera_id) = 0;
    virtual std::string get_serial_number(int camera_id) = 0;
    virtual std::string get_sdk_version() = 0;
    virtual std::string get_firmware_version(int camera_id) = 0;

    virtual void set_camera_mode_normal(int camera_id) = 0;
    virtual void set_auto_save_param(int camera_id, bool enable) = 0;
    virtual void restore_default_param(int camera_id) = 0;

protected:
    ~SVBSDK() = default;
};

}  // namespace alpacacore::vendor::svbony
