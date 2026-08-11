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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>

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
    //
    // open_count on top of that: some QHY cameras (e.g. miniCam8M) have an
    // integrated CFW controlled through the SAME physical handle as the
    // camera. The camera driver and the filter wheel driver each call
    // open_camera()/close_camera() independently for the same camera_id;
    // open_count lets the first opener's OpenQHYCCD stay live and shared
    // while either owner is connected, and only the last owner's
    // close_camera() actually erases the entry (mirrors ToupTek's
    // open_shared_by_id/close_shared for its camera+thermal-switch pairing).
    struct SharedHandle {
        std::shared_ptr<qhyccd_handle> handle;
        int open_count{0};
        // Serialises EVERY SDK call against this specific handle (see
        // get_handle_ref() below) -- real miniCam8M hardware showed that a
        // GetQHYCCDSingleFrame stuck mid-transfer doesn't just leave its own
        // call hanging: a concurrent call from another thread on the SAME
        // handle -- even a normally-instant register read like
        // GetQHYCCDParam -- also hangs forever, presumably because the wedged
        // handle's internal USB session state blocks any further traffic on
        // it, not just the specific transfer in flight. Serialising with a
        // real mutex, instead of relying on every caller to separately check
        // an "exposing" flag, means a caller now waits its turn (bounded by
        // however long the download takes) instead of the request itself
        // vanishing into an unbounded hang. shared_ptr, not embedded by
        // value: callers keep a copy alive across their SDK call (mirroring
        // the `handle` shared_ptr immediately below), so a concurrent
        // close_camera() erasing this map entry can't destroy a mutex a
        // waiting/held caller still needs.
        std::shared_ptr<std::mutex> call_mutex{std::make_shared<std::mutex>()};
    };
    std::unordered_map<std::string, SharedHandle> handles;
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
        if (it == handles.end() || !it->second.handle) {
            throw AlpacaException("QHY camera not open: " + camera_id, AlpacaError::NotConnected);
        }
        return it->second.handle;
    }

    struct HandleRef {
        std::shared_ptr<qhyccd_handle> handle;
        std::shared_ptr<std::mutex> call_mutex;
    };

    // Fetch both the handle and its per-handle call_mutex under the (brief)
    // bookkeeping lock, then release it -- callers lock *call_mutex
    // themselves for the actual SDK call, so this map lookup never sits
    // behind (or blocks) a call already in flight on some OTHER handle.
    HandleRef get_handle_ref(const std::string& camera_id) const {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = handles.find(camera_id);
        if (it == handles.end() || !it->second.handle) {
            throw AlpacaException("QHY camera not open: " + camera_id, AlpacaError::NotConnected);
        }
        return HandleRef{it->second.handle, it->second.call_mutex};
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

    // Shared open: if another owner (camera driver or CFW driver) already
    // has this id open, just bump the ref count onto the SAME handle instead
    // of erasing and re-opening -- a second OpenQHYCCD on an id that's
    // already open would either fail or hand back an independent handle the
    // SDK doesn't expect two live owners for.
    //
    // Accepted trade-off (matches ToupTek's open_shared_by_id): this means a
    // reconnect no longer self-heals a stale handle left by an unplug/replug
    // while a second owner is still connected -- e.g. camera + CFW both
    // connected, USB drops and re-enumerates, user reconnects only the
    // camera; the reused handle is dead until *every* owner disconnects and
    // reconnects. No liveness check on reuse today; if this bites in
    // practice, the fix is a cheap probe (e.g. IsQHYCCDControlAvailable) on
    // reuse that re-opens on failure.
    auto it = pimpl_->handles.find(camera_id);
    if (it != pimpl_->handles.end() && it->second.open_count > 0) {
        ++it->second.open_count;
        return;
    }

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

    pimpl_->handles[camera_id] =
        Impl::SharedHandle{std::shared_ptr<qhyccd_handle>(raw_handle,
                                                          [](qhyccd_handle* h) {
                                                              if (h != nullptr) {
                                                                  try {
                                                                      CloseQHYCCD(h);
                                                                  } catch (...) {  // NOLINT(bugprone-empty-catch)
                                                                      // Best-effort close during teardown; nothing to
                                                                      // recover here.
                                                                  }
                                                              }
                                                          }),
                           1};
}

