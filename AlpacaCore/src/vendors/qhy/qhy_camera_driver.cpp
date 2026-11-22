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

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/camera_frame_request.h>
#include <alpacacore/util/capture_session.h>
#include <alpacacore/util/device_name.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/image_validation.h>
#include <alpacacore/util/last_exposure.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace alpacacore::vendor::qhy {

namespace {

// Map Alpaca pulse guide direction to QHY guide direction.
// Alpaca: 0=North, 1=South, 2=East, 3=West
// QHY:    0=EAST,  1=NORTH, 2=SOUTH, 3=WEST
uint32_t alpaca_to_qhy_guide_direction(int alpaca_direction) {
    switch (alpaca_direction) {
    case 0: return guide_direction::NORTH;
    case 1: return guide_direction::SOUTH;
    case 2: return guide_direction::EAST;
    case 3: return guide_direction::WEST;
    default: return guide_direction::NORTH;
    }
}

// Return true if the bin value is supported (1-4 checked via CAM_BINnXn).
bool bin_is_supported(QHYSDK& sdk, const std::string& camera_id, int bin) {
    switch (bin) {
    case 1: return sdk.is_control_available(camera_id, control::BIN1X1);
    case 2: return sdk.is_control_available(camera_id, control::BIN2X2);
    case 3: return sdk.is_control_available(camera_id, control::BIN3X3);
    case 4: return sdk.is_control_available(camera_id, control::BIN4X4);
    default: return false;
    }
}

int max_supported_bin(QHYSDK& sdk, const std::string& camera_id) {
    for (int bin = 4; bin >= 1; --bin) {
        if (bin_is_supported(sdk, camera_id, bin)) {
            return bin;
        }
    }
    return 1;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
// Exposure state
// ────────────────────────────────────────────────────────────────────────────

// The frame-request texts (#2261) in QHY's words.
constexpr util::CameraRequestTexts kRequestTexts = [] {
    util::CameraRequestTexts texts;
    texts.frame_size_not_positive = "ROI dimensions are invalid";
    texts.frame_above_sensor = "ROI exceeds sensor area for current binning";
    texts.frame_start_off_sensor = "Start position + ROI exceeds sensor area";
    texts.frame_beyond_sensor = "Start position + ROI exceeds sensor area";
    return texts;
}();
static_assert(util::all_texts_set(kRequestTexts), "every frame-request text is set");

// ────────────────────────────────────────────────────────────────────────────
// QHYCameraDriver
// ────────────────────────────────────────────────────────────────────────────

class QHYCameraDriver : public CameraDriver, protected alpacacore::AsyncConnectable {
    enum class CoolerOffState : uint8_t { None, Pending, Issued };
    struct CachedInt {
        std::optional<int> value;
        std::exception_ptr error;

        int get() const {
            if (error) std::rethrow_exception(error);
            if (!value) {
                throw AlpacaException("Camera control cache unavailable", AlpacaError::InvalidOperation);
            }
            return *value;
        }
    };

    template <typename Read>
    static CachedInt cache_read(Read&& read) {
        try {
            return {read(), {}};
        } catch (...) {
            return {{}, std::current_exception()};
        }
    }

    struct ControlCache {
        CachedInt value, min, max;
    };

    ControlCache read_control(const std::string& id, int control, const char* name) {
        ControlCache result;
        result.value = cache_read([&] { return param_to_int(sdk_.get_param(id, control), name); });
        try {
            const auto range = sdk_.get_param_range(id, control);
            if (!range.available) {
                throw AlpacaException(std::string(name) + " range not available", AlpacaError::PropertyNotImplemented);
            }
            result.min.value = param_to_int(range.min, name);
            result.max.value = param_to_int(range.max, name);
        } catch (...) {
            result.min.error = result.max.error = std::current_exception();
        }
        return result;
    }

public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    QHYCameraDriver(int device_number, std::optional<std::string> camera_id, std::optional<int> camera_index,
                    QHYSDK& sdk, QHYWorkerStartHook on_worker_start = {},
                    std::chrono::seconds watchdog_margin = std::chrono::seconds(60),
                    util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("QHY"),
          sdk_(sdk),
          clock_(clock),
          watchdog_margin_(watchdog_margin),
          on_worker_start_(std::move(on_worker_start)),
          device_number_(device_number),
          camera_id_(std::move(camera_id)),
          camera_index_(camera_index),
          camera_info_{},
          camera_info_valid_(false),
          readout_modes_{},
          readout_mode_(0),
          cached_gain_{},
          cached_offset_{},
          bin_x_(1),
          bin_y_(1),
          num_x_(0),
          num_y_(0),
          start_x_(0),
          start_y_(0),
          bits_(16),
          target_temp_(0.0),
          cooler_on_(true),
          connected_(false),
          pulse_guiding_end_{} {
        // Deliberately NO SDK touch here (the old construction-time
        // try_preload_camera_info is gone): libqhyccd's first entry point
        // spawns its PnP listener thread, which segfaults the whole process
        // on hosts without a working USB stack — and driver construction
        // happens on the configure/management path, which must never crash
        // the server. Cost: the web UI shows the model name only after the
        // first Connect instead of immediately. camera_info_ is populated at
        // open+init.
    }

    ~QHYCameraDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        stop_all_threads();
        if (connected_.load()) {
            try {
                // Use the synchronous implementation during destruction so that
                // teardown completes before the object is destroyed.
                set_connected_impl(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Error during destruction: " + std::string(e.what()));
            }
        }
        // Issue #1736: the disconnect above may start an OFF cleanup; join it last.
        // No time bound: a wedged PWM write keeps the driver alive, never detached.
        cooler_off_slot_.cancel_all_and_join();
    }

    // ── AlpacaDriver ─────────────────────────────────────────────────────────

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        // camera_info_.model is populated at open+init (construction must not
        // touch the SDK — see the constructor comment). Return it whenever available.
        if (!camera_info_.model.empty()) {
            return util::branded_name("QHY", camera_info_.model);
        }
        return "QHY Camera";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Camera;
    }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_id_.has_value()) {
            return "QHY_" + camera_id_.value();
        }
        return "QHY_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "QHY CCD Camera Driver";
    }

    std::string get_driver_info() const override {
        // The SDK version cache is empty until the first connect (the wrapper
        // must not touch libqhyccd pre-init); omit the suffix rather than
        // rendering a malformed "(SDK )".
        const auto ver = sdk_.get_sdk_version();
        return ver.empty() ? "AlpacaCore QHY Camera Driver" : "AlpacaCore QHY Camera Driver (SDK " + ver + ")";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only. DriverInfo's
    // pre-existing SDK mention is left as-is but deliberately not extended.
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = sdk_.get_sdk_version();
        if (version.empty()) {
            return std::nullopt;
        }
        return version;
    }

    int get_interface_version() const override {
        return 4;  // ICameraV4 (Platform 7)
    }

    bool get_connected() const override {
        return connected_.load();
    }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    void set_connected(bool connected) override {
        // Alpaca semantics require set_connected to be synchronous so that
        // a subsequent get_connected() immediately reflects the new state.
        // Keep the heavy work here; the explicit connect()/disconnect()
        // methods are the async entry points that use start_connection_task.
        set_connected_impl(connected);
    }

private:
    // Internal synchronous implementation used by the connection thread and
    // destructor. Performs the actual SDK open/close and state changes.
    void set_connected_impl(bool connected) {
        std::unique_lock<std::mutex> transition_lock(transition_mutex_);
        // Disconnection path: the slot bodies take mutex_, so the waits for them
        // below run without it.
        if (!connected) {
            // Best-effort, exception-safe teardown. The goal on this path is
            // "do no harm": never let SDK failures propagate to clients or
            // destructors, and always avoid deadlocks with the temp thread.
            //
            // Decision 0020 rule 5: stop the SDK exposure, then close the session and join its body, BEFORE the SDK
            // close below: the body blocks in GetQHYCCDSingleFrame on the camera id, so closing underneath it is a
            // use-after-close inside libqhyccd. stop_or_abort_exposure_locked() marks the exposure stopping (its broken
            // download is dropped, not kept) and its token tells the body the stop is meant for it, or ends a start
            // still reserved; disconnecting_ refuses a
            // StartExposure from here on. The join has no time bound: when cancel_exposure() does not interrupt
            // the frame call, the disconnect waits for that one call (decision 0020, the QHY row; as on ZWO).
            std::string exposure_id;
            bool stop_exposure = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (record_disconnect_if_connect_in_flight(connected_.load()) || !connected_.load()) {
                    return;
                }
                disconnecting_ = true;
                ++connection_generation_;
                const std::uint64_t token = stop_or_abort_exposure_locked();
                stop_exposure = token != 0 || session_.busy();
                exposure_id = camera_id_.value_or("");
            }
            if (stop_exposure) {
                cancel_sdk_exposure(exposure_id, "during disconnect");
            }
            run_disconnect_stop_hook();
            session_.disconnect_and_join();
            // Bound the wait for OFF cleanup, which waits for every PID call to
            // return; the OFF body never takes mutex_.
            // These workers can all be waiting on the same SDK call; spend one reap budget.
            const auto cleanup_deadline = clock_.now() + std::chrono::seconds(2);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_temp_control_locked();
                telemetry_slot_.cancel();
            }
            wait_for_cooler_off(cleanup_deadline);
            // Issue #1736: bounded waits, never a join or a detach. A body still
            // inside an SDK call with no timeout stays cancelled; the destructor joins it.
            wait_for_body(temp_slot_, "temp-control", cleanup_deadline);
            wait_for_body(telemetry_slot_, "telemetry", cleanup_deadline);

