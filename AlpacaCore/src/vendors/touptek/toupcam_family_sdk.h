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

// The one implementation of ToupTekSDK, shared by the ToupTek SDK family.
//
// Several vendors ship the ToupTek camera SDK under their own name: Altair's
// altaircam is toupcam with the Toupcam_ / TOUPCAM_ prefixes renamed to
// Altaircam_ / ALTAIRCAM_ and a different USB vendor id, with identical
// signatures, struct layouts and constant values (INDI's indi-toupbase builds
// one driver against each OEM library the same way). Rather than copy the
// wrapper per brand, ToupcamFamilySDK<Api> holds every SDK interaction and a
// small per-brand traits struct supplies the prefixed functions and constants.
//
// Include this header ONLY from a vendor SDK wrapper .cpp, the one translation
// unit that also includes that vendor's raw SDK header (toupcam.h, altaircam.h)
// and defines its Api traits. It includes no SDK header itself and names no SDK
// symbol except through Api, so it cannot pick the wrong one and the include
// order does not matter.
//
// An Api traits struct provides:
//   - static constexpr const char* kPrefix   ("Toupcam", "Altaircam"); used in
//     error messages so a failure names the library that produced it;
//   - DeviceV2 / FrameInfoV4                  (the brand's struct types);
//   - one static function per entry of ALPACACORE_TOUPCAM_FAMILY_FUNCTIONS,
//     taking and returning HToupcam where the SDK takes or returns its own
//     handle type (both are opaque pointers to a one-int struct);
//   - one static constexpr per entry of ALPACACORE_TOUPCAM_FAMILY_CONSTANTS.
// The two X-macro lists below are the whole contract, so a brand's traits are
// generated from them and cannot omit an entry without failing to compile.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>

#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Every SDK function ToupcamFamilySDK calls, without its brand prefix.
#define ALPACACORE_TOUPCAM_FAMILY_FUNCTIONS(X) \
    X(Version)                                 \
    X(EnumV2)                                  \
    X(Open)                                    \
    X(Close)                                   \
    X(StartPullModeWithCallback)               \
    X(Stop)                                    \
    X(Trigger)                                 \
    X(WaitImageV4)                             \
    X(get_ExpTimeRange)                        \
    X(get_ExpoTime)                            \
    X(put_ExpoTime)                            \
    X(put_AutoExpoEnable)                      \
    X(get_ExpoAGainRange)                      \
    X(get_ExpoAGain)                           \
    X(put_ExpoAGain)                           \
    X(get_Roi)                                 \
    X(put_Roi)                                 \
    X(get_Option)                              \
    X(put_Option)                              \
    X(get_Size)                                \
    X(get_FinalSize)                           \
    X(get_RawFormat)                           \
    X(get_Temperature)                         \
    X(get_SerialNumber)                        \
    X(get_FwVersion)                           \
    X(get_PixelSize)                           \
    X(ST4PlusGuide)                            \
    X(ST4PlusGuideState)                       \
    X(AAF)

// Every SDK constant ToupcamFamilySDK reads, without its brand prefix.
#define ALPACACORE_TOUPCAM_FAMILY_CONSTANTS(X) \
    X(MAX)                                     \
    X(BLACKLEVEL8_MAX)                         \
    X(FLAG_AUTOFOCUSER)                        \
    X(FLAG_BLACKLEVEL)                         \
    X(FLAG_CG)                                 \
    X(FLAG_CGHDR)                              \
    X(FLAG_FAN)                                \
    X(FLAG_FILTERWHEEL)                        \
    X(FLAG_HEAT)                               \
    X(FLAG_HIGH_FULLWELL)                      \
    X(FLAG_MONO)                               \
    X(FLAG_RAW10)                              \
    X(FLAG_RAW11)                              \
    X(FLAG_RAW12)                              \
    X(FLAG_RAW14)                              \
    X(FLAG_RAW16)                              \
    X(FLAG_ST4)                                \
    X(FLAG_TEC)                                \
    X(FLAG_TEC_ONOFF)                          \
    X(FLAG_TRIGGER_SOFTWARE)                   \
    X(OPTION_BINNING)                          \
    X(OPTION_BITDEPTH)                         \
    X(OPTION_BLACKLEVEL)                       \
    X(OPTION_CG)                               \
    X(OPTION_FAN)                              \
    X(OPTION_FILTERWHEEL_POSITION)             \
    X(OPTION_FILTERWHEEL_SLOT)                 \
    X(OPTION_HEAT)                             \
    X(OPTION_HEAT_MAX)                         \
    X(OPTION_HIGH_FULLWELL)                    \
    X(OPTION_RAW)                              \
    X(OPTION_TAILLIGHT)                        \
    X(OPTION_TEC)                              \
    X(OPTION_TECTARGET)                        \
    X(OPTION_TEC_VOLTAGE)                      \
    X(OPTION_TEC_VOLTAGE_MAX)                  \
    X(OPTION_TRIGGER)