void QHYSDKWrapper::init_camera(const std::string& camera_id) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    check_result(InitQHYCCD(ref.handle.get()), "InitQHYCCD");
}

void QHYSDKWrapper::close_camera(const std::string& camera_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    auto it = pimpl_->handles.find(camera_id);
    if (it == pimpl_->handles.end()) {
        return;  // not open -- idempotent
    }
    if (it->second.open_count > 1) {
        --it->second.open_count;  // another owner (camera or CFW) still has it open
        return;
    }
    // Last owner: erase, don't call CloseQHYCCD directly. The shared_ptr
    // deleter runs it once the last reference drops. If control_temp()/
    // set_param()/get_single_frame() are mid-call on this handle right now
    // (they hold their own copy while pimpl_->mutex is released -- see those
    // functions), this erase just drops OUR reference; the actual hardware
    // close is deferred until that in-flight call returns and its copy goes
    // out of scope, instead of racing it.
    pimpl_->handles.erase(it);
}

// ────────────────────────────────────────────────────────────────────────────
// Chip info
// ────────────────────────────────────────────────────────────────────────────

bool QHYSDKWrapper::get_chip_info(const std::string& camera_id, QHYCameraInfo& info) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    qhyccd_handle* handle = ref.handle.get();

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
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    return IsQHYCCDControlAvailable(ref.handle.get(), static_cast<CONTROL_ID>(control_id)) == QHYCCD_SUCCESS;
}

double QHYSDKWrapper::get_param(const std::string& camera_id, int control_id) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    return GetQHYCCDParam(ref.handle.get(), static_cast<CONTROL_ID>(control_id));
}

QHYControlRange QHYSDKWrapper::get_param_range(const std::string& camera_id, int control_id) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    qhyccd_handle* handle = ref.handle.get();

    QHYControlRange range{};
    uint32_t ret = GetQHYCCDParamMinMaxStep(handle, static_cast<CONTROL_ID>(control_id),
                                            &range.min, &range.max, &range.step);
    range.available = (ret == QHYCCD_SUCCESS);
    return range;
}

void QHYSDKWrapper::set_param(const std::string& camera_id, int control_id, double value) {
    // Some params (e.g. MANULPWM) can block for an extended time on real
    // hardware, same as ControlQHYCCDTemp below. Fetch the handle+call_mutex
    // under pimpl_->mutex (brief), then release it before the call: close_camera()
    // during a concurrent disconnect only needs pimpl_->mutex to erase its map
    // entry, and must not stall behind this call. The per-handle call_mutex
    // (held for the actual SDK call) is what actually serialises this against
    // every other call on the SAME handle -- see the SharedHandle comment.
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    check_result(SetQHYCCDParam(ref.handle.get(), static_cast<CONTROL_ID>(control_id), value), "SetQHYCCDParam");
}

// ────────────────────────────────────────────────────────────────────────────
// Image configuration
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::set_resolution(const std::string& camera_id,
                                   uint32_t start_x, uint32_t start_y,
                                   uint32_t width, uint32_t height) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    check_result(SetQHYCCDResolution(ref.handle.get(), start_x, start_y, width, height), "SetQHYCCDResolution");
}

void QHYSDKWrapper::set_bin_mode(const std::string& camera_id,
                                 uint32_t wbin, uint32_t hbin) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    check_result(SetQHYCCDBinMode(ref.handle.get(), wbin, hbin), "SetQHYCCDBinMode");
}

void QHYSDKWrapper::set_bits_mode(const std::string& camera_id, uint32_t bits) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    check_result(SetQHYCCDBitsMode(ref.handle.get(), bits), "SetQHYCCDBitsMode");
}

uint32_t QHYSDKWrapper::get_mem_length(const std::string& camera_id) {
    ALPACA_LOG_DEBUG("QHY", "get_mem_length: acquiring call_mutex...");
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    ALPACA_LOG_DEBUG("QHY", "get_mem_length: call_mutex acquired, calling GetQHYCCDMemLength...");
    uint32_t len = GetQHYCCDMemLength(ref.handle.get());
    ALPACA_LOG_DEBUG("QHY", "get_mem_length: GetQHYCCDMemLength returned " + std::to_string(len));
    return len;
}