            std::string id;
            {
                std::lock_guard<std::mutex> cooler_lock(cooler_off_lifecycle_mutex_);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // Clear state before close; retain the model for the disconnected web UI.
                    const std::string model = camera_info_.model;
                    camera_info_ = {};
                    camera_info_.model = model;
                    camera_info_valid_ = false;
                    readout_modes_.clear();
                    readout_mode_ = 0;
                    cached_gain_.reset();
                    cached_offset_.reset();
                    gain_state_ = {};
                    offset_state_ = {};
                    max_bin_ = {};
                    telemetry_temp_valid_ = false;
                    telemetry_temp_unsupported_ = false;
                    telemetry_power_valid_ = false;
                    reset_exposure_state_locked();
                    exposure_range_valid_ = false;
                    connected_.store(false);
                    disconnecting_ = false;
                    id = camera_id_.value_or("");
                }
                // Acceptance ends with Connected=false; cleanup owns every remaining OFF.
                try {
                    start_cooler_off_worker();
                } catch (const std::exception& e) {
                    ALPACA_LOG_ERROR("QHY", "Failed to prepare cooler OFF during disconnect: " + std::string(e.what()));
                }
            }
            wait_for_cooler_off(cleanup_deadline);
            std::lock_guard<std::mutex> io_lock(io_mutex_);
            if (!id.empty()) {
                // The exposure body was joined above, so no exposure call is left on this handle.
                try {
                    sdk_.close_camera(id);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("QHY", "close_camera failed during disconnect: " +
                        std::string(e.what()));
                }
            }
            return;
        }

        // Connection path
        std::unique_lock<std::mutex> lock(mutex_);
        // Base gate (obligation 5) BEFORE the idempotency check: a connect must
        // honor a newer pending disconnect by staying down. This function split
        // on `connected` above, so only the connect-side gate belongs here.
        if (consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected_.load()) {
            return; // already connected
        }

        lock.unlock();
        // A redundant connect returned above, before queuing behind a live SDK call.
        std::unique_lock<std::mutex> io_lock(io_mutex_);
        const auto resolved = resolve_camera();
        const std::string id = resolved.camera_id;
        lock.lock();

        if (cooler_off_running_->load()) {
            throw AlpacaException("Camera cooler OFF is still finishing; try again shortly",
                                  AlpacaError::InvalidOperation);
        }

        QHYCameraInfo info{};
        info.camera_id = id;
        info.model = camera_info_.model.empty() ? resolved.model : camera_info_.model;
        exposure_range_valid_ = false;
        lock.unlock();

        sdk_.open_camera(id);
        // Roll back the ref-counted open if any init step throws: open_camera()
        // now shares one handle per camera_id with the CFW driver via an
        // open_count, so an unmatched open is no longer self-healed by the next
        // connect — it would pin the handle (CloseQHYCCD never firing) for the
        // life of the process. Same pattern as the filter wheel's connect path.
        try {
            sdk_.init_camera(id);

            // Populate full camera info
            if (info.model.empty()) {
                sdk_.get_camera_model(id, info.model);
            }
            const bool info_valid = sdk_.get_chip_info(id, info);

            // Select bit depth (prefer 16-bit)
            int bits = 8;
            if (sdk_.is_control_available(id, control::BITS16)) {
                bits = 16;
            }
            sdk_.set_bits_mode(id, bits);

            // Default to full-frame 1x1 binning
            sdk_.set_bin_mode(id, 1, 1);
            sdk_.set_resolution(id, 0, 0, info.max_width, info.max_height);

            // Enumerate readout modes
            auto modes = load_readout_modes(id);
            const auto range = read_exposure_range(id);
            const auto gain = read_control(id, control::GAIN, "Gain");
            const auto offset = read_control(id, control::OFFSET, "Offset");
            const auto max_bin = cache_read([&] { return max_supported_bin(sdk_, id); });

            // Seed the cached CCD temperature with one CURTEMP read so
            // CCDTemperature works before the telemetry worker's first poll
            // (open-astro#941). Runs before that worker starts, so the two
            // cannot race on the cache. A failed read is not an error and
            // never substitutes a value: the property keeps reporting one.
            bool temp_valid = false;
            double temperature = 0.0;
            if (info_valid && info.has_cooler) {
                try {
                    if (sdk_.is_control_available(id, control::CURTEMP)) {
                        temperature = sdk_.get_param(id, control::CURTEMP);
                        temp_valid = true;
                    }
                } catch (const std::exception& e) {
                    ALPACA_LOG_DEBUG("QHY", "Connect-time CURTEMP seed failed: " + std::string(e.what()));
                }
            }
            lock.lock();
            camera_id_ = id;
            camera_info_ = std::move(info);
            camera_info_valid_ = info_valid;
            bits_ = bits;
            bin_x_ = bin_y_ = 1;
            start_x_ = start_y_ = 0;
            num_x_ = static_cast<int>(camera_info_.max_width);
            num_y_ = static_cast<int>(camera_info_.max_height);
            readout_modes_ = std::move(modes);
            readout_mode_ = 0;
            exposure_range_ = range;
            exposure_range_valid_ = true;
            gain_state_ = gain;
            offset_state_ = offset;
            max_bin_ = max_bin;
            cooler_off_request_->store(CoolerOffState::None);
            cooler_off_warned_ = std::make_shared<std::atomic<bool>>(false);
            reset_exposure_state_locked();
            // Rule 8: the session reopens on connect; it refuses while a disconnect's join is still running.
            session_.open();
            telemetry_ccd_temp_c_ = temperature;
            telemetry_temp_valid_ = temp_valid;
            telemetry_temp_unsupported_ = false;
        } catch (...) {
            if (lock.owns_lock()) {
                lock.unlock();
            }
            try {
                sdk_.close_camera(id);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Failed to roll back camera open after connect error: " + std::string(e.what()));
            }
            throw;
        }
        connected_.store(true);
        const auto generation = ++connection_generation_;
        const bool has_cooler = camera_info_.has_cooler;
        const bool cooler_on = cooler_on_;
        const uint64_t cooler_on_request = cooler_on_requests_;
        lock.unlock();
        io_lock.unlock();
        transition_lock.unlock();

        // Start threads without holding the lock so they don't block the caller.
        if (has_cooler) {
            start_telemetry_thread(generation);
            if (cooler_on) {
                try {
                    start_temp_control_thread(generation);
                } catch (const std::exception& e) {
                    ALPACA_LOG_ERROR("QHY", "Failed to start temperature control: " + std::string(e.what()));
                    std::lock_guard<std::mutex> state_lock(mutex_);
                    if (generation == connection_generation_ && cooler_on_request == cooler_on_requests_) {
                        cooler_on_ = false;  // as for CoolerOn=true: a refused start regulates nothing
                    }
                }
            }
        }
    }

