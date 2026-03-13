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

namespace alpacacore::vendor::qhy {

/**
 * @brief Basic camera info returned by enumeration (no open required).
 *
 * Full chip info (pixel size, dimensions, capabilities) is populated after
 * the camera is opened and initialized via get_chip_info().
 */
struct QHYCameraInfo {
    std::string camera_id;        //!< unique string ID from SDK (e.g., "QHY600-Pro-M-c7b72b")
    std::string model;            //!< camera model name (e.g., "QHY600-Pro-M")
    uint32_t max_width{};         //!< maximum image width in pixels
    uint32_t max_height{};        //!< maximum image height in pixels
    double pixel_size_x_um{};     //!< pixel width in microns
    double pixel_size_y_um{};     //!< pixel height in microns
    uint32_t bpp{};               //!< bits per pixel (native depth)
    bool is_color{};              //!< true if Bayer color sensor
    uint32_t bayer_pattern{};     //!< BAYER_ID from SDK: BAYER_GB=1, BAYER_GR=2, BAYER_BG=3, BAYER_RG=4 (0 = monochrome)
    bool has_cooler{};            //!< true if camera has TEC cooler
    bool has_st4_port{};          //!< true if camera has ST-4 guide port
    bool has_shutter{};           //!< true if camera has mechanical shutter
};

/**
 * @brief Min/max/step range for a camera control parameter.
 */
struct QHYControlRange {
    double min{};
    double max{};
    double step{};
    bool available{};
};

/**
 * @brief QHY CONTROL_ID constants mirrored from qhyccdstruct.h.
 *
 * These allow the camera driver to reference controls without including
 * the QHY SDK headers directly.
 */
namespace control {
    constexpr int BRIGHTNESS        = 0;
    constexpr int CONTRAST          = 1;
    constexpr int GAIN              = 6;
    constexpr int OFFSET            = 7;
    constexpr int EXPOSURE          = 8;   //!< exposure time in microseconds
    constexpr int SPEED             = 9;
    constexpr int TRANSFERBIT       = 10;
    constexpr int CURTEMP           = 14;  //!< current CCD temperature (Celsius)
    constexpr int CURPWM            = 15;  //!< current cooler PWM (0-100)
    constexpr int MANULPWM          = 16;  //!< manual cooler PWM set
    constexpr int COOLER            = 18;  //!< capability: has cooler
    constexpr int ST4PORT           = 19;  //!< capability: has ST-4 port
    constexpr int CAM_COLOR         = 20;  //!< capability: is color camera
    constexpr int BIN1X1            = 21;  //!< capability: 1x1 binning
    constexpr int BIN2X2            = 22;  //!< capability: 2x2 binning
    constexpr int BIN3X3            = 23;  //!< capability: 3x3 binning
    constexpr int BIN4X4            = 24;  //!< capability: 4x4 binning
    constexpr int MECHANICALSHUTTER = 25;  //!< capability: mechanical shutter
    constexpr int CHIPTEMP          = 32;  //!< capability: chip temperature sensor
    constexpr int BITS8             = 34;  //!< capability: 8-bit output
    constexpr int BITS16            = 35;  //!< capability: 16-bit output
    constexpr int IS_EXPOSING_DONE  = 45;  //!< poll: 1 if exposure complete
} // namespace control

/**
 * @brief QHY guide port direction constants (QHY convention).
 *
 * Note: Differs from Alpaca (0=North, 1=South, 2=East, 3=West).
 * QHY guide direction mapping:
 *   0 = EAST  (RA+)
 *   1 = NORTH (Dec+)
 *   2 = SOUTH (Dec-)
 *   3 = WEST  (RA-)
 */
namespace guide_direction {
    constexpr uint32_t EAST  = 0;
    constexpr uint32_t NORTH = 1;
    constexpr uint32_t SOUTH = 2;
    constexpr uint32_t WEST  = 3;
} // namespace guide_direction

/**
 * @brief Singleton wrapper around the QHYCCD SDK.
 *
 * Manages SDK resource lifetime (InitQHYCCDResource / ReleaseQHYCCDResource),
 * camera handle lifecycle, and exposes all per-camera SDK operations.
 * SDK headers are not exposed; all types are standard C++.
 */
class QHYSDKWrapper {
public:
    static QHYSDKWrapper& instance();

