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

#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>

#define TOUPCAM_HRESULT_ERRORCODE_NEEDED
#include <toupcam.h>

#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

namespace alpacacore::vendor::touptek {

namespace {

std::string hresult_to_string(HRESULT hr) {
    switch (static_cast<unsigned>(hr)) {
    case 0x00000000u: return "S_OK";
    case 0x00000001u: return "S_FALSE";
    case 0x8000ffffu: return "E_UNEXPECTED";
    case 0x80004001u: return "E_NOTIMPL";
    case 0x80004002u: return "E_NOINTERFACE";
    case 0x80070005u: return "E_ACCESSDENIED";
    case 0x8007000eu: return "E_OUTOFMEMORY";
    case 0x80070057u: return "E_INVALIDARG";
    case 0x80004003u: return "E_POINTER";
    case 0x80004005u: return "E_FAIL";
    case 0x8001010eu: return "E_WRONG_THREAD";
    case 0x8007001fu: return "E_GEN_FAILURE";
    case 0x800700aau: return "E_BUSY";
    case 0x8000000au: return "E_PENDING";
    case 0x8001011fu: return "E_TIMEOUT";
    case 0x80072743u: return "E_UNREACH";
    case 0x800704C7u: return "E_CANCELLED";
    default: {
        std::ostringstream oss;
        oss << "HRESULT 0x" << std::hex << static_cast<unsigned>(hr);
        return oss.str();
    }
    }
}

int map_hresult(HRESULT hr) {
    switch (static_cast<unsigned>(hr)) {
    case 0x80070057u: // E_INVALIDARG
    case 0x80004003u: // E_POINTER
        return AlpacaError::InvalidValue;
    case 0x80004001u: // E_NOTIMPL
        return AlpacaError::NotImplemented;
    case 0x800700aau: // E_BUSY
    case 0x8000ffffu: // E_UNEXPECTED
    case 0x8001010eu: // E_WRONG_THREAD
        return AlpacaError::InvalidOperation;
    case 0x80070005u: // E_ACCESSDENIED
        return AlpacaError::NotConnected;
    default:
        return AlpacaError::DriverException;
    }
}

void throw_on_error(HRESULT hr, const char* context) {
    if (SUCCEEDED(hr)) {
        return;
    }
    throw AlpacaException(std::string(context) + ": " + hresult_to_string(hr),
                          map_hresult(hr));
}

} // namespace

class ToupTekSDKWrapper::Impl {
public:
    std::mutex mutex_;

    // Reference-counted shared opens keyed by the camera's opaque id, so the
    // camera driver and the thermal-switch driver can operate on the same
    // physical camera at once (Toupcam allows only one open handle per device).
    // Toupcam_Open fires once (first opener); Toupcam_Close fires once (last
    // closer). Mirrors the Player One wrapper's usage_ map.
    struct SharedCam {
        HToupcam handle{nullptr};
        int open_count{0};
    };
    std::map<std::string, SharedCam> shared_by_id_;
    std::map<HToupcam, std::string> id_by_handle_;

    // Every currently-open SDK handle — both ref-counted by-id opens and the
    // untracked open_camera_by_index() legacy handles. Lets close_shared() be
    // idempotent: a handle not in this set is a double-close and is ignored
    // rather than passed to Toupcam_Close (which, on an already-closed handle,
    // could hit a recycled value and tear down an innocent holder). Note this
    // does not defend against a caller that closes a stale handle AFTER another
    // open recycled the same value — that is a use-after-free in the caller.
    std::set<HToupcam> open_handles_;

    // Reference-counted open/close shared by EVERY device type that opens by id
    // (camera, focuser, filter wheel). Keyed by the device's opaque id, so two
    // driver instances on the same physical device (e.g. a camera and its
    // integrated autofocuser, or a camera and its thermal switch) share one
    // Toupcam_Open instead of the second raw-open returning null. Callers must
    // already hold mutex_.
    HToupcam open_shared_by_id(const std::string& id, const char* unavailable_msg) {
        auto it = shared_by_id_.find(id);
        if (it != shared_by_id_.end() && it->second.open_count > 0) {
            ++it->second.open_count;
            return it->second.handle;
        }
        HToupcam h = Toupcam_Open(id.empty() ? nullptr : id.c_str());
        if (!h) {
            throw AlpacaException(unavailable_msg, AlpacaError::NotConnected);
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
            Toupcam_Close(h);
            throw;
        }
        return h;
    }