public:
    // ── CameraDriver ─────────────────────────────────────────────────────────

    int get_bayer_offset_x() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not applicable to monochrome sensor",
                                  AlpacaError::PropertyNotImplemented);
        }
        // BayerOffsetX is the X offset of the Red pixel from (0,0).
        // BAYER_GB=1: GBRG → R at (0,1) → X=0
        // BAYER_GR=2: GRBG → R at (1,0) → X=1
        // BAYER_BG=3: BGGR → R at (1,1) → X=1
        // BAYER_RG=4: RGGB → R at (0,0) → X=0
        uint32_t p = camera_info_.bayer_pattern;
        return (p == 2 || p == 3) ? 1 : 0;
    }

    int get_bayer_offset_y() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not applicable to monochrome sensor",
                                  AlpacaError::PropertyNotImplemented);
        }
        // BayerOffsetY is the Y offset of the Red pixel from (0,0).
        // BAYER_GB=1: GBRG → R at (0,1) → Y=1
        // BAYER_GR=2: GRBG → R at (1,0) → Y=0
        // BAYER_BG=3: BGGR → R at (1,1) → Y=1
        // BAYER_RG=4: RGGB → R at (0,0) → Y=0
        uint32_t p = camera_info_.bayer_pattern;
        return (p == 1 || p == 3) ? 1 : 0;
    }

    int get_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_x_;
    }

    void set_bin_x(int bin_x) override {
        set_bin_locked(bin_x, bin_x);
    }

    int get_bin_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_y_;
    }

    void set_bin_y(int bin_y) override {
        set_bin_locked(bin_y, bin_y);
    }

    // Decision 0020: the session serves the state and runs the watchdog (a GetQHYCCDSingleFrame that hangs past the
    // exposure plus the margin fails the exposure; CameraState then reads Idle and the failure is raised by
    // ImageReady and ImageArray until the next StartExposure or a reconnect, rule 3).
    CameraState get_camera_state() const override {
        ensure_connected();
        return session_.camera_state();
    }

    int get_camera_x_size() const override {
        // Caches are cleared on disconnect; NotConnected rather than serving a
        // previous camera's dimensions (same for the caps getters below).
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return camera_info_valid_ ? static_cast<int>(camera_info_.max_width) : 0;
    }

    int get_camera_y_size() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return camera_info_valid_ ? static_cast<int>(camera_info_.max_height) : 0;
    }

    bool get_can_abort_exposure() const override {
        return true;
    }

    bool get_can_asymmetric_bin() const override {
        return false;
    }

    bool get_can_fast_readout() const override {
        return false;
    }

    bool get_can_get_cooler_power() const override {
        // QHY cooler power telemetry (CURPWM) has been observed to stall the
        // SDK on some platforms/cameras. To guarantee that Alpaca requests do
        // not hang, we currently do not advertise this capability.
        return false;
    }

    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_st4_port;
    }

    bool get_can_set_ccd_temperature() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_cooler;
    }

    bool get_can_stop_exposure() const override {
        // StopExposure would have to hand back the frame acquired so far, but
        // the QHY single-frame mode only delivers a frame after the full
        // integration, so cancelling discards it. AbortExposure is the
        // supported way to end an exposure early.
        return false;
    }

    double get_ccd_temperature() const override {
        ALPACA_LOG_TRACE("QHY", "get_ccd_temperature entry");
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!camera_info_valid_ || !camera_info_.has_cooler) {
            ALPACA_LOG_TRACE("QHY", "get_ccd_temperature exit (no cooler)");
            throw AlpacaException("This QHY camera has no readable CCD temperature sensor",
                                  AlpacaError::PropertyNotImplemented);
        }
        // The value comes from the telemetry thread's last good CURTEMP read.
        // Never substitute the setpoint or a fixed number for a missing reading.
        if (telemetry_temp_valid_) {
            ALPACA_LOG_TRACE("QHY", "get_ccd_temperature exit (telemetry)");
            return telemetry_ccd_temp_c_;
        }
        if (telemetry_temp_unsupported_) {
            throw AlpacaException("This QHY camera does not report a CCD temperature",
                                  AlpacaError::PropertyNotImplemented);
        }
        throw AlpacaException("The CCD temperature has not been read from the camera yet",
                              AlpacaError::InvalidOperation);
    }

    bool get_cooler_on() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!camera_info_valid_ || !camera_info_.has_cooler) {
            throw AlpacaException("This QHY camera has no cooler", AlpacaError::PropertyNotImplemented);
        }
        return cooler_on_;
    }

    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (cooler_on) {
            uint64_t generation;
            uint64_t request;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ensure_connected();
                if (camera_info_valid_ && !camera_info_.has_cooler) {
                    throw AlpacaException("This QHY camera has no cooler", AlpacaError::PropertyNotImplemented);
                }
                cooler_on_ = true;
                request = ++cooler_on_requests_;
                auto pending = CoolerOffState::Pending;
                cooler_off_request_->compare_exchange_strong(pending, CoolerOffState::None);
                if (disconnecting_) {
                    return;
                }
                generation = connection_generation_;
            }
            // Must not hold mutex_ here: start_temp_control_thread() locks internally.
            try {
                start_temp_control_thread(generation);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                // A newer ON reports its own start; undo only this request.
                if (generation == connection_generation_ && request == cooler_on_requests_) {
                    cooler_on_ = false;  // a refused start regulates nothing
                }
                throw;
            }
            return;
        }

        {
            std::lock_guard<std::mutex> request_lock(*cooler_off_request_mutex_);
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            if (camera_info_valid_ && !camera_info_.has_cooler) {
                throw AlpacaException("This QHY camera has no cooler", AlpacaError::PropertyNotImplemented);
            }
            cooler_on_ = false;
            // Even a coalesced OFF must stop a worker started by an intervening ON.
            stop_temp_control_locked();
            if (!cooler_off_running_->load() || cooler_off_request_->load() != CoolerOffState::Issued) {
                cooler_off_request_->store(CoolerOffState::Pending);
            }
            if (disconnecting_) {
                return;
            }
        }
        std::lock_guard<std::mutex> cooler_lock(cooler_off_lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_.load() || disconnecting_) return;  // disconnect owns the pending request
        }
        start_cooler_off_worker();
    }

    double get_cooler_power() const override {
        ensure_connected();
        // Explicitly report that cooler power is not implemented to avoid
        // higher-level clients repeatedly polling this property and triggering
        // timeouts on QHY SDK edge cases.
        throw AlpacaException("Cooler power reporting not implemented for QHY cameras",
                              AlpacaError::PropertyNotImplemented);
    }

    double get_electrons_per_adu() const override {
        ensure_connected();
        // The QHY SDK does not expose e-/ADU, so there is no honest value.
        throw AlpacaException("ElectronsPerADU is not available for QHY cameras", AlpacaError::PropertyNotImplemented);
    }

    double get_exposure_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_exposure_range_valid_locked();
        const auto& range = exposure_range_;
        if (!range.available) {
            return 3600.0; // 1 hour default
        }
        return range.max / 1'000'000.0; // microseconds → seconds
    }

    double get_exposure_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_exposure_range_valid_locked();
        return exposure_min_us_locked() / 1'000'000.0;
    }

    double get_exposure_resolution() const override {
        return 0.000001; // 1 microsecond
    }

    bool get_fast_readout() const override {
        ensure_connected();
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    void set_fast_readout(bool) override {
        ensure_connected();
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    double get_full_well_capacity() const override {
        ensure_connected();
        // TODO: QHY SDK exposes this via CAM_CurveFullWell on supported cameras
        throw AlpacaException("FullWellCapacity is not available for QHY cameras", AlpacaError::PropertyNotImplemented);
    }

    int get_gain() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return gain_state_.value.get();
    }

    void set_gain(int gain) override {
        ensure_connected();
        check_configuration_write();
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        check_configuration_write();
        const auto id = camera_id_value();
        auto range = sdk_.get_param_range(id, control::GAIN);
        if (range.available &&
            (gain < param_to_int(range.min, "GainMin") || gain > param_to_int(range.max, "GainMax"))) {
            throw AlpacaException("Gain value out of range", AlpacaError::InvalidValue);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            gain_state_.value = {};  // A throwing write may already have changed hardware.
        }
        sdk_.set_param(id, control::GAIN, static_cast<double>(gain));
        const auto applied = read_control(id, control::GAIN, "Gain");
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (disconnecting_) throw AlpacaException("Camera is disconnecting", AlpacaError::NotConnected);
        cached_gain_ = gain;
        gain_state_ = applied;
    }

    int get_gain_max() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return gain_state_.max.get();
    }

    int get_gain_min() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return gain_state_.min.get();
    }

    std::vector<std::string> get_gains() const override {
        ensure_connected();
        throw AlpacaException("Named gain list not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_has_shutter() const override {
        ALPACA_LOG_TRACE("QHY", "get_has_shutter entry");
        std::lock_guard<std::mutex> lock(mutex_);
        bool v = camera_info_valid_ && camera_info_.has_shutter;
        ALPACA_LOG_TRACE("QHY", "get_has_shutter exit");
        return v;
    }

    double get_heat_sink_temperature() const override {
        ensure_connected();
        // No heat-sink sensor is read; the CCD temperature is not a substitute.
        throw AlpacaException("HeatSinkTemperature is not available for QHY cameras",
                              AlpacaError::PropertyNotImplemented);
    }

    ImageArray get_image_array() const override {
        ensure_connected();
        // The frame the body validated and published; a kept failure raises DriverException, and no frame yet (none
        // taken, or still exposing) is asked-too-early InvalidOperation, so the request never waits for the exposure.
        return *session_.image();
    }

    std::string get_image_array_variant() const override {
        return "Int32";
    }

    bool get_image_ready() const override {
        ensure_connected();
        // A failure after the exposure started is raised here until the next StartExposure or a reconnect.
        return session_.image_ready();
    }

    bool get_is_pulse_guiding() const override {
        if (!pulse_guiding_.load()) {
            return false;
        }
        // Expiry check and clear under ONE mutex_ hold: an unlocked
        // store(false) after the check could overwrite a concurrent
        // pulse_guide's fresh flag/end-time write and falsely report
        // "not guiding" mid-pulse (Player One camera, PR #119 round 4).
        std::lock_guard<std::mutex> lock(mutex_);
        if (clock_.now() >= pulse_guiding_end_) {
            pulse_guiding_.store(false);
            return false;
        }
        return true;
    }

    double get_last_exposure_duration() const override {
        ensure_connected();
        return session_.last_exposure_duration();
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        ensure_connected();
        return session_.last_exposure_start_time();
    }

    int get_max_adu() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        int depth = (bits_ > 0) ? bits_ : static_cast<int>(camera_info_.bpp);
        if (depth <= 0) {
            return 65535;
        }
        return static_cast<int>((1ULL << depth) - 1ULL);
    }

    int get_max_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return max_bin_.get();
    }

    int get_max_bin_y() const override {
        return get_max_bin_x();
    }

    int get_num_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_x_;
    }

    void set_num_x(int num_x) override { set_roi_size_locked(num_x, std::nullopt); }

    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }

    void set_num_y(int num_y) override { set_roi_size_locked(std::nullopt, num_y); }

    int get_offset() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return offset_state_.value.get();
    }

    void set_offset(int offset) override {
        ensure_connected();
        check_configuration_write();
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        check_configuration_write();
        const auto id = camera_id_value();
        auto range = sdk_.get_param_range(id, control::OFFSET);
        if (range.available &&
            (offset < param_to_int(range.min, "OffsetMin") || offset > param_to_int(range.max, "OffsetMax"))) {
            throw AlpacaException("Offset value out of range", AlpacaError::InvalidValue);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            offset_state_.value = {};
        }
        sdk_.set_param(id, control::OFFSET, static_cast<double>(offset));
        const auto applied = read_control(id, control::OFFSET, "Offset");
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (disconnecting_) throw AlpacaException("Camera is disconnecting", AlpacaError::NotConnected);
        cached_offset_ = offset;
        offset_state_ = applied;
    }

    int get_offset_max() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return offset_state_.max.get();
    }

    int get_offset_min() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return offset_state_.min.get();
    }

    std::vector<std::string> get_offsets() const override {
        ensure_connected();
        throw AlpacaException("Named offset list not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        ensure_connected();
        return session_.percent_completed();
    }

    double get_pixel_size_x() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return camera_info_valid_ ? camera_info_.pixel_size_x_um : 0.0;
    }

    double get_pixel_size_y() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return camera_info_valid_ ? camera_info_.pixel_size_y_um : 0.0;
    }

    int get_readout_mode() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return readout_mode_;
    }

    void set_readout_mode(int mode) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            if (mode < 0 || mode >= static_cast<int>(readout_modes_.size())) {
                throw AlpacaException("Invalid readout mode index", AlpacaError::InvalidValue);
            }
        }
        check_configuration_write();
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        ensure_connected();
        if (mode < 0 || mode >= static_cast<int>(readout_modes_.size())) {
            throw AlpacaException("Invalid readout mode index", AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();
        const std::string id = camera_id_.value_or("");
        if (id.empty()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        exposure_range_valid_ = false;  // The SDK can change hardware before its re-initialization throws.
        gain_state_ = {};
        offset_state_ = {};
        max_bin_ = {};
        const auto gain = cached_gain_;
        const auto offset = cached_offset_;
        // Binning only changes under io_mutex_, which this call holds throughout.
        const int bin = bin_x_;
        auto updated_info = camera_info_;
        mode_change_active_.store(true);
        struct ModeChangeGuard {
            std::atomic<bool>& active;
            ~ModeChangeGuard() { active.store(false); }
        } mode_change_guard{mode_change_active_};
        lock.unlock();
        sdk_.set_readout_mode(id, static_cast<uint32_t>(mode));
        const auto range = read_exposure_range(id);

        // set_readout_mode() re-runs InitQHYCCD on the same handle to fix
        // "Linearity HDR"'s slow download (see its own comment). Switching
        // modes can also change Gain/Offset out from under the client
        // (confirmed on real hardware); re-push whatever was last explicitly
        // set so a switch between two otherwise-independent modes doesn't
        // quietly change exposure calibration.
        //
        // Known SDK/firmware limitation, NOT something this fix caused: on
        // real miniCam8M hardware, Gain/Offset writes silently no-op (return
        // QHYCCD_SUCCESS but the readback never changes) while readout mode
        // 1 ("Linearity HDR") is active -- confirmed with BOTH the old
        // single-InitQHYCCD call order (pre-dating this whole investigation,
        // no re-init involved at all) AND a full CloseQHYCCD+OpenQHYCCD
        // cycle (not just a second InitQHYCCD on the same handle), so it is
        // not an artifact of the re-init trick above. HDR mode likely
        // requires fixed gain/offset internally to combine its multiple
        // capture stages, and the SDK just doesn't expose that as an error.
        // This re-push is therefore a no-op while in HDR mode (same as it
        // always silently was before this driver ever touched read modes at
        // all) and only meaningfully restores Gain/Offset when switching
        // between modes where the SDK actually honors them.
        if (gain.has_value()) {
            try {
                sdk_.set_param(id, control::GAIN, static_cast<double>(*gain));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Failed to restore gain after readout mode change: " + std::string(e.what()));
            }
        }
        if (offset.has_value()) {
            try {
                sdk_.set_param(id, control::OFFSET, static_cast<double>(*offset));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Failed to restore offset after readout mode change: " + std::string(e.what()));
            }
        }

        // After changing readout mode, refresh chip info as dimensions may change
        const bool info_valid = sdk_.get_chip_info(id, updated_info);
        // Keep the client's binning (open-astro#1678): the re-initialization may
        // reset the SDK bin mode, and the ROI is in binned pixels.
        const int new_max_w = static_cast<int>(updated_info.max_width) / bin;
        const int new_max_h = static_cast<int>(updated_info.max_height) / bin;
        try {
            sdk_.set_bin_mode(id, static_cast<uint32_t>(bin), static_cast<uint32_t>(bin));
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "Failed to restore binning after readout mode change: " + std::string(e.what()));
        }
        if (info_valid) {
            try {
                sdk_.set_resolution(id, 0, 0, static_cast<uint32_t>(new_max_w), static_cast<uint32_t>(new_max_h));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Failed to reset ROI after readout mode change: " +
                                std::string(e.what()));
            }
        }
        const auto applied_gain = read_control(id, control::GAIN, "Gain");
        const auto applied_offset = read_control(id, control::OFFSET, "Offset");
        const auto max_bin = cache_read([&] { return max_supported_bin(sdk_, id); });
        lock.lock();
        ensure_connected();
        if (disconnecting_) throw AlpacaException("Camera is disconnecting", AlpacaError::NotConnected);
        gain_state_ = applied_gain;
        offset_state_ = applied_offset;
        max_bin_ = max_bin;
        readout_mode_ = mode;
        if (info_valid) {
            camera_info_ = updated_info;
            num_x_ = new_max_w;
            num_y_ = new_max_h;
            start_x_ = 0;
            start_y_ = 0;
        }
        exposure_range_ = range;
        exposure_range_valid_ = true;
    }

    std::vector<std::string> get_readout_modes() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (readout_modes_.empty()) {
            return {"Normal"};
        }
        return readout_modes_;
    }

    std::string get_sensor_name() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return camera_info_valid_ ? camera_info_.model : std::string();  // unknown: "" (ICameraV4 SensorName)
    }

    SensorType get_sensor_type() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!camera_info_valid_ || !camera_info_.is_color) {
            return SensorType::Monochrome;
        }
        return SensorType::RGGB;
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!camera_info_valid_ || !camera_info_.has_cooler) {
            throw AlpacaException("Cooler not available", AlpacaError::PropertyNotImplemented);
        }
        return target_temp_;
    }

    void set_set_ccd_temperature(double temperature) override {
        // Reject physically impossible or unreasonable setpoints before locking.
        if (temperature <= -273.15) {
            throw AlpacaException(
                "Set point " + std::to_string(temperature) + " is at or below absolute zero",
                AlpacaError::InvalidValue);
        }
        if (temperature > 60.0) {
            throw AlpacaException(
                "Set point " + std::to_string(temperature) + " exceeds maximum allowed (60°C)",
                AlpacaError::InvalidValue);
        }
        ensure_connected();
        check_configuration_write();
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (camera_info_valid_ && !camera_info_.has_cooler) {
            throw AlpacaException("Cooler not available", AlpacaError::PropertyNotImplemented);
        }
        ensure_not_exposing_locked();
        target_temp_ = temperature;
        // If cooler is on and connected, the temp control thread picks up the new target
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }

    void set_start_x(int start_x) override { set_start_pos_locked(start_x, std::nullopt); }

    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }

    void set_start_y(int start_y) override { set_start_pos_locked(std::nullopt, start_y); }

    double get_sub_exposure_duration() const override {
        ensure_connected();
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void set_sub_exposure_duration(double) override {
        ensure_connected();
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void abort_exposure() override { cancel_running_exposure(); }

    void pulse_guide(int direction, int duration) override {
        ensure_connected();
        if (!get_can_pulse_guide()) {
            throw AlpacaException("Pulse guide not supported on this camera",
                                  AlpacaError::NotImplemented);
        }
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid pulse guide direction", AlpacaError::InvalidValue);
        }
        if (duration <= 0) {
            throw AlpacaException("Invalid pulse guide duration", AlpacaError::InvalidValue);
        }
        if (duration > kDefaultMaxPulseGuideMs) {  // #2292
            throw AlpacaException("Pulse guide duration must be <= " + std::to_string(kDefaultMaxPulseGuideMs) + " ms",
                                  AlpacaError::InvalidValue);
        }

        uint32_t qhy_dir = alpaca_to_qhy_guide_direction(direction);
        static_assert(kDefaultMaxPulseGuideMs <= 65535, "the QHY SDK takes a 16-bit pulse length");
        const auto dur_ms = static_cast<uint16_t>(duration);
        std::string cam_id = camera_id_value();

        // End timestamp BEFORE the flag, both under one mutex_ hold: a reader
        // that observes the flag as true must never see a stale (epoch) end
        // time, or the self-clearing getter would clear the pulse immediately
        // (same shape as the Player One camera, PR #119).
        // One initiator at a time, so a refused pulse's hand-back below is
        // exact even when two pulses are refused together. No body takes it.
        std::lock_guard<std::mutex> initiator_lock(pulse_initiator_mutex_);
        uint64_t pulse = 0;
        std::chrono::steady_clock::time_point previous_end;
        bool previous_flag = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            previous_end = pulse_guiding_end_;
            previous_flag = pulse_guiding_.load();
            pulse_guiding_end_ = clock_.now() + std::chrono::milliseconds(duration);
            pulse_guiding_.store(true);
            pulse = ++pulse_count_;
        }

        // ControlQHYCCDGuide blocks the calling thread for the full pulse
        // duration (confirmed on real miniCam8M hardware via ConformU: a
        // 2000ms pulse blocked the HTTP handler for exactly 2000ms), which
        // blows the ASCOM STANDARD (1.0s) response-time target for an
        // initiator method. Run the SDK call in pulse_slot_ so PulseGuide
        // returns immediately; the flag then clears exactly when the physical
        // pulse completes. The destructor joins the body (issue #1736).
        // The body ignores its context on purpose: a superseded pulse is still a
        // pulse the client asked for, so it guides once the SDK call lock frees.
        try {
            pulse_slot_.start([this, cam_id, qhy_dir, dur_ms, pulse](util::OperationContext&) {
                try {
                    sdk_.guide(cam_id, qhy_dir, dur_ms);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("QHY", "PulseGuide worker failed: " + std::string(e.what()));
                } catch (...) {
                    ALPACA_LOG_WARN("QHY", "PulseGuide worker failed: non-std exception");
                }
                clear_pulse_flag(pulse);
            });
        } catch (...) {
            // Hand the flag and end time back to the newest older pulse, so a
            // stale-bound refusal (older pulses still run) does not hold the flag
            // until this pulse's end. A refusal with nothing older guiding (a
            // refused thread, a drain) puts the flag back down.
            std::lock_guard<std::mutex> lock(mutex_);
            if (pulse == pulse_count_) {
                pulse_guiding_end_ = previous_end;
                --pulse_count_;
                if (!previous_flag) {
                    pulse_guiding_.store(false);
                }
            }
            throw;
        }
    }

    void start_exposure(double duration, bool light) override {
        // #2259 canonical order (util/camera_frame_request.h): a duration invalid whatever the camera is
        // (non-finite, negative, or too large to convert to microseconds) precedes the connection check.
        util::check_exposure_duration(duration, kRequestTexts);
        ensure_connected();

        // Reject an oversized request before it queues for I/O. An in-flight mode change has invalidated its range;
        // validate that range after acquiring io_mutex_, when the transaction has finished.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_range_valid_) {
                validate_exposure_locked(duration);
            }
        }

        // Serialises the exposure setup with mode changes and configuration writes (order: I/O -> state). The body
        // never takes it, so a blocked download stays cancellable and the session start below never waits on it.
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        ExposureRequest request;
        // Decision 0020 rule 9: reserved under mutex_ with the settings snapshot, so a gain/offset/geometry/mode
        // write cannot slip in before session_.start() has published Exposing. An exposure running or starting
        // refuses the reservation with InvalidOperation (#2259 step 5): a second StartExposure no longer supersedes.
        util::CaptureReservation reservation;
        double minimum_us = 0.0;
        bool body_returning = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            if (disconnecting_) {
                throw AlpacaException("Camera is disconnecting", AlpacaError::NotConnected);
            }
            validate_exposure_locked(duration);
            minimum_us = exposure_min_us_locked();
            reservation = session_.reserve();
            request.id = camera_id_.value_or("");
            request.bin_x = static_cast<uint32_t>(bin_x_);
            request.bin_y = static_cast<uint32_t>(bin_y_);
            request.start_x = static_cast<uint32_t>(start_x_);
            request.start_y = static_cast<uint32_t>(start_y_);
            request.num_x = num_x_;
            request.num_y = num_y_;
            request.bits = static_cast<uint32_t>(bits_);
            request.readout_mode = readout_mode_;
            body_returning = session_.busy();
        }

        // A body the watchdog failed can still be inside GetQHYCCDSingleFrame, holding the SDK's per-handle call
        // mutex that every setup call below takes. Stop it first (cancel_exposure() bypasses that mutex), then
        // wait, bounded, for it to return (decision 0020 rule 5); a body the cancel does not release refuses the
        // start instead of hanging it. The wait runs with no driver lock: the body takes mutex_.
        if (body_returning) {
            cancel_sdk_exposure(request.id, "before re-arm");
            const auto deadline = clock_.now() + util::CaptureSession::kStartTimeout;
            while (session_.busy() && clock_.now() < deadline) {
                clock_.sleep_for(std::chrono::milliseconds(20));
            }
            if (session_.busy()) {
                throw AlpacaException("Camera is finishing a previous exposure; try again shortly",
                                      AlpacaError::InvalidOperation);
            }
        }

        // Apply exposure time (microseconds). A duration below ExposureMin (0, a bias frame, included) runs at the
        // minimum (#2259), never at 0 us. The setup runs here, on the caller's thread as before, so a failed write is
        // the StartExposure's own DriverException; only ExpQHYCCDSingleFrame and GetQHYCCDSingleFrame must share a
        // thread, and both run in the body.
        request.exposure_us = std::max(duration * 1'000'000.0, minimum_us);
        sdk_.set_param(request.id, control::EXPOSURE, request.exposure_us);

        // Close mechanical shutter for dark/bias frames if the camera supports it
        if (!light && sdk_.is_control_available(request.id, control::MECHANICALSHUTTER)) {
            sdk_.set_param(request.id, control::MECHANICALSHUTTER, 1.0);  // 1 = closed
        }

        // Order matches QHY's own SingleFrameMode.cpp sample (resolution
        // before bin mode) -- not confirmed load-bearing on its own, but kept
        // aligned with the only sequence QHY ships and validates.
        sdk_.set_resolution(request.id, request.start_x, request.start_y, static_cast<uint32_t>(request.num_x),
                            static_cast<uint32_t>(request.num_y));
        sdk_.set_bin_mode(request.id, request.bin_x, request.bin_y);
        sdk_.set_bits_mode(request.id, request.bits);

        // Registered with the SDK wrapper, keyed by camera_id: the paired CFW driver shares this physical handle and
        // calls open_camera() independently, and QHYSDKWrapper::open_camera() refuses a second handle while this
        // flag says the exposure may still be inside the SDK (review finding on PR #201). True from here until the
        // body returns; a start the session refuses, or a body it never runs, clears it when the body is dropped.
        auto in_sdk = std::make_shared<std::atomic<bool>>(true);
        sdk_.register_exposure_worker(request.id, in_sdk);
        auto clear_in_sdk = std::shared_ptr<std::atomic<bool>>(
            in_sdk.get(), [keep = in_sdk](std::atomic<bool>* flag) { flag->store(false); });

        // Decision 0020 rule 4: the session deadline is the exposure plus the watchdog margin; the body extends it
        // for a slow readout mode once the buffer size is known (CaptureControl::extend_deadline, never shorter).
        const double exposure_s = request.exposure_us / 1'000'000.0;
        if (session_.start(
                std::move(reservation), exposure_s,
                [this, request, clear_in_sdk](util::CaptureControl& c) { run_exposure(c, request, *clear_in_sdk); },
                std::chrono::duration_cast<std::chrono::nanoseconds>(watchdog_margin_))) {
            // An AbortExposure, or a disconnect, arrived while the start was reserved: no body ran, nothing was
            // armed (rule 9). The disconnect's is NotConnected, as when its join drops the reservation first.
            std::lock_guard<std::mutex> lock(mutex_);
            if (disconnecting_ || !connected_.load()) {
                throw AlpacaException("Camera is disconnecting", AlpacaError::NotConnected);
            }
            ALPACA_LOG_DEBUG("QHY", "StartExposure ended by an AbortExposure that arrived while it started");
        }
    }

    void stop_exposure() override {
        ensure_connected();
        throw AlpacaException("StopExposure is not supported (CanStopExposure is false); use AbortExposure",
                              AlpacaError::MethodNotImplemented);
    }

    // Test seam (#2431): see set_disconnect_stop_hook_for_testing() in the header.
    void set_disconnect_stop_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(test_hook_mutex_);
        disconnect_stop_hook_ = std::move(hook);
    }