namespace alpacacore::vendor::touptek {

namespace toupcam_family_detail {

// HRESULT is `int` in every family SDK on Linux; the values are the Windows
// HRESULT codes, compared as unsigned.
inline std::string hresult_to_string(int hr) {
    switch (static_cast<unsigned>(hr)) {
        case 0x00000000u:
            return "S_OK";
        case 0x00000001u:
            return "S_FALSE";
        case 0x8000ffffu:
            return "E_UNEXPECTED";
        case 0x80004001u:
            return "E_NOTIMPL";
        case 0x80004002u:
            return "E_NOINTERFACE";
        case 0x80070005u:
            return "E_ACCESSDENIED";
        case 0x8007000eu:
            return "E_OUTOFMEMORY";
        case 0x80070057u:
            return "E_INVALIDARG";
        case 0x80004003u:
            return "E_POINTER";
        case 0x80004005u:
            return "E_FAIL";
        case 0x8001010eu:
            return "E_WRONG_THREAD";
        case 0x8007001fu:
            return "E_GEN_FAILURE";
        case 0x800700aau:
            return "E_BUSY";
        case 0x8000000au:
            return "E_PENDING";
        case 0x8001011fu:
            return "E_TIMEOUT";
        case 0x80072743u:
            return "E_UNREACH";
        case 0x800704C7u:
            return "E_CANCELLED";
        default: {
            std::ostringstream oss;
            oss << "HRESULT 0x" << std::hex << static_cast<unsigned>(hr);
            return oss.str();
        }
    }
}

inline int map_hresult(int hr) {
    switch (static_cast<unsigned>(hr)) {
        case 0x80070057u:  // E_INVALIDARG
        case 0x80004003u:  // E_POINTER
            return AlpacaError::InvalidValue;
        case 0x80004001u:  // E_NOTIMPL
            return AlpacaError::NotImplemented;
        case 0x800700aau:  // E_BUSY
        case 0x8000ffffu:  // E_UNEXPECTED
        case 0x8001010eu:  // E_WRONG_THREAD
            return AlpacaError::InvalidOperation;
        case 0x80070005u:  // E_ACCESSDENIED
            return AlpacaError::NotConnected;
        default:
            return AlpacaError::DriverException;
    }
}

constexpr int kS_OK = 0;
constexpr unsigned kE_TIMEOUT = 0x8001011fu;

}  // namespace toupcam_family_detail

/**
 * ToupTekSDK over one brand of the ToupTek SDK family (see the file comment).
 *
 * - One instance per brand, held as that brand's process-wide singleton
 *   (ToupTekSDKWrapper::instance(), AltairSDKWrapper::instance()), because the
 *   SDK's enumeration state is process-scoped.
 * - A std::mutex serializes calls into the SDK (the SDK is thread-safe, but
 *   centralising access keeps error translation deterministic).
 * - Driver code owns the handles returned from open_*_by_id().
 *
 * HRESULT < 0 translates to AlpacaException via check(). S_FALSE (1) is
 * treated as success (no-op) per SDK semantics.
 */
template <class Api>
class ToupcamFamilySDK final : public ToupTekSDK {
public:
    ToupcamFamilySDK() = default;
    ToupcamFamilySDK(const ToupcamFamilySDK&) = delete;
    ToupcamFamilySDK& operator=(const ToupcamFamilySDK&) = delete;

    std::string get_sdk_version() override {
        const char* v = Api::Version();
        return v ? v : "unknown";
    }