    void close_shared(HToupcam handle) {
        // Idempotent guard: if we have no record of this handle being open it is a
        // double-close. Do NOT fall through to Toupcam_Close — the handle value may
        // have been recycled by a later open, and closing it would kill that
        // innocent holder. (The bot-suggested "not in id_by_handle_ -> return" guard
        // would instead have leaked every untracked open_camera_by_index handle,
        // which is legitimately absent from id_by_handle_ and must still close here.)
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
                // erase and Toupcam_Close a handle still in use (undefined
                // behaviour). Only a count > 1 means another driver still holds it.
                if (sit->second.open_count > 1) {
                    --sit->second.open_count;
                    return;  // Another driver still holds this device open.
                }
                shared_by_id_.erase(sit);  // final release: fall through to Close
            }
            id_by_handle_.erase(hit);
        }
        // Reached only on an actual close: an untracked index handle (missed the
        // id_by_handle_ lookup — the legacy non-ref-counted path) or a tracked
        // handle whose ref count just hit zero above. Drop it from the live set
        // first so a later double-close is ignored by the guard at the top.
        open_handles_.erase(live);
        if (handle) {
            Toupcam_Close(handle);
        }
    }
};

ToupTekSDKWrapper::ToupTekSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ToupTekSDKWrapper::~ToupTekSDKWrapper() = default;

ToupTekSDKWrapper& ToupTekSDKWrapper::instance() {
    static ToupTekSDKWrapper wrapper;
    return wrapper;
}

std::string ToupTekSDKWrapper::get_sdk_version() {
    const char* v = Toupcam_Version();
    return v ? v : "unknown";
}

std::vector<ToupCameraInfo> ToupTekSDKWrapper::enumerate_cameras() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ToupcamDeviceV2 arr[TOUPCAM_MAX]{};
    unsigned count = Toupcam_EnumV2(arr);
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
        // Toupcam_EnumV2 also lists standalone AFW filter wheels and AAF
        // focusers, which are not cameras. Skip them (mirroring the flag guards
        // in enumerate_filter_wheels/enumerate_focusers) so cameras[cameraIndex]
        // — and the cameraIndex=0 default — never resolves to an accessory when
        // a camera and an AFW/AAF are attached together.
        if ((arr[i].model->flag & (TOUPCAM_FLAG_FILTERWHEEL | TOUPCAM_FLAG_AUTOFOCUSER)) != 0) {
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
        info.is_color = (info.flags & TOUPCAM_FLAG_MONO) == 0;
        info.supports_pulse_guide = (info.flags & TOUPCAM_FLAG_ST4) != 0;
        info.supports_cooler = (info.flags & TOUPCAM_FLAG_TEC) != 0;
        info.supports_tec_onoff = (info.flags & TOUPCAM_FLAG_TEC_ONOFF) != 0;
        info.supports_trigger_software = (info.flags & TOUPCAM_FLAG_TRIGGER_SOFTWARE) != 0;
        info.supports_high_fullwell = (info.flags & TOUPCAM_FLAG_HIGH_FULLWELL) != 0;
        info.supports_cghdr = (info.flags & TOUPCAM_FLAG_CGHDR) != 0;
        info.supports_cg = info.supports_cghdr || (info.flags & TOUPCAM_FLAG_CG) != 0;
        info.supports_blacklevel = (info.flags & TOUPCAM_FLAG_BLACKLEVEL) != 0;
        info.supports_heat = (info.flags & TOUPCAM_FLAG_HEAT) != 0;
        info.supports_fan = (info.flags & TOUPCAM_FLAG_FAN) != 0;

        // Toupcam supports digital binning 1..8 via OPTION_BINNING on every
        // camera. Expose 1..4 as the commonly-useful range.
        info.supported_bins = {1, 2, 3, 4};

        // Infer the maximum raw bit depth from the capability flags.
        if (info.flags & TOUPCAM_FLAG_RAW16) info.bit_depth_max = 16;
        else if (info.flags & TOUPCAM_FLAG_RAW14) info.bit_depth_max = 14;
        else if (info.flags & TOUPCAM_FLAG_RAW12) info.bit_depth_max = 12;
        else if (info.flags & TOUPCAM_FLAG_RAW11) info.bit_depth_max = 11;
        else if (info.flags & TOUPCAM_FLAG_RAW10) info.bit_depth_max = 10;
        else if (info.flags & TOUPCAM_FLAG_RAW8)  info.bit_depth_max = 8;
        else info.bit_depth_max = 8;

        result.push_back(std::move(info));
    }
    return result;
}