private:
    // What one exposure body needs, snapshotted by start_exposure under mutex_ with the reservation: the body reads
    // no driver geometry, and the settings are locked until it returns.
    struct ExposureRequest {
        std::string id;
        double exposure_us{0.0};  // the applied register value, ExposureMin included
        uint32_t bin_x{1};
        uint32_t bin_y{1};
        uint32_t start_x{0};
        uint32_t start_y{0};
        int num_x{0};
        int num_y{0};
        uint32_t bits{16};
        int readout_mode{0};
    };

    // One downloaded frame and the ROI it was taken with.
    struct QhyFrame {
        std::vector<uint8_t> buffer;
        uint32_t width{};
        uint32_t height{};
        uint32_t bpp{};
        uint32_t channels{};
        int image_width{};  // NumX/NumY of the request
        int image_height{};
    };

    // cancel_exposure() deliberately takes no per-handle call mutex, so it interrupts a blocked
    // GetQHYCCDSingleFrame. A failure is logged: the caller goes on to its join either way.
    void cancel_sdk_exposure(const std::string& id, const char* when) {
        if (id.empty()) return;
        try {
            sdk_.cancel_exposure(id);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "cancel_exposure " + std::string(when) + " failed: " + std::string(e.what()));
        }
    }

    // The body must not touch the camera again: the driver stopped this exposure (AbortExposure, a disconnect) or
    // the session took it away. Caller holds mutex_.
    bool body_stopped_locked(const util::CaptureControl& c) const {
        return c.stop_reason() != util::StopReason::None || stop_token_ == c.token() || disconnecting_;
    }

    // The exposure body (decision 0020), on the session's thread. It runs the WHOLE armed sequence --
    // ExpQHYCCDSingleFrame through GetQHYCCDSingleFrame -- on one thread: QHY's SDK keeps thread-affine state for a
    // single-frame session, and GetQHYCCDSingleFrame never returns when the two calls run on different threads
    // (miniCam8M, real hardware; QHY's own SingleFrameMode.cpp sample keeps them on one thread). No driver lock is
    // held across an SDK call. A failure throws, and the session keeps its text for ImageReady and ImageArray (rule
    // 3); a failure after the driver stopped the exposure is dropped (CaptureSession::begin_stop()).
    void run_exposure(util::CaptureControl& c, const ExposureRequest& request, std::atomic<bool>& in_sdk) {
        struct InSdk {
            std::atomic<bool>& flag;
            ~InSdk() { flag.store(false); }
        } in_sdk_guard{in_sdk};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (body_stopped_locked(c)) return;
        }

        // Host trigger boundary (LastExposureStartTime, the PercentCompleted base), not setup, settle or download
        // time; SDK latency is unmeasured (issue #1477).
        c.exposure_started();
        ALPACA_LOG_DEBUG("QHY", "exposure body: calling start_single_frame...");
        bool read_directly = false;
        try {
            read_directly = sdk_.start_single_frame(request.id);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "start_single_frame failed: " + std::string(e.what()));
            throw AlpacaException("The camera could not start the exposure", AlpacaError::DriverException);
        }
        // Rule 5: a stop that landed before the arm found nothing to cancel; this body stops what it started.
        const auto stop_if_stopped = [&] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!body_stopped_locked(c)) return false;
            }
            cancel_sdk_exposure(request.id, "after a stop");
            return true;
        };
        if (stop_if_stopped()) return;

        // Per QHY's own SingleFrameMode.cpp sample: when ExpQHYCCDSingleFrame does NOT return QHYCCD_READ_DIRECTLY,
        // the sample sleeps 1s before calling GetQHYCCDSingleFrame. The camera firmware apparently needs this settle
        // time to finish transitioning into a readable state. Rule 4: the wait runs on the task clock and ends early
        // when the body must stop.
        if (!read_directly) {
            ALPACA_LOG_DEBUG("QHY", "exposure body: settling 1s before GetMemLength/GetSingleFrame...");
            if (!c.wait_for(std::chrono::seconds(1))) {
                cancel_sdk_exposure(request.id, "after a stopped settle");
                return;
            }
        }

        // Fetched here (after Exp, like the sample), not before arming: matches the one sequence QHY ships and
        // validates.
        uint32_t mem_length = 0;
        try {
            mem_length = sdk_.get_mem_length(request.id);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "get_mem_length failed: " + std::string(e.what()));
        }
        if (mem_length == 0) {
            // ExpQHYCCDSingleFrame DID succeed to get here -- the SDK is armed with nothing left to read it back out.
            // Cancel so this failure does not leave an armed-but-abandoned exposure behind (review finding on PR
            // #201). No later exposure can be armed on this handle yet: a start waits for this body to return.
            cancel_sdk_exposure(request.id, "after a get_mem_length failure");
            throw AlpacaException("The camera reported no frame buffer for the exposure", AlpacaError::DriverException);
        }
        std::vector<uint8_t> buffer(mem_length, 0);

        // Extend the watchdog from the ACTUAL buffer size, now that it is known: the deadline set at start covers the
        // exposure plus a flat margin, which assumes every readout mode downloads at the same rate. It does not:
        // confirmed on real miniCam8M hardware that a 71MB "Linearity HDR" frame can take ~64s through
        // GetQHYCCDSingleFrame regardless of USBTRAFFIC pacing, while a 36MB "Full Resolution" frame completes in a
        // few seconds. 500,000 B/s is roughly half the ~1.1 MB/s measured for HDR -- a floor, not a target -- and
        // extend_deadline() only ever moves the deadline later (#1736). Gated on a readout mode other than 0: index
        // 0 is the SDK's default, fastest mode on every QHY camera examined and has never shown this behaviour, and
        // its 36MB buffer would compute an ~87s floor that lets a genuine hang go undetected longer than the flat
        // margin (review finding on PR #201).
        if (request.readout_mode != 0) {
            constexpr double kMinTransferBytesPerSecond = 500'000.0;
            const auto min_transfer = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(static_cast<double>(mem_length) / kMinTransferBytesPerSecond));
            if (!c.extend_deadline(min_transfer + std::chrono::seconds(15))) {
                cancel_sdk_exposure(request.id, "after a lost exposure");
                return;
            }
        }

        ALPACA_LOG_DEBUG("QHY", "exposure body: calling get_single_frame...");
        QhyFrame frame;
        bool ok = false;
        // Caught like every other SDK call in the body (#2292): the wrapper throws NotConnected once the handle is
        // closed; the session keeps its text as the failure.
        std::string frame_error;
        try {
            ok = sdk_.get_single_frame(request.id, buffer.data(), frame.width, frame.height, frame.bpp, frame.channels);
        } catch (const std::exception& e) {
            frame_error = e.what();
            ALPACA_LOG_WARN("QHY", "get_single_frame failed: " + frame_error);
        }
        ALPACA_LOG_DEBUG("QHY", "exposure body: get_single_frame returned ok=" + std::string(ok ? "true" : "false"));
        if (!ok) {
            throw AlpacaException(frame_error.empty() ? "The camera failed to deliver the frame" : frame_error,
                                  AlpacaError::DriverException);
        }
        if (const std::string invalid = invalid_frame_reason(frame, buffer.size()); !invalid.empty()) {
            throw AlpacaException(invalid, AlpacaError::DriverException);
        }
        frame.buffer = std::move(buffer);
        // The ROI is the request's: settings are locked while the body runs, and a later NumX/NumY change cannot
        // reshape this frame.
        frame.image_width = request.num_x;
        frame.image_height = request.num_y;
        ImageArray expected_shape;
        expected_shape.width = request.num_x;
        expected_shape.height = request.num_y;
        expected_shape.rank = frame.channels == 3 ? 3 : 2;
        alpacacore::util::validate_image_shape(expected_shape);
        auto image = std::make_shared<ImageArray>(build_image_array(frame));
        alpacacore::util::validate_image_array(*image);

        // Published under mutex_ with the stop check, so an AbortExposure or a disconnect that stopped this exposure
        // never sees its late frame served (a late successful SDK return after AbortExposure is not an accepted
        // image). The session mutex is a leaf under mutex_.
        std::lock_guard<std::mutex> lock(mutex_);
        if (body_stopped_locked(c)) return;
        (void)c.image_ready(std::move(image));
    }

    // Why a frame the SDK reported is unusable, or "" when it is usable.
    static std::string invalid_frame_reason(const QhyFrame& frame, std::size_t buffer_size) {
        const uint32_t w = frame.width;
        const uint32_t h = frame.height;
        const uint32_t bpp = frame.bpp;
        const uint32_t channels = frame.channels;
        if (w == 0 || h == 0) {
            return "Camera returned invalid image data: frame dimensions must be positive";
        }
        if ((bpp != 8 && bpp != 16) || (channels != 1 && channels != 3) || (channels == 3 && bpp != 8)) {
            return "Camera returned invalid image data: unsupported frame format";
        }
        if (static_cast<std::size_t>(w) > std::numeric_limits<std::size_t>::max() / h) {
            return "Camera returned invalid image data: frame dimensions overflow the buffer size";
        }
        const std::size_t pixels = static_cast<std::size_t>(w) * h;
        const std::size_t samples_per_pixel = static_cast<std::size_t>(channels);
        const std::size_t bytes_per_sample = bpp / 8;
        constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
        if (pixels > kMaxSize / samples_per_pixel || pixels * samples_per_pixel > kMaxSize / bytes_per_sample) {
            return "Camera returned invalid image data: frame dimensions overflow the buffer size";
        }
        if (pixels * samples_per_pixel * bytes_per_sample > buffer_size) {
            return "Camera returned invalid image data: frame exceeds reported buffer capacity";
        }
        return {};
    }

    // The disconnect's and the destructor's stop, under mutex_: ends a StartExposure that holds its reservation
    // and has not published Exposing the way an AbortExposure does (rule 9), so its start() runs no body, or marks
    // the running exposure stopping and records its token. Returns that token, 0 when none runs. The abort is
    // recorded first: StartExposure's session_.start() does not take mutex_, so it can publish Exposing between
    // the two calls, and only this order then finds it. A start in that gap with a begin_stop() first got no stop
    // and no recorded abort; its body met disconnecting_ and returned without an image, and the session kept "the
    // exposure ended without an image" (DriverException from ImageReady and ImageArray) until
    // disconnect_and_join() cleared it (#2431). No new reservation can come in between: reserve() runs under
    // mutex_, which the caller holds.
    std::uint64_t stop_or_abort_exposure_locked() {
        if (session_.record_abort()) {
            return 0;
        }
        const std::uint64_t token = session_.begin_stop();
        if (token != 0) {
            stop_token_ = token;
        }
        return token;
    }

    // Test seam: runs in the disconnect between its stop and disconnect_and_join(), with no state, I/O or session
    // lock held (only transition_mutex_). Empty in production.
    void run_disconnect_stop_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(test_hook_mutex_);
            hook = disconnect_stop_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // AbortExposure (decision 0020 rule 5): stops the SDK exposure, then ends the session's exposure and joins its
    // body. Its worst case is one whole GetQHYCCDSingleFrame call, when cancel_exposure() does not interrupt it (as
    // ZWO's abort waits for one SDK call). A finished frame survives; a kept failure stays until the next
    // StartExposure or a reconnect (rule 3). An abort that meets a start still reserved is recorded and ends that
    // start (rule 9). A body the watchdog already failed that is still inside the SDK is stopped, not joined: the
    // next StartExposure waits for it.
    void cancel_running_exposure() {
        ensure_connected();
        std::string id;
        std::uint64_t token = 0;
        bool body_returning = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            // Recorded before the state is read, as on ZWO and ToupTek: a start that publishes Exposing between
            // a begin_stop() that found nothing and the record_abort() that came after it made the abort a no-op,
            // and the exposure ran on (#2431 review).
            if (session_.record_abort()) return;
            token = session_.begin_stop();
            if (token == 0) {
                body_returning = session_.busy();
            } else {
                stop_token_ = token;
            }
            id = camera_id_.value_or("");
        }
        if (token == 0 && !body_returning) return;
        cancel_sdk_exposure(id, "on AbortExposure");
        if (token != 0) {
            session_.abort();
        }
    }

    // ── Members ──────────────────────────────────────────────────────────────

    // The SDK seam (issue #321). Every body (the exposure in session_, the slots below) is joined, and reaches the
    // SDK through `sdk_`. The SDK object outlives every driver built on it -- the singleton is process-scoped, and a
    // test fake must be declared before the driver it feeds.
    QHYSDK& sdk_;
    // Declared before every member that is built from it (the slots and the session below).
    // The cooler-off body captures &clock_ by value.
    util::TaskClock& clock_;
    const std::chrono::seconds watchdog_margin_;
    // Test-only (issue #510): see QHYWorkerStartHook in the header. Empty in
    // production, and never reassigned after construction.
    const QHYWorkerStartHook on_worker_start_;
    int device_number_;
    std::optional<std::string> camera_id_;
    std::optional<int> camera_index_;

    QHYCameraInfo camera_info_;
    bool camera_info_valid_;

    std::vector<std::string> readout_modes_;
    int readout_mode_;
    // Last value the client explicitly set via set_gain()/set_offset().
    // set_readout_mode() re-pushes these after its re-InitQHYCCD() call,
    // which resets Gain/Offset to hardware defaults (review finding on
    // PR #201, confirmed on real miniCam8M hardware: Gain 77->9, Offset
    // 55->100 after a plain mode switch with no error or indication).
    // Unset (nullopt) until the client sets one explicitly -- with nothing
    // cached there's nothing meaningful to restore, so re-init's default is
    // left alone exactly as before this fix.
    std::optional<int> cached_gain_;
    std::optional<int> cached_offset_;
    ControlCache gain_state_, offset_state_;
    CachedInt max_bin_;

    int bin_x_;
    int bin_y_;
    int num_x_;
    int num_y_;
    int start_x_;
    int start_y_;
    int bits_; // active bit depth (8 or 16)

    double target_temp_;
    bool cooler_on_;

    std::atomic<bool> connected_;
    // Foreground order: transition -> I/O -> state -> session (a leaf, decision 0020 rule 6).
    // Bodies use only the wrapper's shared handle/call mutex for I/O; they must
    // never keep a driver-owned lock across an uncancellable SDK call.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    uint64_t connection_generation_{0};  // state lock; rejects obsolete worker-start tails
    bool disconnecting_{false};

    QHYControlRange exposure_range_{};  // Refreshed outside acquisitions; validation must not queue behind download.
    bool exposure_range_valid_{false};
    std::atomic<bool> mode_change_active_{false};  // Polling pauses only for the transaction, not a failed range.
    // The session token of the exposure an AbortExposure or a disconnect stopped (CaptureSession::begin_stop()), so
    // its body publishes nothing after the stop. State lock.
    std::uint64_t stop_token_{0};
    mutable std::mutex test_hook_mutex_;
    std::function<void()> disconnect_stop_hook_;  // test-only, guarded by test_hook_mutex_

    // Temperature regulation (temp_slot_ below). temp_active_ (state lock) is
    // true from a start until the next OFF or disconnect, so a repeated ON does
    // not supersede a live body. temp_calls_in_flight_ counts bodies inside
    // ControlQHYCCDTemp, current or superseded; the OFF body writes PWM zero only
    // at zero, so a late PID call cannot follow it.
    bool temp_active_{false};
    uint64_t cooler_on_requests_{0};  // state lock; every accepted CoolerOn=true bumps it
    std::mutex slot_start_mutex_;     // temperature and telemetry starts; before mutex_
    std::shared_ptr<std::atomic<int>> temp_calls_in_flight_{std::make_shared<std::atomic<int>>(0)};

    // Cooler-off body (cooler_off_slot_ below). cooler_off_lifecycle_mutex_
    // serialises its start against disconnect's bounded wait.
    std::mutex cooler_off_lifecycle_mutex_;
    // True until the retained action is released; request state decides whether
    // a repeated OFF coalesces with this worker or needs another PWM-zero attempt.
    // shared_ptr, not a plain member: the body captures no driver state, and
    // the wrapper's prepare_cooler_off() action shares this flag.
    std::shared_ptr<std::atomic<bool>> cooler_off_running_{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<bool>> cooler_off_warned_{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<CoolerOffState>> cooler_off_request_{
        std::make_shared<std::atomic<CoolerOffState>>(CoolerOffState::None)};
    // OFF acceptance and a cancelled worker's claim/retirement are indivisible.
    std::shared_ptr<std::mutex> cooler_off_request_mutex_{std::make_shared<std::mutex>()};

    // Telemetry (non-blocking cached temperature / cooler power), polled by telemetry_slot_.
    double telemetry_ccd_temp_c_{0.0};
    double telemetry_cooler_power_{0.0}; // percentage 0.0–100.0
    bool telemetry_temp_valid_{false};
    // CURTEMP was probed and the SDK says the camera has no such control.
    bool telemetry_temp_unsupported_{false};
    bool telemetry_power_valid_{false};

    // Pulse guide. The body clears the flag only for its own pulse
    // (pulse_count_, state lock); the getter self-clears from the end time.
    mutable std::atomic<bool> pulse_guiding_{false};  // the const getter self-clears it
    std::chrono::steady_clock::time_point pulse_guiding_end_;
    uint64_t pulse_count_{0};
    std::mutex pulse_initiator_mutex_;  // pulse_guide() callers; before mutex_

    // Issue #1736 (decision 0006): one slot per background body; the exposure
    // runs in session_. Declared after every member a body uses; the destructor
    // joins them explicitly before any member dies.
    util::OperationGeneration temp_generation_;
    util::OperationGeneration telemetry_generation_;
    util::OperationGeneration cooler_off_generation_;
    util::OperationGeneration pulse_generation_;
    util::AsyncOperation temp_slot_{"qhy-temp-control", temp_generation_, clock_};
    util::AsyncOperation telemetry_slot_{"qhy-telemetry", telemetry_generation_, clock_};
    util::AsyncOperation cooler_off_slot_{"qhy-cooler-off", cooler_off_generation_, clock_};
    util::AsyncOperation pulse_slot_{"qhy-pulse-guide", pulse_generation_, clock_};
    // The exposure (decision 0020, #1736): its state, watchdog, kept failure and LastExposure*, with the body in the
    // session's slot. Declared last: destroyed first, joining the body before any member it uses dies.
    util::CaptureSession session_{"QHY", clock_,
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(watchdog_margin_)};

    // ── Helpers ──────────────────────────────────────────────────────────────

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    void ensure_exposure_range_valid_locked() const {
        if (!exposure_range_valid_) {
            throw AlpacaException(
                "Exposure configuration is unknown after a failed readout mode change; "
                "set ReadoutMode again or reconnect",
                AlpacaError::DriverException);
        }
    }

    // ExposureMin in microseconds, and the floor StartExposure writes (#2259): the SDK range minimum, or 1 us when
    // the range is unavailable or reports 0 or less, so a 0 s bias frame never writes 0 us.
    double exposure_min_us_locked() const {
        constexpr double kDefaultMinimumUs = 1.0;
        if (!exposure_range_.available || !(exposure_range_.min > 0.0)) {
            return kDefaultMinimumUs;
        }
        return exposure_range_.min;
    }

    QHYControlRange read_exposure_range(const std::string& id) {
        const auto range = sdk_.get_param_range(id, control::EXPOSURE);
        if (!range.available && sdk_.is_control_available(id, control::EXPOSURE)) {
            throw AlpacaException("Failed to read the camera's exposure range", AlpacaError::DriverException);
        }
        return range;
    }

    // The frame, then ExposureMax. QHY keeps its own range check (vendor reason): the SDK range is a double that a
    // model may not report (3600 s then), it is invalid during a mode change, and #1736 rewrites this path.
    void validate_exposure_locked(double duration) const {
        const util::CameraFrame frame{bin_x_, bin_y_, num_x_, num_y_, start_x_, start_y_};
        if (camera_info_valid_) {
            util::check_frame_on_sensor(frame, static_cast<int>(camera_info_.max_width) / bin_x_,
                                        static_cast<int>(camera_info_.max_height) / bin_y_, kRequestTexts);
        } else if (num_x_ <= 0 || num_y_ <= 0) {
            throw AlpacaException(kRequestTexts.frame_size_not_positive, AlpacaError::InvalidValue);
        }
        ensure_exposure_range_valid_locked();
        const double maximum = exposure_range_.available ? exposure_range_.max / 1'000'000.0 : 3600.0;
        if (duration > maximum) {
            throw AlpacaException("Exposure duration exceeds ExposureMax", AlpacaError::InvalidValue);
        }
    }

    void check_configuration_write() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_not_exposing_locked();
    }

    // Reject runtime register writes (gain/offset/bin/ROI/readout/temp) while a
    // frame is integrating: the exposure body is blocked in
    // GetQHYCCDSingleFrame holding no lock, so a mid-exposure write would race
    // the live integration and corrupt the frame or the buffer geometry.
    // Checked under mutex_, the lock start_exposure reserves its start under
    // (decision 0020 rule 9). The session also answers for a body the watchdog
    // failed that is still in the SDK: refuse before any SDK query can queue
    // behind its call mutex. Caller must hold mutex_.
    void ensure_not_exposing_locked() const {
        if (disconnecting_) {
            throw AlpacaException("Cannot change camera settings while disconnecting", AlpacaError::InvalidOperation);
        }
        session_.ensure_settings_unlocked();
    }

    std::string camera_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        return camera_id_.value();
    }

    QHYCameraInfo resolve_camera() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (camera_index_.has_value() && !camera_id_.has_value()) {
            const int index = camera_index_.value();
            lock.unlock();
            auto cameras = sdk_.enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No QHY cameras detected", AlpacaError::NotConnected);
            }
            if (index < 0 || index >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range: " + std::to_string(index),
                                      AlpacaError::InvalidValue);
            }
            const auto& info = cameras[static_cast<std::size_t>(index)];
            ALPACA_LOG_INFO("QHY", "Resolved camera index " + std::to_string(index) +
                            " → ID: " + info.camera_id);
            return info;
        }
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not specified", AlpacaError::InvalidValue);
        }
        QHYCameraInfo info{};
        info.camera_id = camera_id_.value();
        info.model = camera_info_.model;
        return info;
    }

    std::vector<std::string> load_readout_modes(const std::string& id) {
        std::vector<std::string> modes;
        try {
            uint32_t num = sdk_.get_num_readout_modes(id);
            for (uint32_t i = 0; i < num; ++i) {
                modes.push_back(sdk_.get_readout_mode_name(id, i));
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("QHY", "Readout mode enumeration failed: " + std::string(e.what()));
        }
        if (modes.empty()) {
            modes.push_back("Normal");
        }
        return modes;
    }

    // The exposure state itself lives in session_; disconnect_and_join() clears it.
    void reset_exposure_state_locked() {
        // A disconnect racing an in-flight pulse must not leave
        // IsPulseGuiding=true for a freshly reconnected client.
        pulse_guiding_.store(false);
        pulse_guiding_end_ = {};
    }

    // Caller holds cooler_off_lifecycle_mutex_; the body never accesses the driver.
    void start_cooler_off_worker() {
        if (cooler_off_running_->load()) return;
        std::string id;
        std::shared_ptr<std::atomic<bool>> warned;
        auto running = std::make_shared<std::atomic<bool>>(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cooler_off_request_->load() != CoolerOffState::Pending) return;
            id = camera_id_.value_or("");
            stop_temp_control_locked();
            warned = cooler_off_warned_;
            cooler_off_running_ = running;
        }
        try {
            auto action = sdk_.prepare_cooler_off(id, running);
            // Not cancellable: an accepted OFF owns its PWM-zero attempt, so the
            // body ignores its context and the destructor waits for it.
            cooler_off_slot_.start([action = std::move(action), running, temp_calls = temp_calls_in_flight_, warned,
                                    request = cooler_off_request_, request_mutex = cooler_off_request_mutex_,
                                    hook = on_worker_start_, clock = &clock_](util::OperationContext&) mutable {
                // Declared before the guard: a failed claim releases the action
                // and clears running before another OFF can publish Pending.
                std::unique_lock<std::mutex> request_lock(*request_mutex, std::defer_lock);
                struct RunningGuard {
                    std::function<void()> action;
                    std::shared_ptr<std::atomic<bool>> flag;
                    ~RunningGuard() {
                        action = {};  // Release the physical handle before allowing a reopen.
                        flag->store(false);
                    }
                } guard{std::move(action), running};
                try {
                    if (hook) hook(QHYWorker::CoolerOff);
                    // shortcut: a permanently wedged PID retains this handle; use cancellation if QHY adds it.
                    while (temp_calls->load() > 0) clock->sleep_for(std::chrono::milliseconds(20));
                    auto pending = CoolerOffState::Pending;
                    request_lock.lock();
                    if (!request->compare_exchange_strong(pending, CoolerOffState::Issued)) return;
                    request_lock.unlock();
                    const auto started = clock->now();
                    ALPACA_LOG_INFO("QHY", "Calling SetQHYCCDParam(MANULPWM,0)...");
                    try {
                        guard.action();
                        const auto ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(clock->now() - started).count();
                        ALPACA_LOG_INFO("QHY",
                                        "SetQHYCCDParam(MANULPWM,0) returned OK after " + std::to_string(ms) + "ms");
                    } catch (const std::exception& e) {
                        logging::log(warned->exchange(true) ? logging::LogLevel::Info : logging::LogLevel::Warn, "QHY",
                                     "SetQHYCCDParam(MANULPWM,0) failed: " + std::string(e.what()));
                    } catch (...) {
                        logging::log(warned->exchange(true) ? logging::LogLevel::Info : logging::LogLevel::Warn, "QHY",
                                     "SetQHYCCDParam(MANULPWM,0) failed: unknown exception");
                    }
                } catch (const std::exception& e) {
                    ALPACA_LOG_ERROR("QHY", "Cooler OFF cleanup failed: " + std::string(e.what()));
                } catch (...) {
                    ALPACA_LOG_ERROR("QHY", "Cooler OFF cleanup failed: unknown exception");
                }
            });
        } catch (...) {
            running->store(false);
            throw;
        }
    }

    // Bounded wait for OFF cleanup; never joins or detaches (issue #1736).
    void wait_for_cooler_off(std::chrono::steady_clock::time_point deadline) {
        std::lock_guard<std::mutex> cooler_lock(cooler_off_lifecycle_mutex_);
        while (cooler_off_running_->load() && clock_.now() < deadline) {
            clock_.sleep_for(std::chrono::milliseconds(20));
        }
        if (cooler_off_running_->load()) {
            ALPACA_LOG_WARN("QHY", "cooler-off worker still running at reap deadline; it finishes on its own");
        }
    }

    // Bounded wait for a cancelled polling body. A body still inside an SDK call
    // with no timeout stays cancelled; the destructor joins it.
    void wait_for_body(const util::AsyncOperation& slot, const char* label,
                       std::chrono::steady_clock::time_point deadline) {
        while (slot.running() && clock_.now() < deadline) {
            clock_.sleep_for(std::chrono::milliseconds(20));
        }
        if (slot.running()) {
            ALPACA_LOG_WARN("QHY", std::string(label) + " thread still running at reap deadline; it stays cancelled");
        }
    }

    // State lock held. The body sees the cancel at its next check.
    void stop_temp_control_locked() {
        temp_active_ = false;
        temp_slot_.cancel();
    }

    // The newest pulse owns the flag: when it finishes it clears the flag, even
    // while older pulses still queue on the SDK call lock.
    void clear_pulse_flag(uint64_t pulse) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pulse == pulse_count_) {
            pulse_guiding_.store(false);
        }
    }

    void stop_all_threads() {
        // Wake a body parked in GetQHYCCDSingleFrame before the join, or the join waits for the whole remaining
        // exposure (decision 0020 rule 5), then close the session and join the body with no time bound.
        std::string exposure_id;
        bool stop_exposure = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::uint64_t token = stop_or_abort_exposure_locked();
            stop_exposure = token != 0 || session_.busy();
            exposure_id = camera_id_.value_or("");
        }
        if (stop_exposure) {
            cancel_sdk_exposure(exposure_id, "during shutdown");
        }
        session_.disconnect_and_join();
        // After the exposure: a queued pulse or poll waits behind its download on
        // the SDK call lock. Without mutex_: the bodies take it. No time bound (#1736).
        temp_slot_.cancel_all_and_join();
        telemetry_slot_.cancel_all_and_join();
        pulse_slot_.cancel_all_and_join();
    }

    // Starts regulation in temp_slot_. Call without mutex_. Never waits for a
    // stopped body (issue #1735 item 1): start() supersedes it. Throws when the
    // slot refuses the start.
    void start_temp_control_thread(uint64_t generation) {
        // Check and start are one step: a starter that passed an old generation's
        // check cannot replace a newer connection's body (#510). Order: this -> mutex_.
        std::lock_guard<std::mutex> start_lock(slot_start_mutex_);
        std::string id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_.load() || disconnecting_ || !cooler_on_ || temp_active_ ||
                generation != connection_generation_) {
                return;
            }
            id = camera_id_.value_or("");
            if (id.empty()) {
                return;
            }
            temp_active_ = true;
        }
        if (on_worker_start_) {
            on_worker_start_(QHYWorker::TempControl);
        }
        try {
            temp_slot_.start([this, id, generation](util::OperationContext& ctx) {
                do {
                    double target = 0.0;
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        // A stop or a newer start that lands before the slot holds
                        // this body is caught here (issue #510).
                        if (ctx.stop_reason() != util::StopReason::None || !cooler_on_ || !connected_.load() ||
                            disconnecting_ || generation != connection_generation_) {
                            return;
                        }
                        target = target_temp_;
                        // Skip this cycle's SDK call while a frame is downloading:
                        // ControlQHYCCDTemp and the exposure body's
                        // GetQHYCCDSingleFrame share the SAME physical USB handle, and
                        // the QHY SDK is not safe against concurrent calls on one
                        // handle from two threads -- a temp call landing mid-transfer
                        // wedges GetQHYCCDSingleFrame forever (no SDK-side timeout).
                        // New regulation also waits for an issued OFF.
                        if (session_.busy() || mode_change_active_.load() || cooler_off_running_->load()) {
                            continue;
                        }
                        ++*temp_calls_in_flight_;
                    }
                    struct InFlight {
                        std::atomic<int>& count;
                        ~InFlight() { --count; }
                    } in_flight{*temp_calls_in_flight_};
                    try {
                        sdk_.control_temp(id, target);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_DEBUG("QHY", "Temp control error: " + std::string(e.what()));
                    }
                } while (ctx.wait_for(std::chrono::seconds(1)));
            });
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            temp_active_ = false;
            throw;
        }
    }

    // Starts the telemetry poll in telemetry_slot_. Call without mutex_.
    void start_telemetry_thread(uint64_t generation) {
        std::lock_guard<std::mutex> start_lock(slot_start_mutex_);  // see start_temp_control_thread()
        std::string id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_.load() || disconnecting_ || generation != connection_generation_) {
                return;
            }
            id = camera_id_.value_or("");
            if (id.empty()) {
                return;
            }
        }
        const auto stopped = [this, generation](util::OperationContext& ctx) {
            return ctx.stop_reason() != util::StopReason::None || !connected_.load() || disconnecting_ ||
                   generation != connection_generation_;
        };
        if (on_worker_start_) {
            on_worker_start_(QHYWorker::Telemetry);
        }
        try {
            telemetry_slot_.start([this, id, stopped](util::OperationContext& ctx) {
                do {
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        if (stopped(ctx)) {
                            return;
                        }
                        if (session_.busy() || mode_change_active_.load()) {
                            // Skip this cycle: see the matching comment in
                            // start_temp_control_thread() -- IsQHYCCDControlAvailable
                            // and GetQHYCCDParam share the exposure body's USB
                            // handle and can wedge its GetQHYCCDSingleFrame call.
                            continue;
                        }
                    }
                    bool have_temp = false;
                    bool probed = false;
                    bool unsupported = false;
                    double temp_c = 0.0;
                    try {
                        if (sdk_.is_control_available(id, control::CURTEMP)) {
                            temp_c = sdk_.get_param(id, control::CURTEMP);
                            have_temp = true;
                        } else {
                            unsupported = true;
                        }
                        probed = true;
                    } catch (const std::exception& e) {
                        ALPACA_LOG_DEBUG("QHY", "Telemetry CURTEMP read failed: " + std::string(e.what()));
                    }
                    std::lock_guard<std::mutex> lk(mutex_);
                    if (stopped(ctx)) {
                        return;
                    }
                    if (have_temp) {
                        telemetry_ccd_temp_c_ = temp_c;
                        telemetry_temp_valid_ = true;
                        telemetry_temp_unsupported_ = false;
                    } else {
                        // A read that failed or a control that is absent must
                        // not leave an old reading looking current.
                        telemetry_temp_valid_ = false;
                        if (probed) {
                            telemetry_temp_unsupported_ = unsupported;
                        }
                    }
                } while (ctx.wait_for(std::chrono::seconds(1)));
            });
        } catch (const std::exception& e) {
            ALPACA_LOG_ERROR("QHY", "Failed to start telemetry: " + std::string(e.what()));
            return;
        }
    }

    // #2259 canonical order: the static value check, the connection, the camera's supported values, then the
    // exposing check. While an exposure runs the SDK cannot be asked (the frame call holds the handle), so the
    // bound before the exposing check is the MaxBin cached at connect; the per-bin SDK query follows it.
    void set_bin_locked(int bin_x, int bin_y) {
        util::check_bin(bin_x, kRequestTexts);
        util::check_bin(bin_y, kRequestTexts);
        // QHY has no bin list: it asks the SDK per bin (below), so check_bin_supported() does not apply.
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
        }
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            if (max_bin_.value && bin_x > *max_bin_.value) {
                throw AlpacaException("Bin value not supported: " + std::to_string(bin_x), AlpacaError::InvalidValue);
            }
        }
        check_configuration_write();
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_not_exposing_locked();
        const std::string id = camera_id_.value_or("");
        const int max_w = static_cast<int>(camera_info_.max_width) / bin_x;
        const int max_h = static_cast<int>(camera_info_.max_height) / bin_y;
        lock.unlock();
        if (!bin_is_supported(sdk_, id, bin_x)) {
            throw AlpacaException("Bin value not supported: " + std::to_string(bin_x),
                                  AlpacaError::InvalidValue);
        }
        try {
            sdk_.set_bin_mode(id, static_cast<uint32_t>(bin_x), static_cast<uint32_t>(bin_y));
            sdk_.set_resolution(id, 0, 0, static_cast<uint32_t>(max_w), static_cast<uint32_t>(max_h));
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "Failed to apply binning: " + std::string(e.what()));
            return;
        }
        lock.lock();
        ensure_connected();
        bin_x_ = bin_x;
        bin_y_ = bin_y;
        num_x_ = max_w;
        num_y_ = max_h;
        start_x_ = 0;
        start_y_ = 0;
    }

    // width/height (or sx/sy) of std::nullopt means "leave that axis unchanged",
    // resolved UNDER mutex_: each public setter passes only its own axis, so a
    // concurrent setter for the other axis can no longer be clobbered by a stale
    // pre-lock get_num_x()/get_num_y() snapshot (lost-update TOCTOU).
    void set_roi_size_locked(std::optional<int> width_opt, std::optional<int> height_opt) {
        if (width_opt) util::check_frame_size(*width_opt, kRequestTexts);
        if (height_opt) util::check_frame_size(*height_opt, kRequestTexts);
        ensure_connected();
        check_configuration_write();
        // Do not bounds-check against sensor dimensions here: ConformU "Reject Bad XSize/YSize"
        // tests require that the setter accepts out-of-range values and that StartExposure
        // rejects them. Just store the value; start_exposure validates before calling the SDK.
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        const int width = width_opt.value_or(num_x_);
        const int height = height_opt.value_or(num_y_);
        util::check_frame_size(width, kRequestTexts);
        util::check_frame_size(height, kRequestTexts);
        ensure_connected();
        ensure_not_exposing_locked();
        num_x_ = width;
        num_y_ = height;
    }

    void set_start_pos_locked(std::optional<int> start_x_opt, std::optional<int> start_y_opt) {
        if (start_x_opt) util::check_frame_start(*start_x_opt, kRequestTexts);
        if (start_y_opt) util::check_frame_start(*start_y_opt, kRequestTexts);
        ensure_connected();
        check_configuration_write();
        // Do not bounds-check start+ROI against sensor dimensions here: ConformU
        // "Reject Bad XStart/YStart" tests require the setter to accept the value and
        // StartExposure to reject it. Just store; start_exposure validates before the SDK call.
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        const int start_x = start_x_opt.value_or(start_x_);
        const int start_y = start_y_opt.value_or(start_y_);
        util::check_frame_start(start_x, kRequestTexts);
        util::check_frame_start(start_y, kRequestTexts);
        ensure_connected();
        ensure_not_exposing_locked();
        start_x_ = start_x;
        start_y_ = start_y;
    }

    // The ImageArray of one downloaded frame (the body builds it before it publishes).
    static ImageArray build_image_array(const QhyFrame& frame) {
        ImageArray image;
        image.width = frame.image_width;
        image.height = frame.image_height;

        if (image.width <= 0 || image.height <= 0 || frame.buffer.empty()) {
            alpacacore::util::throw_invalid_camera_image("invalid QHY dimensions or empty frame buffer");
        }

        const uint32_t eff_w = frame.width > 0 ? frame.width : static_cast<uint32_t>(frame.image_width);
        const uint32_t eff_h = frame.height > 0 ? frame.height : static_cast<uint32_t>(frame.image_height);
        const uint32_t bpp = frame.bpp;
        const uint32_t ch = frame.channels;

        if (eff_w == 0 || eff_h == 0 || (bpp != 8 && bpp != 16) || (ch != 1 && ch != 3) || (ch == 3 && bpp != 8)) {
            alpacacore::util::throw_invalid_camera_image("unsupported QHY frame layout");
        }
        constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
        if (static_cast<std::size_t>(eff_w) > kMaxSize / eff_h) {
            alpacacore::util::throw_invalid_camera_image("QHY frame dimensions overflow the buffer size");
        }
        const std::size_t frame_pixels = static_cast<std::size_t>(eff_w) * eff_h;
        const std::size_t samples_per_pixel = ch;
        const std::size_t bytes_per_sample = bpp / 8;
        if (frame_pixels > kMaxSize / samples_per_pixel ||
            frame_pixels * samples_per_pixel > kMaxSize / bytes_per_sample ||
            frame_pixels * samples_per_pixel * bytes_per_sample > frame.buffer.size()) {
            alpacacore::util::throw_invalid_camera_image("QHY frame buffer is shorter than its dimensions");
        }

        // Color (3-channel) output
        if (ch == 3) {
            image.rank = 3;
            image.data.resize(static_cast<std::size_t>(image.width) *
                              static_cast<std::size_t>(image.height) * 3);
            const std::size_t buf_stride = static_cast<std::size_t>(eff_w) * 3;
            const std::size_t out_stride = static_cast<std::size_t>(image.width) * 3;
            for (int row = 0; row < image.height; ++row) {
                if (static_cast<uint32_t>(row) >= eff_h) break;
                const std::size_t src = static_cast<std::size_t>(row) * buf_stride;
                const std::size_t dst = static_cast<std::size_t>(row) * out_stride;
                const std::size_t copy = std::min(buf_stride, out_stride);
                for (std::size_t i = 0; i < copy && src + i < frame.buffer.size(); ++i) {
                    image.data[dst + i] = frame.buffer[src + i];
                }
            }
            return image;
        }

        // Monochrome output
        image.rank = 2;
        const std::size_t pixel_count = static_cast<std::size_t>(image.width) *
                                        static_cast<std::size_t>(image.height);
        image.data.resize(pixel_count, 0);

        if (bpp == 16) {
            for (int row = 0; row < image.height; ++row) {
                for (int col = 0; col < image.width; ++col) {
                    const std::size_t out_idx =
                        static_cast<std::size_t>(row) * static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(col);
                    if (static_cast<uint32_t>(row) < eff_h && static_cast<uint32_t>(col) < eff_w) {
                        const std::size_t src =
                            (static_cast<std::size_t>(row) * static_cast<std::size_t>(eff_w) +
                             static_cast<std::size_t>(col)) * 2;
                        if (src + 1 < frame.buffer.size()) {
                            // GetQHYCCDSingleFrame fills the buffer with native
                            // little-endian uint16 samples (low byte first), the same
                            // layout PHD2's cam_qhy and INDI's qhy_ccd read it as. The
                            // previous high-byte-first read swapped every pixel: a
                            // QHY5III585M frame came out as noise with ringed stars and
                            // the guider's star detector found nothing.
                            uint16_t val = static_cast<uint16_t>(frame.buffer[src]) |
                                           (static_cast<uint16_t>(frame.buffer[src + 1]) << 8);
                            image.data[out_idx] = static_cast<std::int32_t>(val);
                        }
                    }
                }
            }
        } else {
            // 8-bit
            for (int row = 0; row < image.height; ++row) {
                for (int col = 0; col < image.width; ++col) {
                    const std::size_t out_idx =
                        static_cast<std::size_t>(row) * static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(col);
                    if (static_cast<uint32_t>(row) < eff_h && static_cast<uint32_t>(col) < eff_w) {
                        const std::size_t src =
                            static_cast<std::size_t>(row) * static_cast<std::size_t>(eff_w) +
                            static_cast<std::size_t>(col);
                        if (src < frame.buffer.size()) {
                            image.data[out_idx] = static_cast<std::int32_t>(frame.buffer[src]);
                        }
                    }
                }
            }
        }
        return image;
    }
};

