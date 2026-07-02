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
    std::unordered_map<std::string, qhyccd_handle*> handles;
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
        // Close all open handles
        for (auto& [id, handle] : handles) {
            if (handle != nullptr) {
                try {
                    CloseQHYCCD(handle);
                } catch (...) {}
            }
        }
        handles.clear();

        if (resource_initialized) {
            ReleaseQHYCCDResource();
        }
    }

    qhyccd_handle* get_handle(const std::string& camera_id) const {
        auto it = handles.find(camera_id);
        if (it == handles.end() || it->second == nullptr) {
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

    // Close existing handle if present
    auto it = pimpl_->handles.find(camera_id);
    if (it != pimpl_->handles.end() && it->second != nullptr) {
        CloseQHYCCD(it->second);
        pimpl_->handles.erase(it);
    }

    char id_buf[64] = {};
    std::strncpy(id_buf, camera_id.c_str(), sizeof(id_buf) - 1);

    qhyccd_handle* handle = OpenQHYCCD(id_buf);
    if (handle == nullptr) {
        throw AlpacaException("Failed to open QHY camera: " + camera_id, AlpacaError::DriverException);
    }

    // Single frame mode
    uint32_t ret = SetQHYCCDStreamMode(handle, 0);
    if (ret != QHYCCD_SUCCESS) {
        CloseQHYCCD(handle);
        throw AlpacaException("Failed to set QHY stream mode for: " + camera_id, AlpacaError::DriverException);
    }

    pimpl_->handles[camera_id] = handle;
}

void QHYSDKWrapper::init_camera(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    check_result(InitQHYCCD(handle), "InitQHYCCD");
}

void QHYSDKWrapper::close_camera(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    auto it = pimpl_->handles.find(camera_id);
    if (it == pimpl_->handles.end() || it->second == nullptr) {
        return;
    }
    CloseQHYCCD(it->second);
    pimpl_->handles.erase(it);
}

// ────────────────────────────────────────────────────────────────────────────
// Chip info
// ────────────────────────────────────────────────────────────────────────────

bool QHYSDKWrapper::get_chip_info(const std::string& camera_id, QHYCameraInfo& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);

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
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    return IsQHYCCDControlAvailable(handle, static_cast<CONTROL_ID>(control_id)) == QHYCCD_SUCCESS;
}

double QHYSDKWrapper::get_param(const std::string& camera_id, int control_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    return GetQHYCCDParam(handle, static_cast<CONTROL_ID>(control_id));
}

QHYControlRange QHYSDKWrapper::get_param_range(const std::string& camera_id, int control_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);

    QHYControlRange range{};
    uint32_t ret = GetQHYCCDParamMinMaxStep(handle, static_cast<CONTROL_ID>(control_id),
                                            &range.min, &range.max, &range.step);
    range.available = (ret == QHYCCD_SUCCESS);
    return range;
}

void QHYSDKWrapper::set_param(const std::string& camera_id, int control_id, double value) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    check_result(
        SetQHYCCDParam(handle, static_cast<CONTROL_ID>(control_id), value),
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
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    check_result(
        SetQHYCCDResolution(handle, start_x, start_y, width, height),
        "SetQHYCCDResolution"
    );
}

void QHYSDKWrapper::set_bin_mode(const std::string& camera_id,
                                 uint32_t wbin, uint32_t hbin) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    check_result(
        SetQHYCCDBinMode(handle, wbin, hbin),
        "SetQHYCCDBinMode"
    );
}

void QHYSDKWrapper::set_bits_mode(const std::string& camera_id, uint32_t bits) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    check_result(
        SetQHYCCDBitsMode(handle, bits),
        "SetQHYCCDBitsMode"
    );
}

uint32_t QHYSDKWrapper::get_mem_length(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    return GetQHYCCDMemLength(handle);
}

// ────────────────────────────────────────────────────────────────────────────
// Exposure
// ────────────────────────────────────────────────────────────────────────────

bool QHYSDKWrapper::start_single_frame(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
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
    // We fetch the handle under the mutex and then release before the blocking call.
    qhyccd_handle* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        handle = pimpl_->get_handle(camera_id);
    }

    uint32_t ret = GetQHYCCDSingleFrame(handle, &width, &height, &bpp, &channels, buffer);
    return ret == QHYCCD_SUCCESS;
}

void QHYSDKWrapper::cancel_exposure(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    auto it = pimpl_->handles.find(camera_id);
    if (it == pimpl_->handles.end() || it->second == nullptr) {
        return;
    }
    // CancelQHYCCDExposingAndReadout is supported by all cameras
    CancelQHYCCDExposingAndReadout(it->second);
}

// ────────────────────────────────────────────────────────────────────────────
// Guide port
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::guide(const std::string& camera_id,
                          uint32_t qhy_direction, uint16_t duration_ms) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
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
    qhyccd_handle* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(pimpl_->mutex);
        handle = pimpl_->get_handle(camera_id);
    }
    // ControlQHYCCDTemp implements PID cooler control — must be called ~every second
    ControlQHYCCDTemp(handle, target_temp_c);
}

// ────────────────────────────────────────────────────────────────────────────
// Readout modes
// ────────────────────────────────────────────────────────────────────────────

uint32_t QHYSDKWrapper::get_num_readout_modes(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    uint32_t num = 0;
    if (GetQHYCCDNumberOfReadModes(handle, &num) != QHYCCD_SUCCESS) {
        return 1; // Default: at least one mode
    }
    return num > 0 ? num : 1;
}

std::string QHYSDKWrapper::get_readout_mode_name(const std::string& camera_id,
                                                  uint32_t mode_index) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
    char name_buf[64] = {};
    if (GetQHYCCDReadModeName(handle, mode_index, name_buf) == QHYCCD_SUCCESS) {
        return std::string(name_buf);
    }
    return "Mode " + std::to_string(mode_index);
}

void QHYSDKWrapper::set_readout_mode(const std::string& camera_id, uint32_t mode_index) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    qhyccd_handle* handle = pimpl_->get_handle(camera_id);
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
