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

#include <alpacacore/vendor/zwo/zwo_camera_identity.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

enum class ZWOImageType : std::uint8_t { Raw8, Rgb24, Raw16, Y8, Unknown };

enum class ZWOBayerPattern : std::uint8_t { None, RG, BG, GR, GB };

enum class ZWOExposureStatus : std::uint8_t { Idle, Working, Success, Failed };

enum class ZWOGuideDirection : std::uint8_t { North, South, East, West };

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

class ZWOSDK {
public:
    virtual std::vector<ZWOCameraInfo> enumerate_cameras() = 0;
    virtual std::vector<ZwoEnumeratedCamera> enumerate_identified_cameras(const std::string& only_model_name = {}) = 0;
    virtual bool get_camera_info_by_id(int camera_id, ZWOCameraInfo& info) = 0;
    virtual bool get_camera_info_by_index(int camera_index, ZWOCameraInfo& info) = 0;

    virtual void open_camera(int camera_id) = 0;
    virtual void init_camera(int camera_id) = 0;
    virtual void close_camera(int camera_id) = 0;

    virtual std::vector<ZWOControlCaps> get_control_caps(int camera_id) = 0;
    virtual bool get_control_value(int camera_id, ZWOControlType type, long& value, bool& is_auto) = 0;
    virtual void set_control_value(int camera_id, ZWOControlType type, long value, bool is_auto) = 0;

    virtual ZWOROIFormat get_roi_format(int camera_id) = 0;
    virtual void set_roi_format(int camera_id, int width, int height, int bin, ZWOImageType type) = 0;

    virtual ZWOStartPos get_start_pos(int camera_id) = 0;
    virtual void set_start_pos(int camera_id, int start_x, int start_y) = 0;

    virtual void start_exposure(int camera_id, bool is_dark) = 0;
    virtual void stop_exposure(int camera_id) = 0;
    virtual ZWOExposureStatus get_exposure_status(int camera_id) = 0;
    virtual void get_data_after_exposure(int camera_id, std::uint8_t* buffer, long buffer_size) = 0;

    virtual void pulse_guide_on(int camera_id, ZWOGuideDirection direction) = 0;
    virtual void pulse_guide_off(int camera_id, ZWOGuideDirection direction) = 0;

    virtual std::string get_serial_number(int camera_id) = 0;
    virtual std::string get_sdk_version() = 0;

protected:
    ~ZWOSDK() = default;
};

class ZWOSDKWrapper final : public ZWOSDK {
public:
    static ZWOSDKWrapper& instance();

    std::vector<ZWOCameraInfo> enumerate_cameras() override;
    /// Every connected camera with its serial. ASIGetSerialNumber needs an
    /// open camera, so each one is opened through the ref-counted
    /// open_camera()/close_camera() pair (a camera already open in this
    /// process stays open). A failed serial read leaves the serial empty.
    /// With `only_model_name` (already trimmed) set, only cameras of that
    /// model are opened; the others are listed with an empty serial.
    std::vector<ZwoEnumeratedCamera> enumerate_identified_cameras(const std::string& only_model_name = {}) override;
    bool get_camera_info_by_id(int camera_id, ZWOCameraInfo& info) override;
    bool get_camera_info_by_index(int camera_index, ZWOCameraInfo& info) override;

    void open_camera(int camera_id) override;
    void init_camera(int camera_id) override;
    void close_camera(int camera_id) override;

    std::vector<ZWOControlCaps> get_control_caps(int camera_id) override;
    bool get_control_value(int camera_id, ZWOControlType type, long& value, bool& is_auto) override;
    void set_control_value(int camera_id, ZWOControlType type, long value, bool is_auto) override;

    ZWOROIFormat get_roi_format(int camera_id) override;
    void set_roi_format(int camera_id, int width, int height, int bin, ZWOImageType type) override;

    ZWOStartPos get_start_pos(int camera_id) override;
    void set_start_pos(int camera_id, int start_x, int start_y) override;

    void start_exposure(int camera_id, bool is_dark) override;
    void stop_exposure(int camera_id) override;
    ZWOExposureStatus get_exposure_status(int camera_id) override;
    void get_data_after_exposure(int camera_id, std::uint8_t* buffer, long buffer_size) override;

    void pulse_guide_on(int camera_id, ZWOGuideDirection direction) override;
    void pulse_guide_off(int camera_id, ZWOGuideDirection direction) override;

    std::string get_serial_number(int camera_id) override;
    std::string get_sdk_version() override;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ZWOSDKWrapper();
    ~ZWOSDKWrapper();
};

}  // namespace alpacacore::vendor::zwo