HToupcam ToupTekSDKWrapper::open_camera_by_index(int camera_index) {
    // WARNING: this index is the SDK's raw Toupcam_EnumV2 order, which still
    // includes AFW filter wheels and AAF focusers — it is NOT the camera-only
    // index space that enumerate_cameras() (and the web UI's cameraIndex) uses.
    // No driver path calls this today (all open by id via open_camera_by_id); it
    // is kept only as a legacy convenience. Prefer open_camera_by_id so the
    // index spaces can never diverge.
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    HToupcam h = Toupcam_OpenByIndex(static_cast<unsigned>(camera_index));
    if (!h) {
        throw AlpacaException("Toupcam_OpenByIndex returned null (camera not available)",
                              AlpacaError::NotConnected);
    }
    // Track even untracked index opens so close_shared() is idempotent for them
    // too. If the insert throws (OOM), close the handle rather than leak it.
    try {
        pimpl_->open_handles_.insert(h);
    } catch (...) {
        Toupcam_Close(h);
        throw;
    }
    return h;
}

HToupcam ToupTekSDKWrapper::open_camera_by_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    // Reference-counted so the camera and thermal-switch drivers coexist on one
    // Toupcam open. Handles opened by index are not tracked and close via the
    // legacy path in close_camera.
    return pimpl_->open_shared_by_id(id, "Toupcam_Open returned null (camera not available)");
}

void ToupTekSDKWrapper::close_camera(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    pimpl_->close_shared(handle);
}

void ToupTekSDKWrapper::start_pull_mode(HToupcam handle,
                                         void (*event_callback)(unsigned, void*),
                                         void* ctx) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_StartPullModeWithCallback(handle, event_callback, ctx),
                   "Toupcam_StartPullModeWithCallback");
}

void ToupTekSDKWrapper::stop(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_Stop(handle), "Toupcam_Stop");
}

void ToupTekSDKWrapper::put_trigger_mode(HToupcam handle, int mode) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_TRIGGER, mode),
                   "Toupcam_put_Option(TRIGGER)");
}

void ToupTekSDKWrapper::trigger(HToupcam handle, unsigned short n_frames) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_Trigger(handle, n_frames), "Toupcam_Trigger");
}

bool ToupTekSDKWrapper::wait_image(HToupcam handle,
                                    unsigned timeout_ms,
                                    void* buffer,
                                    int bits,
                                    int row_pitch,
                                    unsigned& actual_width,
                                    unsigned& actual_height) {
    // Do NOT hold the wrapper mutex across WaitImageV4 — it blocks until a
    // frame is ready (or timeout), and we need the disconnect path to be
    // able to call Toupcam_Stop without waiting for the exposure to finish.
    ToupcamFrameInfoV4 info{};
    HRESULT hr = Toupcam_WaitImageV4(handle, timeout_ms, buffer, 0, bits, row_pitch, &info);
    if (static_cast<unsigned>(hr) == 0x8001011fu /* E_TIMEOUT */) {
        return false;
    }
    throw_on_error(hr, "Toupcam_WaitImageV4");
    actual_width = info.v3.width;
    actual_height = info.v3.height;
    return true;
}

ToupExpRange ToupTekSDKWrapper::get_exposure_range(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ToupExpRange range;
    throw_on_error(Toupcam_get_ExpTimeRange(handle, &range.min_us, &range.max_us, &range.def_us),
                   "Toupcam_get_ExpTimeRange");
    return range;
}

unsigned ToupTekSDKWrapper::get_exposure_us(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    unsigned value = 0;
    throw_on_error(Toupcam_get_ExpoTime(handle, &value), "Toupcam_get_ExpoTime");
    return value;
}

void ToupTekSDKWrapper::put_exposure_us(HToupcam handle, unsigned exposure_us) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_ExpoTime(handle, exposure_us), "Toupcam_put_ExpoTime");
}

void ToupTekSDKWrapper::put_auto_exposure(HToupcam handle, bool enable) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_AutoExpoEnable(handle, enable ? 1 : 0),
                   "Toupcam_put_AutoExpoEnable");
}

ToupGainRange ToupTekSDKWrapper::get_gain_range(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ToupGainRange range;
    throw_on_error(Toupcam_get_ExpoAGainRange(handle, &range.min, &range.max, &range.def),
                   "Toupcam_get_ExpoAGainRange");
    return range;
}

