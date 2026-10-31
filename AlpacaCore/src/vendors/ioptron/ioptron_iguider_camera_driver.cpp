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

// iOptron iGuider guide camera: a UVC camera behind V4L2 (no vendor SDK).
//
// The camera streams continuously from connect to disconnect, and an exposure
// is the first suitable frame of that stream. Both halves come from the vendor's
// Windows driver, confirmed on Linux by INDI (PR #2012): the stream is never
// stopped between exposures, because a stopped iGuider often does not restart
// (the kernel's uvcvideo driver does not clear the endpoint halt that Windows
// clears), and the stream is started at the shortest exposure and set to the
// requested one only after the first frame, because a stream started at a long
// exposure takes up to ~4x that exposure to deliver anything.
//
// Threads and locks: one stream worker, run in the stream_ slot, owns every
// V4L2 call from the end of connect until disconnect joins it. mutex_ is a
// short state lock and is never held across a V4L2 call; connect does its I/O
// under transition_mutex_ only (decision 0016).
//
// Link loss (decision 0009): a stall or V4L2 error latches a fault, which the
// next frame clears. The link is LOST at once when the camera is gone (an
// unplug: IGuiderDeviceGone), or when a fault has stood for
// util::kLinkStalenessBound; the worker then closes the camera and stops, and
// only the client's Connect relinks.

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/image_validation.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_camera_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_v4l2.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_v4l2_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::ioptron {

namespace {

constexpr const char* kLog = "iGuider";

constexpr int kWidth = 1280;
constexpr int kHeight = 960;
constexpr double kPixelSizeUm = 3.75;
// Sensor figures from the vendor's ASCOM driver (ASCOM.iOptroniGuider.Camera 1.3.x).
constexpr double kElectronsPerAdu = 1.28;
constexpr double kFullWellCapacity = 14500.0;
constexpr int kMaxAdu = 255;

// Gain g in [0, 13] maps onto the UVC gain control's 64..480, step 32.
constexpr int kGainMin = 0;
constexpr int kGainMax = 13;
constexpr std::int32_t gain_to_uvc(int gain) { return 64 + 32 * gain; }

constexpr std::int32_t kManualExposure = 1;  // V4L2_EXPOSURE_MANUAL

// Connect waits this long for the first frame. ConformU abandons a Platform 7
// Connect after 5 s of Connecting, and the first frame at the shortest exposure
// takes ~150 ms (INDI PR #2012).
constexpr auto kFirstFrameTimeout = std::chrono::seconds(3);
// One read_frame() wait: also the worst-case delay before the worker sees a stop.
constexpr auto kReadTimeout = std::chrono::milliseconds(200);
// A frame finishes arriving about one readout after its integration ends
// (2.4 MB at 9 fps over USB 2.0). An unchanged-settings frame is used only when
// it arrived at least duration + this after the request.
constexpr double kReadoutMarginSeconds = 0.12;
// A control write reaches the frames late: measured on the rig (2026-10-21), a
// new exposure first showed on the 3rd or 4th frame after the write, the frame
// in flight and up to two more still at the old value. So the 4th frame after
// a write is the first one used.
constexpr int kSettleFrames = 4;
// No frame for this long latches a link fault and restarts the stream (the
// vendor driver and INDI both use 10 s).
constexpr auto kStallTimeout = std::chrono::seconds(10);
static_assert(kStallTimeout < util::kLinkStalenessBound,
              "decision 0009: the loss bound must exceed the latch threshold");
// After a V4L2 failure, wait this long before restarting the stream.
constexpr auto kRetryInterval = std::chrono::seconds(1);
// An exposure with no frame this long after its last possible frame fails.
constexpr auto kExposureGrace = std::chrono::seconds(10);

std::chrono::nanoseconds seconds_to_duration(double seconds) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds));
}

struct Roi {
    int start_x = 0;
    int start_y = 0;
    int num_x = kWidth;
    int num_y = kHeight;
};