    std::vector<ToupCameraInfo> enumerate_cameras() override {
        std::lock_guard<std::mutex> lock(mutex_);
        typename Api::DeviceV2 arr[Api::MAX]{};
        unsigned count = Api::EnumV2(arr);
        std::vector<ToupCameraInfo> result;
        result.reserve(count);
        int camera_index = 0;
        for (unsigned i = 0; i < count; ++i) {
            // Skip entries with no model block, matching enumerate_focusers/
            // enumerate_filter_wheels. Without this, a null-model device would fall
            // through the flag check below (the && short-circuits to false) and be
            // pushed as a phantom camera with zero flags/dimensions at index 0,
            // displacing the real camera.
            if (!arr[i].model) {
                continue;
            }
            // EnumV2 also lists standalone AFW filter wheels and AAF focusers,
            // which are not cameras. Skip them (mirroring the flag guards in
            // enumerate_filter_wheels/enumerate_focusers) so cameras[cameraIndex]
            // — and the cameraIndex=0 default — never resolves to an accessory
            // when a camera and an AFW/AAF are attached together.
            if ((arr[i].model->flag & (Api::FLAG_FILTERWHEEL | Api::FLAG_AUTOFOCUSER)) != 0) {
                continue;
            }
            ToupCameraInfo info;
            info.index = camera_index++;
            info.id = arr[i].id;
            info.name = arr[i].displayname;
            // model is guaranteed non-null by the guard at the top of the loop.
            info.model_name = arr[i].model->name ? arr[i].model->name : "";
            info.flags = arr[i].model->flag;
            info.pixel_size_um_x = arr[i].model->xpixsz;
            info.pixel_size_um_y = arr[i].model->ypixsz;
            info.max_fan_speed = arr[i].model->maxfanspeed;
            if (arr[i].model->preview > 0) {
                info.max_width = static_cast<int>(arr[i].model->res[0].width);
                info.max_height = static_cast<int>(arr[i].model->res[0].height);
            }
            info.is_color = (info.flags & Api::FLAG_MONO) == 0;
            info.supports_pulse_guide = (info.flags & Api::FLAG_ST4) != 0;
            info.supports_cooler = (info.flags & Api::FLAG_TEC) != 0;
            info.supports_tec_onoff = (info.flags & Api::FLAG_TEC_ONOFF) != 0;
            info.supports_trigger_software = (info.flags & Api::FLAG_TRIGGER_SOFTWARE) != 0;
            info.supports_high_fullwell = (info.flags & Api::FLAG_HIGH_FULLWELL) != 0;
            info.supports_cghdr = (info.flags & Api::FLAG_CGHDR) != 0;
            info.supports_cg = info.supports_cghdr || (info.flags & Api::FLAG_CG) != 0;
            info.supports_blacklevel = (info.flags & Api::FLAG_BLACKLEVEL) != 0;
            info.supports_heat = (info.flags & Api::FLAG_HEAT) != 0;
            info.supports_fan = (info.flags & Api::FLAG_FAN) != 0;

            // The SDK supports digital binning 1..8 via OPTION_BINNING on every
            // camera. Expose 1..4 as the commonly-useful range.
            info.supported_bins = {1, 2, 3, 4};

            // Infer the maximum raw bit depth from the capability flags.
            if (info.flags & Api::FLAG_RAW16)
                info.bit_depth_max = 16;
            else if (info.flags & Api::FLAG_RAW14)
                info.bit_depth_max = 14;
            else if (info.flags & Api::FLAG_RAW12)
                info.bit_depth_max = 12;
            else if (info.flags & Api::FLAG_RAW11)
                info.bit_depth_max = 11;
            else if (info.flags & Api::FLAG_RAW10)
                info.bit_depth_max = 10;
            else
                info.bit_depth_max = 8;  // FLAG_RAW8, or no raw-depth flag at all

            result.push_back(std::move(info));
        }
        return result;
    }

    // Reference-counted so the camera and thermal-switch drivers coexist on one
    // SDK open.
    HToupcam open_camera_by_id(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_shared_by_id(id, "camera");
    }

    void close_camera(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        close_shared(handle);
    }