unsigned short ToupTekSDKWrapper::get_gain(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    unsigned short value = 0;
    throw_on_error(Toupcam_get_ExpoAGain(handle, &value), "Toupcam_get_ExpoAGain");
    return value;
}

void ToupTekSDKWrapper::put_gain(HToupcam handle, unsigned short gain) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_ExpoAGain(handle, gain), "Toupcam_put_ExpoAGain");
}

ToupROIFormat ToupTekSDKWrapper::get_roi(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ToupROIFormat roi;
    throw_on_error(Toupcam_get_Roi(handle, &roi.start_x, &roi.start_y,
                                   &roi.width, &roi.height),
                   "Toupcam_get_Roi");
    return roi;
}

void ToupTekSDKWrapper::put_roi(HToupcam handle, unsigned x, unsigned y,
                                 unsigned w, unsigned h) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Roi(handle, x, y, w, h), "Toupcam_put_Roi");
}

void ToupTekSDKWrapper::put_binning(HToupcam handle, int bin) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_BINNING, bin),
                   "Toupcam_put_Option(BINNING)");
}

int ToupTekSDKWrapper::get_binning(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 1;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_BINNING, &value),
                   "Toupcam_get_Option(BINNING)");
    return value;
}

void ToupTekSDKWrapper::put_bitdepth(HToupcam handle, int bitdepth) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_BITDEPTH, bitdepth),
                   "Toupcam_put_Option(BITDEPTH)");
}

int ToupTekSDKWrapper::get_bitdepth(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_BITDEPTH, &value),
                   "Toupcam_get_Option(BITDEPTH)");
    return value;
}

void ToupTekSDKWrapper::put_raw(HToupcam handle, int enable) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_RAW, enable),
                   "Toupcam_put_Option(RAW)");
}

int ToupTekSDKWrapper::get_option(HToupcam handle, unsigned option) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, option, &value), "Toupcam_get_Option");
    return value;
}

void ToupTekSDKWrapper::put_option(HToupcam handle, unsigned option, int value) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, option, value), "Toupcam_put_Option");
}

void ToupTekSDKWrapper::get_size(HToupcam handle, int& width, int& height) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    width = 0;
    height = 0;
    throw_on_error(Toupcam_get_Size(handle, &width, &height), "Toupcam_get_Size");
}

void ToupTekSDKWrapper::get_final_size(HToupcam handle, int& width, int& height) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    width = 0;
    height = 0;
    throw_on_error(Toupcam_get_FinalSize(handle, &width, &height), "Toupcam_get_FinalSize");
}

void ToupTekSDKWrapper::get_raw_format(HToupcam handle, unsigned& four_cc, unsigned& bits_per_pixel) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    four_cc = 0;
    bits_per_pixel = 0;
    throw_on_error(Toupcam_get_RawFormat(handle, &four_cc, &bits_per_pixel),
                   "Toupcam_get_RawFormat");
}

int ToupTekSDKWrapper::get_temperature_deciC(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    short t = 0;
    throw_on_error(Toupcam_get_Temperature(handle, &t), "Toupcam_get_Temperature");
    return static_cast<int>(t);
}

void ToupTekSDKWrapper::put_tec_enable(HToupcam handle, bool enable) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_TEC, enable ? 1 : 0),
                   "Toupcam_put_Option(TEC)");
}

bool ToupTekSDKWrapper::get_tec_enable(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_TEC, &value),
                   "Toupcam_get_Option(TEC)");
    return value != 0;
}

void ToupTekSDKWrapper::put_tec_target_deciC(HToupcam handle, int deci_c) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_TECTARGET, deci_c),
                   "Toupcam_put_Option(TECTARGET)");
}

int ToupTekSDKWrapper::get_tec_target_deciC(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_TECTARGET, &value),
                   "Toupcam_get_Option(TECTARGET)");
    return value;
}

int ToupTekSDKWrapper::get_tec_voltage_deciV(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_TEC_VOLTAGE, &value),
                   "Toupcam_get_Option(TEC_VOLTAGE)");
    return value;
}

int ToupTekSDKWrapper::get_tec_voltage_max_deciV(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_TEC_VOLTAGE_MAX, &value),
                   "Toupcam_get_Option(TEC_VOLTAGE_MAX)");
    return value;
}

int ToupTekSDKWrapper::get_high_fullwell(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_HIGH_FULLWELL, &value),
                   "Toupcam_get_Option(HIGH_FULLWELL)");
    return value;
}