    // ── Enumeration ──────────────────────────────────────────────────────────

    /**
     * @brief Scan and enumerate connected QHY cameras.
     *
     * Returns basic info (camera_id + model) for each camera found.
     * Chip dimensions and capabilities require calling get_chip_info() after
     * open_camera() + init_camera().
     */
    std::vector<QHYCameraInfo> enumerate_cameras();

    /**
     * @brief Get the model name for a camera ID without opening it.
     */
    bool get_camera_model(const std::string& camera_id, std::string& model);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void open_camera(const std::string& camera_id);
    void init_camera(const std::string& camera_id);
    void close_camera(const std::string& camera_id);

    // ── Chip info (requires open + init) ────────────────────────────────────

    /**
     * @brief Populate full QHYCameraInfo after open+init.
     *
     * Calls GetQHYCCDChipInfo and capability checks.
     */
    bool get_chip_info(const std::string& camera_id, QHYCameraInfo& info);

    // ── Control parameters ───────────────────────────────────────────────────

    bool is_control_available(const std::string& camera_id, int control_id);
    double get_param(const std::string& camera_id, int control_id);
    QHYControlRange get_param_range(const std::string& camera_id, int control_id);
    void set_param(const std::string& camera_id, int control_id, double value);

    // ── Image configuration ──────────────────────────────────────────────────

    void set_resolution(const std::string& camera_id,
                        uint32_t start_x, uint32_t start_y,
                        uint32_t width, uint32_t height);
    void set_bin_mode(const std::string& camera_id, uint32_t wbin, uint32_t hbin);
    void set_bits_mode(const std::string& camera_id, uint32_t bits);
    uint32_t get_mem_length(const std::string& camera_id);

    // ── Exposure ─────────────────────────────────────────────────────────────

    /**
     * @brief Start a single frame exposure.
     * @return true if SDK returned QHYCCD_READ_DIRECTLY (old cameras — read immediately)
     */
    bool start_single_frame(const std::string& camera_id);

    /**
     * @brief Blocking call that waits for a single frame and copies it into buffer.
     *
     * buffer must be pre-allocated to at least get_mem_length() bytes.
     * @return true on success, false on failure
     */
    bool get_single_frame(const std::string& camera_id,
                          uint8_t* buffer,
                          uint32_t& width, uint32_t& height,
                          uint32_t& bpp, uint32_t& channels);

    /**
     * @brief Cancel the current exposure (and discard the frame).
     *
     * Uses CancelQHYCCDExposingAndReadout — all cameras support this.
     */
    void cancel_exposure(const std::string& camera_id);

    // ── Guide port ──────────────────────────────────────────────────────────

    /**
     * @brief Issue a pulse guide command.
     *
     * @param qhy_direction  QHY direction: EAST=0, NORTH=1, SOUTH=2, WEST=3
     * @param duration_ms    Duration in milliseconds
     */
    void guide(const std::string& camera_id, uint32_t qhy_direction, uint16_t duration_ms);

    // ── Temperature ──────────────────────────────────────────────────────────

    /**
     * @brief Drive the TEC toward target_temp_c using the SDK's PID controller.
     *
     * Must be called periodically (approximately every second) while cooling
     * is active.
     */
    void control_temp(const std::string& camera_id, double target_temp_c);

    // ── Readout modes ────────────────────────────────────────────────────────

    uint32_t get_num_readout_modes(const std::string& camera_id);
    std::string get_readout_mode_name(const std::string& camera_id, uint32_t mode_index);
    void set_readout_mode(const std::string& camera_id, uint32_t mode_index);

    // ── Misc ─────────────────────────────────────────────────────────────────

    std::string get_sdk_version();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    QHYSDKWrapper();
    ~QHYSDKWrapper();

    QHYSDKWrapper(const QHYSDKWrapper&) = delete;
    QHYSDKWrapper& operator=(const QHYSDKWrapper&) = delete;
};

} // namespace alpacacore::vendor::qhy
