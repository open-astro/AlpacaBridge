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
    bool supports_high_fullwell{};  // TOUPCAM_FLAG_HIGH_FULLWELL
    bool supports_cg{};             // TOUPCAM_FLAG_CG (HCG/LCG conversion gain)
    bool supports_cghdr{};          // TOUPCAM_FLAG_CGHDR (adds an HDR conversion gain)
    bool supports_blacklevel{};     // TOUPCAM_FLAG_BLACKLEVEL (ASCOM Offset)
    bool supports_heat{};           // TOUPCAM_FLAG_HEAT (anti-fog dew heater)
    bool supports_fan{};            // TOUPCAM_FLAG_FAN (cooling fan)
    unsigned max_fan_speed{};       // model->maxfanspeed (fan speed range [0, max])
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
 * Information about a ToupTek AAF (Astro Auto Focuser) device discovered via
 * Toupcam_EnumV2. Identified by the TOUPCAM_FLAG_AUTOFOCUSER capability bit.
 */
struct ToupFocuserInfo {
    int index{};
    std::string id;          // opaque id from Toupcam_EnumV2, used for Toupcam_Open
    std::string name;        // displayname
    std::string model_name;  // model->name
    unsigned long long flags{};
};

/**
 * Information about a ToupTek AFW (Astro Filter Wheel) device discovered via
 * Toupcam_EnumV2. Identified by the TOUPCAM_FLAG_FILTERWHEEL capability bit.
 * Covers the standalone AFW-M models (5- and 7-slot).
 */
struct ToupFilterWheelInfo {
    int index{};
    std::string id;          // opaque id from Toupcam_EnumV2, used for Toupcam_Open
    std::string name;        // displayname
    std::string model_name;  // model->name
    unsigned long long flags{};
};

/**
 * AAF (Astro Auto Focuser) action codes.
 *
 * Mirrors TOUPCAM_AAF_* in toupcam.h so driver code can avoid including the
 * raw SDK header. The numeric values match the SDK definitions.
 */