// Whether the subframe lies on the sensor. Subtracts rather than adds: the
// setters accept any StartX/StartY >= 0 and NumX/NumY >= 1, so start + num can
// overflow int (StartX = INT_MAX, NumX = 1 wrapped negative and passed).
bool roi_fits(const Roi& roi) {
    return roi.start_x >= 0 && roi.start_y >= 0 && roi.num_x >= 1 && roi.num_y >= 1 &&
           roi.start_x <= kWidth - roi.num_x && roi.start_y <= kHeight - roi.num_y;
}

ImageArray crop_luma(const std::vector<std::uint8_t>& luma, const Roi& roi) {
    if (luma.size() != static_cast<std::size_t>(kWidth) * kHeight) {
        util::throw_invalid_camera_image("iGuider frame has the wrong size");
    }
    if (!roi_fits(roi)) {
        util::throw_invalid_camera_image("iGuider subframe lies outside the sensor");
    }
    ImageArray image;
    image.width = roi.num_x;
    image.height = roi.num_y;
    image.rank = 2;
    const auto shape = util::validate_image_shape(image);
    image.data.resize(shape.element_count);
    for (int row = 0; row < roi.num_y; ++row) {
        const std::size_t src = static_cast<std::size_t>(row + roi.start_y) * kWidth + roi.start_x;
        const std::size_t dst = static_cast<std::size_t>(row) * roi.num_x;
        std::copy(luma.begin() + static_cast<std::ptrdiff_t>(src),
                  luma.begin() + static_cast<std::ptrdiff_t>(src + roi.num_x),
                  image.data.begin() + static_cast<std::ptrdiff_t>(dst));
    }
    return image;
}

}  // namespace