void ToupTekSDKWrapper::put_high_fullwell(HToupcam handle, bool enable) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_HIGH_FULLWELL, enable ? 1 : 0),
                   "Toupcam_put_Option(HIGH_FULLWELL)");
}

int ToupTekSDKWrapper::get_cg(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_CG, &value), "Toupcam_get_Option(CG)");
    return value;
}

void ToupTekSDKWrapper::put_cg(HToupcam handle, int cg) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_CG, cg), "Toupcam_put_Option(CG)");
}

int ToupTekSDKWrapper::get_blacklevel(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_BLACKLEVEL, &value), "Toupcam_get_Option(BLACKLEVEL)");
    return value;
}

void ToupTekSDKWrapper::put_blacklevel(HToupcam handle, int value) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_BLACKLEVEL, value), "Toupcam_put_Option(BLACKLEVEL)");
}

int ToupTekSDKWrapper::get_blacklevel_max(HToupcam handle, int deep_bits) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    // The black-level range scales with the current output bit depth:
    // TOUPCAM_BLACKLEVEL8_MAX (31) at 8-bit, up to 31*256 at 16-bit. In 8-bit
    // output mode the max is the 8-bit value; in deep mode it follows the
    // camera's native (deep) bit count.
    int mode = 0;
    // Propagate a BITDEPTH read failure instead of silently defaulting to 8-bit:
    // a camera actually streaming in deep mode would otherwise report OffsetMax
    // as 31 rather than its true deep-mode maximum. Every other getter here
    // throws on SDK failure; match that so the caller sees NotConnected/error.
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_BITDEPTH, &mode), "Toupcam_get_Option(BITDEPTH)");
    int bits = (mode == 0) ? 8 : deep_bits;
    if (bits < 8) {
        bits = 8;
    }
    return TOUPCAM_BLACKLEVEL8_MAX << (bits - 8);
}

int ToupTekSDKWrapper::get_heat_max(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_HEAT_MAX, &value), "Toupcam_get_Option(HEAT_MAX)");
    return value;
}

int ToupTekSDKWrapper::get_heat(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_HEAT, &value), "Toupcam_get_Option(HEAT)");
    return value;
}

void ToupTekSDKWrapper::put_heat(HToupcam handle, int level) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_HEAT, level), "Toupcam_put_Option(HEAT)");
}

int ToupTekSDKWrapper::get_fan(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_FAN, &value), "Toupcam_get_Option(FAN)");
    return value;
}

void ToupTekSDKWrapper::put_fan(HToupcam handle, int speed) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_FAN, speed), "Toupcam_put_Option(FAN)");
}

int ToupTekSDKWrapper::get_taillight(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_TAILLIGHT, &value), "Toupcam_get_Option(TAILLIGHT)");
    return value;
}

void ToupTekSDKWrapper::put_taillight(HToupcam handle, bool on) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_TAILLIGHT, on ? 1 : 0), "Toupcam_put_Option(TAILLIGHT)");
}

std::string ToupTekSDKWrapper::get_serial_number(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    char sn[32] = {};
    HRESULT hr = Toupcam_get_SerialNumber(handle, sn);
    if (FAILED(hr)) {
        return "";
    }
    return std::string(sn);
}

std::string ToupTekSDKWrapper::get_firmware_version(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    char fw[16] = {};
    HRESULT hr = Toupcam_get_FwVersion(handle, fw);
    if (FAILED(hr)) {
        return "";
    }
    return std::string(fw);
}

void ToupTekSDKWrapper::get_pixel_size(HToupcam handle, unsigned resolution_index,
                                        float& x, float& y) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    x = 0.0f;
    y = 0.0f;
    HRESULT hr = Toupcam_get_PixelSize(handle, resolution_index, &x, &y);
    if (FAILED(hr)) {
        x = 0.0f;
        y = 0.0f;
    }
}

void ToupTekSDKWrapper::pulse_guide(HToupcam handle, ToupGuideDirection direction,
                                     unsigned duration_ms) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_ST4PlusGuide(handle, static_cast<unsigned>(direction), duration_ms),
                   "Toupcam_ST4PlusGuide");
}

