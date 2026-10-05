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
 * Abstract interface over the ToupTek SDK operations the drivers use.
 *
 * This is the fault-injection seam (issue #104): production code talks to the
 * ToupTekSDKWrapper singleton below; unit tests substitute a scripted fake
 * (AlpacaCore/tests/fake_touptek_sdk.h) that can throw from any specific call,
 * return canned enumerations/positions, and count opens vs closes — so the
 * highest-risk driver paths (error/throw handling, cleanup, ref-counting,
 * reconnect) are exercisable without hardware. Every driver factory has an
 * overload taking a ToupTekSDK&; the default overload passes the singleton.
 *
 * Contract notes for implementors (fakes included):
 * - Methods report failure by THROWING AlpacaException, never by return code
 *   (except wait_image's documented bool-on-timeout).
 * - open_*_by_id are reference-counted per id: the physical open happens on
 *   the first opener, the physical close when the last holder releases, and
 *   two opens of the same id return the SAME handle.
 */
class ToupTekSDK {
public:
    virtual ~ToupTekSDK() = default;

    virtual std::string get_sdk_version() = 0;

    virtual std::vector<ToupCameraInfo> enumerate_cameras() = 0;
    virtual HToupcam open_camera_by_id(const std::string& id) = 0;
    virtual void close_camera(HToupcam handle) = 0;

    // Streaming lifecycle
    virtual void start_pull_mode(HToupcam handle, void (*event_callback)(unsigned event, void* ctx), void* ctx) = 0;
    virtual void stop(HToupcam handle) = 0;
    virtual void put_trigger_mode(HToupcam handle, int mode) = 0;
    virtual void trigger(HToupcam handle, unsigned short n_frames) = 0;
    virtual bool wait_image(HToupcam handle, unsigned timeout_ms, void* buffer, int bits, int row_pitch,
                            unsigned& actual_width, unsigned& actual_height) = 0;

    // Exposure & gain
    virtual ToupExpRange get_exposure_range(HToupcam handle) = 0;
    virtual unsigned get_exposure_us(HToupcam handle) = 0;
    virtual void put_exposure_us(HToupcam handle, unsigned exposure_us) = 0;
    virtual void put_auto_exposure(HToupcam handle, bool enable) = 0;
    virtual ToupGainRange get_gain_range(HToupcam handle) = 0;
    virtual unsigned short get_gain(HToupcam handle) = 0;
    virtual void put_gain(HToupcam handle, unsigned short gain) = 0;

    // ROI / format / binning
    virtual ToupROIFormat get_roi(HToupcam handle) = 0;
    virtual void put_roi(HToupcam handle, unsigned x, unsigned y, unsigned w, unsigned h) = 0;
    virtual void put_binning(HToupcam handle, int bin) = 0;
    virtual int get_binning(HToupcam handle) = 0;
    virtual void put_bitdepth(HToupcam handle, int bitdepth) = 0;
    virtual int get_bitdepth(HToupcam handle) = 0;
    virtual void put_raw(HToupcam handle, int enable) = 0;
    virtual int get_option(HToupcam handle, unsigned option) = 0;
    virtual void put_option(HToupcam handle, unsigned option, int value) = 0;

    // Frame size / format
    virtual void get_size(HToupcam handle, int& width, int& height) = 0;
    virtual void get_final_size(HToupcam handle, int& width, int& height) = 0;
    virtual void get_raw_format(HToupcam handle, unsigned& four_cc, unsigned& bits_per_pixel) = 0;

    // Cooler
    virtual int get_temperature_deciC(HToupcam handle) = 0;
    virtual void put_tec_enable(HToupcam handle, bool enable) = 0;
    virtual bool get_tec_enable(HToupcam handle) = 0;
    virtual void put_tec_target_deciC(HToupcam handle, int deci_c) = 0;
    virtual int get_tec_target_deciC(HToupcam handle) = 0;
    virtual int get_tec_voltage_deciV(HToupcam handle) = 0;
    virtual int get_tec_voltage_max_deciV(HToupcam handle) = 0;

    // High full well / conversion gain / black level
    virtual int get_high_fullwell(HToupcam handle) = 0;
    virtual void put_high_fullwell(HToupcam handle, bool enable) = 0;
    virtual int get_cg(HToupcam handle) = 0;
    virtual void put_cg(HToupcam handle, int cg) = 0;
    virtual int get_blacklevel(HToupcam handle) = 0;
    virtual void put_blacklevel(HToupcam handle, int value) = 0;
    virtual int get_blacklevel_max(HToupcam handle, int deep_bits) = 0;

    // Thermal controls (cooled-camera Switch)
    virtual int get_heat_max(HToupcam handle) = 0;
    virtual int get_heat(HToupcam handle) = 0;
    virtual void put_heat(HToupcam handle, int level) = 0;
    virtual int get_fan(HToupcam handle) = 0;
    virtual void put_fan(HToupcam handle, int speed) = 0;
    virtual int get_taillight(HToupcam handle) = 0;
    virtual void put_taillight(HToupcam handle, bool on) = 0;

    // Camera metadata
    virtual std::string get_serial_number(HToupcam handle) = 0;
    virtual std::string get_firmware_version(HToupcam handle) = 0;
    virtual void get_pixel_size(HToupcam handle, unsigned resolution_index, float& x, float& y) = 0;

    // ST4 pulse guide
    virtual void pulse_guide(HToupcam handle, ToupGuideDirection direction, unsigned duration_ms) = 0;
    virtual bool is_guiding(HToupcam handle) = 0;

    // AAF (Astro Auto Focuser)
    virtual std::vector<ToupFocuserInfo> enumerate_focusers() = 0;
    virtual HToupcam open_focuser_by_id(const std::string& id) = 0;
    virtual void close_focuser(HToupcam handle) = 0;
    virtual void aaf_set(HToupcam handle, int action, int value, const char* context) = 0;
    virtual int aaf_get(HToupcam handle, int action, const char* context) = 0;
    virtual int aaf_range(HToupcam handle, int range_action, int target_action, const char* context) = 0;

    // AFW (Astro Filter Wheel)
    virtual std::vector<ToupFilterWheelInfo> enumerate_filter_wheels() = 0;
    virtual HToupcam open_filter_wheel_by_id(const std::string& id) = 0;
    virtual void close_filter_wheel(HToupcam handle) = 0;
    virtual int get_filter_wheel_slot_count(HToupcam handle) = 0;
    virtual void set_filter_wheel_slot_count(HToupcam handle, int slot_count) = 0;
    virtual void reset_filter_wheel(HToupcam handle) = 0;
    virtual int get_filter_wheel_position(HToupcam handle) = 0;
    virtual void set_filter_wheel_position(HToupcam handle, int position) = 0;
};

/**
 * The production ToupTek SDK (toupcamsdk 20260128, libtoupcam).
 *
 * The implementation is ToupcamFamilySDK (src/vendors/touptek/toupcam_family_sdk.h) over
 * Toupcam_* calls, shared with the Altair wrapper, which runs the same code
 * over Altair's renamed copy of this SDK. Driver code reaches it only through
 * the ToupTekSDK interface above.
 *
 * Singleton because the SDK's enumeration state is process-scoped. It
 * serializes calls into the SDK, reference-counts opens per device id, and
 * translates HRESULT < 0 into AlpacaException.
 */
class ToupTekSDKWrapper {
public:
    static ToupTekSDK& instance();

    ToupTekSDKWrapper() = delete;
};

} // namespace alpacacore::vendor::touptek
