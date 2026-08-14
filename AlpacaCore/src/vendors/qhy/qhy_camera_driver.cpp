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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
bool bin_is_supported(const std::string& camera_id, int bin) {
    auto& sdk = QHYSDKWrapper::instance();
    switch (bin) {
    case 1: return sdk.is_control_available(camera_id, control::BIN1X1);
    case 2: return sdk.is_control_available(camera_id, control::BIN2X2);
    case 3: return sdk.is_control_available(camera_id, control::BIN3X3);
    case 4: return sdk.is_control_available(camera_id, control::BIN4X4);
    default: return false;
    }
}

int max_supported_bin(const std::string& camera_id) {
    for (int bin = 4; bin >= 1; --bin) {
        if (bin_is_supported(camera_id, bin)) {
            return bin;
        }
    }
    return 1;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
// Exposure state
// ────────────────────────────────────────────────────────────────────────────

enum class QHYExposureStatus {
    Idle,
    Working,
    Success,
    Failed
};

// ────────────────────────────────────────────────────────────────────────────
// QHYCameraDriver
// ────────────────────────────────────────────────────────────────────────────

class QHYCameraDriver : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    QHYCameraDriver(int device_number, std::optional<std::string> camera_id, std::optional<int> camera_index)
        : AsyncConnectable("QHY"),
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
          exposure_status_(QHYExposureStatus::Idle),
          image_ready_(false),
          last_exposure_duration_(0.0),
          last_exposure_start_{},
          last_exposure_valid_(false),
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
            return camera_info_.model;
        }
        return "QHY Camera";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Camera;
    }

    std::string get_unique_id() const override {
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
        const auto ver = QHYSDKWrapper::instance().get_sdk_version();
        return ver.empty() ? "AlpacaCore QHY Camera Driver" : "AlpacaCore QHY Camera Driver (SDK " + ver + ")";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only. DriverInfo's
    // pre-existing SDK mention is left as-is but deliberately not extended.
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = QHYSDKWrapper::instance().get_sdk_version();
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

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

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
        // Disconnection path: the temp thread acquires mutex_ on each iteration,
        // so we must join it BEFORE holding mutex_ to avoid deadlock.
        if (!connected) {
            // Best-effort, exception-safe teardown. The goal on this path is
            // "do no harm": never let SDK failures propagate to clients or
            // destructors, and always avoid deadlocks with the temp thread.
            //
            // Stop-and-join the exposure worker BEFORE the SDK close below:
            // the worker blocks in GetQHYCCDSingleFrame on the camera id, so
            // closing underneath it is a use-after-close inside libqhyccd.
            // Cancel first so the blocking read wakes and the join returns
            // promptly instead of waiting out the full exposure. Hold
            // exposure_lifecycle_mutex_ from before the join through the close
            // so a concurrent start_exposure cannot spawn a fresh worker in
            // the join→close gap (same shape as the ToupTek camera). Lock
            // order: exposure_lifecycle_mutex_ -> mutex_.
            std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
            // See the matching comment in start_exposure(): joinable() alone
            // misses a worker that a prior reap already detach()ed but which
            // may still be alive and stuck in GetQHYCCDSingleFrame -- retry
            // cancel_exposure() while exposure_thread_running_ says so.
            bool worker_may_still_be_running = exposure_thread_running_ && exposure_thread_running_->load();
            if (exposure_thread_.joinable() || worker_may_still_be_running) {
                std::string cancel_id;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cancel_id = camera_id_.value_or("");
                }
                if (!cancel_id.empty()) {
                    try {
                        QHYSDKWrapper::instance().cancel_exposure(cancel_id);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("QHY", "cancel_exposure failed during disconnect: " + std::string(e.what()));
                    }
                }
                if (exposure_thread_.joinable()) {
                    join_exposure_thread();
                }
            }
            // Join the cooler-off worker before taking mutex_ below: it takes
            // mutex_ itself and joins the temp thread.
            join_cooler_off_thread();
            std::thread temp_to_join;
            std::thread telemetry_to_join;
            std::shared_ptr<std::atomic<bool>> temp_running_to_join;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Base gate (obligation 4) BEFORE the idempotency check: a sync
                // disconnect during an in-flight connect looks idempotent (both
                // sides see disconnected) and would be silently dropped without
                // the record. This function split on `connected` above, so only
                // the disconnect-side gate belongs in this branch.
                if (record_disconnect_if_connect_in_flight(connected_.load())) {
                    return;
                }
                if (!connected_.load()) {
                    return; // already disconnected
                }
                temp_thread_stop_->store(true);
                temp_running_to_join = temp_thread_running_;
                temp_to_join = std::move(temp_thread_);
                telemetry_thread_stop_.store(true);
                telemetry_to_join = std::move(telemetry_thread_);
            }
            try {
                join_temp_thread(temp_to_join, temp_running_to_join);  // outside mutex_; bounded, may detach
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Temp thread join failed during disconnect: " + std::string(e.what()));
            }
            if (telemetry_to_join.joinable()) {
                try {
                    telemetry_to_join.join();
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("QHY", "Telemetry thread join failed during disconnect: " +
                        std::string(e.what()));
                }
            }

            std::lock_guard<std::mutex> lock(mutex_);
            // Clear driver state and cached capabilities BEFORE the SDK close
            // (AGENTS.md): a throwing close must not leave the driver
            // half-connected serving a previous camera's caps/ranges. Keep the
            // model string so the web UI device name survives a disconnect
            // (see the constructor comment).
            {
                const std::string model = camera_info_.model;
                camera_info_ = {};
                camera_info_.model = model;
                camera_info_valid_ = false;
            }
            readout_modes_.clear();
            readout_mode_ = 0;
            cached_gain_.reset();
            cached_offset_.reset();
            telemetry_temp_valid_ = false;
            telemetry_power_valid_ = false;
            reset_exposure_state_locked();
            connected_.store(false);
            auto& sdk = QHYSDKWrapper::instance();
            const std::string& id = camera_id_.value_or("");
            if (!id.empty()) {
                // Exposure worker cancel attempted above; join_exposure_thread()
                // may have joined it cleanly OR hit its 2s timeout and
                // detached a still-running zombie instead (see its own
                // comment) -- this close_camera() call proceeds either way.
                // That's intentional and safe here (the zombie holds its own
                // shared_ptr<qhyccd_handle> copy, so this doesn't touch a
                // closed handle), but it does erase this id's entry from the
                // SDK wrapper's handle map once the last owner releases it,
                // which matters for the next connect() -- see the
                // exposure_thread_running_ check there.
                try {
                    sdk.close_camera(id);
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

        auto& sdk = QHYSDKWrapper::instance();
        const std::string& id = resolve_camera_id_locked();

        // A prior disconnect's join_exposure_thread() may have hit its 2s
        // timeout and detached a wedged exposure worker instead of waiting
        // for it -- that worker can still be alive, blocked inside
        // GetQHYCCDSingleFrame on the OLD qhyccd_handle, holding its own
        // shared_ptr<qhyccd_handle> copy (which is what keeps CloseQHYCCD
        // deferred rather than a use-after-close). disconnect() calls
        // sdk.close_camera(id) unconditionally in that case, which erases
        // the handle map's entry for `id` once the last owner releases it.
        // open_camera() below only reuses a handle while a live map entry
        // survives; once it's erased, this connect() would OpenQHYCCD() a
        // SECOND, independent handle to the same physical USB device while
        // the zombie's is potentially still in flight on the first one --
        // undefined territory for the vendor SDK, and a much harder failure
        // mode to diagnose than the hang this generation-tracking exists to
        // bound in the first place (review finding on PR #201). Refuse
        // instead of risking a dual-handle open; the client can retry once
        // the zombie's blocking call eventually returns and this flag
        // clears (or the process is restarted, if the SDK call never
        // returns at all -- the same ceiling every other reap path in this
        // file already has).
        if (exposure_thread_running_ && exposure_thread_running_->load()) {
            throw AlpacaException(
                "Camera cannot reconnect while a previous exposure download is still finishing; try again shortly",
                AlpacaError::InvalidOperation);
        }

        sdk.open_camera(id);
        // Roll back the ref-counted open if any init step throws: open_camera()
        // now shares one handle per camera_id with the CFW driver via an
        // open_count, so an unmatched open is no longer self-healed by the next
        // connect — it would pin the handle (CloseQHYCCD never firing) for the
        // life of the process. Same pattern as the filter wheel's connect path.
        try {
            sdk.init_camera(id);

            // Populate full camera info
            QHYCameraInfo info{};
            info.camera_id = id;
            if (!camera_info_valid_ || camera_info_.model.empty()) {
                sdk.get_camera_model(id, info.model);
            } else {
                info.model = camera_info_.model;
            }
            if (sdk.get_chip_info(id, info)) {
                camera_info_ = info;
                camera_info_valid_ = true;
            }

            // Select bit depth (prefer 16-bit)
            bits_ = 8;
            if (sdk.is_control_available(id, control::BITS16)) {
                bits_ = 16;
            }
            sdk.set_bits_mode(id, bits_);

            // Default to full-frame 1x1 binning
            bin_x_ = 1;
            bin_y_ = 1;
            start_x_ = 0;
            start_y_ = 0;
            num_x_ = static_cast<int>(camera_info_.max_width);
            num_y_ = static_cast<int>(camera_info_.max_height);
            sdk.set_bin_mode(id, 1, 1);
            sdk.set_resolution(id, 0, 0, static_cast<uint32_t>(num_x_), static_cast<uint32_t>(num_y_));

            // Enumerate readout modes
            load_readout_modes_locked(id);

            reset_exposure_state_locked();
        } catch (...) {
            try {
                sdk.close_camera(id);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Failed to roll back camera open after connect error: " + std::string(e.what()));
            }
            throw;
        }
        connected_.store(true);

        // Start threads without holding the lock so they don't block the caller.
        if (camera_info_.has_cooler) {
            lock.unlock();
            start_telemetry_thread();
            if (cooler_on_) {
                start_temp_control_thread();
            }
        }
    }

public:
    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name),
                              AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override {
        return false;
    }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    // ── CameraDriver ─────────────────────────────────────────────────────────

    int get_bayer_offset_x() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
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

    CameraState get_camera_state() const override {
        if (!connected_.load()) {
            return CameraState::Idle;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        switch (exposure_status_) {
        case QHYExposureStatus::Working:
            // Watchdog (SVBONY/ToupTek shape): if GetQHYCCDSingleFrame hangs
            // past the exposure duration + margin, mark the exposure Failed
            // instead of reporting Exposing forever. Failed maps to Idle below
            // on the next poll; ImageArray throws for this exposure.
            if (exposure_deadline_valid_ && std::chrono::steady_clock::now() >= exposure_deadline_) {
                ALPACA_LOG_WARN("QHY", "Exposure deadline exceeded; marking exposure failed.");
                exposure_status_ = QHYExposureStatus::Failed;
                image_ready_ = false;
                exposure_deadline_valid_ = false;
                return CameraState::Idle;
            }
            return CameraState::Exposing;
        case QHYExposureStatus::Failed:
            // A failed exposure leaves the camera fully ready for the next
            // one — the failure surfaces through ImageReady staying false and
            // ImageArray throwing. A sticky Error state poisons every
            // subsequent operation (same class ConformU exposed on the ZWO
            // camera: one transient failure cascaded into 18 issues).
            return CameraState::Idle;
        case QHYExposureStatus::Idle:
        case QHYExposureStatus::Success:
        default:
            return CameraState::Idle;
        }
    }

    int get_camera_x_size() const override {
        // Caches are cleared on disconnect; NotConnected rather than serving a
        // previous camera's dimensions (same for the caps getters below).
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? static_cast<int>(camera_info_.max_width) : 0;
    }

    int get_camera_y_size() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
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
        return true;
    }

    double get_ccd_temperature() const override {
        ALPACA_LOG_TRACE("QHY", "get_ccd_temperature entry");
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.has_cooler) {
            ALPACA_LOG_TRACE("QHY", "get_ccd_temperature exit (no cooler)");
            return 0.0;
        }
        // Prefer the last good value from the telemetry thread when available.
        if (telemetry_temp_valid_) {
            ALPACA_LOG_TRACE("QHY", "get_ccd_temperature exit (telemetry)");
            return telemetry_ccd_temp_c_;
        }
        // Fallback: approximate from setpoint / ambient rather than blocking.
        if (cooler_on_) {
            ALPACA_LOG_TRACE("QHY", "get_ccd_temperature exit (fallback target)");
            return target_temp_;
        }
        ALPACA_LOG_TRACE("QHY", "get_ccd_temperature exit (fallback 20)");
        return 20.0;
    }

    bool get_cooler_on() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.has_cooler) {
            return false;
        }
        return cooler_on_;
    }

    void set_cooler_on(bool cooler_on) override {
        if (cooler_on) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (camera_info_valid_ && !camera_info_.has_cooler) {
                    throw AlpacaException("Cooler not available on this camera",
                                          AlpacaError::NotImplemented);
                }
                cooler_on_ = true;
                if (!connected_.load()) {
                    return;
                }
            }
            // Must not hold mutex_ here: start_temp_control_thread() locks internally.
            start_temp_control_thread();
            return;
        }

        // Turning cooler OFF: do not touch mutex_ on the HTTP thread. Something
        // (temp or telemetry thread, or SDK) can hold it or contend for ~10s;
        // doing the full turn-off in a background thread avoids ConformU timeouts.
        if (camera_info_valid_ && !camera_info_.has_cooler) {
            return;
        }
        // Serialise spawn vs join under cooler_off_lifecycle_mutex_; the
        // previous worker (if any) has either finished or is finishing —
        // joining it here bounds us to one worker. It is joined (or, on
        // timeout, detached — see join_cooler_off_thread()) again in
        // disconnect and the destructor.
        std::lock_guard<std::mutex> cooler_lock(cooler_off_lifecycle_mutex_);
        if (cooler_off_thread_.joinable()) {
            if (cooler_off_running_->load()) {
                // A turn-off worker is already in flight (it can block for
                // seconds joining the temp thread mid-PID-call). Joining it
                // here would stall this HTTP thread for that whole duration —
                // the exact ConformU-timeout stall the background worker
                // exists to avoid. Turn-off is idempotent: just return.
                return;
            }
            cooler_off_thread_.join();  // finished worker — instant reap
        }
        // Fresh flag per generation, not a store(true) on the shared member: a
        // prior generation's timed-out-and-detached zombie (join_cooler_off_thread)
        // keeps its own captured copy of the OLD flag, so reusing the member
        // would let this new worker's completion be masked by the zombie's
        // later RunningGuard clearing the SAME flag out from under it (review
        // finding). Also set the member only after the thread ctor succeeds:
        // if it throws (e.g. thread limit), nothing was spawned to ever clear
        // the flag, so publishing it early would wedge cooler-off forever.
        auto running_flag = std::make_shared<std::atomic<bool>>(true);
        cooler_off_thread_ = std::thread([this, running_flag]() {
            // Clear the in-flight flag on every exit path, including throws.
            // Captures the shared_ptr (not `this`): if join_cooler_off_thread()
            // times out and detaches this thread while it's still stuck inside
            // SetQHYCCDParam below, this is the ONLY thing that runs after the
            // blocking call returns, and it must not touch a driver that may
            // by then be destroyed.
            struct RunningGuard {
                std::shared_ptr<std::atomic<bool>> flag;
                ~RunningGuard() { flag->store(false); }
            } running_guard{running_flag};
            std::thread temp_to_join;
            std::shared_ptr<std::atomic<bool>> temp_running_to_join;
            std::string cam_id_for_pwm;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!cooler_on_) {
                    return; // already off
                }
                cooler_on_ = false;
                if (!connected_.load()) {
                    return;
                }
                temp_thread_stop_->store(true);
                temp_running_to_join = temp_thread_running_;
                temp_to_join = std::move(temp_thread_);
                cam_id_for_pwm = camera_id_.value_or(""); // mutex_ already held; don't call camera_id_value()
            }
            join_temp_thread(temp_to_join, temp_running_to_join);  // bounded; may detach
            if (!cam_id_for_pwm.empty()) {
                // Timing is logged because this call has no SDK-side timeout
                // and can occasionally run long on real hardware (ConformU
                // finding) -- worth being able to see how long after the fact.
                // Safe to call after a detach: only touches local state.
                const auto call_start = std::chrono::steady_clock::now();
                ALPACA_LOG_INFO("QHY", "Calling SetQHYCCDParam(MANULPWM,0)...");
                try {
                    QHYSDKWrapper::instance().set_param(cam_id_for_pwm,
                                                        control::MANULPWM, 0.0);
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - call_start)
                                                .count();
                    ALPACA_LOG_INFO(
                        "QHY", "SetQHYCCDParam(MANULPWM,0) returned OK after " + std::to_string(elapsed_ms) + "ms");
                } catch (const std::exception& e) {
                    // MANULPWM may not be writable on all cameras — ignore
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - call_start)
                                                .count();
                    ALPACA_LOG_INFO("QHY", "SetQHYCCDParam(MANULPWM,0) threw after " + std::to_string(elapsed_ms) +
                                               "ms: " + e.what());
                }
            }
        });
        // Publish only after the thread is actually running: if the ctor
        // above throws, we never reach here, so cooler_off_running_ still
        // reflects the previous (already-finished) generation instead of
        // being wedged true with no worker left to clear it.
        cooler_off_running_ = running_flag;
    }

    double get_cooler_power() const override {
        // Explicitly report that cooler power is not implemented to avoid
        // higher-level clients repeatedly polling this property and triggering
        // timeouts on QHY SDK edge cases.
        throw AlpacaException("Cooler power reporting not implemented for QHY cameras",
                              AlpacaError::PropertyNotImplemented);
    }

    double get_electrons_per_adu() const override {
        // TODO: QHY SDK does not expose e-/ADU directly; return 1.0 as placeholder
        return 1.0;
    }

    double get_exposure_max() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::EXPOSURE);
        if (!range.available) {
            return 3600.0; // 1 hour default
        }
        return range.max / 1'000'000.0; // microseconds → seconds
    }

    double get_exposure_min() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::EXPOSURE);
        if (!range.available) {
            return 0.000001; // 1 µs default
        }
        double min_s = range.min / 1'000'000.0;
        return min_s > 0.0 ? min_s : 0.000001;
    }

    double get_exposure_resolution() const override {
        return 0.000001; // 1 microsecond
    }

    bool get_fast_readout() const override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    void set_fast_readout(bool) override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    double get_full_well_capacity() const override {
        // TODO: QHY SDK exposes this via CAM_CurveFullWell on supported cameras
        return 0.0;
    }

    int get_gain() const override {
        ensure_connected();
        return static_cast<int>(
            QHYSDKWrapper::instance().get_param(camera_id_value(), control::GAIN));
    }

    void set_gain(int gain) override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::GAIN);
        if (range.available && (gain < static_cast<int>(range.min) ||
                                gain > static_cast<int>(range.max))) {
            throw AlpacaException("Gain value out of range", AlpacaError::InvalidValue);
        }
        // After the InvalidValue range check (validation precedes state checks).
        // Hold mutex_ across the register write itself: releasing it between
        // ensure_not_exposing_locked() and set_param would let a racing
        // start_exposure begin integration in the gap and land this write
        // mid-frame — the exact race the guard exists to close. The id is
        // read inline (camera_id_value() would re-lock and self-deadlock).
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_not_exposing_locked();
        const std::string id = camera_id_.value_or("");
        if (id.empty()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        QHYSDKWrapper::instance().set_param(id, control::GAIN, static_cast<double>(gain));
        cached_gain_ = gain;
    }

    int get_gain_max() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::GAIN);
        if (!range.available) {
            throw AlpacaException("Gain range not available", AlpacaError::NotImplemented);
        }
        return static_cast<int>(range.max);
    }

    int get_gain_min() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::GAIN);
        if (!range.available) {
            throw AlpacaException("Gain range not available", AlpacaError::NotImplemented);
        }
        return static_cast<int>(range.min);
    }

    std::vector<std::string> get_gains() const override {
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
        ALPACA_LOG_TRACE("QHY", "get_heat_sink_temperature entry");
        double t = get_ccd_temperature();
        ALPACA_LOG_TRACE("QHY", "get_heat_sink_temperature exit");
        return t;
    }

    ImageArray get_image_array() const override {
        ensure_connected();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!last_exposure_valid_) {
                throw AlpacaException("No exposure has been taken", AlpacaError::InvalidOperation);
            }
        }

        // Wait until exposure thread completes (poll with short sleep)
        for (int i = 0; i < 600; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (exposure_status_ == QHYExposureStatus::Success) {
                    return build_image_array_locked();
                }
                if (exposure_status_ == QHYExposureStatus::Failed) {
                    throw AlpacaException("Exposure failed", AlpacaError::DriverException);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        throw AlpacaException("Timeout waiting for image data", AlpacaError::DriverException);
    }

    std::string get_image_array_variant() const override {
        return "Int32";
    }

    bool get_image_ready() const override {
        ALPACA_LOG_TRACE("QHY", "get_image_ready entry");
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            ALPACA_LOG_TRACE("QHY", "get_image_ready exit (no exposure)");
            return false;
        }
        bool v = (exposure_status_ == QHYExposureStatus::Success);
        ALPACA_LOG_TRACE("QHY", "get_image_ready exit");
        return v;
    }

    bool get_is_pulse_guiding() const override {
        if (!pulse_guiding_->load()) {
            return false;
        }
        // Expiry check and clear under ONE mutex_ hold: an unlocked
        // store(false) after the check could overwrite a concurrent
        // pulse_guide's fresh flag/end-time write and falsely report
        // "not guiding" mid-pulse (Player One camera, PR #119 round 4).
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::chrono::steady_clock::now() >= pulse_guiding_end_) {
            pulse_guiding_->store(false);
            return false;
        }
        return true;
    }

    double get_last_exposure_duration() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("No exposure has been taken", AlpacaError::ValueNotSet);
        }
        return last_exposure_duration_;
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("No exposure has been taken", AlpacaError::ValueNotSet);
        }
        return last_exposure_start_;
    }

    int get_max_adu() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        int depth = (bits_ > 0) ? bits_ : static_cast<int>(camera_info_.bpp);
        if (depth <= 0) {
            return 65535;
        }
        return static_cast<int>((1ULL << depth) - 1ULL);
    }

    int get_max_bin_x() const override {
        ensure_connected();
        return max_supported_bin(camera_id_value());
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
        ensure_connected();
        return static_cast<int>(
            QHYSDKWrapper::instance().get_param(camera_id_value(), control::OFFSET));
    }

    void set_offset(int offset) override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::OFFSET);
        if (range.available && (offset < static_cast<int>(range.min) ||
                                offset > static_cast<int>(range.max))) {
            throw AlpacaException("Offset value out of range", AlpacaError::InvalidValue);
        }
        // Hold mutex_ across the register write (see set_gain for rationale).
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_not_exposing_locked();
        const std::string id = camera_id_.value_or("");
        if (id.empty()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        QHYSDKWrapper::instance().set_param(id, control::OFFSET, static_cast<double>(offset));
        cached_offset_ = offset;
    }

    int get_offset_max() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::OFFSET);
        if (!range.available) {
            throw AlpacaException("Offset range not available", AlpacaError::NotImplemented);
        }
        return static_cast<int>(range.max);
    }

    int get_offset_min() const override {
        ensure_connected();
        auto range = QHYSDKWrapper::instance().get_param_range(camera_id_value(),
                                                               control::OFFSET);
        if (!range.available) {
            throw AlpacaException("Offset range not available", AlpacaError::NotImplemented);
        }
        return static_cast<int>(range.min);
    }

    std::vector<std::string> get_offsets() const override {
        throw AlpacaException("Named offset list not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        if (!connected_.load()) {
            return 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (exposure_status_ == QHYExposureStatus::Success) {
            return 100.0;
        }
        if (exposure_status_ != QHYExposureStatus::Working) {
            return 0.0;
        }
        if (last_exposure_duration_ <= 0.0) {
            return 0.0;
        }
        auto now = std::chrono::system_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_exposure_start_).count();
        double percent = (elapsed / last_exposure_duration_) * 100.0;
        return std::clamp(percent, 0.0, 100.0);
    }

    double get_pixel_size_x() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_x_um : 0.0;
    }

    double get_pixel_size_y() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_y_um : 0.0;
    }

    int get_readout_mode() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return readout_mode_;
    }

    void set_readout_mode(int mode) override {
        ensure_connected();
        // Hold mutex_ across the whole apply + geometry refresh (all fast
        // metadata/register calls, no blocking image wait): releasing it
        // after ensure_not_exposing_locked() would let a racing
        // start_exposure begin integration against half-applied readout
        // geometry (see set_gain for the rationale on the inline id read).
        std::lock_guard<std::mutex> lock(mutex_);
        if (mode < 0 || mode >= static_cast<int>(readout_modes_.size())) {
            throw AlpacaException("Invalid readout mode index", AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();
        const std::string id = camera_id_.value_or("");
        if (id.empty()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        QHYSDKWrapper::instance().set_readout_mode(id, static_cast<uint32_t>(mode));

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
        if (cached_gain_.has_value()) {
            try {
                QHYSDKWrapper::instance().set_param(id, control::GAIN, static_cast<double>(*cached_gain_));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Failed to restore gain after readout mode change: " + std::string(e.what()));
            }
        }
        if (cached_offset_.has_value()) {
            try {
                QHYSDKWrapper::instance().set_param(id, control::OFFSET, static_cast<double>(*cached_offset_));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Failed to restore offset after readout mode change: " + std::string(e.what()));
            }
        }

        // After changing readout mode, refresh chip info as dimensions may change
        QHYCameraInfo updated_info;
        if (QHYSDKWrapper::instance().get_chip_info(id, updated_info)) {
            int new_max_w = static_cast<int>(updated_info.max_width);
            int new_max_h = static_cast<int>(updated_info.max_height);
            try {
                QHYSDKWrapper::instance().set_resolution(id, 0, 0,
                    static_cast<uint32_t>(new_max_w), static_cast<uint32_t>(new_max_h));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Failed to reset ROI after readout mode change: " +
                                std::string(e.what()));
            }
            readout_mode_ = mode;
            camera_info_ = updated_info;
            num_x_ = new_max_w;
            num_y_ = new_max_h;
            start_x_ = 0;
            start_y_ = 0;
        } else {
            readout_mode_ = mode;
        }
    }

    std::vector<std::string> get_readout_modes() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (readout_modes_.empty()) {
            return {"Normal"};
        }
        return readout_modes_;
    }

    std::string get_sensor_name() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.model : "QHY Sensor";
    }

    SensorType get_sensor_type() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            return SensorType::Monochrome;
        }
        return SensorType::RGGB;
    }

    double get_set_ccd_temperature() const override {
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
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
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void set_sub_exposure_duration(double) override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void abort_exposure() override {
        stop_exposure();
    }

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

        uint32_t qhy_dir = alpaca_to_qhy_guide_direction(direction);
        uint16_t dur_ms = static_cast<uint16_t>(std::min(duration, 65535));
        // Copy, not reference: the detached thread below must not touch `this`.
        std::string cam_id = camera_id_value();

        // End timestamp BEFORE the flag, both under one mutex_ hold: a reader
        // that observes the flag as true must never see a stale (epoch) end
        // time, or the self-clearing getter would clear the pulse immediately
        // (same shape as the Player One camera, PR #119).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guiding_end_ = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(duration);
            pulse_guiding_->store(true);
        }

        // ControlQHYCCDGuide blocks the calling thread for the full pulse
        // duration (confirmed on real miniCam8M hardware via ConformU: a
        // 2000ms pulse blocked the HTTP handler for exactly 2000ms), which
        // blows the ASCOM STANDARD (1.0s) response-time target for an
        // initiator method. Run the SDK call on a detached thread so
        // PulseGuide returns immediately, matching the async pattern ASCOM
        // expects; the flag then clears exactly when the physical pulse
        // completes rather than being tracked by a separate sleep timer that
        // could drift out of sync with the SDK call.
        //
        // The thread captures NO object state — only the copied camera ID
        // and the shared_ptr flag — so it cannot dereference a destroyed
        // driver if the object is torn down mid-pulse (AGENTS.md: never
        // detach a thread that touches `this`; the Player One camera fixed
        // this exact bug with the same shared_ptr pattern).
        std::thread([cam_id, qhy_dir, dur_ms, flag = pulse_guiding_]() {
            // guide() throws (NotConnected if a disconnect races this thread's
            // start, or any SDK failure via check_result); an exception
            // escaping a std::thread entry point calls std::terminate, so a
            // disconnect racing a pulse would crash the whole server. The
            // flag must clear on every exit path or IsPulseGuiding sticks
            // true forever.
            try {
                QHYSDKWrapper::instance().guide(cam_id, qhy_dir, dur_ms);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "PulseGuide worker failed: " + std::string(e.what()));
            } catch (...) {
                ALPACA_LOG_WARN("QHY", "PulseGuide worker failed: non-std exception");
            }
            flag->store(false);
        }).detach();
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        if (duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative",
                                  AlpacaError::InvalidValue);
        }

        // Held through the thread spawn at the end: serialises the spawn against
        // the join in stop_exposure (join racing the thread-assignment is UB on
        // std::thread — a concurrent StartExposure/AbortExposure pair could hit
        // join_exposure_thread() and `exposure_thread_ = ...` on the same object).
        // Lock order: exposure_lifecycle_mutex_ -> mutex_ (all locks below nest).
        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        // Re-check under the lifecycle lock: a disconnect holds this mutex
        // from the exposure-worker join through the SDK close, so a
        // start_exposure that was blocked on it must not arm a closed camera.
        ensure_connected();

        const std::string& id = camera_id_value();
        auto& sdk = QHYSDKWrapper::instance();

        // Validate ROI against sensor bounds (setters defer this check to here)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (num_x_ <= 0 || num_y_ <= 0) {
                throw AlpacaException("ROI dimensions are invalid", AlpacaError::InvalidValue);
            }
            if (camera_info_valid_) {
                int max_w = static_cast<int>(camera_info_.max_width)  / bin_x_;
                int max_h = static_cast<int>(camera_info_.max_height) / bin_y_;
                if (num_x_ > max_w || num_y_ > max_h) {
                    throw AlpacaException("ROI exceeds sensor area for current binning",
                                          AlpacaError::InvalidValue);
                }
                if (start_x_ + num_x_ > max_w || start_y_ + num_y_ > max_h) {
                    throw AlpacaException("Start position + ROI exceeds sensor area",
                                          AlpacaError::InvalidValue);
                }
            }
        }

        // Reap the previous worker FIRST, before any other SDK call in this
        // function. cancel_exposure() deliberately does not take the SDK
        // wrapper's per-handle call_mutex (see its definition) specifically
        // so it can interrupt a wedged GetQHYCCDSingleFrame that's holding
        // that mutex; every setter below (set_param, is_control_available,
        // set_resolution, set_bin_mode, set_bits_mode) DOES take it. If a
        // prior download is genuinely stuck and its worker was detached by
        // join_exposure_thread()'s timeout, that detached thread is still
        // holding call_mutex from inside GetQHYCCDSingleFrame -- calling any
        // mutexed setter before this reap would block this StartExposure on
        // that same call_mutex forever, never reaching the cancel that's
        // supposed to break the hang (review finding on PR #201).
        //
        // exposure_thread_.joinable() alone is NOT enough to decide whether
        // to retry: std::thread::detach() (inside join_exposure_thread()'s
        // 2s-timeout path) makes joinable() false immediately, even though
        // the detached worker may still be alive and stuck in
        // GetQHYCCDSingleFrame, still holding call_mutex. Gating solely on
        // joinable() would then skip cancel_exposure() on every StartExposure
        // after the first timeout, reproducing exactly the hang this reorder
        // was meant to fix once the zombie is holding the mutex instead of
        // just its own call (second review finding on PR #201).
        // exposure_thread_running_ is the shared flag the RunningGuard in
        // launch_exposure_worker_locked() clears on actual return from the
        // worker lambda -- it stays true for as long as that lambda (and
        // whatever call_mutex-guarded SDK call it's blocked in) is still
        // running, independent of whether the std::thread object itself was
        // detached. Retrying cancel_exposure() while it's still true gives a
        // wedged-but-not-permanently-unresponsive handle another chance to
        // unblock before the mutexed setters below would otherwise hang.
        bool worker_may_still_be_running = exposure_thread_running_ && exposure_thread_running_->load();
        if (exposure_thread_.joinable() || worker_may_still_be_running) {
            try {
                sdk.cancel_exposure(id);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "cancel_exposure before re-arm failed: " + std::string(e.what()));
            }
            if (exposure_thread_.joinable()) {
                join_exposure_thread();
            }
        }

        // Apply exposure time (microseconds)
        double exposure_us = duration * 1'000'000.0;
        sdk.set_param(id, control::EXPOSURE, exposure_us);

        // Close mechanical shutter for dark/bias frames if the camera supports it
        if (!light && sdk.is_control_available(id, control::MECHANICALSHUTTER)) {
            sdk.set_param(id, control::MECHANICALSHUTTER, 1.0); // 1 = closed
        }

        // Apply current ROI and binning (do not hold mutex_ across SDK calls)
        uint32_t bin_x_u = 0;
        uint32_t bin_y_u = 0;
        uint32_t start_x_u = 0;
        uint32_t start_y_u = 0;
        uint32_t num_x_u = 0;
        uint32_t num_y_u = 0;
        uint32_t bits_u = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bin_x_u = static_cast<uint32_t>(bin_x_);
            bin_y_u = static_cast<uint32_t>(bin_y_);
            start_x_u = static_cast<uint32_t>(start_x_);
            start_y_u = static_cast<uint32_t>(start_y_);
            num_x_u = static_cast<uint32_t>(num_x_);
            num_y_u = static_cast<uint32_t>(num_y_);
            bits_u = static_cast<uint32_t>(bits_);
        }
        // Order matches QHY's own SingleFrameMode.cpp sample (resolution
        // before bin mode) -- not confirmed load-bearing on its own, but kept
        // aligned with the only sequence QHY ships and validates.
        sdk.set_resolution(id, start_x_u, start_y_u, num_x_u, num_y_u);
        sdk.set_bin_mode(id, bin_x_u, bin_y_u);
        sdk.set_bits_mode(id, bits_u);

        {
            // Publish Working BEFORE the SDK exposure start so a concurrent
            // setter's ensure_not_exposing_locked() (checked under this same
            // mutex_) cannot pass during StartQHYCCDSingleFrame and write a
            // register into the starting frame. Watchdog deadline: exposure
            // time + a generous margin for SDK readout/transfer. This flat
            // 60s margin covers ordinary readout; the exposure worker below
            // extends it further once the actual buffer size is known --
            // see the comment there for why (miniCam8's "Linearity HDR"
            // readout mode reliably takes ~64s to download regardless of
            // exposure duration, exceeding even this flat margin).
            std::lock_guard<std::mutex> lock(mutex_);
            last_exposure_duration_ = duration;
            last_exposure_start_ = std::chrono::system_clock::now();
            last_exposure_valid_ = true;
            image_ready_ = false;
            exposure_status_ = QHYExposureStatus::Working;
            exposure_deadline_ = std::chrono::steady_clock::now() +
                                 std::chrono::microseconds(static_cast<long long>(exposure_us)) +
                                 std::chrono::seconds(60);
            exposure_deadline_valid_ = true;
            exposure_buffer_.clear();
            exposure_width_ = 0;
            exposure_height_ = 0;
            exposure_bpp_ = 0;
            exposure_channels_ = 0;
        }

        // Fresh flags per generation -- see the member comment for why not a
        // store() reset of the shared members.
        auto exposure_running = std::make_shared<std::atomic<bool>>(true);
        auto exposure_superseded = std::make_shared<std::atomic<bool>>(false);
        {
            // Also take mutex_ for this reassignment (in addition to
            // exposure_lifecycle_mutex_, already held for the whole
            // function): ensure_not_exposing_locked() now reads
            // exposure_thread_running_ under mutex_ alone (setters never
            // take exposure_lifecycle_mutex_), so the shared_ptr member
            // itself needs to be safe to read under either lock. Holding
            // both here on write is what makes that valid.
            std::lock_guard<std::mutex> lock(mutex_);
            exposure_thread_running_ = exposure_running;
            exposure_thread_superseded_ = exposure_superseded;
        }

        // Launch background thread to run the WHOLE exposure sequence --
        // ExpQHYCCDSingleFrame through GetQHYCCDSingleFrame -- on one thread
        // (previous worker already reaped above, before the frame was armed).
        //
        // ExpQHYCCDSingleFrame used to be called synchronously here on the
        // HTTP handler thread, with only GetQHYCCDSingleFrame backgrounded.
        // On real miniCam8M hardware that reproduced as GetQHYCCDSingleFrame
        // never returning -- confirmed NOT a concurrency issue (every other
        // SDK call in this process is now serialized against this one via
        // QHYSDKWrapper's per-handle call_mutex, and the hang still happened
        // with literally nothing else touching the SDK) and NOT an SDK/USB/
        // firmware issue (QHY's own SingleFrameMode.cpp sample, built and run
        // standalone against this exact camera, completes both a 20ms and a
        // 4s exposure cleanly -- the one thing that sample does that this
        // driver didn't was keep Exp and Get on the SAME thread). QHY's SDK
        // appears to keep thread-affine state for a single-frame session.
        exposure_thread_ = std::thread([this, id, exposure_running, exposure_superseded]() {
            struct RunningGuard {
                std::shared_ptr<std::atomic<bool>> flag;
                ~RunningGuard() { flag->store(false); }
            } running_guard{exposure_running};

            auto& sdk = QHYSDKWrapper::instance();

            ALPACA_LOG_DEBUG("QHY", "exposure worker: calling start_single_frame...");
            bool read_directly = false;
            try {
                read_directly = sdk.start_single_frame(id);
                ALPACA_LOG_DEBUG("QHY", "exposure worker: start_single_frame returned, read_directly=" +
                                            std::string(read_directly ? "true" : "false"));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "start_single_frame failed: " + std::string(e.what()));
                if (exposure_superseded->load()) {
                    return;
                }
                std::lock_guard<std::mutex> lk(mutex_);
                exposure_status_ = QHYExposureStatus::Failed;
                image_ready_ = false;
                exposure_deadline_valid_ = false;
                return;
            }

            // Per QHY's own SingleFrameMode.cpp sample: when ExpQHYCCDSingleFrame
            // does NOT return QHYCCD_READ_DIRECTLY, the sample sleeps 1s before
            // calling GetQHYCCDSingleFrame. The camera firmware apparently needs
            // this settle time to finish transitioning into a readable state.
            if (!read_directly) {
                ALPACA_LOG_DEBUG("QHY", "exposure worker: sleeping 1s before GetMemLength/GetSingleFrame...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            // Fetched here (after Exp, like the sample), not before arming:
            // matches the one sequence QHY ships and validates.
            ALPACA_LOG_DEBUG("QHY", "exposure worker: calling get_mem_length...");
            uint32_t mem_length = 0;
            try {
                mem_length = sdk.get_mem_length(id);
                ALPACA_LOG_DEBUG("QHY", "exposure worker: get_mem_length returned " + std::to_string(mem_length));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "get_mem_length failed: " + std::string(e.what()));
            }
            if (mem_length == 0) {
                // Check supersession BEFORE cancelling: cancel_exposure()
                // operates on the physical handle, not this generation. If
                // join_exposure_thread() already timed out (2s) and detached
                // this worker -- e.g. while it was in the mandatory 1s settle
                // sleep or queued behind call_mutex -- a newer start_exposure()
                // generation may already be armed and mid-download on the same
                // handle by the time this stale worker reaches mem_length==0.
                // Cancelling unconditionally here would silently abort that
                // new, legitimate exposure (review finding on PR #201); the
                // next generation's own reap-before-arm logic already issues
                // its own cancel_exposure() if it actually needs one.
                if (exposure_superseded->load()) {
                    return;
                }
                // Unlike the start_single_frame failure above, ExpQHYCCDSingleFrame
                // DID succeed to get here -- the SDK is armed with nothing left to
                // read it back out. Cancel so this failure doesn't leave an
                // armed-but-abandoned exposure behind (review finding on PR #201);
                // the next start_exposure()/disconnect() would otherwise be the
                // first thing to notice and clean it up.
                try {
                    sdk.cancel_exposure(id);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("QHY",
                                    "cancel_exposure after get_mem_length failure failed: " + std::string(e.what()));
                }
                std::lock_guard<std::mutex> lk(mutex_);
                exposure_status_ = QHYExposureStatus::Failed;
                image_ready_ = false;
                exposure_deadline_valid_ = false;
                return;
            }
            std::vector<uint8_t> local_buf(mem_length, 0);

            // Re-derive the watchdog deadline from the ACTUAL buffer size,
            // now that it's known -- the deadline set at arm time only
            // accounts for exposure_us + a flat 15s margin, which assumes
            // download time scales with mem_length the same way for every
            // readout mode. It doesn't: confirmed on real miniCam8M
            // hardware (both through this driver and via a standalone,
            // single-threaded repro completely outside it) that a 71MB
            // "Linearity HDR" frame could reliably take ~64s to transfer via
            // GetQHYCCDSingleFrame regardless of USBTRAFFIC pacing, while a
            // 36MB "Full Resolution" frame completes in a few seconds (the
            // separate readout-mode/InitQHYCCD-ordering fix elsewhere in
            // this file made that ~64s case rare rather than eliminating
            // the theoretical risk entirely, so the floor below stays as a
            // safety net rather than being removed). The flat 15s margin
            // only covers Full-Resolution-sized transfers; without this
            // extension the watchdog was killing HDR downloads that were
            // still correctly in progress. 500,000 B/s is roughly half the
            // ~1.1 MB/s measured for HDR -- a floor, not a target, so it
            // only EXTENDS (never shortens) the deadline already set at arm
            // time.
            //
            // Gated on readout_mode_ != 0: index 0 is the SDK's default/
            // fastest mode on every QHY camera examined during this
            // investigation and has never shown this slow-transfer
            // behavior, so applying the same buffer-size floor to it would
            // only over-extend its watchdog for no benefit -- e.g. its own
            // 36MB buffer computes to an ~87s floor, letting a genuine hang
            // on that mode go undetected far longer than the flat 60s
            // margin already covers it for (review finding on PR #201).
            //
            // Read without mutex_: safe because set_readout_mode() calls
            // ensure_not_exposing_locked() under mutex_ before it ever
            // writes readout_mode_, and exposure_status_ was published as
            // Working (also under mutex_) before this worker was spawned --
            // so a concurrent set_readout_mode() call can't succeed while
            // this worker is running, and readout_mode_ can't change out
            // from under this read for the life of this exposure.
            if (readout_mode_ != 0) {
                constexpr double kMinTransferBytesPerSecond = 500'000.0;
                auto min_transfer = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(static_cast<double>(mem_length) / kMinTransferBytesPerSecond));
                auto extended_deadline = std::chrono::steady_clock::now() + min_transfer + std::chrono::seconds(15);
                if (!exposure_superseded->load()) {
                    std::lock_guard<std::mutex> lk(mutex_);
                    if (exposure_deadline_valid_ && extended_deadline > exposure_deadline_) {
                        exposure_deadline_ = extended_deadline;
                    }
                }
            }

            ALPACA_LOG_DEBUG("QHY", "exposure worker: calling get_single_frame...");
            uint32_t w = 0, h = 0, bpp = 0, channels = 0;
            bool ok = sdk.get_single_frame(id, local_buf.data(), w, h, bpp, channels);
            ALPACA_LOG_DEBUG("QHY",
                             "exposure worker: get_single_frame returned ok=" + std::string(ok ? "true" : "false"));

            // Recheck immediately after the (possibly indefinitely long)
            // blocking call, BEFORE touching `this` again: if
            // join_exposure_thread() timed out and detached this thread
            // while it was inside GetSingleFrame, a newer exposure
            // generation may already own exposure_buffer_/exposure_status_,
            // or `this` itself may already be destroyed.
            if (exposure_superseded->load()) {
                return;
            }

            std::lock_guard<std::mutex> lk(mutex_);
            if (ok) {
                exposure_buffer_ = std::move(local_buf);
                exposure_width_    = w;
                exposure_height_   = h;
                exposure_bpp_      = bpp;
                exposure_channels_ = channels;
                exposure_status_   = QHYExposureStatus::Success;
                image_ready_       = true;
            } else {
                exposure_status_ = QHYExposureStatus::Failed;
                image_ready_     = false;
            }
            exposure_deadline_valid_ = false;
        });
    }

    void stop_exposure() override {
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_status_ != QHYExposureStatus::Working) {
                return;
            }
        }
        // Serialise this join against start_exposure's spawn (join vs
        // thread-assignment on the same std::thread is UB).
        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        // Cancel exposure (causes GetSingleFrame to return with an error)
        try {
            QHYSDKWrapper::instance().cancel_exposure(camera_id_value());
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "cancel_exposure error: " + std::string(e.what()));
        }
        join_exposure_thread();

        std::lock_guard<std::mutex> lock(mutex_);
        exposure_status_ = QHYExposureStatus::Idle;
        exposure_deadline_valid_ = false;
        image_ready_ = false;
        last_exposure_valid_ = false;
    }

