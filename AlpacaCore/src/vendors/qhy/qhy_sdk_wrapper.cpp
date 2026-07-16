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

#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>
#include <alpacacore/util/error_handling.h>

// QHY SDK headers — only included in this translation unit
#define __CPP_MODE__ 1
#include <qhyccd.h>

#include <mutex>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <cstring>

namespace alpacacore::vendor::qhy {

namespace {

void check_result(uint32_t ret, const char* context) {
    if (ret != QHYCCD_SUCCESS) {
        std::ostringstream oss;
        oss << context << " failed (code=0x" << std::hex << ret << ")";
        throw AlpacaException(oss.str(), AlpacaError::DriverException);
    }
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
// Impl
// ────────────────────────────────────────────────────────────────────────────

class QHYSDKWrapper::Impl {
public:
    mutable std::mutex mutex;
    // shared_ptr, not a raw pointer: a handful of calls (control_temp,
    // set_param, get_single_frame) release `mutex` before making their
    // blocking SDK call, so a concurrent close_camera() erasing the map
    // entry must not invalidate a handle those calls are still using
    // (ConformU finding: SetQHYCCDParam(MANULPWM) and ControlQHYCCDTemp can
    // both run long enough on real hardware for a disconnect to legitimately
    // give up waiting and move on). The deleter runs CloseQHYCCD only when
    // the LAST reference drops -- ours in this map, or a copy still held by
    // one of those in-flight blocking calls -- so the physical close is
    // deferred rather than racing a call that's still using the handle.
    std::unordered_map<std::string, std::shared_ptr<qhyccd_handle>> handles;
    mutable bool resource_initialized{false};
    mutable bool resource_init_attempted{false};
    mutable std::string sdk_version_cache;  // filled on successful init

    Impl() {
        // Deliberately NO libqhyccd call here — not even the debug-output
        // suppression: the FIRST call into the library spawns the SDK's
        // PnpEventListenerThread, which calls
        // libusb_hotplug_register_callback on the libusb context even when
        // libusb_init failed — a segfault on any host without a working USB
        // stack (verified: USB-less sandbox, backtrace through
        // PnpEventListenerThread -> libusb_hotplug_register_callback ->
        // pthread_mutex_lock on garbage). Constructing the wrapper happens on
        // configure/management paths (e.g. a configureddevices poll reading
        // the SDK version), which must never crash the server; the resource
        // comes up lazily on the first real camera operation (see
        // ensure_resource), i.e. at connect, where a user is actively
        // attaching hardware.
    }

    ~Impl() {
        // Each shared_ptr's deleter calls CloseQHYCCD; clearing the map drops
        // our reference on all of them (any still held by an in-flight
        // blocking call keeps that specific handle alive a little longer,
        // which is fine -- the process is tearing down either way).
        handles.clear();

        if (resource_initialized) {
            ReleaseQHYCCDResource();
        }
    }

    std::shared_ptr<qhyccd_handle> get_handle(const std::string& camera_id) const {
        auto it = handles.find(camera_id);
        if (it == handles.end() || !it->second) {
            throw AlpacaException("QHY camera not open: " + camera_id, AlpacaError::NotConnected);
        }
        return it->second;
    }

    // Lazy one-shot init (see the constructor comment for why). Callers hold
    // `mutex`, which serializes the attempt.
    void ensure_resource() const {
        if (!resource_init_attempted) {
            resource_init_attempted = true;
            // Suppress SDK debug output first (this is the point where the
            // library may spawn its listener thread — see the ctor comment).
            EnableQHYCCDMessage(false);
            EnableQHYCCDLogFile(false);
            if (InitQHYCCDResource() == QHYCCD_SUCCESS) {
                resource_initialized = true;
                uint32_t year = 0, month = 0, day = 0, subday = 0;
                if (GetQHYCCDSDKVersion(&year, &month, &day, &subday) == QHYCCD_SUCCESS) {
                    std::ostringstream oss;
                    oss << year << "." << std::setw(2) << std::setfill('0') << month << "." << std::setw(2)
                        << std::setfill('0') << day << "." << subday;
                    sdk_version_cache = oss.str();
                }
            }
        }
        if (!resource_initialized) {
            throw AlpacaException("QHY SDK resource not initialized", AlpacaError::DriverException);
        }
    }
};

// ────────────────────────────────────────────────────────────────────────────
// Singleton
// ────────────────────────────────────────────────────────────────────────────

QHYSDKWrapper& QHYSDKWrapper::instance() {
    static QHYSDKWrapper instance;
    return instance;
}

QHYSDKWrapper::QHYSDKWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

QHYSDKWrapper::~QHYSDKWrapper() = default;

// ────────────────────────────────────────────────────────────────────────────
// Enumeration
// ────────────────────────────────────────────────────────────────────────────

std::vector<QHYCameraInfo> QHYSDKWrapper::enumerate_cameras() {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->ensure_resource();

    uint32_t count = ScanQHYCCD();
    // ScanQHYCCD returns QHYCCD_ERROR (0xFFFFFFFF) on failure — treat as 0 cameras.
    if (count == QHYCCD_ERROR || count > 64) {
        return {};
    }
    std::vector<QHYCameraInfo> cameras;
    cameras.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        char id_buf[64] = {};
        if (GetQHYCCDId(i, id_buf) != QHYCCD_SUCCESS) {
            continue;
        }

        QHYCameraInfo info;
        info.camera_id = std::string(id_buf);

        char model_buf[64] = {};
        if (GetQHYCCDModel(id_buf, model_buf) == QHYCCD_SUCCESS) {
            info.model = std::string(model_buf);
        } else {
            info.model = info.camera_id;
        }

        cameras.push_back(std::move(info));
    }

    return cameras;
}

bool QHYSDKWrapper::get_camera_model(const std::string& camera_id, std::string& model) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->ensure_resource();