bool ToupTekSDKWrapper::is_guiding(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    HRESULT hr = Toupcam_ST4PlusGuideState(handle);
    // Judge by SUCCEEDED/FAILED, never by == S_OK: the SDK header warns that
    // S_FALSE (0x1) is a DISTINCT success code (here meaning "not guiding"), so a
    // bare `== S_OK` conflates it with error HRESULTs. throw_on_error surfaces a
    // real (negative) error instead of mapping it to "not guiding" — otherwise a
    // transient SDK hiccup would clear the caller's in-flight guide flag while the
    // hardware is still pulsing, and a guider polling IsPulseGuiding could fire the
    // next move mid-pulse. After it, only success codes remain.
    throw_on_error(hr, "Toupcam_ST4PlusGuideState");
    return hr == S_OK;  // S_OK => guiding; S_FALSE (or any other success) => not guiding
}

std::vector<ToupFocuserInfo> ToupTekSDKWrapper::enumerate_focusers() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ToupcamDeviceV2 arr[TOUPCAM_MAX]{};
    unsigned count = Toupcam_EnumV2(arr);
    std::vector<ToupFocuserInfo> result;
    int focuser_index = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!arr[i].model) continue;
        unsigned long long flags = arr[i].model->flag;
        if ((flags & TOUPCAM_FLAG_AUTOFOCUSER) == 0) {
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

HToupcam ToupTekSDKWrapper::open_focuser_by_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    return pimpl_->open_shared_by_id(id, "Toupcam_Open returned null (focuser not available)");
}

void ToupTekSDKWrapper::close_focuser(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    pimpl_->close_shared(handle);
}

void ToupTekSDKWrapper::aaf_set(HToupcam handle, int action, int value, const char* context) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_AAF(handle, action, value, nullptr), context);
}

int ToupTekSDKWrapper::aaf_get(HToupcam handle, int action, const char* context) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_AAF(handle, action, 0, &value), context);
    return value;
}

int ToupTekSDKWrapper::aaf_range(HToupcam handle, int range_action, int target_action,
                                  const char* context) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_AAF(handle, range_action, target_action, &value), context);
    return value;
}

std::vector<ToupFilterWheelInfo> ToupTekSDKWrapper::enumerate_filter_wheels() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    ToupcamDeviceV2 arr[TOUPCAM_MAX]{};
    unsigned count = Toupcam_EnumV2(arr);
    std::vector<ToupFilterWheelInfo> result;
    int wheel_index = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!arr[i].model) continue;
        unsigned long long flags = arr[i].model->flag;
        if ((flags & TOUPCAM_FLAG_FILTERWHEEL) == 0) {
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

HToupcam ToupTekSDKWrapper::open_filter_wheel_by_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    return pimpl_->open_shared_by_id(id, "Toupcam_Open returned null (filter wheel not available)");
}

void ToupTekSDKWrapper::close_filter_wheel(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    pimpl_->close_shared(handle);
}

int ToupTekSDKWrapper::get_filter_wheel_slot_count(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_FILTERWHEEL_SLOT, &value),
                   "Toupcam_get_Option(FILTERWHEEL_SLOT)");
    return value;
}

void ToupTekSDKWrapper::set_filter_wheel_slot_count(HToupcam handle, int slot_count) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_FILTERWHEEL_SLOT, slot_count),
                   "Toupcam_put_Option(FILTERWHEEL_SLOT)");
}

void ToupTekSDKWrapper::reset_filter_wheel(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    // -1 triggers the wheel's home/reset cycle.
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_FILTERWHEEL_POSITION, -1),
                   "Toupcam_put_Option(FILTERWHEEL_POSITION=-1 reset)");
}

int ToupTekSDKWrapper::get_filter_wheel_position(HToupcam handle) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    int value = 0;
    throw_on_error(Toupcam_get_Option(handle, TOUPCAM_OPTION_FILTERWHEEL_POSITION, &value),
                   "Toupcam_get_Option(FILTERWHEEL_POSITION)");
    // -1 means the wheel is in motion; the position bits are the low byte.
    if (value < 0) {
        return -1;
    }
    return value & 0xff;
}

void ToupTekSDKWrapper::set_filter_wheel_position(HToupcam handle, int position) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    // Low byte = target slot; the direction bit (val >> 8) is left at 0
    // (clockwise), matching the toupbase reference driver's default. This is a
    // single absolute move — the firmware handles the traverse once the wheel
    // has been homed at connect (see ToupTekFilterWheelDriver::set_connected).
    throw_on_error(Toupcam_put_Option(handle, TOUPCAM_OPTION_FILTERWHEEL_POSITION, position & 0xff),
                   "Toupcam_put_Option(FILTERWHEEL_POSITION)");
}

} // namespace alpacacore::vendor::touptek