namespace ToupAAF {
    constexpr int SetPosition     = 0x01;
    constexpr int GetPosition     = 0x02;
    constexpr int SetZero         = 0x03;
    constexpr int SetDirection    = 0x05;
    constexpr int GetDirection    = 0x06;
    constexpr int SetMaxIncrement = 0x07;
    constexpr int GetMaxIncrement = 0x08;
    constexpr int SetFine         = 0x09;
    constexpr int GetFine         = 0x0a;
    constexpr int SetCoarse       = 0x0b;
    constexpr int GetCoarse       = 0x0c;
    constexpr int SetBuzzer       = 0x0d;
    constexpr int GetBuzzer       = 0x0e;
    constexpr int SetBacklash     = 0x0f;
    constexpr int GetBacklash     = 0x10;
    constexpr int GetAmbientTemp  = 0x12;
    constexpr int GetTemp         = 0x14; // tenths of Celsius
    constexpr int IsMoving        = 0x16;
    constexpr int Halt            = 0x17;
    constexpr int SetMaxStep      = 0x1b;
    constexpr int GetMaxStep      = 0x1c;
    constexpr int GetStepSize     = 0x1e;
    constexpr int RangeMin        = 0xfd;
    constexpr int RangeMax        = 0xfe;
    constexpr int RangeDef        = 0xff;
}

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
    //
    // DEPRECATED: this index is the SDK's raw Toupcam_EnumV2 order (still includes
    // AFW/AAF accessories), NOT the camera-only index space that enumerate_cameras()
    // and the web UI's cameraIndex use, and it opens OUTSIDE the reference-counted
    // by-id sharing — so it can Toupcam_Close a device another driver holds open by
    // id. No production path uses it. Always open by id via open_camera_by_id().
    [[deprecated("index space diverges from enumerate_cameras(); use open_camera_by_id()")]]
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

    // High full well ---------------------------------------------------------
    // TOUPCAM_OPTION_HIGH_FULLWELL: 0 = disable, 1 = enable. Gated by
    // supports_high_fullwell; exposed to ASCOM as a ReadoutMode.
    int get_high_fullwell(HToupcam handle);
    void put_high_fullwell(HToupcam handle, bool enable);

    // Conversion gain (TOUPCAM_OPTION_CG): 0 = LCG, 1 = HCG, 2 = HDR (only on
    // FLAG_CGHDR cameras). Gated by supports_cg; folded into ASCOM ReadoutModes.
    int get_cg(HToupcam handle);
    void put_cg(HToupcam handle, int cg);

    // Black level (ASCOM Offset) ---------------------------------------------
    // TOUPCAM_OPTION_BLACKLEVEL. Range is [0, get_blacklevel_max]; the max scales
    // with the current output bit depth (31 at 8-bit up to 31*256 at 16-bit), so
    // it takes the camera's deep-mode bit count. Gated by supports_blacklevel.
    int get_blacklevel(HToupcam handle);
    void put_blacklevel(HToupcam handle, int value);
    int get_blacklevel_max(HToupcam handle, int deep_bits);

    // Thermal controls (cooled-camera Switch) --------------------------------
    // Dew (anti-fog) heater: level in [0, get_heat_max]. 0 = off.
    int get_heat_max(HToupcam handle);
    int get_heat(HToupcam handle);
    void put_heat(HToupcam handle, int level);
    // Cooling fan: speed in [0, model->maxfanspeed]. 0 = off.
    int get_fan(HToupcam handle);
    void put_fan(HToupcam handle, int speed);

    // Tail indicator LED (TOUPCAM_OPTION_TAILLIGHT): 0 = off, 1 = on. There is
    // no capability flag — probe by calling get_taillight and catching the
    // error on cameras that don't support it.
    int get_taillight(HToupcam handle);
    void put_taillight(HToupcam handle, bool on);

    // Camera metadata --------------------------------------------------------
    std::string get_serial_number(HToupcam handle);
    std::string get_firmware_version(HToupcam handle);
    void get_pixel_size(HToupcam handle, unsigned resolution_index, float& x, float& y);

    // ST4 pulse guide --------------------------------------------------------
    void pulse_guide(HToupcam handle, ToupGuideDirection direction, unsigned duration_ms);
    // Returns true if the camera is currently guiding.
    bool is_guiding(HToupcam handle);

    // AAF (Astro Auto Focuser) -----------------------------------------------
    // Enumeration filters Toupcam_EnumV2 results by TOUPCAM_FLAG_AUTOFOCUSER.
    std::vector<ToupFocuserInfo> enumerate_focusers();

    // Open by Toupcam_EnumV2 id. Throws on failure.
    HToupcam open_focuser_by_id(const std::string& id);

    void close_focuser(HToupcam handle);

    // Generic AAF write: Toupcam_AAF(handle, action, value, nullptr).
    void aaf_set(HToupcam handle, int action, int value, const char* context);

    // Generic AAF read: Toupcam_AAF(handle, action, 0, &out).
    int aaf_get(HToupcam handle, int action, const char* context);

    // AAF range query: Toupcam_AAF(handle, RANGEMAX|RANGEMIN|RANGEDEF, action, &out).
    int aaf_range(HToupcam handle, int range_action, int target_action, const char* context);

    // AFW (Astro Filter Wheel) ----------------------------------------------
    // Enumeration filters Toupcam_EnumV2 results by TOUPCAM_FLAG_FILTERWHEEL.
    std::vector<ToupFilterWheelInfo> enumerate_filter_wheels();

    // Open by Toupcam_EnumV2 id. Throws on failure.
    HToupcam open_filter_wheel_by_id(const std::string& id);

    void close_filter_wheel(HToupcam handle);

    // Number of filter slots reported by the wheel firmware
    // (TOUPCAM_OPTION_FILTERWHEEL_SLOT).
    int get_filter_wheel_slot_count(HToupcam handle);

    // Write the slot count back to the wheel (TOUPCAM_OPTION_FILTERWHEEL_SLOT is
    // [RW]). The toupbase reference driver does this at connect right after
    // reading it, re-applying the wheel's slot configuration.
    void set_filter_wheel_slot_count(HToupcam handle, int slot_count);

    // Home/reset the wheel (TOUPCAM_OPTION_FILTERWHEEL_POSITION = -1). Required
    // at connect so the firmware establishes its slot reference — without it the
    // wheel hunts and never lands (notably after a firmware update).
    void reset_filter_wheel(HToupcam handle);

    // Current slot (0-based). Returns -1 while the wheel is in motion, matching
    // the ASCOM FilterWheel Position contract (TOUPCAM_OPTION_FILTERWHEEL_POSITION).
    int get_filter_wheel_position(HToupcam handle);

    // Move to slot 0..N-1 (single absolute move; direction bit left at 0 =
    // clockwise, matching the toupbase default).
    void set_filter_wheel_position(HToupcam handle, int position);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ToupTekSDKWrapper();
    ~ToupTekSDKWrapper();
};

} // namespace alpacacore::vendor::touptek
