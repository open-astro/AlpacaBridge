// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <optional>

namespace alpacacore::vendor::svbony {

enum class SVBImageType {
    Raw8,
    Raw16,
    Y8,
    Y16,
    Rgb24,
    Rgb32
};

enum class SVBBayerPattern {
    None,
    RG,
    BG,
    GR,
    GB
};

enum class SVBExposureStatus {
    Idle,
    Working,
    Success,
    Failed
};

enum class SVBGuideDirection {
    North,
    South,
    East,
    West
};

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

class SVBSDKWrapper {
public:
    static SVBSDKWrapper& instance();

    std::vector<SVBCameraInfo> enumerate_cameras();
    bool get_camera_info_by_index(int camera_index, SVBCameraInfo& info);

    void open_camera(int camera_id);
    void close_camera(int camera_id);

    std::vector<SVBControlCaps> get_control_caps(int camera_id);
    bool get_control_value(int camera_id, SVBControlType type, long& value, bool& is_auto);
    void set_control_value(int camera_id, SVBControlType type, long value, bool is_auto);

    SVBROIFormat get_roi_format(int camera_id);
    void set_roi_format(int camera_id, int start_x, int start_y, int width, int height, int bin);

    SVBImageType get_output_image_type(int camera_id);
    void set_output_image_type(int camera_id, SVBImageType type);

    void start_video_capture(int camera_id);
    void stop_video_capture(int camera_id);
    void get_video_data(int camera_id, std::uint8_t* buffer, long buffer_size, int wait_ms);

    void pulse_guide(int camera_id, SVBGuideDirection direction, int duration_ms);

    float get_sensor_pixel_size(int camera_id);
    std::string get_serial_number(int camera_id);
    std::string get_sdk_version();
    std::string get_firmware_version(int camera_id);

    void set_camera_mode_normal(int camera_id);
    void set_auto_save_param(int camera_id, bool enable);
    void restore_default_param(int camera_id);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    SVBSDKWrapper();
    ~SVBSDKWrapper();
};

} // namespace alpacacore::vendor::svbony