private:
    // ── Members ──────────────────────────────────────────────────────────────

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
    mutable std::mutex mutex_;

    // Exposure state. exposure_status_ is mutable because the watchdog in
    // get_camera_state (const) marks a hung exposure Failed once the deadline
    // passes (same shape as the SVBONY/ToupTek exposure deadline).
    mutable QHYExposureStatus exposure_status_;
    mutable bool image_ready_;
    std::vector<uint8_t> exposure_buffer_;
    uint32_t exposure_width_{};
    uint32_t exposure_height_{};
    uint32_t exposure_bpp_{};
    uint32_t exposure_channels_{};
    double last_exposure_duration_;
    std::chrono::system_clock::time_point last_exposure_start_;
    bool last_exposure_valid_;
    std::thread exposure_thread_;
    // Serialises the exposure thread's lifecycle: spawn (start_exposure) vs join
    // (stop_exposure). Join racing the spawn's thread-assignment is UB on
    // std::thread. The destructor's join is exempt (runs after the connection
    // thread is joined; no client calls in flight). Lock order:
    // exposure_lifecycle_mutex_ -> mutex_; the exposure thread never takes it.
    std::mutex exposure_lifecycle_mutex_;
    // GetQHYCCDSingleFrame has no SDK-side timeout and can hang indefinitely
    // (it did, before the exposing-guards on temp_thread_/telemetry_thread_
    // above existed). join_exposure_thread() bounds the wait and detaches on
    // timeout so a wedged download cannot hang every later
    // start_exposure()/stop_exposure()/disconnect() on this camera forever.
    // Both shared_ptr, fresh per generation (not a store() reset of the
    // member), same reasoning as temp_thread_stop_/temp_thread_running_: a
    // prior generation's timed-out-and-detached zombie keeps its own copy, so
    // resetting the member in place would let the zombie's eventual
    // completion clobber a newer exposure's result.
    std::shared_ptr<std::atomic<bool>> exposure_thread_running_{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<bool>> exposure_thread_superseded_{std::make_shared<std::atomic<bool>>(false)};

    // Watchdog deadline: get_camera_state marks the exposure Failed once
    // now >= this, so a GetQHYCCDSingleFrame that never returns cannot leave
    // the driver reporting Exposing forever. Guarded by mutex_.
    mutable std::chrono::steady_clock::time_point exposure_deadline_{};
    mutable bool exposure_deadline_valid_{false};

    // Temperature control. Both flags are shared_ptr, not plain members:
    // ControlQHYCCDTemp (the per-iteration SDK call) has no timeout of its
    // own and can occasionally run well past its documented ~10s PID-loop
    // figure (ConformU finding), so join_temp_thread() below bounds the wait
    // and detaches on timeout. A detached thread must never touch `this`
    // (AGENTS.md) -- these shared_ptrs, plus the immediate stop-flag
    // recheck in start_temp_control_thread() right after the blocking call,
    // are what make that safe. Same pattern as pulse_guiding_/
    // cooler_off_running_.
    std::thread temp_thread_;
    std::shared_ptr<std::atomic<bool>> temp_thread_stop_{std::make_shared<std::atomic<bool>>(false)};
    // True for the temp thread's entire lifetime (spawn to natural exit);
    // lets join_temp_thread() know whether a detach is actually needed.
    std::shared_ptr<std::atomic<bool>> temp_thread_running_{std::make_shared<std::atomic<bool>>(false)};

    // Cooler-off worker. cooler_off_lifecycle_mutex_ serialises its spawn vs
    // join; joined in disconnect and the destructor.
    std::thread cooler_off_thread_;
    std::mutex cooler_off_lifecycle_mutex_;
    // True while a cooler-off worker is in flight; lets a second CoolerOn=false
    // return immediately instead of blocking on the join (see set_cooler_on).
    // shared_ptr, not a plain member: SetQHYCCDParam(MANULPWM) can hang
    // indefinitely on real hardware with no SDK-side timeout (ConformU
    // finding), so join_cooler_off_thread() bounds its wait and detaches on
    // timeout. A detached thread must never touch `this` once it might
    // outlive the driver (AGENTS.md) — the shared_ptr keeps this flag alive
    // independent of `this`, same pattern as pulse_guiding_ below.
    std::shared_ptr<std::atomic<bool>> cooler_off_running_{std::make_shared<std::atomic<bool>>(false)};

    // Serialises temp/telemetry thread starts: two concurrent starters could
    // both pass the joinable() pre-check and the loser would destroy a
    // joinable std::thread (std::terminate).
    std::mutex thread_start_mutex_;

    // Telemetry (non-blocking cached temperature / cooler power)
    std::thread telemetry_thread_;
    std::atomic<bool> telemetry_thread_stop_{false};
    double telemetry_ccd_temp_c_{0.0};
    double telemetry_cooler_power_{0.0}; // percentage 0.0–100.0
    bool telemetry_temp_valid_{false};
    bool telemetry_power_valid_{false};

    // Pulse guide. shared_ptr so the detached flag-clear thread can co-own the
    // flag and clear it after the pulse WITHOUT capturing `this`
    // (destructor-safe — the Player One camera's round-9 fix). The timestamp
    // getter still self-clears for readers in the interim.
    std::shared_ptr<std::atomic<bool>> pulse_guiding_{std::make_shared<std::atomic<bool>>(false)};
    std::chrono::steady_clock::time_point pulse_guiding_end_;

    // ── Helpers ──────────────────────────────────────────────────────────────

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    // Reject runtime register writes (gain/offset/bin/ROI/readout/temp) while a
    // frame is integrating: the exposure worker is blocked in
    // GetQHYCCDSingleFrame holding no lock, so a mid-exposure write would race
    // the live integration and corrupt the frame or the buffer geometry.
    // Checked under mutex_ — the same lock start_exposure publishes
    // exposure_status_ under (AGENTS.md TOCTOU rule). Caller must hold mutex_.
    void ensure_not_exposing_locked() const {
        if (exposure_status_ == QHYExposureStatus::Working) {
            throw AlpacaException("Cannot change camera settings during an exposure", AlpacaError::InvalidOperation);
        }
        // get_camera_state()'s watchdog marks a hung exposure Failed purely
        // from elapsed time -- it does NOT confirm exposure_thread_ has
        // actually returned. A genuinely wedged GetQHYCCDSingleFrame call can
        // still be running (and still holding QHYSDKWrapper's call_mutex)
        // well after exposure_status_ flips to Failed. Every setter that
        // reaches this check next holds mutex_ across its own call_mutex-
        // guarded SDK write (set_gain, set_offset, set_readout_mode,
        // set_bin_xy, set_temperature, ...); if one of them were let through
        // here it would block forever on call_mutex while STILL HOLDING
        // mutex_, and every other operation on this instance -- including
        // start_exposure()'s and disconnect()'s own reap/cancel logic, which
        // also need mutex_ -- would then stall forever behind it. That's a
        // whole-driver deadlock reachable only via this narrow window
        // (review finding on PR #201). Reject explicitly instead: a stuck
        // worker degrades to "camera busy, try again" rather than silently
        // wedging every other property and the paired CFW (shared handle).
        if (exposure_thread_running_ && exposure_thread_running_->load()) {
            throw AlpacaException("Camera is finishing a previous exposure; try again shortly",
                                  AlpacaError::InvalidOperation);
        }
    }

    const std::string& camera_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        return camera_id_.value();
    }

    const std::string& resolve_camera_id_locked() {
        if (camera_index_.has_value() && !camera_id_.has_value()) {
            auto cameras = QHYSDKWrapper::instance().enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No QHY cameras detected", AlpacaError::NotConnected);
            }
            int index = camera_index_.value();
            if (index < 0 || index >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range: " + std::to_string(index),
                                      AlpacaError::InvalidValue);
            }
            const auto& info = cameras[static_cast<std::size_t>(index)];
            ALPACA_LOG_INFO("QHY", "Resolved camera index " + std::to_string(index) +
                            " → ID: " + info.camera_id);
            camera_id_ = info.camera_id;
            if (info.model.empty() == false) {
                camera_info_.model = info.model;
            }
        }
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not specified", AlpacaError::InvalidValue);
        }
        return camera_id_.value();
    }

    void load_readout_modes_locked(const std::string& id) {
        readout_modes_.clear();
        try {
            uint32_t num = QHYSDKWrapper::instance().get_num_readout_modes(id);
            for (uint32_t i = 0; i < num; ++i) {
                readout_modes_.push_back(
                    QHYSDKWrapper::instance().get_readout_mode_name(id, i));
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("QHY", "Readout mode enumeration failed: " + std::string(e.what()));
        }
        if (readout_modes_.empty()) {
            readout_modes_.push_back("Normal");
        }
        readout_mode_ = 0;
    }

    void reset_exposure_state_locked() {
        // A disconnect racing an in-flight pulse must not leave
        // IsPulseGuiding=true for a freshly reconnected client.
        pulse_guiding_->store(false);
        pulse_guiding_end_ = {};
        exposure_status_   = QHYExposureStatus::Idle;
        exposure_deadline_valid_ = false;
        image_ready_       = false;
        last_exposure_duration_ = 0.0;
        last_exposure_start_    = std::chrono::system_clock::time_point{};
        last_exposure_valid_    = false;
        exposure_buffer_.clear();
        exposure_width_    = 0;
        exposure_height_   = 0;
        exposure_bpp_      = 0;
        exposure_channels_ = 0;
    }

    // Bounded wait + detach fallback, mirroring join_temp_thread() /
    // join_cooler_off_thread(): a plain join() here would let a wedged
    // GetQHYCCDSingleFrame block every later start_exposure()/stop_exposure()/
    // disconnect() on this camera forever. Marks the generation superseded
    // before detaching so the zombie's completion handler (see the lambda in
    // start_exposure()) discards its result instead of clobbering a newer
    // exposure's state.
    void join_exposure_thread() {
        if (!exposure_thread_.joinable()) {
            return;
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (exposure_thread_running_->load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (exposure_thread_running_->load()) {
            ALPACA_LOG_WARN("QHY", "Exposure download worker exceeded 2s reap timeout; detaching.");
            exposure_thread_superseded_->store(true);
            exposure_thread_.detach();
        } else {
            exposure_thread_.join();  // finished -- instant reap
        }
    }

    void join_cooler_off_thread() {
        std::lock_guard<std::mutex> cooler_lock(cooler_off_lifecycle_mutex_);
        if (!cooler_off_thread_.joinable()) {
            return;
        }
        // This worker joins temp_thread_ before touching the SDK, and
        // ControlQHYCCDTemp (the temp thread's per-iteration SDK call) is
        // documented elsewhere in this file as blocking ~10s (PID loop),
        // occasionally longer (ConformU finding). SetQHYCCDParam itself also
        // has no SDK-side timeout. ASCOM clients (ConformU included) apply
        // their own ~5s budget to the bare Disconnect() call, so this can't
        // simply wait out the documented worst case -- it must return well
        // under that regardless of how long the underlying hardware call
        // actually takes. Bound the wait short and detach on timeout; safe
        // because the QHY SDK wrapper's handle is now reference-counted
        // (shared_ptr, not a raw pointer -- see qhy_sdk_wrapper.cpp), so a
        // still-running SetQHYCCDParam call can't be invalidated by a
        // concurrent close_camera(), and everything this worker does after
        // this point runs through cooler_off_running_ (a shared_ptr, not
        // `this` -- see the RunningGuard in set_cooler_on), so detaching here
        // can never touch a driver that outlives it.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (cooler_off_running_->load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (cooler_off_running_->load()) {
            ALPACA_LOG_WARN("QHY", "cooler-off worker exceeded 2s timeout; detaching");
            cooler_off_thread_.detach();
        } else {
            cooler_off_thread_.join();  // finished worker — instant reap
        }
    }

    void stop_all_threads() {
        // Do not hold mutex_ across join: temp and telemetry threads acquire
        // mutex_ in their loops; joining while holding it would deadlock.
        // The cooler-off worker takes mutex_ and joins the temp thread, so it
        // too must be joined without mutex_ held.
        join_cooler_off_thread();
        std::thread temp_to_join;
        std::thread telemetry_to_join;
        std::shared_ptr<std::atomic<bool>> temp_running_to_join;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            temp_thread_stop_->store(true);
            temp_running_to_join = temp_thread_running_;
            temp_to_join = std::move(temp_thread_);
            telemetry_thread_stop_.store(true);
            telemetry_to_join = std::move(telemetry_thread_);
        }
        try {
            join_temp_thread(temp_to_join, temp_running_to_join);  // bounded; may detach
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "Temp thread join failed during shutdown: " + std::string(e.what()));
        }
        if (telemetry_to_join.joinable()) {
            try {
                telemetry_to_join.join();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Telemetry thread join failed during shutdown: " +
                    std::string(e.what()));
            }
        }
        // Wake a worker parked in GetQHYCCDSingleFrame before joining, or the
        // join blocks for the whole remaining exposure (AGENTS.md abort rule).
        if (exposure_thread_.joinable()) {
            std::string cancel_id;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cancel_id = camera_id_.value_or("");
            }
            if (!cancel_id.empty()) {
                try {
                    QHYSDKWrapper::instance().cancel_exposure(cancel_id);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("QHY", "cancel_exposure failed during shutdown: " + std::string(e.what()));
                }
            }
        }
        join_exposure_thread();
    }

    // Start temp control thread. Call without holding mutex_ so the HTTP thread
    // does not block ~10s (new thread would otherwise contend for mutex_).
    void start_temp_control_thread() {
        // Serialise concurrent starters: without this, two callers could both
        // pass the joinable() pre-check and the loser would destroy a joinable
        // std::thread (std::terminate). Lock order: thread_start_mutex_ -> mutex_.
        std::lock_guard<std::mutex> start_lock(thread_start_mutex_);
        std::string id;
        std::shared_ptr<std::atomic<bool>> stop_flag;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (temp_thread_.joinable()) {
                return;
            }
            // Fresh flag per generation, not a store(false) reset of the
            // shared member: a prior generation's timed-out-and-detached
            // zombie (join_temp_thread) keeps its own captured copy of the
            // OLD flag, so resetting the member here would let this
            // reconnect un-stop the zombie via the SAME object -- it would
            // recheck stop_flag, see false, and keep looping, driving the
            // cooler alongside the new thread and touching `this` again
            // (review finding). Captured into a local under the same lock
            // that publishes it to the member, so the lambda below never
            // has to re-read the (possibly concurrently reassigned) member.
            stop_flag = std::make_shared<std::atomic<bool>>(false);
            temp_thread_stop_ = stop_flag;
            id = camera_id_.value_or("");
        }
        if (id.empty()) {
            return;
        }
        // Same reasoning as temp_thread_stop_ above: a fresh object so a
        // zombie's eventual RunningGuard destructor can never clear the new
        // generation's in-flight flag out from under join_temp_thread().
        auto running_flag = std::make_shared<std::atomic<bool>>(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            temp_thread_running_ = running_flag;
        }
        std::thread t([this, id, stop_flag, running_flag]() {
            struct RunningGuard {
                std::shared_ptr<std::atomic<bool>> flag;
                ~RunningGuard() { flag->store(false); }
            } running_guard{running_flag};
            while (!stop_flag->load()) {
                double target = 0.0;
                bool exposing = false;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    target = target_temp_;
                    exposing = (exposure_status_ == QHYExposureStatus::Working);
                }
                // Skip this cycle's SDK call while a frame is downloading:
                // ControlQHYCCDTemp and the exposure worker's
                // GetQHYCCDSingleFrame share the SAME physical USB handle, and
                // the QHY SDK is not safe against concurrent calls on one
                // handle from two threads -- a temp call landing mid-transfer
                // wedges GetQHYCCDSingleFrame forever (no SDK-side timeout),
                // which is exactly the "exposure reaches full time but never
                // downloads" failure this fixes. Skipping here (not locking
                // around the SDK call) keeps this thread from itself blocking
                // on a multi-second exposure/readout.
                if (connected_.load() && !exposing) {
                    try {
                        QHYSDKWrapper::instance().control_temp(id, target);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_DEBUG("QHY", "Temp control error: " + std::string(e.what()));
                    }
                }
                // Recheck the stop flag immediately after the (possibly long,
                // occasionally >10s -- ConformU finding) blocking SDK call,
                // BEFORE touching `this` again: if join_temp_thread() timed
                // out and detached this thread while it was inside
                // control_temp(), `this` may already be destroyed by the
                // time we get here.
                if (stop_flag->load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
        std::lock_guard<std::mutex> lock(mutex_);
        // Serialised by thread_start_mutex_: temp_thread_ can only have been
        // moved out (by a stop path) since the check above, never re-assigned.
        temp_thread_ = std::move(t);
    }

    // Bounded wait + detach fallback for the temp-control thread, mirroring
    // join_cooler_off_thread() -- see that function's comment for why 2s
    // (well under ASCOM clients' ~5s Disconnect() budget) instead of the
    // documented ~10s ControlQHYCCDTemp worst case. Safe to detach: see the
    // comment on temp_thread_running_/temp_thread_stop_ and the recheck in
    // start_temp_control_thread() above. Caller must have already set
    // temp_thread_stop_ and moved the thread out of temp_thread_.
    //
    // running_flag must be the SAME generation's flag the caller captured
    // under mutex_ alongside `t` (not a fresh read of the temp_thread_running_
    // member here): start_temp_control_thread() may reassign that member to a
    // new generation's flag concurrently with this call, and reading the
    // member directly would race that reassignment / silently watch the
    // wrong generation's flag (review finding).
    void join_temp_thread(std::thread& t, const std::shared_ptr<std::atomic<bool>>& running_flag) {
        if (!t.joinable()) {
            return;
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (running_flag->load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (running_flag->load()) {
            ALPACA_LOG_WARN("QHY", "temp-control thread exceeded 2s timeout; detaching");
            t.detach();
        } else {
            t.join();  // finished — instant reap
        }
    }

    // Called only from set_connected_impl while holding mutex_. Starts thread
    // without holding lock for thread construction to avoid blocking.
    void start_temp_control_thread_locked() {
        start_temp_control_thread();
    }

    // Start telemetry thread. Call without holding mutex_ to avoid blocking.
    void start_telemetry_thread() {
        // Same double-start guard as start_temp_control_thread.
        std::lock_guard<std::mutex> start_lock(thread_start_mutex_);
        std::string id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (telemetry_thread_.joinable()) {
                return;
            }
            telemetry_thread_stop_.store(false);
            id = camera_id_.value_or("");
        }
        if (id.empty()) {
            return;
        }
        std::thread t([this, id]() {
            auto& sdk = QHYSDKWrapper::instance();
            while (!telemetry_thread_stop_.load()) {
                bool connected = connected_.load();
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    if (!connected || !camera_info_valid_ || !camera_info_.has_cooler) {
                        telemetry_temp_valid_ = false;
                        telemetry_power_valid_ = false;
                    }
                }

                if (!connected) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }

                bool exposing = false;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    exposing = (exposure_status_ == QHYExposureStatus::Working);
                }
                if (exposing) {
                    // Skip this cycle: see the matching comment in
                    // start_temp_control_thread() -- IsQHYCCDControlAvailable
                    // and GetQHYCCDParam share the exposure worker's USB
                    // handle and can wedge its GetQHYCCDSingleFrame call.
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }

                try {
                    if (sdk.is_control_available(id, control::CURTEMP)) {
                        double t = sdk.get_param(id, control::CURTEMP);
                        std::lock_guard<std::mutex> lk(mutex_);
                        telemetry_ccd_temp_c_ = t;
                        telemetry_temp_valid_ = true;
                    }
                } catch (const std::exception& e) {
                    ALPACA_LOG_DEBUG("QHY", "Telemetry CURTEMP read failed: " + std::string(e.what()));
                }

                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
        std::lock_guard<std::mutex> lock(mutex_);
        // Serialised by thread_start_mutex_ (see start_temp_control_thread).
        telemetry_thread_ = std::move(t);
    }

    void start_telemetry_thread_locked() {
        start_telemetry_thread();
    }

    void stop_telemetry_thread_locked() {
        telemetry_thread_stop_.store(true);
        if (telemetry_thread_.joinable()) {
            telemetry_thread_.join();
        }
    }

    void set_bin_locked(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
        }
        if (bin_x <= 0 || bin_y <= 0) {
            throw AlpacaException("Bin value must be positive", AlpacaError::InvalidValue);
        }
        // Hold mutex_ across the whole check + SDK apply, like every other
        // setter (see set_gain): a check/release/write gap would let a racing
        // start_exposure begin integration against half-applied binning. The
        // old "don't hold mutex_ across SDK calls, control_temp holds the SDK
        // mutex ~10s" rationale is stale — control_temp snapshots the handle
        // and releases the wrapper mutex before its blocking PID call, so
        // these are all fast register writes with brief wrapper-mutex holds.
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string id = camera_id_.value_or("");
        const int max_w = static_cast<int>(camera_info_.max_width) / bin_x;
        const int max_h = static_cast<int>(camera_info_.max_height) / bin_y;
        if (!bin_is_supported(id, bin_x)) {
            throw AlpacaException("Bin value not supported: " + std::to_string(bin_x),
                                  AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();
        try {
            QHYSDKWrapper::instance().set_bin_mode(id, static_cast<uint32_t>(bin_x),
                                                   static_cast<uint32_t>(bin_y));
            QHYSDKWrapper::instance().set_resolution(id, 0, 0,
                static_cast<uint32_t>(max_w), static_cast<uint32_t>(max_h));
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("QHY", "Failed to apply binning: " + std::string(e.what()));
            return;
        }
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
        ensure_connected();
        // Do not bounds-check against sensor dimensions here: ConformU "Reject Bad XSize/YSize"
        // tests require that the setter accepts out-of-range values and that StartExposure
        // rejects them. Just store the value; start_exposure validates before calling the SDK.
        std::lock_guard<std::mutex> lock(mutex_);
        const int width = width_opt.value_or(num_x_);
        const int height = height_opt.value_or(num_y_);
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();
        num_x_ = width;
        num_y_ = height;
    }

    void set_start_pos_locked(std::optional<int> start_x_opt, std::optional<int> start_y_opt) {
        ensure_connected();
        // Do not bounds-check start+ROI against sensor dimensions here: ConformU
        // "Reject Bad XStart/YStart" tests require the setter to accept the value and
        // StartExposure to reject it. Just store; start_exposure validates before the SDK call.
        std::lock_guard<std::mutex> lock(mutex_);
        const int start_x = start_x_opt.value_or(start_x_);
        const int start_y = start_y_opt.value_or(start_y_);
        if (start_x < 0 || start_y < 0) {
            throw AlpacaException("Start position must be non-negative", AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();
        start_x_ = start_x;
        start_y_ = start_y;
    }

    ImageArray build_image_array_locked() const {
        ImageArray image;
        image.width  = num_x_;
        image.height = num_y_;

        if (image.width <= 0 || image.height <= 0 || exposure_buffer_.empty()) {
            image.rank = 0;
            return image;
        }

        const uint32_t eff_w = exposure_width_  > 0 ? exposure_width_  : static_cast<uint32_t>(num_x_);
        const uint32_t eff_h = exposure_height_ > 0 ? exposure_height_ : static_cast<uint32_t>(num_y_);
        const uint32_t bpp   = exposure_bpp_;
        const uint32_t ch    = exposure_channels_;

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
                for (std::size_t i = 0; i < copy && src + i < exposure_buffer_.size(); ++i) {
                    image.data[dst + i] = exposure_buffer_[src + i];
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
                        if (src + 1 < exposure_buffer_.size()) {
                            // QHY delivers 16-bit data big-endian
                            uint16_t val = (static_cast<uint16_t>(exposure_buffer_[src]) << 8) |
                                            static_cast<uint16_t>(exposure_buffer_[src + 1]);
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
                        if (src < exposure_buffer_.size()) {
                            image.data[out_idx] = static_cast<std::int32_t>(exposure_buffer_[src]);
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

std::unique_ptr<CameraDriver> create_qhy_camera(int device_number,
                                                 const std::string& camera_id) {
    return std::make_unique<QHYCameraDriver>(device_number, camera_id, std::nullopt);
}

std::unique_ptr<CameraDriver> create_qhy_camera_by_index(int device_number, int camera_index) {
    return std::make_unique<QHYCameraDriver>(device_number, std::nullopt, camera_index);
}

} // namespace alpacacore::vendor::qhy