    // Streaming lifecycle ----------------------------------------------------
    void start_pull_mode(HToupcam handle, void (*event_callback)(unsigned, void*), void* ctx) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::StartPullModeWithCallback(handle, event_callback, ctx), "StartPullModeWithCallback");
    }

    void stop(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::Stop(handle), "Stop");
    }

    // 0 = video, 1 = software trigger.
    void put_trigger_mode(HToupcam handle, int mode) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_TRIGGER, mode), "put_Option(TRIGGER)");
    }

    void trigger(HToupcam handle, unsigned short n_frames) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::Trigger(handle, n_frames), "Trigger");
    }

    // Returns true if a frame was delivered within timeout_ms. Throws on
    // non-timeout errors. On success, actual_width/height carry the frame
    // dimensions reported by the SDK.
    bool wait_image(HToupcam handle, unsigned timeout_ms, void* buffer, int bits, int row_pitch, unsigned& actual_width,
                    unsigned& actual_height) override {
        // Do NOT hold the wrapper mutex across WaitImageV4 — it blocks until a
        // frame is ready (or timeout), and we need the disconnect path to be
        // able to call Stop without waiting for the exposure to finish.
        typename Api::FrameInfoV4 info{};
        const int hr = Api::WaitImageV4(handle, timeout_ms, buffer, 0, bits, row_pitch, &info);
        if (static_cast<unsigned>(hr) == toupcam_family_detail::kE_TIMEOUT) {
            return false;
        }
        check(hr, "WaitImageV4");
        actual_width = info.v3.width;
        actual_height = info.v3.height;
        return true;
    }

    // Exposure & gain --------------------------------------------------------
    ToupExpRange get_exposure_range(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ToupExpRange range;
        check(Api::get_ExpTimeRange(handle, &range.min_us, &range.max_us, &range.def_us), "get_ExpTimeRange");
        return range;
    }

    unsigned get_exposure_us(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        unsigned value = 0;
        check(Api::get_ExpoTime(handle, &value), "get_ExpoTime");
        return value;
    }

    void put_exposure_us(HToupcam handle, unsigned exposure_us) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_ExpoTime(handle, exposure_us), "put_ExpoTime");
    }

    void put_auto_exposure(HToupcam handle, bool enable) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_AutoExpoEnable(handle, enable ? 1 : 0), "put_AutoExpoEnable");
    }

    ToupGainRange get_gain_range(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ToupGainRange range;
        check(Api::get_ExpoAGainRange(handle, &range.min, &range.max, &range.def), "get_ExpoAGainRange");
        return range;
    }

    unsigned short get_gain(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        unsigned short value = 0;
        check(Api::get_ExpoAGain(handle, &value), "get_ExpoAGain");
        return value;
    }

    void put_gain(HToupcam handle, unsigned short gain) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_ExpoAGain(handle, gain), "put_ExpoAGain");
    }

    // ROI / format / binning -------------------------------------------------
    ToupROIFormat get_roi(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ToupROIFormat roi;
        check(Api::get_Roi(handle, &roi.start_x, &roi.start_y, &roi.width, &roi.height), "get_Roi");
        return roi;
    }

    void put_roi(HToupcam handle, unsigned x, unsigned y, unsigned w, unsigned h) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Roi(handle, x, y, w, h), "put_Roi");
    }

    // 1, 2, 3, 4...
    void put_binning(HToupcam handle, int bin) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_BINNING, bin), "put_Option(BINNING)");
    }

    int get_binning(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 1;
        check(Api::get_Option(handle, Api::OPTION_BINNING, &value), "get_Option(BINNING)");
        return value;
    }

    // 0 = 8-bit mode, 1 = 16-bit mode (subset of PIXEL_FORMAT). Reconfiguring
    // requires the stream to be stopped — the driver handles that.
    void put_bitdepth(HToupcam handle, int bitdepth) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_BITDEPTH, bitdepth), "put_Option(BITDEPTH)");
    }

    int get_bitdepth(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_BITDEPTH, &value), "get_Option(BITDEPTH)");
        return value;
    }

    void put_raw(HToupcam handle, int enable) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_RAW, enable), "put_Option(RAW)");
    }

    int get_option(HToupcam handle, unsigned option) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, option, &value), "get_Option");
        return value;
    }

    void put_option(HToupcam handle, unsigned option, int value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, option, value), "put_Option");
    }

    // Frame size / format ----------------------------------------------------
    void get_size(HToupcam handle, int& width, int& height) override {
        std::lock_guard<std::mutex> lock(mutex_);
        width = 0;
        height = 0;
        check(Api::get_Size(handle, &width, &height), "get_Size");
    }

    void get_final_size(HToupcam handle, int& width, int& height) override {
        std::lock_guard<std::mutex> lock(mutex_);
        width = 0;
        height = 0;
        check(Api::get_FinalSize(handle, &width, &height), "get_FinalSize");
    }

    // FourCC returned by the SDK (e.g. 'RGGB', 'YYYY'). bits_per_pixel carries
    // the native pixel depth.
    void get_raw_format(HToupcam handle, unsigned& four_cc, unsigned& bits_per_pixel) override {
        std::lock_guard<std::mutex> lock(mutex_);
        four_cc = 0;
        bits_per_pixel = 0;
        check(Api::get_RawFormat(handle, &four_cc, &bits_per_pixel), "get_RawFormat");
    }

    // Cooler -----------------------------------------------------------------
    // Temperature is returned in 0.1 degrees Celsius.
    int get_temperature_deciC(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        short t = 0;
        check(Api::get_Temperature(handle, &t), "get_Temperature");
        return static_cast<int>(t);
    }

    void put_tec_enable(HToupcam handle, bool enable) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_TEC, enable ? 1 : 0), "put_Option(TEC)");
    }

    bool get_tec_enable(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_TEC, &value), "get_Option(TEC)");
        return value != 0;
    }

    void put_tec_target_deciC(HToupcam handle, int deci_c) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_TECTARGET, deci_c), "put_Option(TECTARGET)");
    }

    int get_tec_target_deciC(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_TECTARGET, &value), "get_Option(TECTARGET)");
        return value;
    }

    int get_tec_voltage_deciV(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_TEC_VOLTAGE, &value), "get_Option(TEC_VOLTAGE)");
        return value;
    }

    int get_tec_voltage_max_deciV(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_TEC_VOLTAGE_MAX, &value), "get_Option(TEC_VOLTAGE_MAX)");
        return value;
    }

    // High full well ---------------------------------------------------------
    // OPTION_HIGH_FULLWELL: 0 = disable, 1 = enable. Gated by
    // supports_high_fullwell; exposed to ASCOM as a ReadoutMode.
    int get_high_fullwell(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_HIGH_FULLWELL, &value), "get_Option(HIGH_FULLWELL)");
        return value;
    }

    void put_high_fullwell(HToupcam handle, bool enable) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_HIGH_FULLWELL, enable ? 1 : 0), "put_Option(HIGH_FULLWELL)");
    }

    // Conversion gain (OPTION_CG): 0 = LCG, 1 = HCG, 2 = HDR (only on
    // FLAG_CGHDR cameras). Gated by supports_cg; folded into ASCOM ReadoutModes.
    int get_cg(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_CG, &value), "get_Option(CG)");
        return value;
    }

    void put_cg(HToupcam handle, int cg) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_CG, cg), "put_Option(CG)");
    }

    // Black level (ASCOM Offset) ---------------------------------------------
    // OPTION_BLACKLEVEL. Range is [0, get_blacklevel_max]; the max scales with
    // the current output bit depth (31 at 8-bit up to 31*256 at 16-bit), so it
    // takes the camera's deep-mode bit count. Gated by supports_blacklevel.
    int get_blacklevel(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_BLACKLEVEL, &value), "get_Option(BLACKLEVEL)");
        return value;
    }

    void put_blacklevel(HToupcam handle, int value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_BLACKLEVEL, value), "put_Option(BLACKLEVEL)");
    }

    int get_blacklevel_max(HToupcam handle, int deep_bits) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // The black-level range scales with the current output bit depth:
        // BLACKLEVEL8_MAX (31) at 8-bit, up to 31*256 at 16-bit. In 8-bit output
        // mode the max is the 8-bit value; in deep mode it follows the camera's
        // native (deep) bit count.
        int mode = 0;
        // Propagate a BITDEPTH read failure instead of silently defaulting to 8-bit:
        // a camera actually streaming in deep mode would otherwise report OffsetMax
        // as 31 rather than its true deep-mode maximum. Every other getter here
        // throws on SDK failure; match that so the caller sees NotConnected/error.
        check(Api::get_Option(handle, Api::OPTION_BITDEPTH, &mode), "get_Option(BITDEPTH)");
        int bits = (mode == 0) ? 8 : deep_bits;
        if (bits < 8) {
            bits = 8;
        }
        return Api::BLACKLEVEL8_MAX << (bits - 8);
    }

    // Thermal controls (cooled-camera Switch) --------------------------------
    // Dew (anti-fog) heater: level in [0, get_heat_max]. 0 = off.
    int get_heat_max(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_HEAT_MAX, &value), "get_Option(HEAT_MAX)");
        return value;
    }

    int get_heat(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_HEAT, &value), "get_Option(HEAT)");
        return value;
    }

    void put_heat(HToupcam handle, int level) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_HEAT, level), "put_Option(HEAT)");
    }

    // Cooling fan: speed in [0, model->maxfanspeed]. 0 = off.
    int get_fan(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_FAN, &value), "get_Option(FAN)");
        return value;
    }

    void put_fan(HToupcam handle, int speed) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_FAN, speed), "put_Option(FAN)");
    }

    // Tail indicator LED (OPTION_TAILLIGHT): 0 = off, 1 = on. There is no
    // capability flag — probe by calling get_taillight and catching the error
    // on cameras that don't support it.
    int get_taillight(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_TAILLIGHT, &value), "get_Option(TAILLIGHT)");
        return value;
    }

    void put_taillight(HToupcam handle, bool on) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_TAILLIGHT, on ? 1 : 0), "put_Option(TAILLIGHT)");
    }

    // Camera metadata --------------------------------------------------------
    std::string get_serial_number(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        char sn[32] = {};
        if (Api::get_SerialNumber(handle, sn) < 0) {
            return "";
        }
        return std::string(sn);
    }

    std::string get_firmware_version(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        char fw[16] = {};
        if (Api::get_FwVersion(handle, fw) < 0) {
            return "";
        }
        return std::string(fw);
    }

    void get_pixel_size(HToupcam handle, unsigned resolution_index, float& x, float& y) override {
        std::lock_guard<std::mutex> lock(mutex_);
        x = 0.0f;
        y = 0.0f;
        if (Api::get_PixelSize(handle, resolution_index, &x, &y) < 0) {
            x = 0.0f;
            y = 0.0f;
        }
    }

    // ST4 pulse guide --------------------------------------------------------
    void pulse_guide(HToupcam handle, ToupGuideDirection direction, unsigned duration_ms) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::ST4PlusGuide(handle, static_cast<unsigned>(direction), duration_ms), "ST4PlusGuide");
    }

    // Returns true if the camera is currently guiding.
    bool is_guiding(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const int hr = Api::ST4PlusGuideState(handle);
        // Judge by success/failure, never by == S_OK alone: the SDK header warns
        // that S_FALSE (0x1) is a DISTINCT success code (here meaning "not
        // guiding"), so a bare `== S_OK` conflates it with error HRESULTs. check()
        // surfaces a real (negative) error instead of mapping it to "not guiding"
        // — otherwise a transient SDK hiccup would clear the caller's in-flight
        // guide flag while the hardware is still pulsing, and a guider polling
        // IsPulseGuiding could fire the next move mid-pulse. After it, only
        // success codes remain.
        check(hr, "ST4PlusGuideState");
        return hr == toupcam_family_detail::kS_OK;  // S_OK => guiding; S_FALSE (or any other success) => not
    }

    // AAF (Astro Auto Focuser) -----------------------------------------------
    // Enumeration filters EnumV2 results by FLAG_AUTOFOCUSER.
    std::vector<ToupFocuserInfo> enumerate_focusers() override {
        std::lock_guard<std::mutex> lock(mutex_);
        typename Api::DeviceV2 arr[Api::MAX]{};
        unsigned count = Api::EnumV2(arr);
        std::vector<ToupFocuserInfo> result;
        int focuser_index = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!arr[i].model) continue;
            unsigned long long flags = arr[i].model->flag;
            if ((flags & Api::FLAG_AUTOFOCUSER) == 0) {
                continue;
            }
            ToupFocuserInfo info;
            info.index = focuser_index++;
            info.id = arr[i].id;
            info.name = arr[i].displayname;
            info.model_name = arr[i].model->name ? arr[i].model->name : "";
            info.flags = flags;
            result.push_back(std::move(info));
        }
        return result;
    }

    HToupcam open_focuser_by_id(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_shared_by_id(id, "focuser");
    }

    void close_focuser(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        close_shared(handle);
    }

    // Generic AAF write: AAF(handle, action, value, nullptr).
    void aaf_set(HToupcam handle, int action, int value, const char* context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        throw_on_error(Api::AAF(handle, action, value, nullptr), context);
    }

    // Generic AAF read: AAF(handle, action, 0, &out).
    int aaf_get(HToupcam handle, int action, const char* context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        throw_on_error(Api::AAF(handle, action, 0, &value), context);
        return value;
    }

    // AAF range query: AAF(handle, RANGEMAX|RANGEMIN|RANGEDEF, action, &out).
    int aaf_range(HToupcam handle, int range_action, int target_action, const char* context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        throw_on_error(Api::AAF(handle, range_action, target_action, &value), context);
        return value;
    }

    // AFW (Astro Filter Wheel) -----------------------------------------------
    // Enumeration filters EnumV2 results by FLAG_FILTERWHEEL.
    std::vector<ToupFilterWheelInfo> enumerate_filter_wheels() override {
        std::lock_guard<std::mutex> lock(mutex_);
        typename Api::DeviceV2 arr[Api::MAX]{};
        unsigned count = Api::EnumV2(arr);
        std::vector<ToupFilterWheelInfo> result;
        int wheel_index = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!arr[i].model) continue;
            unsigned long long flags = arr[i].model->flag;
            if ((flags & Api::FLAG_FILTERWHEEL) == 0) {
                continue;
            }
            ToupFilterWheelInfo info;
            info.index = wheel_index++;
            info.id = arr[i].id;
            info.name = arr[i].displayname;
            info.model_name = arr[i].model->name ? arr[i].model->name : "";
            info.flags = flags;
            result.push_back(std::move(info));
        }
        return result;
    }

    HToupcam open_filter_wheel_by_id(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_shared_by_id(id, "filter wheel");
    }

    void close_filter_wheel(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        close_shared(handle);
    }

    // Number of filter slots reported by the wheel firmware
    // (OPTION_FILTERWHEEL_SLOT).
    int get_filter_wheel_slot_count(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_FILTERWHEEL_SLOT, &value), "get_Option(FILTERWHEEL_SLOT)");
        return value;
    }

    // Write the slot count back to the wheel (OPTION_FILTERWHEEL_SLOT is [RW]).
    // The toupbase reference driver does this at connect right after reading
    // it, re-applying the wheel's slot configuration.
    void set_filter_wheel_slot_count(HToupcam handle, int slot_count) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_FILTERWHEEL_SLOT, slot_count), "put_Option(FILTERWHEEL_SLOT)");
    }

    // Home/reset the wheel (OPTION_FILTERWHEEL_POSITION = -1). Required at
    // connect so the firmware establishes its slot reference — without it the
    // wheel hunts and never lands (notably after a firmware update).
    void reset_filter_wheel(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_FILTERWHEEL_POSITION, -1),
              "put_Option(FILTERWHEEL_POSITION=-1 reset)");
    }

    // Current slot (0-based). Returns -1 while the wheel is in motion, matching
    // the ASCOM FilterWheel Position contract (OPTION_FILTERWHEEL_POSITION).
    int get_filter_wheel_position(HToupcam handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int value = 0;
        check(Api::get_Option(handle, Api::OPTION_FILTERWHEEL_POSITION, &value), "get_Option(FILTERWHEEL_POSITION)");
        // -1 means the wheel is in motion; the position bits are the low byte.
        if (value < 0) {
            return -1;
        }
        return value & 0xff;
    }

    // Move to slot 0..N-1. Low byte = target slot; the direction bit (val >> 8)
    // is left at 0 (clockwise), matching the toupbase reference driver's
    // default. This is a single absolute move — the firmware handles the
    // traverse once the wheel has been homed at connect (see
    // ToupTekFilterWheelDriver::set_connected).
    void set_filter_wheel_position(HToupcam handle, int position) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(Api::put_Option(handle, Api::OPTION_FILTERWHEEL_POSITION, position & 0xff),
              "put_Option(FILTERWHEEL_POSITION)");
    }

