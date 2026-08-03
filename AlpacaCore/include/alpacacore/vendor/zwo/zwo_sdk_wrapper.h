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
#include <memory>
#include <string>
#include <vector>
#include <optional>

namespace alpacacore::vendor::zwo {

enum class ZWOImageType {
    Raw8,
    Rgb24,
    Raw16,
    Y8
};

enum class ZWOBayerPattern {
    None,
    RG,
    BG,
    GR,
    GB
};

enum class ZWOExposureStatus {
    Idle,
    Working,
    Success,
    Failed
};

enum class ZWOGuideDirection {
    North,
    South,
    East,
    West
};

enum class ZWOControlType {
    Gain,
    Exposure,
    Offset,
    Temperature,
    CoolerOn,
    CoolerPower,
    TargetTemperature,
    HighSpeedMode,
    AntiDewHeater
};

struct ZWOControlCaps {
    ZWOControlType type;
    std::string name;
    std::string description;
    long min_value{};
    long max_value{};
    long default_value{};
    bool is_auto_supported{};
    bool is_writable{};
};

struct ZWOCameraInfo {
    int camera_id{};
    std::string name;
    int max_width{};
    int max_height{};
    bool is_color{};
    ZWOBayerPattern bayer_pattern{ZWOBayerPattern::None};
    std::vector<int> supported_bins;
    std::vector<ZWOImageType> supported_formats;
    double pixel_size_um{};
    bool has_shutter{};
    bool has_st4_port{};
    bool has_cooler{};
    double electrons_per_adu{};
    int bit_depth{};
};

struct ZWOROIFormat {
    int width{};
    int height{};
    int bin{};
    ZWOImageType image_type{ZWOImageType::Raw8};
};

struct ZWOStartPos {
    int start_x{};
    int start_y{};
};

class ZWOSDKWrapper {
public:
    static ZWOSDKWrapper& instance();

    std::vector<ZWOCameraInfo> enumerate_cameras();
    bool get_camera_info_by_id(int camera_id, ZWOCameraInfo& info);
    bool get_camera_info_by_index(int camera_index, ZWOCameraInfo& info);

    void open_camera(int camera_id);
    void init_camera(int camera_id);
    void close_camera(int camera_id);

    std::vector<ZWOControlCaps> get_control_caps(int camera_id);
    bool get_control_value(int camera_id, ZWOControlType type, long& value, bool& is_auto);
    void set_control_value(int camera_id, ZWOControlType type, long value, bool is_auto);

    ZWOROIFormat get_roi_format(int camera_id);
    void set_roi_format(int camera_id, int width, int height, int bin, ZWOImageType type);

    ZWOStartPos get_start_pos(int camera_id);
    void set_start_pos(int camera_id, int start_x, int start_y);

    void start_video_capture(int camera_id);
    void stop_video_capture(int camera_id);
    // Blocks up to wait_ms for the next video frame; true = frame copied,
    // false = timeout. Throws on any other SDK error.
    bool get_video_data(int camera_id, std::uint8_t* buffer, long buffer_size, int wait_ms);
    int get_dropped_frames(int camera_id);

    void start_exposure(int camera_id, bool is_dark);
    void stop_exposure(int camera_id);
    ZWOExposureStatus get_exposure_status(int camera_id);
    void get_data_after_exposure(int camera_id, std::uint8_t* buffer, long buffer_size);

    void pulse_guide_on(int camera_id, ZWOGuideDirection direction);
    void pulse_guide_off(int camera_id, ZWOGuideDirection direction);

    std::string get_serial_number(int camera_id);
    std::string get_sdk_version();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ZWOSDKWrapper();
    ~ZWOSDKWrapper();
};

} // namespace alpacacore::vendor::zwo