// ────────────────────────────────────────────────────────────────────────────
// Exposure
// ────────────────────────────────────────────────────────────────────────────

bool QHYSDKWrapper::start_single_frame(const std::string& camera_id) {
    // Held across ExpQHYCCDSingleFrame like every other call (see the
    // SharedHandle comment): this call arms the exposure, and letting some
    // OTHER call (e.g. a gain read) interleave here has the same wedge risk
    // as interleaving with the download itself.
    ALPACA_LOG_DEBUG("QHY", "start_single_frame: acquiring call_mutex...");
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    ALPACA_LOG_DEBUG("QHY", "start_single_frame: call_mutex acquired, calling ExpQHYCCDSingleFrame...");
    uint32_t ret = ExpQHYCCDSingleFrame(ref.handle.get());
    ALPACA_LOG_DEBUG(
        "QHY", "start_single_frame: ExpQHYCCDSingleFrame returned 0x" + [ret] {
            std::ostringstream o;
            o << std::hex << ret;
            return o.str();
        }());
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
    // NOTE: This is the long blocking call the per-handle call_mutex exists
    // for (see the SharedHandle comment) -- every other call on this camera
    // now queues behind it instead of racing it and wedging. cancel_exposure()
    // below is the deliberate exception: it must be able to reach
    // CancelQHYCCDExposingAndReadout WHILE this call is blocked, from another
    // thread, to have any chance of waking it up.
    ALPACA_LOG_DEBUG("QHY", "get_single_frame: acquiring call_mutex...");
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    ALPACA_LOG_DEBUG("QHY", "get_single_frame: call_mutex acquired, calling GetQHYCCDSingleFrame...");
    uint32_t ret = GetQHYCCDSingleFrame(ref.handle.get(), &width, &height, &bpp, &channels, buffer);
    ALPACA_LOG_DEBUG(
        "QHY", "get_single_frame: GetQHYCCDSingleFrame returned 0x" + [ret] {
            std::ostringstream o;
            o << std::hex << ret;
            return o.str();
        }());
    return ret == QHYCCD_SUCCESS;
}

void QHYSDKWrapper::cancel_exposure(const std::string& camera_id) {
    // Deliberately does NOT take the per-handle call_mutex that every other
    // function in this file now serialises on (see the SharedHandle comment
    // and get_single_frame() above): this is the SDK's own mechanism for
    // interrupting a GetQHYCCDSingleFrame that's already blocked inside that
    // mutex on another thread. Taking the same mutex here would deadlock --
    // this call would wait for the very call it exists to interrupt.
    // CancelQHYCCDExposingAndReadout is documented/observed to return
    // quickly regardless of whether a read is in flight, so this stays safe
    // to call concurrently.
    std::lock_guard<std::mutex> lock(pimpl_->mutex);
    auto it = pimpl_->handles.find(camera_id);
    if (it == pimpl_->handles.end() || !it->second.handle) {
        return;
    }
    CancelQHYCCDExposingAndReadout(it->second.handle.get());
}

// ────────────────────────────────────────────────────────────────────────────
// Guide port
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::guide(const std::string& camera_id,
                          uint32_t qhy_direction, uint16_t duration_ms) {
    // Holds the per-handle call_mutex for the full ControlQHYCCDGuide call (up
    // to the pulse duration, e.g. 2s) -- the caller (qhy_camera_driver's
    // pulse_guide()) already runs this on its own detached thread so it never
    // blocks the HTTP thread, but it still serialises against every other SDK
    // call on this handle for that duration (see the SharedHandle comment).
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    check_result(ControlQHYCCDGuide(ref.handle.get(), qhy_direction, duration_ms), "ControlQHYCCDGuide");
}

// ────────────────────────────────────────────────────────────────────────────
// Temperature control
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::control_temp(const std::string& camera_id, double target_temp_c) {
    // NOTE: ControlQHYCCDTemp blocks for ~10s (PID loop) and now goes through
    // the per-handle call_mutex like every other call (see the SharedHandle
    // comment) -- the camera driver's temp-control thread already skips its
    // own call while an exposure is in flight (see qhy_camera_driver.cpp), so
    // this mutex is a backstop for calls this file doesn't otherwise know
    // about, not the primary way exposures avoid stalling behind a PID cycle.
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    // ControlQHYCCDTemp implements PID cooler control — must be called ~every second
    ControlQHYCCDTemp(ref.handle.get(), target_temp_c);
}

