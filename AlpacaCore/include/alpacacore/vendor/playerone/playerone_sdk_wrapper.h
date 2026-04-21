// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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

namespace alpacacore::vendor::playerone {

enum class PlayerOneBayerPattern {
    None,
    RG,
    BG,
    GR,
    GB
};

enum class PlayerOneGuideDirection {
    North = 0,
    South = 1,
    East = 2,
    West = 3
};

enum class PlayerOneImageFormat {
    Raw8,
    Raw16,
    Rgb24,
    Mono8,
    Unknown
};

// Subset of POACameraState mirrored so the public header does not pull in
// PlayerOneCamera.h. Values match the SDK enum.
enum class PlayerOneCameraState {
    Closed = 0,
    Opened = 1,
    Exposing = 2
};

struct PlayerOneCameraInfo {
    int index{};
    int camera_id{-1};         // SDK handle (POACameraProperties::cameraID)
    std::string name;          // cameraModelName + optional userCustomID
    std::string sensor_model;  // sensorModelName (e.g. "IMX462")
    std::string serial_number; // POACameraProperties::SN
    int max_width{};
    int max_height{};
    int bit_depth{};
    bool is_color{};
    bool is_usb3{};
    PlayerOneBayerPattern bayer{PlayerOneBayerPattern::None};
    double pixel_size_um{};
    std::vector<int> supported_bins;
    std::vector<PlayerOneImageFormat> supported_formats;
    bool has_st4_port{};
    bool has_cooler{};
};

// Capability snapshot built from POAGetConfigAttributes during connect.
// Holds only the fields the driver needs — we do not leak POAConfigAttributes.
struct PlayerOneConfigCaps {
    bool has_gain{};
    bool gain_writable{};
    long gain_min{};
    long gain_max{};
    long gain_default{};
    bool gain_supports_auto{};

    bool has_offset{};
    bool offset_writable{};
    long offset_min{};
    long offset_max{};
    long offset_default{};

    bool has_exposure{};        // POA_EXPOSURE (us, long)
    long exposure_min_us{};
    long exposure_max_us{};
    long exposure_default_us{};

    bool has_temperature{};     // POA_TEMPERATURE (float, read)
    bool has_cooler{};          // POA_COOLER
    bool has_target_temp{};     // POA_TARGET_TEMP
    long target_temp_min{};
    long target_temp_max{};
    bool has_cooler_power{};    // POA_COOLER_POWER
    bool has_egain{};           // POA_EGAIN
    bool has_usb_bandwidth{};   // POA_USB_BANDWIDTH_LIMIT
    long usb_bandwidth_min{};
    long usb_bandwidth_max{};
    bool has_fan_power{};
    bool has_heater_power{};
    bool has_guide_st4{};       // at least one of POA_GUIDE_NORTH/SOUTH/EAST/WEST
};

/**
 * Thin wrapper around the Player One Camera SDK v3.10.0.
 *
 * - Singleton: enumeration state (POAGetCameraCount) is process-wide.
 * - A std::mutex serializes calls into the SDK. Blocking calls (get_image_data)
 *   release the mutex internally so abort / disconnect paths aren't wedged
 *   behind a long wait.
 * - Driver code tracks the SDK's int cameraID; the wrapper's open_camera /
 *   close_camera calls return no handle (the ID is all we need).
 *
 * POAErrors != POA_OK is translated to AlpacaException via throw_on_error().
 */
class PlayerOneSDKWrapper {
public:
    static PlayerOneSDKWrapper& instance();

    std::string get_sdk_version();
    int get_api_version();

    std::vector<PlayerOneCameraInfo> enumerate_cameras();

    // Opens + inits the camera. Safe to call once per connect.
    void open_camera(int camera_id);
    void init_camera(int camera_id);
    void close_camera(int camera_id);

    // Re-read the properties struct for a specific ID (useful after setting
    // userCustomID or to pull serial_number once opened).
    PlayerOneCameraInfo get_camera_properties_by_id(int camera_id);

    // Capability probe — walks POAGetConfigAttributes(0..count).
    PlayerOneConfigCaps probe_config_caps(int camera_id);

    // Generic config access. Caller is responsible for knowing the value type.
    long get_config_int(int camera_id, int config_id, bool* is_auto = nullptr);
    double get_config_float(int camera_id, int config_id, bool* is_auto = nullptr);
    bool get_config_bool(int camera_id, int config_id, bool* is_auto = nullptr);
    void set_config_int(int camera_id, int config_id, long value, bool is_auto = false);
    void set_config_float(int camera_id, int config_id, double value, bool is_auto = false);
    void set_config_bool(int camera_id, int config_id, bool value, bool is_auto = false);

    // ROI / format / binning. After every setter, re-query (SDK may align).
    PlayerOneImageFormat get_image_format(int camera_id);
    void set_image_format(int camera_id, PlayerOneImageFormat format);
    void get_image_size(int camera_id, int& width, int& height);
    void set_image_size(int camera_id, int width, int height);
    void get_image_start_pos(int camera_id, int& start_x, int& start_y);
    void set_image_start_pos(int camera_id, int start_x, int start_y);
    int get_image_bin(int camera_id);
    void set_image_bin(int camera_id, int bin);

    // Exposure flow.
    void start_exposure(int camera_id, bool single_frame);
    void stop_exposure(int camera_id);
    PlayerOneCameraState get_camera_state(int camera_id);
    bool image_ready(int camera_id);

    // Blocks up to timeout_ms. Does NOT hold the wrapper mutex during the
    // wait. Throws on non-timeout errors. Returns true on success, false on
    // timeout.
    bool get_image_data(int camera_id,
                        std::uint8_t* buffer,
                        std::size_t buffer_size,
                        int timeout_ms);

    // ST4 pulse guide low-level: driver handles duration via std::thread.
    // direction maps to POA_GUIDE_NORTH/SOUTH/EAST/WEST.
    void pulse_guide_on(int camera_id, PlayerOneGuideDirection direction);
    void pulse_guide_off(int camera_id, PlayerOneGuideDirection direction);

    // Cooler helpers (thin wrappers over set_config_*).
    double get_temperature_c(int camera_id);
    bool get_cooler_on(int camera_id);
    void set_cooler_on(int camera_id, bool on);
    int get_target_temp_c(int camera_id);
    void set_target_temp_c(int camera_id, int target_c);
    int get_cooler_power_percent(int camera_id);
    double get_egain(int camera_id);

    // Sensor mode (optional — 0 means not supported).
    int get_sensor_mode_count(int camera_id);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    PlayerOneSDKWrapper();
    ~PlayerOneSDKWrapper();
};

} // namespace alpacacore::vendor::playerone