private:
    static void throw_on_error(int hr, const char* context) {
        if (hr >= 0) {
            return;
        }
        throw AlpacaException(std::string(context) + ": " + toupcam_family_detail::hresult_to_string(hr),
                              toupcam_family_detail::map_hresult(hr));
    }

    // throw_on_error with the brand's function name ("Toupcam_put_Roi",
    // "Altaircam_put_Roi") as the context; built only on failure.
    static void check(int hr, const char* function) {
        if (hr >= 0) {
            return;
        }
        throw_on_error(hr, (std::string(Api::kPrefix) + "_" + function).c_str());
    }

    std::mutex mutex_;

    // Reference-counted shared opens keyed by the device's opaque id, so the
    // camera driver and the thermal-switch driver can operate on the same
    // physical camera at once (the SDK allows only one open handle per device).
    // Open fires once (first opener); Close fires once (last closer). Mirrors
    // the Player One wrapper's usage_ map.
    struct SharedCam {
        HToupcam handle{nullptr};
        int open_count{0};
    };
    std::map<std::string, SharedCam> shared_by_id_;
    std::map<HToupcam, std::string> id_by_handle_;

    // Every currently-open SDK handle. Lets close_shared() be idempotent: a
    // handle not in this set is a double-close and is ignored rather than passed
    // to Close (which, on an already-closed handle, could hit a recycled value
    // and tear down an innocent holder). Note this does not defend against a
    // caller that closes a stale handle AFTER another open recycled the same
    // value — that is a use-after-free in the caller.
    std::set<HToupcam> open_handles_;

    // Reference-counted open/close shared by EVERY device type that opens by id
    // (camera, focuser, filter wheel). Keyed by the device's opaque id, so two
    // driver instances on the same physical device (e.g. a camera and its
    // integrated autofocuser, or a camera and its thermal switch) share one
    // Open instead of the second raw-open returning null. Callers must already
    // hold mutex_.
    HToupcam open_shared_by_id(const std::string& id, const char* device_kind) {
        auto it = shared_by_id_.find(id);
        if (it != shared_by_id_.end() && it->second.open_count > 0) {
            ++it->second.open_count;
            return it->second.handle;
        }
        HToupcam h = Api::Open(id.empty() ? nullptr : id.c_str());
        if (!h) {
            throw AlpacaException(std::string(Api::kPrefix) + "_Open returned null (" + device_kind + " not available)",
                                  AlpacaError::NotConnected);
        }
        // Publish into both maps exception-safely: if either insertion throws
        // (only on OOM for these small key/value types), the caller never receives
        // h, so close_shared() would never run and the SDK handle would leak for
        // the singleton's lifetime. Roll back any partial state and close the
        // handle before rethrowing.
        try {
            shared_by_id_[id] = SharedCam{h, 1};
            id_by_handle_[h] = id;
            open_handles_.insert(h);
        } catch (...) {
            shared_by_id_.erase(id);
            id_by_handle_.erase(h);
            open_handles_.erase(h);
            Api::Close(h);
            throw;
        }
        return h;
    }

    void close_shared(HToupcam handle) {
        // Idempotent guard: if we have no record of this handle being open it is a
        // double-close. Do NOT fall through to Close — the handle value may have
        // been recycled by a later open, and closing it would kill that innocent
        // holder.
        auto live = open_handles_.find(handle);
        if (live == open_handles_.end()) {
            return;
        }
        auto hit = id_by_handle_.find(handle);
        if (hit != id_by_handle_.end()) {
            auto sit = shared_by_id_.find(hit->second);
            if (sit != shared_by_id_.end()) {
                // A tracked handle always has open_count >= 1 here (a zero-count
                // entry is erased below and removed from both maps). Guard the
                // decrement rather than doing "--open_count > 0" unconditionally:
                // if a mispaired double-close drove the count to <= 1 while another
                // holder still existed, the raw decrement would underflow past the
                // erase and Close a handle still in use (undefined behaviour).
                // Only a count > 1 means another driver still holds it.
                if (sit->second.open_count > 1) {
                    --sit->second.open_count;
                    return;  // Another driver still holds this device open.
                }
                shared_by_id_.erase(sit);  // final release: fall through to Close
            }
            id_by_handle_.erase(hit);
        }
        // Reached only when a tracked handle's ref count just hit zero above.
        // Drop it from the live set first so a later double-close is ignored by
        // the guard at the top.
        open_handles_.erase(live);
        if (handle) {
            Api::Close(handle);
        }
    }
};

}  // namespace alpacacore::vendor::touptek