class IGuiderCameraDriver final : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    IGuiderCameraDriver(int device_number, std::string device_path, IGuiderV4L2& v4l2, util::TaskClock& clock)
        : AsyncConnectable("iGuider"),
          v4l2_(v4l2),
          clock_(clock),
          device_number_(device_number),
          configured_path_(std::move(device_path)),
          stream_("iGuider stream", stream_generation_, clock) {}

    ~IGuiderCameraDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // The teardown itself, not set_connected(false): no virtual call from a
        // destructor (clang-analyzer VirtualCall). No connection task is left
        // to record or consume a disconnect, so the base gates have no work.
        {
            std::lock_guard<std::mutex> transition_lock(transition_mutex_);
            if (connected_.load()) {
                try {
                    stop_and_close();
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN(kLog, "Error during destruction: " + std::string(e.what()));
                }
            }
        }
        // A failed connect never starts the worker; this joins one in any other case.
        stream_.cancel_all_and_join();
    }

    // ---- Common members ---------------------------------------------------

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "iOptron iGuider"; }
    DeviceType get_device_type() const override { return DeviceType::Camera; }
    std::string fallback_unique_id() const override { return "IOPTRON_IGUIDER_" + std::to_string(device_number_); }
    std::string get_description() const override { return "iOptron iGuider guide camera"; }
    std::string get_driver_info() const override { return "AlpacaCore iOptron iGuider Camera Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }  // ICameraV4 (Platform 7)

    bool get_connected() const override { return connected_.load() && !link_lost_.load(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    std::string get_link_fault() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return link_fault_;
    }

    void set_connected(bool connected) override {
        // The connect does seconds of V4L2 I/O; this serialises a sync
        // disconnect against it, which the base gates cannot see (#528).
        std::lock_guard<std::mutex> transition_lock(transition_mutex_);
        // Only the client's Connect relinks a lost link: it tears the dead
        // session down and connects afresh (decision 0009, point 4).
        if (connected && connected_.load() && link_lost_.load()) stop_and_close();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Base gates BEFORE the idempotency check (async_connectable.h).
            if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
            if (connected && consume_pending_disconnect(connected_.load())) return;
            if (connected == connected_.load()) return;
        }
        if (connected) {
            open_and_start();
        } else {
            stop_and_close();
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override { return false; }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    // ---- Capabilities (static) -------------------------------------------

    bool get_can_abort_exposure() const override { return true; }
    bool get_can_asymmetric_bin() const override { return false; }
    bool get_can_fast_readout() const override { return false; }
    bool get_can_get_cooler_power() const override { return false; }
    bool get_can_pulse_guide() const override { return false; }
    bool get_can_set_ccd_temperature() const override { return false; }
    bool get_can_stop_exposure() const override { return false; }
    bool get_has_shutter() const override { return false; }

    // ---- Sensor --------------------------------------------------------------

    int get_camera_x_size() const override { return connected_value(kWidth); }
    int get_camera_y_size() const override { return connected_value(kHeight); }
    double get_pixel_size_x() const override { return connected_value(kPixelSizeUm); }
    double get_pixel_size_y() const override { return connected_value(kPixelSizeUm); }
    int get_max_adu() const override { return connected_value(kMaxAdu); }
    double get_electrons_per_adu() const override { return connected_value(kElectronsPerAdu); }
    double get_full_well_capacity() const override { return connected_value(kFullWellCapacity); }
    SensorType get_sensor_type() const override { return connected_value(SensorType::Monochrome); }
    // The vendor does not publish the sensor part number (an AR0130 by its
    // figures, unconfirmed); ASCOM asks for an empty string when it is unknown.
    std::string get_sensor_name() const override { return connected_value(std::string()); }

    int get_bayer_offset_x() const override { not_implemented_when_connected("BayerOffsetX"); }
    int get_bayer_offset_y() const override { not_implemented_when_connected("BayerOffsetY"); }

    // ---- Binning, subframe, readout ------------------------------------------

    int get_bin_x() const override { return connected_value(1); }
    int get_bin_y() const override { return connected_value(1); }
    void set_bin_x(int bin) override { set_bin(bin); }
    void set_bin_y(int bin) override { set_bin(bin); }
    int get_max_bin_x() const override { return connected_value(1); }
    int get_max_bin_y() const override { return connected_value(1); }

    int get_num_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_link_locked();
        return roi_.num_x;
    }
    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_link_locked();
        return roi_.num_y;
    }
    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_link_locked();
        return roi_.start_x;
    }
    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_link_locked();
        return roi_.start_y;
    }
    // The window is checked against the sensor at StartExposure (ASCOM); a
    // setter refuses only what no window can hold.
    void set_num_x(int value) override { set_roi_member(value, 1, &Roi::num_x); }
    void set_num_y(int value) override { set_roi_member(value, 1, &Roi::num_y); }
    void set_start_x(int value) override { set_roi_member(value, 0, &Roi::start_x); }
    void set_start_y(int value) override { set_roi_member(value, 0, &Roi::start_y); }

    int get_readout_mode() const override { return connected_value(0); }
    void set_readout_mode(int mode) override {
        if (mode != 0) throw AlpacaException("Invalid readout mode", AlpacaError::InvalidValue);
        ensure_connected();
    }
    std::vector<std::string> get_readout_modes() const override {
        return connected_value(std::vector<std::string>{"Normal"});
    }

    bool get_fast_readout() const override { not_implemented_when_connected("FastReadout"); }
    void set_fast_readout(bool) override { not_implemented_when_connected("FastReadout"); }

    // ---- Gain and offset -------------------------------------------------

    int get_gain() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_link_locked();
        return gain_;
    }

    void set_gain(int gain) override {
        if (gain < kGainMin || gain > kGainMax) {
            throw AlpacaException(
                "Gain must be between " + std::to_string(kGainMin) + " and " + std::to_string(kGainMax),
                AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_link_locked();
        gain_ = gain;  // the stream worker writes it before its next frame
    }

    int get_gain_max() const override { return connected_value(kGainMax); }
    int get_gain_min() const override { return connected_value(kGainMin); }
    std::vector<std::string> get_gains() const override { not_implemented_when_connected("Gains"); }

    int get_offset() const override { not_implemented_when_connected("Offset"); }
    void set_offset(int) override { not_implemented_when_connected("Offset"); }
    int get_offset_max() const override { not_implemented_when_connected("OffsetMax"); }
    int get_offset_min() const override { not_implemented_when_connected("OffsetMin"); }
    std::vector<std::string> get_offsets() const override { not_implemented_when_connected("Offsets"); }

    // ---- Cooling (none) ------------------------------------------------------

    double get_ccd_temperature() const override { not_implemented_when_connected("CCDTemperature"); }
    double get_heat_sink_temperature() const override { not_implemented_when_connected("HeatSinkTemperature"); }
    bool get_cooler_on() const override { not_implemented_when_connected("CoolerOn"); }
    void set_cooler_on(bool) override { not_implemented_when_connected("CoolerOn"); }
    double get_cooler_power() const override { not_implemented_when_connected("CoolerPower"); }
    double get_set_ccd_temperature() const override { not_implemented_when_connected("SetCCDTemperature"); }
    void set_set_ccd_temperature(double) override { not_implemented_when_connected("SetCCDTemperature"); }

    // ---- Exposure ------------------------------------------------------------

    double get_exposure_min() const override { return connected_value(kIGuiderDurations.front()); }
    double get_exposure_max() const override { return connected_value(kIGuiderDurations.back()); }
    // The steps are not uniform (0.01 s up to 0.2 s, 0.5 s above 1 s); this is
    // the finest, as the vendor driver reports. StartExposure rounds to a step
    // and LastExposureDuration reports the step taken.
    double get_exposure_resolution() const override { return connected_value(0.01); }

    double get_sub_exposure_duration() const override { not_implemented_when_connected("SubExposureDuration"); }
    void set_sub_exposure_duration(double) override { not_implemented_when_connected("SubExposureDuration"); }

    void start_exposure(double duration, bool /*light: no shutter*/) override {
        if (!std::isfinite(duration) || duration < 0.0 || duration > kIGuiderDurations.back()) {
            throw AlpacaException(
                "Exposure duration must be between 0 and " + std::to_string(kIGuiderDurations.back()) + " seconds",
                AlpacaError::InvalidValue);
        }
        std::shared_ptr<const ImageArray> retired_image;
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!roi_fits(roi_)) {
            throw AlpacaException("Subframe StartX/StartY + NumX/NumY exceeds the 1280x960 sensor",
                                  AlpacaError::InvalidValue);
        }
        ensure_link_locked();
        const std::size_t step = iguider_exposure_step(duration);
        const double taken = kIGuiderDurations[step];
        // The frames the settle waits out run at the exposure being replaced.
        const double replaced = kIGuiderDurations[iguider_tick_step(desired_ticks_)];
        desired_ticks_ = kIGuiderTicks[step];
        exposure_ticks_ = desired_ticks_;
        exposure_duration_ = taken;
        exposure_roi_ = roi_;
        request_time_ = clock_.now();
        // Room for the settle frames at the longer of the two exposures and one
        // frame at the new one, then the grace.
        exposure_deadline_ = request_time_ +
                             seconds_to_duration(kSettleFrames * (std::max(taken, replaced) + kReadoutMarginSeconds) +
                                                 taken + kReadoutMarginSeconds) +
                             kExposureGrace;
        ++exposure_token_;  // supersedes an exposure still in flight
        exposure_pending_ = true;
        image_ready_ = false;
        retired_image = std::move(last_image_);
        exposure_failure_.clear();
    }

    void abort_exposure() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (exposure_pending_) {
            exposure_pending_ = false;
            ++exposure_token_;
        }
    }

    void stop_exposure() override {
        ensure_connected();
        throw AlpacaException("iGuider cannot stop an exposure early (CanStopExposure is false)",
                              AlpacaError::MethodNotImplemented);
    }

    CameraState get_camera_state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        latch_exposure_timeout_locked();
        if (!link_fault_.empty()) return CameraState::Error;
        return exposure_pending_ ? CameraState::Exposing : CameraState::Idle;
    }

    bool get_image_ready() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        latch_exposure_timeout_locked();
        ensure_link_locked();
        throw_exposure_failure_locked();
        return image_ready_;
    }

    ImageArray get_image_array() const override {
        std::shared_ptr<const ImageArray> image;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            latch_exposure_timeout_locked();
            ensure_link_locked();
            throw_exposure_failure_locked();
            if (!image_ready_ || !last_image_) {
                throw AlpacaException::asked_too_early("Image not ready");
            }
            image = last_image_;
        }
        return *image;  // copy outside the state lock
    }

    std::string get_image_array_variant() const override {
        ensure_connected();
        return "Int32";
    }

    double get_percent_completed() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        latch_exposure_timeout_locked();
        ensure_link_locked();
        if (!exposure_pending_) return image_ready_ ? 100.0 : 0.0;
        const double total = exposure_duration_ + kReadoutMarginSeconds;
        const double elapsed = std::chrono::duration<double>(clock_.now() - request_time_).count();
        return std::clamp(elapsed / total * 100.0, 0.0, 99.0);
    }

    double get_last_exposure_duration() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return last_exposure_duration_locked();
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        (void)last_exposure_duration_locked();  // InvalidOperation until an image succeeds.
        // uvcvideo advertises SOE even for host arrival timestamps; no verified iGuider photon-start source.
        throw AlpacaException("iGuider does not provide a verified exposure start timestamp",
                              AlpacaError::PropertyNotImplemented);
    }

    // ---- Guiding (through the mount, not the camera) -----------------------

    bool get_is_pulse_guiding() const override {
        ensure_connected();
        return false;
    }

    void pulse_guide(int, int) override {
        ensure_connected();
        throw AlpacaException("iGuider has no ST-4 port; guide through the mount (CanPulseGuide is false)",
                              AlpacaError::MethodNotImplemented);
    }