    char id_buf[64] = {};
    std::strncpy(id_buf, camera_id.c_str(), sizeof(id_buf) - 1);

    char model_buf[64] = {};
    if (GetQHYCCDModel(id_buf, model_buf) == QHYCCD_SUCCESS) {
        model = std::string(model_buf);
        return true;
    }
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::open_camera(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    pimpl_->ensure_resource();

    // Drop any existing handle for this id. Erase (not an explicit
    // CloseQHYCCD) so a stale handle still in use by an in-flight blocking
    // call elsewhere isn't closed out from under it -- see the Impl comment.
    pimpl_->handles.erase(camera_id);

    char id_buf[64] = {};
    std::strncpy(id_buf, camera_id.c_str(), sizeof(id_buf) - 1);

    qhyccd_handle* raw_handle = OpenQHYCCD(id_buf);
    if (raw_handle == nullptr) {
        throw AlpacaException("Failed to open QHY camera: " + camera_id, AlpacaError::DriverException);
    }

    // Single frame mode
    uint32_t ret = SetQHYCCDStreamMode(raw_handle, 0);
    if (ret != QHYCCD_SUCCESS) {
        CloseQHYCCD(raw_handle);
        throw AlpacaException("Failed to set QHY stream mode for: " + camera_id, AlpacaError::DriverException);
    }

    pimpl_->handles[camera_id] = std::shared_ptr<qhyccd_handle>(raw_handle, [](qhyccd_handle* h) {
        if (h != nullptr) {
            try {
                CloseQHYCCD(h);
            } catch (...) {}
        }
    });
}

void QHYSDKWrapper::init_camera(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    check_result(InitQHYCCD(handle), "InitQHYCCD");
}

void QHYSDKWrapper::close_camera(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    // Erase, don't call CloseQHYCCD directly: the shared_ptr deleter runs it
    // once the last reference drops. If control_temp()/set_param()/
    // get_single_frame() are mid-call on this handle right now (they hold
    // their own copy while pimpl_->mutex is released -- see those functions),
    // this erase just drops OUR reference; the actual hardware close is
    // deferred until that in-flight call returns and its copy goes out of
    // scope, instead of racing it.
    pimpl_->handles.erase(camera_id);
}

// ────────────────────────────────────────────────────────────────────────────
// Chip info
// ────────────────────────────────────────────────────────────────────────────

bool QHYSDKWrapper::get_chip_info(const std::string& camera_id, QHYCameraInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();

    double chipw = 0.0, chiph = 0.0;
    uint32_t imagew = 0, imageh = 0;
    double pixelw = 0.0, pixelh = 0.0;
    uint32_t bpp = 0;

    uint32_t ret = GetQHYCCDChipInfo(handle, &chipw, &chiph,
                                     &imagew, &imageh,
                                     &pixelw, &pixelh, &bpp);
    if (ret != QHYCCD_SUCCESS) {
        return false;
    }

    info.camera_id       = camera_id;
    info.max_width       = imagew;
    info.max_height      = imageh;
    info.pixel_size_x_um = pixelw;
    info.pixel_size_y_um = pixelh;
    info.bpp             = bpp;

    // Color detection.
    // IsQHYCCDControlAvailable(handle, CAM_IS_COLOR) returns the BAYER_ID enum
    // (BAYER_GB=1, BAYER_GR=2, BAYER_BG=3, BAYER_RG=4) for color cameras, or
    // QHYCCD_ERROR (0xFFFFFFFF) for monochrome cameras.  Comparing to
    // QHYCCD_SUCCESS (0) would always yield false — check against QHYCCD_ERROR.
    uint32_t bayer = IsQHYCCDControlAvailable(handle, CAM_IS_COLOR);
    info.is_color      = (bayer != QHYCCD_ERROR);
    info.bayer_pattern = info.is_color ? bayer : 0u;

    // Cooler detection
    info.has_cooler = (IsQHYCCDControlAvailable(handle, CONTROL_COOLER) == QHYCCD_SUCCESS);

    // ST-4 guide port detection
    info.has_st4_port = (IsQHYCCDControlAvailable(handle, CONTROL_ST4PORT) == QHYCCD_SUCCESS);

    // Mechanical shutter detection
    info.has_shutter = (IsQHYCCDControlAvailable(handle, CAM_MECHANICALSHUTTER) == QHYCCD_SUCCESS);

    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Control parameters
// ────────────────────────────────────────────────────────────────────────────

bool QHYSDKWrapper::is_control_available(const std::string& camera_id, int control_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    return IsQHYCCDControlAvailable(handle, static_cast<CONTROL_ID>(control_id)) == QHYCCD_SUCCESS;
}

double QHYSDKWrapper::get_param(const std::string& camera_id, int control_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    return GetQHYCCDParam(handle, static_cast<CONTROL_ID>(control_id));
}

QHYControlRange QHYSDKWrapper::get_param_range(const std::string& camera_id, int control_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();

    QHYControlRange range{};
    uint32_t ret = GetQHYCCDParamMinMaxStep(handle, static_cast<CONTROL_ID>(control_id),
                                            &range.min, &range.max, &range.step);
    range.available = (ret == QHYCCD_SUCCESS);
    return range;
}

void QHYSDKWrapper::set_param(const std::string& camera_id, int control_id, double value) {
    // Some params (e.g. MANULPWM) can block for an extended time on real
    // hardware, same as ControlQHYCCDTemp below. Do not hold pimpl_->mutex
    // for the duration -- every other SDK call takes the same mutex and
    // would stall, including close_camera() during a concurrent disconnect.
    // Keep the shared_ptr itself alive (not just a raw pointer) across the
    // unlocked call: a concurrent close_camera() may erase the map entry
    // while this call is still in flight, and this copy is what keeps the
    // handle valid until SetQHYCCDParam returns (see the Impl comment).
    std::shared_ptr<qhyccd_handle> handle;
    {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        handle = pimpl_->get_handle(camera_id);
    }
    check_result(
        SetQHYCCDParam(handle.get(), static_cast<CONTROL_ID>(control_id), value),
        "SetQHYCCDParam"
    );
}

// ────────────────────────────────────────────────────────────────────────────
// Image configuration
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::set_resolution(const std::string& camera_id,
                                   uint32_t start_x, uint32_t start_y,
                                   uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    check_result(
        SetQHYCCDResolution(handle, start_x, start_y, width, height),
        "SetQHYCCDResolution"
    );
}

void QHYSDKWrapper::set_bin_mode(const std::string& camera_id,
                                 uint32_t wbin, uint32_t hbin) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    check_result(
        SetQHYCCDBinMode(handle, wbin, hbin),
        "SetQHYCCDBinMode"
    );
}

void QHYSDKWrapper::set_bits_mode(const std::string& camera_id, uint32_t bits) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    check_result(
        SetQHYCCDBitsMode(handle, bits),
        "SetQHYCCDBitsMode"
    );
}

uint32_t QHYSDKWrapper::get_mem_length(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    return GetQHYCCDMemLength(handle);
}

// ────────────────────────────────────────────────────────────────────────────
// Exposure
// ────────────────────────────────────────────────────────────────────────────

bool QHYSDKWrapper::start_single_frame(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    uint32_t ret = ExpQHYCCDSingleFrame(handle);
    if (ret == QHYCCD_READ_DIRECTLY) {
        return true;
    }
    if (ret != QHYCCD_SUCCESS) {
        std::ostringstream oss;
        oss << "ExpQHYCCDSingleFrame failed (code=0x" << std::hex << ret << ")";
        throw AlpacaException(oss.str(), AlpacaError::DriverException);
    }
    return false;
}

bool QHYSDKWrapper::get_single_frame(const std::string& camera_id,
                                     uint8_t* buffer,
                                     uint32_t& width, uint32_t& height,
                                     uint32_t& bpp, uint32_t& channels) {
    // NOTE: This is a blocking call — do not hold the mutex for the duration.
    // We fetch the handle under the mutex and then release before the
    // blocking call. Keep the shared_ptr itself (not just a raw pointer)
    // alive across the call -- see the Impl comment and set_param() above.
    std::shared_ptr<qhyccd_handle> handle;
    {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        handle = pimpl_->get_handle(camera_id);
    }

    uint32_t ret = GetQHYCCDSingleFrame(handle.get(), &width, &height, &bpp, &channels, buffer);
    return ret == QHYCCD_SUCCESS;
}

void QHYSDKWrapper::cancel_exposure(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    auto it = pimpl_->handles.find(camera_id);
    if (it == pimpl_->handles.end() || !it->second) {
        return;
    }
    // CancelQHYCCDExposingAndReadout is supported by all cameras
    CancelQHYCCDExposingAndReadout(it->second.get());
}

// ────────────────────────────────────────────────────────────────────────────
// Guide port
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::guide(const std::string& camera_id,
                          uint32_t qhy_direction, uint16_t duration_ms) {
    // Holds pimpl_->mutex for the full ControlQHYCCDGuide call (up to the
    // pulse duration, e.g. 2s) -- the caller (qhy_camera_driver's
    // pulse_guide()) already runs this on its own detached thread so it never
    // blocks the HTTP thread, but it still serializes against every other SDK
    // call for that duration. Matches existing behavior; not changed here.
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    check_result(
        ControlQHYCCDGuide(handle, qhy_direction, duration_ms),
        "ControlQHYCCDGuide"
    );
}

// ────────────────────────────────────────────────────────────────────────────
// Temperature control
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::control_temp(const std::string& camera_id, double target_temp_c) {
    // NOTE: ControlQHYCCDTemp blocks for ~10s (PID loop). Do not hold pimpl_->mutex
    // for the duration — every other SDK call takes the same mutex and would stall.
    // Fetch the handle under the lock, release, then call the blocking function.
    // Keep the shared_ptr itself alive across the call -- see the Impl
    // comment and set_param() above.
    std::shared_ptr<qhyccd_handle> handle;
    {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        handle = pimpl_->get_handle(camera_id);
    }
    // ControlQHYCCDTemp implements PID cooler control — must be called ~every second
    ControlQHYCCDTemp(handle.get(), target_temp_c);
}

// ────────────────────────────────────────────────────────────────────────────
// Readout modes
// ────────────────────────────────────────────────────────────────────────────

uint32_t QHYSDKWrapper::get_num_readout_modes(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    uint32_t num = 0;
    if (GetQHYCCDNumberOfReadModes(handle, &num) != QHYCCD_SUCCESS) {
        return 1; // Default: at least one mode
    }
    return num > 0 ? num : 1;
}

std::string QHYSDKWrapper::get_readout_mode_name(const std::string& camera_id,
                                                  uint32_t mode_index) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    char name_buf[64] = {};
    if (GetQHYCCDReadModeName(handle, mode_index, name_buf) == QHYCCD_SUCCESS) {
        return std::string(name_buf);
    }
    return "Mode " + std::to_string(mode_index);
}

void QHYSDKWrapper::set_readout_mode(const std::string& camera_id, uint32_t mode_index) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id).get();
    check_result(SetQHYCCDReadMode(handle, mode_index), "SetQHYCCDReadMode");
}

// ────────────────────────────────────────────────────────────────────────────
// SDK version
// ────────────────────────────────────────────────────────────────────────────

std::string QHYSDKWrapper::get_sdk_version() {
    // Served from the cache filled at ensure_resource time: this getter is
    // reached from configure/management paths (configureddevices SdkVersion,
    // DriverInfo) where touching libqhyccd at all is unsafe pre-init — the
    // library's first entry point spawns its PnP listener thread, which
    // segfaults on hosts without a working USB stack. Empty until the first
    // real camera operation initializes the SDK.
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    return pimpl_->sdk_version_cache;
}

} // namespace alpacacore::vendor::qhy