// ────────────────────────────────────────────────────────────────────────────
// Filter wheel (integrated CFW)
// ────────────────────────────────────────────────────────────────────────────

void QHYSDKWrapper::move_cfw(const std::string& camera_id, int position) {
    // Matches the release-before-call pattern used by set_param()/
    // control_temp()/get_single_frame() above: fetch the shared_ptr under the
    // lock, then call the SDK unlocked so this can't stack behind (or block)
    // get_cfw_position()'s much longer hold below. Keep the shared_ptr itself
    // alive across the call so a concurrent close_camera() can't invalidate
    // the handle mid-call (see the Impl comment).
    // The single-ASCII-digit protocol below only has room for slots 0-9;
    // QHY's current lineup tops out at 9 slots, but the web UI accepts a
    // custom slot count up to 16, so make the protocol's real ceiling
    // explicit rather than silently sending a bogus byte for position 10+.
    if (position > 9) {
        throw AlpacaException("Filter position out of range for QHY CFW protocol (max 9)", AlpacaError::InvalidValue);
    }

    // The CFW shares this camera's physical handle (see the SharedHandle
    // comment / qhy_sdk_wrapper.h), so a filter move commanded while the
    // camera is mid-exposure now queues behind the per-handle call_mutex
    // instead of racing GetQHYCCDSingleFrame and wedging both.
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);

    // SDK convention (per the 25.09.29 SDK's ControlCFW.cpp sample): the order is a
    // single ASCII digit '0'-'9' for the target slot, not a raw byte.
    char order = static_cast<char>('0' + position);
    check_result(SendOrder2QHYCCDCFW(ref.handle.get(), &order, 1), "SendOrder2QHYCCDCFW");
}

int QHYSDKWrapper::get_cfw_position(const std::string& camera_id) {
    // GetQHYCCDCFWStatus is a genuine ~100-130ms hardware round trip on real
    // miniCam8M hardware (ConformU finding, see qhy_filterwheel_driver.cpp),
    // and the filter wheel driver's get_position() calls this on every poll
    // while a move is in flight. Now goes through the per-handle call_mutex
    // like every other call (see the SharedHandle comment): a Position poll
    // arriving while the camera is downloading a frame will wait for that
    // download instead of racing it -- slower than the old always-unlocked
    // call, but correct; the old behavior could wedge both the download and
    // this read together on real hardware.
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);

    char status[64] = {};
    if (GetQHYCCDCFWStatus(ref.handle.get(), status) != QHYCCD_SUCCESS) {
        return -1;
    }
    // Settled position is reported as an ASCII digit; any other value means
    // the wheel is still moving or the status is not yet known.
    if (status[0] < '0' || status[0] > '9') {
        return -1;
    }
    return status[0] - '0';
}

// ────────────────────────────────────────────────────────────────────────────
// Readout modes
// ────────────────────────────────────────────────────────────────────────────

uint32_t QHYSDKWrapper::get_num_readout_modes(const std::string& camera_id) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    uint32_t num = 0;
    if (GetQHYCCDNumberOfReadModes(ref.handle.get(), &num) != QHYCCD_SUCCESS) {
        return 1; // Default: at least one mode
    }
    return num > 0 ? num : 1;
}

std::string QHYSDKWrapper::get_readout_mode_name(const std::string& camera_id,
                                                  uint32_t mode_index) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    char name_buf[64] = {};
    if (GetQHYCCDReadModeName(ref.handle.get(), mode_index, name_buf) == QHYCCD_SUCCESS) {
        return std::string(name_buf);
    }
    return "Mode " + std::to_string(mode_index);
}

void QHYSDKWrapper::set_readout_mode(const std::string& camera_id, uint32_t mode_index) {
    auto ref = pimpl_->get_handle_ref(camera_id);
    std::lock_guard<std::mutex> call_lock(*ref.call_mutex);
    check_result(SetQHYCCDReadMode(ref.handle.get(), mode_index), "SetQHYCCDReadMode");
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