private:
    // What the stream worker last wrote to the camera, and its stall bookkeeping.
    struct StreamState {
        std::int32_t applied_ticks = kIGuiderTicks.front();
        std::int32_t applied_gain = gain_to_uvc(kGainMin);
        int frames_since_change = 0;
        util::TaskClock::clock::time_point change_time{};
        util::TaskClock::clock::time_point last_frame{};
        bool awaiting_first_frame = false;  // after a restart: hold the controls until a frame lands
        bool needs_restart = false;
        std::optional<util::TaskClock::clock::time_point> fault_since;  // when the standing fault latched
        bool restart_logged = false;                                    // one INFO restart line per fault
    };

    IGuiderV4L2& v4l2_;
    util::TaskClock& clock_;
    const int device_number_;
    const std::string configured_path_;

    std::atomic<bool> connected_{false};
    // Decision 0009: set under mutex_ by the stream worker when the link is
    // lost, cleared by the next connect. connected_ stays the session flag, so
    // a Disconnect still tears the session down.
    std::atomic<bool> link_lost_{false};
    mutable std::mutex mutex_;  // short state lock; never held across a V4L2 call
    std::mutex transition_mutex_;

    // Guarded by mutex_.
    int handle_ = -1;
    int gain_ = kGainMin;
    std::int32_t desired_ticks_ = kIGuiderTicks.front();
    Roi roi_;
    std::string link_fault_;  // latched stream fault; empty while frames flow, kept after a loss

    // The exposure in flight, guarded by mutex_.
    mutable bool exposure_pending_ = false;
    mutable std::uint64_t exposure_token_ = 0;
    std::int32_t exposure_ticks_ = 0;
    double exposure_duration_ = 0.0;
    Roi exposure_roi_;
    util::TaskClock::clock::time_point request_time_{};
    util::TaskClock::clock::time_point exposure_deadline_{};
    mutable std::string exposure_failure_;

    // The last result, guarded by mutex_.
    bool image_ready_ = false;
    std::shared_ptr<const ImageArray> last_image_;
    // No LastExposure pair: the stream supplies a duration, but no verified UTC photon-start timestamp.
    std::optional<double> last_exposure_duration_;

    // Last members: destroyed first, so the worker is joined before anything it touches.
    util::OperationGeneration stream_generation_;
    util::AsyncOperation stream_;

    void ensure_connected() const {
        if (!connected_.load() || link_lost_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    // A faulted link refuses reads and writes (decision 0009, rule 1). Caller holds mutex_.
    void ensure_link_locked() const {
        if (!link_fault_.empty()) {
            throw AlpacaException("iGuider communications compromised: " + link_fault_, AlpacaError::DriverException);
        }
    }

    template <class T>
    T connected_value(T value) const {
        ensure_connected();
        return value;
    }

    [[noreturn]] void not_implemented_when_connected(const char* member) const {
        ensure_connected();
        throw AlpacaException(std::string(member) + " is not supported by the iGuider",
                              AlpacaError::PropertyNotImplemented);
    }

    void set_bin(int bin) {
        if (bin != 1) throw AlpacaException("The iGuider does not bin; BinX/BinY must be 1", AlpacaError::InvalidValue);
        ensure_connected();
    }

    void set_roi_member(int value, int minimum, int Roi::*member) {
        if (value < minimum) {
            throw AlpacaException("Subframe value " + std::to_string(value) + " is out of range",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_link_locked();
        roi_.*member = value;
    }

    void throw_exposure_failure_locked() const {
        if (!exposure_failure_.empty()) {
            throw AlpacaException(exposure_failure_, AlpacaError::DriverException);
        }
    }

    double last_exposure_duration_locked() const {
        if (!last_exposure_duration_) {
            throw AlpacaException("No image has been successfully acquired yet", AlpacaError::InvalidOperation);
        }
        return *last_exposure_duration_;
    }

    void latch_exposure_timeout_locked() const {
        if (exposure_pending_ && clock_.now() >= exposure_deadline_) {
            ALPACA_LOG_WARN(kLog, "Exposure got no frame before its deadline");
            exposure_failure_ = "iGuider delivered no frame for the exposure; check the USB connection";
            exposure_pending_ = false;
            ++exposure_token_;
        }
    }

    void reset_session_locked(std::shared_ptr<const ImageArray>& retired_image) {
        gain_ = kGainMin;
        desired_ticks_ = kIGuiderTicks.front();
        roi_ = Roi{};
        link_fault_.clear();
        link_lost_.store(false);
        exposure_pending_ = false;
        ++exposure_token_;
        exposure_failure_.clear();
        image_ready_ = false;
        retired_image = std::move(last_image_);
        last_exposure_duration_.reset();
    }

    // ---- Connect / disconnect (under transition_mutex_, never mutex_) ------

    std::string resolve_path() {
        if (!configured_path_.empty()) return configured_path_;
        const auto found = v4l2_.enumerate();
        if (found.empty()) {
            throw AlpacaException(
                "No iOptron iGuider found. Check that it is plugged in (lsusb shows 0130:1466) and that the "
                "service user is in the 'video' group, or set its device path in the web UI.",
                AlpacaError::DriverException);
        }
        if (found.size() > 1) {
            ALPACA_LOG_INFO(kLog, std::to_string(found.size()) + " iGuiders found; using " + found.front().path +
                                      ". Set a device path to choose another.");
        }
        return found.front().path;
    }

    void close_quietly(int handle) noexcept {
        try {
            v4l2_.close(handle);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLog, "Closing the iGuider failed: " + std::string(e.what()));
        }
    }

    void open_and_start() {
        const std::string path = resolve_path();
        const int handle = v4l2_.open(path);
        // Everything after the open closes the handle on failure: nothing else
        // would, since connected_ is still false.
        try {
            v4l2_.set_control(handle, IGuiderControl::AutoExposure, kManualExposure);
            v4l2_.set_control(handle, IGuiderControl::ExposureAbsolute, kIGuiderTicks.front());
            v4l2_.set_control(handle, IGuiderControl::Gain, gain_to_uvc(kGainMin));
            v4l2_.start_stream(handle, kWidth, kHeight);
            wait_for_first_frame(handle, path);
        } catch (const AlpacaException&) {
            close_quietly(handle);
            throw;
        } catch (const std::exception& e) {
            close_quietly(handle);
            throw AlpacaException(std::string("iGuider connect failed: ") + e.what(), AlpacaError::DriverException);
        }

        std::shared_ptr<const ImageArray> retired_image;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handle_ = handle;
            reset_session_locked(retired_image);
        }
        try {
            stream_.start([this, handle](util::OperationContext& ctx) { stream_body(ctx, handle); });
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                handle_ = -1;
            }
            close_quietly(handle);
            throw;
        }
        connected_.store(true);
        ALPACA_LOG_INFO(kLog, "Connected to " + path + " (1280x960 mono, streaming)");
    }

    void wait_for_first_frame(int handle, const std::string& path) {
        std::vector<std::uint8_t> luma;
        const auto deadline = clock_.now() + kFirstFrameTimeout;
        // Also bounded by a count of reads the clock did not move across, so a
        // frozen test clock cannot spin forever. Only those count: a dropped
        // frame returns at once, and counting it would cut the 3 s budget short.
        constexpr int kMaxFrozenReads = static_cast<int>(kFirstFrameTimeout / kReadTimeout);
        int frozen_reads = 0;
        while (frozen_reads < kMaxFrozenReads && clock_.now() < deadline) {
            const auto before = clock_.now();
            if (v4l2_.read_frame(handle, luma, kReadTimeout)) return;
            if (clock_.now() == before) ++frozen_reads;
        }
        throw AlpacaException("iGuider at " + path +
                                  " opened but sent no frames within 3 s. Re-plug it into a USB port on the computer "
                                  "itself (not a hub) and connect again.",
                              AlpacaError::DriverException);
    }

    void stop_and_close() {
        std::shared_ptr<const ImageArray> retired_image;
        int handle = -1;
        {
            // Publish disconnected and clear state BEFORE the close, so an
            // operation racing it fails at its connection check.
            std::lock_guard<std::mutex> lock(mutex_);
            handle = handle_;
            handle_ = -1;
            reset_session_locked(retired_image);
            connected_.store(false);
        }
        stream_.cancel_all_and_join();  // without mutex_ (slot rule 10)
        {
            // The worker may have latched a fault between the clear above and the join.
            std::lock_guard<std::mutex> lock(mutex_);
            link_fault_.clear();
        }
        if (handle >= 0) close_quietly(handle);
        ALPACA_LOG_INFO(kLog, "Disconnected");
    }

    // ---- Stream worker (the only V4L2 caller while connected) --------------

    void stream_body(util::OperationContext& ctx, int handle) {
        std::vector<std::uint8_t> luma;
        StreamState s;
        s.last_frame = clock_.now();
        s.change_time = s.last_frame;
        s.frames_since_change = 1;  // connect already took one frame at these settings
        while (ctx.stop_reason() == util::StopReason::None) {
            try {
                if (s.needs_restart) restart_stream(handle, s);
                apply_settings(handle, s);
                if (v4l2_.read_frame(handle, luma, kReadTimeout)) {
                    on_frame(luma, s);
                } else if (clock_.now() - s.last_frame >= kStallTimeout) {
                    latch_link_fault("no frame for " + std::to_string(kStallTimeout.count()) + " s", s);
                    s.needs_restart = true;
                    s.last_frame = clock_.now();  // one restart per stall period
                }
            } catch (const IGuiderDeviceGone& e) {
                lose_link(handle, e.what());
                return;
            } catch (const std::exception& e) {
                latch_link_fault(e.what(), s);
                s.needs_restart = true;
                if (!ctx.wait_for(kRetryInterval)) return;
            }
            if (s.fault_since && clock_.now() - *s.fault_since >= util::kLinkStalenessBound) {
                lose_link(handle, get_link_fault());
                return;
            }
        }
    }

    void restart_stream(int handle, StreamState& s) {
        if (s.restart_logged) {
            ALPACA_LOG_DEBUG(kLog, "Restarting the iGuider stream again");
        } else {
            ALPACA_LOG_INFO(kLog, "Restarting the iGuider stream");
            s.restart_logged = true;
        }
        v4l2_.stop_stream(handle);
        v4l2_.set_control(handle, IGuiderControl::ExposureAbsolute, kIGuiderTicks.front());
        v4l2_.start_stream(handle, kWidth, kHeight);
        s.applied_ticks = kIGuiderTicks.front();
        s.applied_gain = -1;  // write it again once frames flow
        s.frames_since_change = 0;
        s.change_time = clock_.now();
        s.last_frame = s.change_time;
        s.awaiting_first_frame = true;
        s.needs_restart = false;
    }

    void apply_settings(int handle, StreamState& s) {
        if (s.awaiting_first_frame) return;
        std::int32_t ticks = 0;
        std::int32_t gain = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ticks = desired_ticks_;
            gain = gain_to_uvc(gain_);
        }
        if (ticks != s.applied_ticks) {
            v4l2_.set_control(handle, IGuiderControl::ExposureAbsolute, ticks);
            s.applied_ticks = ticks;
            s.frames_since_change = 0;
            s.change_time = clock_.now();
        }
        if (gain != s.applied_gain) {
            v4l2_.set_control(handle, IGuiderControl::Gain, gain);
            s.applied_gain = gain;
            s.frames_since_change = 0;
            s.change_time = clock_.now();
        }
    }

    void on_frame(const std::vector<std::uint8_t>& luma, StreamState& s) {
        const auto arrival = clock_.now();
        s.last_frame = arrival;
        s.awaiting_first_frame = false;
        s.fault_since.reset();
        s.restart_logged = false;
        ++s.frames_since_change;
        clear_link_fault();

        std::uint64_t token = 0;
        Roi roi;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!exposure_pending_ || s.applied_ticks != exposure_ticks_ || s.frames_since_change < kSettleFrames) {
                return;
            }
            // A frame integrated wholly after the request: either the controls
            // changed after it and the settle frames have passed, or it arrived
            // a full exposure plus readout after it.
            const bool settled_after_request = s.change_time >= request_time_;
            const bool long_enough =
                arrival - request_time_ >= seconds_to_duration(exposure_duration_ + kReadoutMarginSeconds);
            if (!settled_after_request && !long_enough) return;
            token = exposure_token_;
            roi = exposure_roi_;
        }

        auto image = std::make_shared<const ImageArray>(crop_luma(luma, roi));  // outside the lock
        std::lock_guard<std::mutex> lock(mutex_);
        latch_exposure_timeout_locked();
        if (!exposure_pending_ || exposure_token_ != token) return;  // aborted or superseded meanwhile
        last_image_ = std::move(image);
        image_ready_ = true;
        last_exposure_duration_ = exposure_duration_;
        exposure_pending_ = false;
        exposure_failure_.clear();
    }

    void latch_link_fault(const std::string& reason, StreamState& s) {
        if (!s.fault_since) s.fault_since = clock_.now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (link_fault_.empty()) {
            ALPACA_LOG_ERROR(kLog, "iGuider stream fault: " + reason + "; restarting the stream");
        }
        link_fault_ = reason;
    }

    // Decision 0009, point 3: Connected reads false and every member throws
    // NotConnected until the client's Connect; the fault text stays for the
    // management listing until then. The worker closes its own handle (it is
    // the only V4L2 caller); a Disconnect that already took handle_ closes it.
    void lose_link(int handle, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_ != handle) return;  // a Disconnect is tearing the session down
            handle_ = -1;
            link_fault_ = reason;
            if (exposure_pending_) {
                exposure_pending_ = false;
                ++exposure_token_;
            }
            link_lost_.store(true);
        }
        ALPACA_LOG_ERROR(kLog, "iGuider link lost: " + reason + "; connect again to resume");
        close_quietly(handle);
    }

    void clear_link_fault() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!link_fault_.empty()) {
            ALPACA_LOG_INFO(kLog, "iGuider stream recovered");
            link_fault_.clear();
        }
    }
};

std::unique_ptr<CameraDriver> create_iguider_camera(int device_number, const std::string& device_path) {
    return create_iguider_camera(device_number, device_path, IGuiderV4L2Wrapper::instance());
}

std::unique_ptr<CameraDriver> create_iguider_camera(int device_number, const std::string& device_path,
                                                    IGuiderV4L2& v4l2, util::TaskClock& clock) {
    return std::make_unique<IGuiderCameraDriver>(device_number, device_path, v4l2, clock);
}

}  // namespace alpacacore::vendor::ioptron