// ────────────────────────────────────────────────────────────────────────────
// Factory functions
// ────────────────────────────────────────────────────────────────────────────

// These default overloads resolve the singleton EAGERLY, at device-creation
// time -- before the seam, it was first touched on the driver's first SDK
// call. That is safe only because QHYSDKWrapper's constructor does no
// libqhyccd work (see Impl's constructor in qhy_sdk_wrapper.cpp) and
// InitQHYCCDResource() stays behind ensure_resource(). If that constructor
// ever gains real work, this moves a libqhyccd call onto the
// configure/management path, which on a USB-less host is the crash path this
// whole seam exists to avoid. Keep the constructor trivial, or make these
// overloads lazy -- issue #368 weighs that change, since today the constraint
// is held by this comment and nothing mechanical.
std::unique_ptr<CameraDriver> create_qhy_camera(int device_number,
                                                 const std::string& camera_id) {
    return create_qhy_camera(device_number, camera_id, QHYSDKWrapper::instance());
}

std::unique_ptr<CameraDriver> create_qhy_camera_by_index(int device_number, int camera_index) {
    return create_qhy_camera_by_index(device_number, camera_index, QHYSDKWrapper::instance());
}

std::unique_ptr<CameraDriver> create_qhy_camera(int device_number, const std::string& camera_id, QHYSDK& sdk,
                                                util::TaskClock& clock) {
    return std::make_unique<QHYCameraDriver>(device_number, camera_id, std::nullopt, sdk, QHYWorkerStartHook{},
                                             std::chrono::seconds(60), clock);
}

std::unique_ptr<CameraDriver> create_qhy_camera_by_index(int device_number, int camera_index, QHYSDK& sdk,
                                                         util::TaskClock& clock) {
    return std::make_unique<QHYCameraDriver>(device_number, std::nullopt, camera_index, sdk, QHYWorkerStartHook{},
                                             std::chrono::seconds(60), clock);
}

std::unique_ptr<CameraDriver> create_qhy_camera(int device_number, const std::string& camera_id, QHYSDK& sdk,
                                                QHYWorkerStartHook on_worker_start,
                                                std::chrono::seconds watchdog_margin, util::TaskClock& clock) {
    return std::make_unique<QHYCameraDriver>(device_number, camera_id, std::nullopt, sdk, std::move(on_worker_start),
                                             watchdog_margin, clock);
}

void set_disconnect_stop_hook_for_testing(CameraDriver& driver, std::function<void()> hook) {
    if (auto* qhy = dynamic_cast<QHYCameraDriver*>(&driver)) {
        qhy->set_disconnect_stop_hook_for_testing(std::move(hook));
    }
}

} // namespace alpacacore::vendor::qhy
