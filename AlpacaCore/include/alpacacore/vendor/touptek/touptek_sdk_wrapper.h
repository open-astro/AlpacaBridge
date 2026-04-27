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

// Forward-declare the SDK handle so this header doesn't pull in toupcam.h.
typedef struct Toupcam_t* HToupcam;

namespace alpacacore::vendor::touptek {

enum class ToupBayerPattern {
    None,
    RG,
    BG,
    GR,
    GB
};

enum class ToupGuideDirection {
    North = 0,
    South = 1,
    East = 2,
    West = 3
};

struct ToupCameraInfo {
    int index{};
    std::string id;            // opaque id from Toupcam_EnumV2, used for Toupcam_Open
    std::string name;          // displayname
    std::string model_name;    // model->name
    unsigned long long flags{};
    int max_width{};
    int max_height{};
    float pixel_size_um_x{};
    float pixel_size_um_y{};
    bool is_color{};           // !(flags & FLAG_MONO)
    ToupBayerPattern bayer{ToupBayerPattern::None};
    std::vector<int> supported_bins;
    bool supports_pulse_guide{};
    bool supports_cooler{};
    bool supports_tec_onoff{};
    bool supports_trigger_software{};
    bool supports_filterwheel{};
    int filterwheel_slots{};
    int bit_depth_max{};
};

struct ToupROIFormat {
    unsigned start_x{};
    unsigned start_y{};
    unsigned width{};
    unsigned height{};
};

struct ToupExpRange {
    unsigned min_us{};
    unsigned max_us{};
    unsigned def_us{};
};

struct ToupGainRange {
    unsigned short min{};
    unsigned short max{};
    unsigned short def{};
};

/**
 * Thin wrapper around the ToupTek SDK (toupcamsdk 20260128).
 *
 * - Singleton because the SDK global enumeration state is process-scoped.
 * - A std::mutex serializes calls into the SDK (the SDK is thread-safe, but
 *   centralising access keeps error translation deterministic).
 * - Driver code owns HToupcam handles returned from open_camera*().
 *
 * HRESULT < 0 translates to AlpacaException via throw_on_error(). S_FALSE (1)
 * is treated as success (no-op) per SDK semantics.
 */
class ToupTekSDKWrapper {
public:
    static ToupTekSDKWrapper& instance();

    std::string get_sdk_version();

    std::vector<ToupCameraInfo> enumerate_cameras();

    // Returns the opened handle (non-null). Throws on failure.
    HToupcam open_camera_by_index(int camera_index);
    HToupcam open_camera_by_id(const std::string& id);

    void close_camera(HToupcam handle);

    // Streaming lifecycle ----------------------------------------------------
    void start_pull_mode(HToupcam handle,
                         void (*event_callback)(unsigned event, void* ctx),
                         void* ctx);
    void stop(HToupcam handle);
    void put_trigger_mode(HToupcam handle, int mode); // 0=video, 1=software
    void trigger(HToupcam handle, unsigned short n_frames);
    // Returns true if a frame was delivered within timeout_ms. Throws on
    // non-timeout errors. On success, actual_width/height carry the frame
    // dimensions reported by the SDK.
    bool wait_image(HToupcam handle,
                    unsigned timeout_ms,
                    void* buffer,
                    int bits,
                    int row_pitch,
                    unsigned& actual_width,
                    unsigned& actual_height);

    // Exposure & gain --------------------------------------------------------
    ToupExpRange get_exposure_range(HToupcam handle);
    unsigned get_exposure_us(HToupcam handle);
    void put_exposure_us(HToupcam handle, unsigned exposure_us);
    void put_auto_exposure(HToupcam handle, bool enable);
    ToupGainRange get_gain_range(HToupcam handle);
    unsigned short get_gain(HToupcam handle);
    void put_gain(HToupcam handle, unsigned short gain);

    // ROI / format / binning -------------------------------------------------
    ToupROIFormat get_roi(HToupcam handle);
    void put_roi(HToupcam handle, unsigned x, unsigned y, unsigned w, unsigned h);
    void put_binning(HToupcam handle, int bin); // 1, 2, 3, 4...
    int get_binning(HToupcam handle);
    // 0 = 8-bit mode, 1 = 16-bit mode (subset of PIXEL_FORMAT). Reconfiguring
    // requires the stream to be stopped — the driver handles that.
    void put_bitdepth(HToupcam handle, int bitdepth);
    int get_bitdepth(HToupcam handle);
    void put_raw(HToupcam handle, int enable);
    int get_option(HToupcam handle, unsigned option);
    void put_option(HToupcam handle, unsigned option, int value);

    // Frame size / format ----------------------------------------------------
    void get_size(HToupcam handle, int& width, int& height);
    void get_final_size(HToupcam handle, int& width, int& height);
    // FourCC returned by the SDK (e.g. 'RGGB', 'YYYY'). bits_per_pixel carries
    // the native pixel depth.
    void get_raw_format(HToupcam handle, unsigned& four_cc, unsigned& bits_per_pixel);

    // Cooler -----------------------------------------------------------------
    // Temperature is returned in 0.1 degrees Celsius.
    int get_temperature_deciC(HToupcam handle);
    void put_tec_enable(HToupcam handle, bool enable);
    bool get_tec_enable(HToupcam handle);
    void put_tec_target_deciC(HToupcam handle, int deci_c);
    int get_tec_target_deciC(HToupcam handle);
    int get_tec_voltage_deciV(HToupcam handle);
    int get_tec_voltage_max_deciV(HToupcam handle);

    // Camera metadata --------------------------------------------------------
    std::string get_serial_number(HToupcam handle);
    std::string get_firmware_version(HToupcam handle);
    void get_pixel_size(HToupcam handle, unsigned resolution_index, float& x, float& y);

    // ST4 pulse guide --------------------------------------------------------
    void pulse_guide(HToupcam handle, ToupGuideDirection direction, unsigned duration_ms);
    // Returns true if the camera is currently guiding.
    bool is_guiding(HToupcam handle);

    // Filter wheel ------------------------------------------------------------
    // Get the number of filter wheel slots (0 if no filter wheel).
    int get_filterwheel_slot_count(HToupcam handle);
    // Get the current filter wheel position. Returns -1 if in motion.
    int get_filterwheel_position(HToupcam handle);
    // Set the filter wheel position. position: 0 to N-1.
    // direction: 0 = clockwise, 1 = auto direction.
    void set_filterwheel_position(HToupcam handle, int position, int direction = 0);
    // Reset the filter wheel.
    void reset_filterwheel(HToupcam handle);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ToupTekSDKWrapper();
    ~ToupTekSDKWrapper();
};

} // namespace alpacacore::vendor::touptek
