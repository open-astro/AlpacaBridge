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
#include <alpacacore/util/image_validation.h>
#include <alpacacore/util/last_exposure.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::vendor::touptek {

namespace {

// FourCC codes reported by Toupcam_get_RawFormat. Bayer layouts here are for
// the raw sensor (not the output after any rotation/flip).
constexpr unsigned kFourCC_RGGB = 0x42474752; // 'R','G','G','B'
constexpr unsigned kFourCC_BGGR = 0x52474742; // 'B','G','G','R'
constexpr unsigned kFourCC_GRBG = 0x47425247; // 'G','R','B','G'
constexpr unsigned kFourCC_GBRG = 0x47524247; // 'G','B','R','G'
constexpr unsigned kFourCC_YYYY = 0x59595959; // mono

ToupBayerPattern four_cc_to_bayer(unsigned four_cc) {
    switch (four_cc) {
    case kFourCC_RGGB: return ToupBayerPattern::RG;
    case kFourCC_BGGR: return ToupBayerPattern::BG;
    case kFourCC_GRBG: return ToupBayerPattern::GR;
    case kFourCC_GBRG: return ToupBayerPattern::GB;
    default:           return ToupBayerPattern::None;
    }
}

std::pair<int, int> bayer_offsets(ToupBayerPattern pattern) {
    switch (pattern) {
    case ToupBayerPattern::RG: return {0, 0};
    case ToupBayerPattern::BG: return {1, 1};
    case ToupBayerPattern::GR: return {1, 0};
    case ToupBayerPattern::GB: return {0, 1};
    default:                   return {0, 0};
    }
}

} // namespace

class ToupTekCameraDriver : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ToupTekCameraDriver(int device_number, int camera_index, ToupTekSDK& sdk, ToupCameraBranding branding,
                        util::TaskClock& clock)
        : AsyncConnectable(branding.label),
          branding_(std::move(branding)),
          sdk_(sdk),
          clock_(clock),
          device_number_(device_number),
          camera_index_(camera_index),
          handle_(nullptr),
          camera_info_(),
          camera_info_valid_(false),
          serial_number_(),
          firmware_version_(),
          connected_(false),
          bin_(1),
          start_x_(0),
          start_y_(0),
          num_x_(0),
          num_y_(0),
          image_ready_(false),
          image_cached_(false),
          last_image_(),
          stream_clean_(false),
          exposure_active_(false),
          pulse_guiding_(false) {
        preload_camera_info();
    }

    ~ToupTekCameraDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        stop_exposure_thread();
        stop_thermal_poller();
        stop_pulse_guide_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(branding_.label, "Error during destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        const_cast<ToupTekCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.name.empty()) {
            return camera_info_.name;
        }
        return branding_.label + " Camera";
    }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return branding_.unique_id_prefix + "_SN_" + serial_number_;
        }
        return branding_.unique_id_prefix + "_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return branding_.label + " Camera Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore " + branding_.label + " Camera Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }  // ICameraV4 (Platform 7)

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // exposure_lifecycle_mutex_ also serialises the thermal poller's
        // spawn (connect path, below) against its join (disconnect path), so
        // the std::thread object is never assigned and joined concurrently.
        // Taken in both directions, before mutex_ (documented lock order).
        std::unique_lock<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        if (!connected) {
            // Join the exposure thread BEFORE taking mutex_ and closing the handle:
            // the thread runs Toupcam_WaitImageV4 on a raw handle snapshot, so
            // closing under it is a use-after-close. It must happen out here —
            // stop_exposure_thread() takes mutex_ itself and joins, so calling it
            // with mutex_ held would self-deadlock. Hold exposure_lifecycle_mutex_
            // from before the join through the close below, so a concurrent
            // start_exposure cannot spawn a fresh thread in the join→close gap
            // (it would block on this mutex, then fail NotConnected after we
            // null the handle). Lock order: exposure_lifecycle_mutex_ -> mutex_.
            stop_exposure_thread();
            // The poller takes mutex_ on every tick, so join it out here too --
            // before mutex_ is taken and the handle closed under it.
            stop_thermal_poller();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            // Connect/disconnect are idempotent (ASCOM): a redundant call while
            // already in the requested state is a no-op. A redundant Connect must
            // NOT reset exposure state — the Platform-7 `connect` endpoint calls
            // connect() unconditionally (no !get_connected() guard, unlike the
            // legacy Connected=true PUT), so wiping here would abort an in-flight
            // exposure (CameraState flips to Idle mid-frame, and a concurrent
            // set_gain/set_readout_mode could then pass ensure_not_exposing and
            // write registers into the live frame) or discard a just-completed image
            // (ImageReady/LastExposure cleared, frame unretrievable). Matches
            // the QHY driver, which already returns here without resetting.
            return;
        }

        auto& sdk = sdk_;

        if (connected) {
            // Enumerate and resolve the target camera.
            auto cameras = sdk.enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No " + branding_.label + " cameras detected", AlpacaError::NotConnected);
            }
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range", AlpacaError::InvalidValue);
            }
            camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
            camera_info_valid_ = true;

            ALPACA_LOG_INFO(branding_.label, "SDK version: " + sdk.get_sdk_version());
            ALPACA_LOG_INFO(branding_.label,
                            "Opening camera index " + std::to_string(camera_index_) + ": " + camera_info_.name);

            handle_ = sdk.open_camera_by_id(camera_info_.id);

            // Configure: disable auto-exposure, enable RAW output, select
            // 16-bit bitdepth when the sensor supports it, and arm software
            // trigger mode before starting pull-mode streaming.
            try {
                sdk.put_auto_exposure(handle_, false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(branding_.label, "put_AutoExpoEnable failed: " + std::string(e.what()));
            }

            try {
                sdk.put_raw(handle_, 1);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(branding_.label, "put_Option(RAW) failed: " + std::string(e.what()));
            }

            if (camera_info_.bit_depth_max > 8) {
                try {
                    sdk.put_bitdepth(handle_, 1);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN(branding_.label, "put_Option(BITDEPTH=1) failed: " + std::string(e.what()));
                }
            }

            // Everything from here until connected_ is set true must release the
            // freshly opened handle on failure: the destructor only closes when
            // connected_, so an unguarded throw (put_trigger_mode, get_serial_number,
            // get_firmware_version, start_pull_mode all propagate) would leak the SDK
            // open — the ref-counted open never balances — and hand a stale handle to
            // the next reconnect. Guard the whole post-open configuration in one try.
            try {
                sdk.put_trigger_mode(handle_, 1);  // software trigger

                // Refresh frame dimensions and Bayer pattern from the open camera.
                int width = 0;
                int height = 0;
                try {
                    sdk.get_size(handle_, width, height);
                } catch (const std::exception& e) {
                    // Best-effort: keep the preloaded sensor size on failure.
                    ALPACA_LOG_DEBUG(branding_.label, "get_Size failed: " + std::string(e.what()));
                }
                if (width > 0 && height > 0) {
                    camera_info_.max_width = width;
                    camera_info_.max_height = height;
                }

                try {
                    unsigned four_cc = 0;
                    unsigned bpp = 0;
                    sdk.get_raw_format(handle_, four_cc, bpp);
                    if (four_cc == kFourCC_YYYY || (camera_info_.flags & 0x10) /* FLAG_MONO */) {
                        camera_info_.is_color = false;
                        camera_info_.bayer = ToupBayerPattern::None;
                    } else {
                        camera_info_.is_color = true;
                        camera_info_.bayer = four_cc_to_bayer(four_cc);
                    }
                    if (bpp > 0) {
                        camera_info_.bit_depth_max = static_cast<int>(bpp);
                    }
                } catch (const std::exception& e) {
                    // Best-effort: keep the preloaded colour/format info on failure.
                    ALPACA_LOG_DEBUG(branding_.label, "get_RawFormat failed: " + std::string(e.what()));
                }

                try {
                    float px = 0.0f;
                    float py = 0.0f;
                    sdk.get_pixel_size(handle_, 0xffffffffu, px, py);
                    if (px > 0.0f) camera_info_.pixel_size_um_x = px;
                    if (py > 0.0f) camera_info_.pixel_size_um_y = py;
                } catch (const std::exception& e) {
                    // Best-effort: keep the preloaded pixel size on failure.
                    ALPACA_LOG_DEBUG(branding_.label, "get_PixelSize failed: " + std::string(e.what()));
                }

                serial_number_ = sdk.get_serial_number(handle_);
                firmware_version_ = sdk.get_firmware_version(handle_);

                bin_ = 1;
                start_x_ = 0;
                start_y_ = 0;
                // Even sensor size (see set_bin_locked / start_exposure): at bin 1
                // this only differs from the raw size on an odd-dimension sensor.
                num_x_ = camera_info_.max_width & ~1;
                num_y_ = camera_info_.max_height & ~1;
                roi_dirty_ = false;
                format_dirty_ = false;

                sdk.start_pull_mode(handle_, &ToupTekCameraDriver::on_event_static, this);
            } catch (const AlpacaException&) {
                sdk.close_camera(handle_);
                handle_ = nullptr;
                throw;
            } catch (const std::exception& e) {
                sdk.close_camera(handle_);
                handle_ = nullptr;
                throw AlpacaException("Failed to configure " + branding_.label + " camera: " + e.what(),
                                      AlpacaError::DriverException);
            }

            {
                // Same invariant as the redundant-reconnect path above: clear
                // exposure_active_ (inside reset_exposure_state_locked) under
                // readout_mutex_. No exposure thread runs yet here, but keeping every
                // caller consistent means the invariant holds by construction.
                std::lock_guard<std::mutex> rlock(readout_mutex_);
                reset_exposure_state_locked();
                // start_pull_mode above established a reusable stream. A client
                // may send AbortExposure while no exposure is active (PHD2 does
                // this when Loop stops); that must not tear this stream down.
                stream_clean_ = true;
            }
            connected_.store(true);
            // Spawned with mutex_ still held (never joined here -- see the top
            // of this function); its first tick blocks on mutex_ until
            // set_connected returns, then primes the caches before the client's
            // first DeviceState poll.
            start_thermal_poller();
            return;
        }

        // Disconnecting. Hold readout_mutex_ (after mutex_, preserving lock order)
        // across the close so any in-flight get/set_readout_mode / set_gain /
        // set_offset — which run under readout_mutex_ with a handle snapshot — has
        // completed before Toupcam_Close, preventing a stale-handle SDK call. The
        // exposure_active_ store also belongs under readout_mutex_ (the documented
        // invariant that the flag only changes under that lock).
        std::lock_guard<std::mutex> rlock(readout_mutex_);
        exposure_active_.store(false);
        if (handle_) {
            try { sdk.stop(handle_); } catch (const std::exception&) {}
            sdk.close_camera(handle_);
            handle_ = nullptr;
        }
        camera_info_ = {};
        camera_info_valid_ = false;
        serial_number_.clear();
        firmware_version_.clear();
        reset_exposure_state_locked();
        connected_.store(false);
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name),
                              AlpacaError::ActionNotImplemented);
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

    int get_bayer_offset_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not supported", AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer).first;
    }
    int get_bayer_offset_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not supported", AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer).second;
    }

    int get_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_;
    }
    void set_bin_x(int bin_x) override { set_bin_locked(bin_x, bin_x); }
    int get_bin_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_;
    }
    void set_bin_y(int bin_y) override { set_bin_locked(bin_y, bin_y); }

    CameraState get_camera_state() const override {
        ensure_connected();
        if (exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_deadline_valid_ && clock_.now() >= exposure_deadline_) {
                ALPACA_LOG_WARN(branding_.label, "Exposure deadline exceeded; forcing CameraState=Idle.");
                // Publish the false-transition under readout_mutex_ too, so the
                // invariant "exposure_active_ only changes under readout_mutex_"
                // (which set_readout_mode relies on) holds on this path as well.
                // Lock order is mutex_ -> readout_mutex_, held consistently.
                std::lock_guard<std::mutex> rlock(readout_mutex_);
                exposure_active_.store(false);
                exposure_deadline_valid_ = false;
                return CameraState::Idle;
            }
            return CameraState::Exposing;
        }
        return CameraState::Idle;
    }

    int get_camera_x_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.max_width : 0;
    }
    int get_camera_y_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.max_height : 0;
    }

    bool get_can_abort_exposure() const override { return true; }
    bool get_can_asymmetric_bin() const override { return false; }
    bool get_can_fast_readout() const override { return false; }
    bool get_can_get_cooler_power() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_cooler;
    }
    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_pulse_guide;
    }
    bool get_can_set_ccd_temperature() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_tec_onoff;
    }
    bool get_can_stop_exposure() const override { return true; }

    // Toupcam_get_Temperature and the TEC get_Option reads are synchronous USB
    // control transfers (~50-70 ms each on an SBC). DeviceState reads
    // CCDTemperature, CoolerPower and HeatSinkTemperature in one call, so served
    // live that is ~0.17 s against a 0.1 s fast-response target -- and ConformU
    // times the FIRST DeviceState after connect, so an on-demand cache cannot
    // help. A background poller (thermal_poll_loop) refreshes the readings every
    // kThermalPollInterval; the getters serve the cached value while it is
    // younger than kThermalCacheMaxAge and fall back to a live read otherwise.
    double get_ccd_temperature() const override {
        ensure_connected();
        auto& sdk = sdk_;
        return with_handle([&](HToupcam h) {
            if (!cache_fresh_locked(temperature_cache_valid_, temperature_cache_time_)) {
                read_temperature_locked(h, sdk);
            }
            return temperature_cache_c_;
        });
    }

    bool get_cooler_on() const override {
        ensure_connected();
        if (!get_can_get_cooler_power()) return false;
        auto& sdk = sdk_;
        return with_handle([&](HToupcam h) { return sdk.get_tec_enable(h); });
    }
    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (!get_can_get_cooler_power()) {
            if (cooler_on) {
                throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
            }
            return;
        }
        auto& sdk = sdk_;
        with_handle([&](HToupcam h) { sdk.put_tec_enable(h, cooler_on); });
    }
    double get_cooler_power() const override {
        ensure_connected();
        if (!get_can_get_cooler_power()) return 0.0;
        auto& sdk = sdk_;
        try {
            // Both reads under one with_handle so they come from the same open.
            // Served from the thermal poller's cache when fresh (see
            // get_ccd_temperature); TEC_VOLTAGE_MAX is a per-model constant read
            // once per connection.
            int v = 0;
            int vmax = 0;
            with_handle([&](HToupcam h) {
                if (!cache_fresh_locked(tec_cache_valid_, tec_cache_time_)) read_tec_voltage_locked(h, sdk);
                v = tec_voltage_deciV_;
                vmax = tec_voltage_max_deciV_;
            });
            if (vmax <= 0) return 0.0;
            double pct = (static_cast<double>(v) / static_cast<double>(vmax)) * 100.0;
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            return pct;
        } catch (const std::exception&) {
            return 0.0;
        }
    }

    double get_electrons_per_adu() const override {
        ensure_connected();
        // The ToupTek SDK exposes no e-/ADU, so there is no honest value (issue #1277).
        throw AlpacaException("ElectronsPerADU is not available for " + branding_.label + " cameras",
                              AlpacaError::PropertyNotImplemented);
    }

    double get_exposure_max() const override {
        ensure_connected();
        auto& sdk = sdk_;
        auto r = with_handle([&](HToupcam h) { return sdk.get_exposure_range(h); });
        return static_cast<double>(r.max_us) / 1'000'000.0;
    }
    double get_exposure_min() const override {
        ensure_connected();
        auto& sdk = sdk_;
        auto r = with_handle([&](HToupcam h) { return sdk.get_exposure_range(h); });
        return static_cast<double>(r.min_us) / 1'000'000.0;
    }
    double get_exposure_resolution() const override { return 0.000001; }

    bool get_fast_readout() const override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }
    void set_fast_readout(bool) override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    double get_full_well_capacity() const override {
        ensure_connected();
        // The SDK exposes no full-well figure (only the High Full Well mode switch); the ADU saturation is not
        // electrons, so there is no honest value (issue #1277).
        throw AlpacaException("FullWellCapacity is not available for " + branding_.label + " cameras",
                              AlpacaError::PropertyNotImplemented);
    }

    int get_gain() const override {
        ensure_connected();
        auto& sdk = sdk_;
        return static_cast<int>(with_handle([&](HToupcam h) { return sdk.get_gain(h); }));
    }
    void set_gain(int gain) override {
        ensure_connected();
        auto& sdk = sdk_;
        // Read the valid range, range-check, and write the register under mutex_ +
        // readout_mutex_ held together (same fix as set_offset). The range was
        // previously read OUTSIDE readout_mutex_, so a concurrent reconnect to a
        // different model (different gain range) could stale the bound between the
        // check and the write. Holding both locks also makes the exposure check
        // atomic with the write, and reads handle_ directly under the lock rather
        // than a pre-lock snapshot. Lock order mutex_ -> readout_mutex_ -> SDK.
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> rlock(readout_mutex_);
        if (!handle_) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        auto range = sdk.get_gain_range(handle_);
        if (gain < range.min || gain > range.max) {
            throw AlpacaException("Gain out of range", AlpacaError::InvalidValue);
        }
        ensure_not_exposing();
        sdk.put_gain(handle_, static_cast<unsigned short>(gain));
    }
    int get_gain_max() const override {
        ensure_connected();
        auto& sdk = sdk_;
        return static_cast<int>(with_handle([&](HToupcam h) { return sdk.get_gain_range(h).max; }));
    }
    int get_gain_min() const override {
        ensure_connected();
        auto& sdk = sdk_;
        return static_cast<int>(with_handle([&](HToupcam h) { return sdk.get_gain_range(h).min; }));
    }
    std::vector<std::string> get_gains() const override {
        throw AlpacaException("Gain descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_has_shutter() const override { return false; }

    double get_heat_sink_temperature() const override { return get_ccd_temperature(); }

    ImageArray get_image_array() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (!exposure_failure_.empty()) {
            throw AlpacaException(exposure_failure_, AlpacaError::DriverException);
        }
        if (!image_ready_ || !image_cached_) {
            throw AlpacaException::asked_too_early("Image not ready");
        }
        return last_image_;
    }
    std::string get_image_array_variant() const override { return "Int32"; }

    bool get_image_ready() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!exposure_failure_.empty()) {
            throw AlpacaException(exposure_failure_, AlpacaError::DriverException);
        }
        return image_ready_ && image_cached_;
    }

    bool get_is_pulse_guiding() const override {
        if (!connected_.load()) return false;
        if (!pulse_guiding_.load()) return false;
        try {
            auto& sdk = sdk_;
            bool guiding = with_handle([&](HToupcam h) { return sdk.is_guiding(h); });
            if (!guiding) pulse_guiding_.store(false);
            return guiding;
        } catch (const std::exception&) {
            return false;
        }
    }

    double get_last_exposure_duration() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return last_exposure_.duration();
    }
    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return last_exposure_.start_time();
    }

    int get_max_adu() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.bit_depth_max <= 0) return 0;
        return static_cast<int>((1ULL << camera_info_.bit_depth_max) - 1ULL);
    }

    int get_max_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_.supported_bins.empty()) return 1;
        return *std::max_element(camera_info_.supported_bins.begin(),
                                 camera_info_.supported_bins.end());
    }
    int get_max_bin_y() const override { return get_max_bin_x(); }

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

    // ASCOM Offset maps to the ToupTek black level (TOUPCAM_OPTION_BLACKLEVEL),
    // available on cameras that report TOUPCAM_FLAG_BLACKLEVEL (e.g. the ATR2600M
    // / IMX571). Cameras without it throw PropertyNotImplemented, as before.
    int get_offset() const override {
        ensure_connected();
        auto& sdk = sdk_;
        return with_handle([&](HToupcam h) {
            ensure_blacklevel_supported_locked();  // same mutex_ hold as the read
            return sdk.get_blacklevel(h);
        });
    }
    void set_offset(int offset) override {
        ensure_connected();
        auto& sdk = sdk_;
        // Compute the max bound (which scales with the live bit depth), range-check,
        // and write the register under mutex_ + readout_mutex_ held together. The
        // bound was previously computed OUTSIDE readout_mutex_, so a concurrent
        // reconnect that changed the bit depth between the check and the write left
        // the accepted bound stale (spurious SDK DriverException). Holding both locks
        // also makes the exposure check atomic with the write (start_exposure
        // publishes exposure_active_ under readout_mutex_). Lock order mutex_ ->
        // readout_mutex_ -> SDK, so the bound is read from handle_ directly, not via
        // offset_max_value()/handle_copy() (which would re-take mutex_).
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> rlock(readout_mutex_);
        if (!handle_) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        ensure_blacklevel_supported_locked();  // same mutex_ hold as the write
        const int max = sdk.get_blacklevel_max(handle_, camera_info_.bit_depth_max);
        if (offset < 0 || offset > max) {
            throw AlpacaException("Offset out of range", AlpacaError::InvalidValue);
        }
        ensure_not_exposing();
        sdk.put_blacklevel(handle_, offset);
    }
    int get_offset_max() const override {
        ensure_connected();
        return offset_max_value();  // support checked inside, under the same lock
    }
    int get_offset_min() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        ensure_blacklevel_supported_locked();
        return 0;  // TOUPCAM_BLACKLEVEL_MIN
    }
    std::vector<std::string> get_offsets() const override {
        // Integer offset mode (OffsetMin/OffsetMax), so the named-offsets list is
        // deliberately not implemented, matching the other camera drivers.
        throw AlpacaException("Offset descriptions not supported",
                              AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        ensure_connected();
        if (!exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            return image_ready_ ? 100.0 : 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_exposure_duration_ <= 0.0 || !exposure_triggered_) return 0.0;
        double elapsed = std::chrono::duration<double>(clock_.now() - exposure_start_steady_).count();
        double pct = (elapsed / pending_exposure_duration_) * 100.0;
        if (pct < 0.0) return 0.0;
        if (pct > 100.0) return 100.0;
        return pct;
    }

    double get_pixel_size_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_um_x : 0.0;
    }
    double get_pixel_size_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_um_y : 0.0;
    }

    // ASCOM ReadoutModes fold the ToupTek conversion-gain (HCG/LCG/HDR) and High
    // Full Well hardware modes into one flat, NINA-friendly dropdown. Each mode
    // fully specifies the state it applies on both axes, so reads round-trip to a
    // stable index. Cameras with neither capability keep the single "Normal"
    // mode and behave exactly as before. See readout_mode_specs().
    int get_readout_mode() const override {
        ensure_connected();  // ASCOM: properties throw NotConnected when
                             // disconnected — even the single-mode early return
                             // below must not short-circuit that (matches the setter)
        auto& sdk = sdk_;
        std::vector<ReadoutModeSpec> specs;
        int cur_cg = 0;
        bool cur_hfw = false;
        {
            // Derive the mode list, the capability flags (which axes exist), AND
            // read both SDK registers under mutex_ + readout_mutex_ held together,
            // so all come from ONE connection: a disconnect (or disconnect+reconnect
            // to a different model) cannot leave a pre-lock specs snapshot paired
            // with register reads from the new handle. This mirrors set_readout_mode,
            // which likewise derives its spec via readout_mode_specs_locked() under
            // the same locks. readout_mutex_ also makes the two register reads atomic
            // w.r.t. set_readout_mode's two-step CG+HFW apply.
            std::lock_guard<std::mutex> lock(mutex_);
            std::lock_guard<std::mutex> rlock(readout_mutex_);
            if (!handle_) {
                throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
            }
            specs = readout_mode_specs_locked();
            const bool has_cg = camera_info_valid_ && camera_info_.supports_cg;
            const bool has_hfw = camera_info_valid_ && camera_info_.supports_high_fullwell;
            // Normal-only camera: neither axis exists, so no SDK register read is
            // issued (cur_cg/cur_hfw stay at their defaults) — the size==1 shortcut
            // below then reports mode 0 without a wasted round-trip.
            cur_cg = has_cg ? sdk.get_cg(handle_) : 0;
            cur_hfw = has_hfw && sdk.get_high_fullwell(handle_) != 0;
        }
        if (specs.size() == 1) {
            return 0;  // Only "Normal".
        }
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const auto& s = specs[i];
            if ((!s.set_cg || s.cg == cur_cg) && (!s.set_hfw || s.hfw == cur_hfw)) {
                return static_cast<int>(i);
            }
        }
        // Current CG/HFW register combination isn't one our specs enumerate (each
        // spec fully determines both axes, so this should be unreachable unless
        // the camera powers up in an undocumented mode). Log it so a real
        // occurrence is observable instead of silently reporting mode 0.
        ALPACA_LOG_WARN(branding_.label, "Readout mode registers (CG=" + std::to_string(cur_cg) + ", HFW=" +
                                             (cur_hfw ? "1" : "0") + ") match no enumerated mode; reporting mode 0");
        return 0;
    }
    void set_readout_mode(int mode) override {
        // Range check first so an out-of-range index is InvalidValue even while
        // disconnected (same precedence as the switch driver's validate_switch_id).
        // While disconnected the list is just "Normal", so only 0 is accepted; the
        // authoritative re-check against the live model happens under the lock below.
        if (mode < 0 || mode >= static_cast<int>(readout_mode_specs().size())) {
            throw AlpacaException("Readout mode index out of range", AlpacaError::InvalidValue);
        }
        // Then require a connection, so a Normal-only camera still throws
        // NotConnected here rather than silently succeeding on the early-return.
        ensure_connected();

        auto& sdk = sdk_;
        // Derive the spec, validate the handle, and apply the CG/HFW writes with
        // mutex_ + readout_mutex_ held together, reading handle_ AND the mode list
        // from ONE connection snapshot (both off camera_info_/handle_ under mutex_).
        // This matches get_readout_mode, which likewise re-derives capabilities
        // under both locks: a disconnect+reconnect to a different model between the
        // pre-lock range check and here cannot pair an old model's CG/HFW spec with
        // the new handle. readout_mutex_ keeps the two-axis apply atomic w.r.t.
        // get_readout_mode's paired reads, and the exposure check under it closes the
        // TOCTOU where an exposure could begin between the check and the writes.
        // Lock order mutex_ -> readout_mutex_.
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> rlock(readout_mutex_);
        if (!handle_) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        const auto specs = readout_mode_specs_locked();
        // Re-validate against the live model: a reconnect could have swapped to a
        // camera with fewer modes since the pre-lock range check above.
        if (mode < 0 || mode >= static_cast<int>(specs.size())) {
            throw AlpacaException("Readout mode index out of range", AlpacaError::InvalidValue);
        }
        const auto& s = specs[static_cast<std::size_t>(mode)];
        ensure_not_exposing();
        // A single "Normal" mode sets neither axis: no SDK write, but the
        // ensure_not_exposing() check above still rejects a mid-exposure change.
        //
        // Write order matters when LEAVING High Full Well: disable HFW before
        // writing CG, in case the firmware treats the two as mutually exclusive
        // and rejects OPTION_CG while OPTION_HIGH_FULLWELL is still enabled —
        // that would throw with only half the mode applied (camera stuck in
        // HFW). Entering HFW keeps the CG-first order for the same reason.
        if (s.set_hfw && !s.hfw) {
            sdk.put_high_fullwell(handle_, false);
        }
        if (s.set_cg) {
            sdk.put_cg(handle_, s.cg);
        }
        if (s.set_hfw && s.hfw) {
            sdk.put_high_fullwell(handle_, true);
        }
    }
    std::vector<std::string> get_readout_modes() const override {
        // Deliberately NO connection check: every other camera driver (ZWO, QHY,
        // SVBONY, Player One) returns a static/cached list while disconnected,
        // and imaging clients (NINA/APT/SGPro) read ReadoutModes during device
        // enumeration before connecting to populate dropdowns. Disconnected, the
        // list derives from the preloaded camera_info_ caps (or just "Normal");
        // the index getter/setter still require a live handle.
        const auto specs = readout_mode_specs();
        std::vector<std::string> names;
        names.reserve(specs.size());
        for (const auto& s : specs) {
            names.push_back(s.name);
        }
        return names;
    }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.model_name.empty()) {
            return camera_info_.model_name;
        }
        return std::string();  // unknown: "" (ICameraV4 SensorName)
    }
    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) return SensorType::Monochrome;
        return SensorType::RGGB;
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        auto& sdk = sdk_;
        int t = with_handle([&](HToupcam h) { return sdk.get_tec_target_deciC(h); });
        return static_cast<double>(t) / 10.0;
    }
    void set_set_ccd_temperature(double temperature) override {
        ensure_connected();
        int deci = static_cast<int>(std::lround(temperature * 10.0));
        auto& sdk = sdk_;
        with_handle([&](HToupcam h) { sdk.put_tec_target_deciC(h, deci); });
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

    void abort_exposure() override { stop_exposure(); }

    void pulse_guide(int direction, int duration) override {
        ensure_connected();
        if (!get_can_pulse_guide()) {
            throw AlpacaException("Pulse guide not supported", AlpacaError::NotImplemented);
        }
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid pulse guide direction", AlpacaError::InvalidValue);
        }
        if (duration <= 0) {
            throw AlpacaException("Invalid pulse guide duration", AlpacaError::InvalidValue);
        }
        ToupGuideDirection dir = ToupGuideDirection::North;
        switch (direction) {
        case 0: dir = ToupGuideDirection::North; break;
        case 1: dir = ToupGuideDirection::South; break;
        case 2: dir = ToupGuideDirection::East;  break;
        case 3: dir = ToupGuideDirection::West;  break;
        }
        auto& sdk = sdk_;
        // Serialise pulse guides against each other (M18): two concurrent
        // calls would otherwise both run stop_pulse_guide_thread's join and
        // race the thread-assignment below (double-join / join-vs-operator= is
        // UB on std::thread). Held across the SDK write + timer restart so a
        // superseding pulse replaces the previous one atomically. Lock order:
        // pulse_guide_lifecycle_mutex_ -> mutex_ (via with_handle); the timer
        // thread never takes it, so the join below can't deadlock.
        std::lock_guard<std::mutex> pg_lifecycle_lock(pulse_guide_lifecycle_mutex_);
        with_handle([&](HToupcam h) { sdk.pulse_guide(h, dir, static_cast<unsigned>(duration)); });
        // Cancel + join any prior timer (fast — the cancel wakes it), then start a
        // fresh joinable one. On cancel the old timer leaves pulse_guiding_ alone so
        // there's no flicker when one pulse immediately supersedes another.
        stop_pulse_guide_thread();
        {
            std::lock_guard<std::mutex> plock(pulse_guide_mutex_);
            pulse_guide_cancel_ = false;
        }
        pulse_guiding_.store(true);
        pulse_guide_thread_ = std::thread([this, duration]() {
            std::unique_lock<std::mutex> plock(pulse_guide_mutex_);
            const bool cancelled = clock_.wait_for(plock, pulse_guide_cv_, std::chrono::milliseconds(duration),
                                                   [this] { return pulse_guide_cancel_; });
            if (!cancelled) {
                pulse_guiding_.store(false);  // natural completion
            }
        });
    }

    // --- Background thermal poller ---------------------------------------
    // All cache fields are guarded by mutex_ (callers hold it via with_handle).

    // During an exposure the last reading is served regardless of age: no
    // thermal USB control transfer may be issued while Toupcam_WaitImageV4 is
    // pending (the SDK answers E_UNEXPECTED / drops the frame, seen on the
    // ATR585M at bin 3-4), and a 1-3 s old temperature is harmless.
    // Freshness is tracked PER quantity (temperature vs TEC voltage): a
    // temperature-only fallback read must not mark the TEC reading fresh, or
    // DeviceState (temperature first, then CoolerPower) right after connect
    // would serve the zero-initialised TEC values -- and keep serving them for
    // a whole exposure, since the freshness check is bypassed while exposing.
    bool cache_fresh_locked(bool valid, util::TaskClock::clock::time_point stamped) const {
        if (!valid) return false;
        if (exposure_active_.load()) return true;
        return clock_.now() - stamped < kThermalCacheMaxAge;
    }

    // Each live read stamps only its own reading, so a getter's fallback read
    // (poller stalled or not yet primed) is reused by the next caller within
    // kThermalCacheMaxAge instead of every call hitting the SDK.
    void read_temperature_locked(HToupcam h, ToupTekSDK& sdk) const {
        temperature_cache_c_ = static_cast<double>(sdk.get_temperature_deciC(h)) / 10.0;
        temperature_cache_time_ = clock_.now();
        temperature_cache_valid_ = true;
    }

    void read_tec_voltage_locked(HToupcam h, ToupTekSDK& sdk) const {
        tec_voltage_deciV_ = sdk.get_tec_voltage_deciV(h);
        if (!tec_voltage_max_valid_) {
            tec_voltage_max_deciV_ = sdk.get_tec_voltage_max_deciV(h);
            tec_voltage_max_valid_ = true;
        }
        tec_cache_time_ = clock_.now();
        tec_cache_valid_ = true;
    }

    // Caller guarantees no poller thread is live (set_connected joins it first).
    void start_thermal_poller() {
        if (thermal_thread_.joinable()) return;  // defensive: never join under mutex_
        {
            std::lock_guard<std::mutex> tlock(thermal_mutex_);
            thermal_stop_ = false;
        }
        thermal_thread_ = std::thread([this] { thermal_poll_loop(); });
    }

    void stop_thermal_poller() {
        {
            std::lock_guard<std::mutex> tlock(thermal_mutex_);
            thermal_stop_ = true;
        }
        thermal_cv_.notify_all();
        if (thermal_thread_.joinable()) thermal_thread_.join();
    }

    void thermal_poll_loop() {
        auto& sdk = sdk_;
        bool first = true;
        for (;;) {
            {
                std::unique_lock<std::mutex> tlock(thermal_mutex_);
                // First tick runs immediately (primes the cache); later ticks
                // wait out the interval unless stopped.
                if (!first) {
                    clock_.wait_for(tlock, thermal_cv_, kThermalPollInterval, [this] { return thermal_stop_; });
                }
                first = false;
                if (thermal_stop_) return;
            }
            // Never touch the SDK while a frame is integrating (see
            // cache_fresh_locked); the cache just ages until it ends.
            if (exposure_active_.load()) continue;
            try {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!handle_ || !connected_.load()) return;
                if (exposure_active_.load()) continue;
                read_temperature_locked(handle_, sdk);
                if (camera_info_valid_ && camera_info_.supports_cooler) {
                    read_tec_voltage_locked(handle_, sdk);
                }
            } catch (const std::exception& e) {
                // Transient SDK failure (e.g. USB hiccup mid-exposure): keep the
                // last good values; the getters fall back to a live read once
                // the cache ages out.
                ALPACA_LOG_DEBUG(branding_.label, "Thermal poll failed: " + std::string(e.what()));
            }
        }
    }

    void stop_pulse_guide_thread() {
        {
            std::lock_guard<std::mutex> plock(pulse_guide_mutex_);
            pulse_guide_cancel_ = true;
        }
        pulse_guide_cv_.notify_all();
        if (pulse_guide_thread_.joinable()) {
            pulse_guide_thread_.join();
        }
    }

    void start_exposure(double duration, bool light) override {
        (void)light;

        if (!std::isfinite(duration) || duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative", AlpacaError::InvalidValue);
        }
        ensure_connected();

        // Held for the whole function, through the thread spawn at the end: a
        // disconnect holds this same mutex from its exposure-thread join through
        // Toupcam_Close, so a spawn can never slip into that join→close gap and
        // hand the new thread a handle that is about to be freed. If the
        // disconnect wins the lock, handle_copy() below throws NotConnected.
        // Also serialises spawn against the joins in stop_exposure — concurrent
        // join vs thread-assignment on the same std::thread is UB.
        // Lock order: exposure_lifecycle_mutex_ -> mutex_ (all locks below nest).
        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);

        auto& sdk = sdk_;
        // Snapshot the handle for the exposure thread below (line ~1011). The
        // thread's use is safe because stop_exposure_thread() joins it before any
        // set_connected(false) can Toupcam_Close the handle (both under
        // exposure_lifecycle_mutex_). The range read here, however, runs
        // synchronously on THIS thread, so it still goes through with_handle(),
        // which holds mutex_ across Toupcam_get_ExpTimeRange.
        HToupcam handle = handle_copy();
        auto range = with_handle([&](HToupcam h) { return sdk.get_exposure_range(h); });

        const double rounded_us = std::max(std::round(duration * 1'000'000.0), static_cast<double>(range.min_us));
        if (rounded_us > range.max_us) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }
        const unsigned exposure_us = static_cast<unsigned>(rounded_us);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            validate_exposure_roi_locked();
        }
        // Validate before cancelling anything; snapshot after the join because
        // stopping the previous worker dirties the stream configuration.
        stop_exposure_thread();

        int active_bin = 0;
        int active_start_x = 0;
        int active_start_y = 0;
        int active_num_x = 0;
        int active_num_y = 0;
        // Frame size the SDK will report for that ROI (see below): the worker
        // treats anything smaller as a malformed frame.
        int expected_w = 0;
        int expected_h = 0;
        // Sensor-coordinate ROI actually programmed into the SDK (see below).
        unsigned roi_x = 0;
        unsigned roi_y = 0;
        unsigned roi_w = 0;
        unsigned roi_h = 0;
        bool dirty_format = false;
        bool dirty_roi = false;
        bool active_16bit = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!camera_info_valid_) {
                throw AlpacaException("Camera info not valid", AlpacaError::DriverException);
            }
            // Pull at the sensor's actual bit depth (M14): connect only enables
            // the 16-bit BITDEPTH option when bit_depth_max > 8, so a RAW8-only
            // camera streams 1 byte/pixel — pulling it as 16-bit with a 2-byte
            // row pitch garbles every frame. Snapshot the mode with the rest of
            // the geometry so the worker's bits/pitch/buffer stay consistent.
            active_16bit = camera_info_.bit_depth_max > 8;
            active_bin = bin_;
            active_start_x = start_x_;
            active_start_y = start_y_;
            active_num_x = num_x_;
            active_num_y = num_y_;
            dirty_format = format_dirty_;
            dirty_roi = roi_dirty_;

            // Bound the binned dimensions by the EVEN sensor size, not the raw
            // size. Toupcam_put_Roi requires an even sensor-coordinate span, so
            // the largest deliverable binned width is floor(even_max/bin); a
            // client that requested floor(raw/bin) on an odd-width sensor would
            // otherwise ask for a span that, once even-rounded, exceeds the
            // sensor and clamps back to fewer than num columns (a black edge
            // column). Deriving max from the even size makes that unreachable.
            const int even_max_w = camera_info_.max_width & ~1;
            const int even_max_h = camera_info_.max_height & ~1;
            validate_exposure_roi_locked();

            // Toupcam_put_Roi requires an even sensor-coordinate offset, width,
            // and height, and the ROI must fit the sensor. The binned span is
            // (num * bin); for odd bin factors that product can be odd (e.g.
            // 3x3 → 4167), which the SDK rejects — no frame is delivered and
            // ImageReady never sets. Round the span UP to even and the offset
            // DOWN to even. If the +1 pad pushes the right/bottom edge past the
            // sensor, shift the even offset left/up by the overflow (at most
            // one sensor pixel) instead of shrinking the span. What the SDK
            // then delivers for the span is worked out below (expected_w/h);
            // the pixel buffer is sized from that, not from num.
            int span_w = active_num_x * active_bin;
            int span_h = active_num_y * active_bin;
            span_w += (span_w & 1);
            span_h += (span_h & 1);
            // Defensive only: with max_w/max_h derived from the even sensor size
            // above, ceil_even(num*bin) <= even_max, so these clamps never fire.
            if (span_w > even_max_w) span_w = even_max_w;
            if (span_h > even_max_h) span_h = even_max_h;
            int roi_x_i = (active_start_x * active_bin) & ~1;
            int roi_y_i = (active_start_y * active_bin) & ~1;
            // Digital binning ALSO rounds the OUTPUT size down to even
            // (toupcam.h, TOUPCAM_OPTION_BINNING: "640/3 to get 212"), so an odd
            // NumX/NumY at bin >= 2 comes back one pixel short of num even though
            // the sensor span above was even (#977, measured on the SC571CC and
            // SC715C: span 2002 at bin 2 delivers 1000, not 1001). Where the
            // sensor has the room, grow the span to the next even OUTPUT size
            // ((num+1)*bin) at the requested origin: the SDK then delivers
            // num+1 real pixels and the buffer stride crops the last one (see
            // the NB below). Where it does not fit (num is the odd maximum of
            // the axis, e.g. 4168/3 = 1389, or an edge-touching sub-frame), keep
            // the ungrown span: the SDK delivers num-1 and build_image_array
            // fills the one missing edge row/column from its neighbour, since
            // that pixel does not exist on the sensor. The origin is never
            // moved by a whole binned pixel to make the grown span fit.
            if (active_bin >= 2 && (active_num_x & 1)) {
                const int grown = (active_num_x + 1) * active_bin;
                if (roi_x_i + grown <= even_max_w) span_w = grown;
            }
            if (active_bin >= 2 && (active_num_y & 1)) {
                const int grown = (active_num_y + 1) * active_bin;
                if (roi_y_i + grown <= even_max_h) span_h = grown;
            }
            // NB: at bin 1 an odd NumX/NumY (the +1 even pad), and at bin >= 2 the
            // grown span, make the SDK frame one pixel wider/taller than
            // requested, so got_w/got_h from WaitImageV4 can exceed
            // active_num_x/active_num_y — this is NOT an off-by-one. The worker
            // lays the frame out at that wider stride and build_image_array reads
            // the first active_num_x columns of each row (WaitImageV4 rejects a
            // rowPitch shorter than the delivered row with E_INVALIDARG, measured
            // on the GPCMOS01200KPF, so the stride can never be the requested
            // width). Reported NumX/NumY stay = active_num_x/y.
            // What the SDK will report for this ROI: the even-padded span at bin
            // 1, floor(span/bin) rounded down to even otherwise. The worker
            // rejects anything smaller as a malformed frame (#913); a value one
            // below num is the documented edge fill, never a short frame.
            expected_w = active_bin >= 2 ? (span_w / active_bin) & ~1 : span_w;
            expected_h = active_bin >= 2 ? (span_h / active_bin) & ~1 : span_h;
            // Only the +1 even pad of the ungrown span can push the edge past the
            // sensor here (the grown span is applied only when it fits).
            if (roi_x_i + span_w > even_max_w) roi_x_i = (even_max_w - span_w) & ~1;
            if (roi_y_i + span_h > even_max_h) roi_y_i = (even_max_h - span_h) & ~1;
            if (roi_x_i < 0) roi_x_i = 0;
            if (roi_y_i < 0) roi_y_i = 0;
            roi_x = static_cast<unsigned>(roi_x_i);
            roi_y = static_cast<unsigned>(roi_y_i);
            roi_w = static_cast<unsigned>(span_w);
            roi_h = static_cast<unsigned>(span_h);

            format_dirty_ = false;
            roi_dirty_ = false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_exposure_duration_ = exposure_us / 1'000'000.0;
            exposure_triggered_ = false;
            exposure_failure_.clear();
            image_ready_ = false;
            image_cached_ = false;
            // From this point until a frame is published successfully, an abort,
            // timeout or failure must stop and re-initialise the stream before it
            // is reused. The worker marks it clean only in the generation-checked
            // publication block at the end.
            stream_clean_ = false;
            exposure_deadline_ = clock_.now() + std::chrono::microseconds(exposure_us) + std::chrono::seconds(15);
            exposure_deadline_valid_ = true;
        }

        {
            // Publish exposure_active_ under readout_mutex_ so set_readout_mode's
            // exposure check (also under readout_mutex_) can't race the start of an
            // integration and write CG/HFW mid-frame (TOCTOU close).
            std::lock_guard<std::mutex> rlock(readout_mutex_);
            exposure_active_.store(true);
        }

        const uint64_t abort_generation = abort_generation_.load();
        exposure_thread_ = std::thread([this, handle, abort_generation, exposure_us, active_bin, active_num_x,
                                        active_num_y, expected_w, expected_h, roi_x, roi_y, roi_w, roi_h, dirty_format,
                                        dirty_roi, active_16bit]() {
            auto& sdk_local = sdk_;
            // Track which reconfigure stages actually completed, so the catch
            // re-marks ONLY the stage that failed — re-marking an
            // already-applied stage would trigger a spurious stream restart on
            // the next exposure.
            bool format_applied = false;
            bool roi_applied = false;
            bool frame_ready = false;
            std::chrono::system_clock::time_point candidate_start;
            try {
                // Reconfiguring bitdepth / pixel format / binning requires the
                // stream to be stopped (SDK returns E_WRONG_THREAD otherwise).
                if (dirty_format) {
                    try {
                        sdk_local.stop(handle);
                    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                        // Stopping an already-stopped stream is harmless; ignore.
                    }
                    sdk_local.put_binning(handle, active_bin);
                    sdk_local.put_trigger_mode(handle, 1);
                    {
                        // An abort that landed during the restart stopped the stream
                        // already; starting it again would undo the abort and park
                        // the worker in a frame wait. Check under mutex_ (the abort
                        // bumps the generation under the same lock as its stop()).
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (abort_generation_.load() != abort_generation) throw ExposureAborted{};
                        sdk_local.start_pull_mode(handle, &ToupTekCameraDriver::on_event_static, this);
                    }
                    format_applied = true;  // flag cleared at snapshot time
                }

                if (dirty_roi) {
                    sdk_local.put_roi(handle, roi_x, roi_y, roi_w, roi_h);
                    roi_applied = true;  // flag cleared at snapshot time
                }

                sdk_local.put_exposure_us(handle, exposure_us);
                // Stamp LastExposureStartTime at the SDK trigger — the point the
                // exposure actually begins — not at start_exposure() call time.
                // Reconfiguring a dirty format/ROI (stop stream, re-configure,
                // restart) can take ~2 s before the trigger, and a timestamp set
                // at the call would read that much early, failing ConformU 4.5.0's
                // LastExposureStartTime accuracy check.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (abort_generation_.load() != abort_generation) throw ExposureAborted{};
                    candidate_start = std::chrono::system_clock::now();
                    exposure_start_steady_ = clock_.now();
                    sdk_local.trigger(handle, 1);
                    exposure_triggered_ = true;
                }

                unsigned timeout_ms = (exposure_us / 1000) + 10000;
                // Buffer rows are stride_px wide at the sensor's byte depth (RAW8
                // = 1 byte/pixel, >8-bit = 16-bit little-endian; M14): the frame
                // the SDK will deliver (expected_w, one wider than num for an odd
                // NumX) or num_x when the SDK delivers one column fewer (the edge
                // fill). The row count is the larger of the two the same way,
                // plus two rows of crash-insurance: the SDK writes got_h rows
                // before any check here can run.
                const std::size_t bytes_per_pixel = active_16bit ? 2 : 1;
                const int stride_px = std::max(expected_w, active_num_x);
                const int buffer_rows = std::max(expected_h, active_num_y) + 2;
                std::vector<std::uint8_t> pixel_buffer(static_cast<std::size_t>(stride_px) * bytes_per_pixel *
                                                       static_cast<std::size_t>(buffer_rows));

                unsigned got_w = 0;
                unsigned got_h = 0;
                // rowPitch = stride_px * bytes_per_pixel: Toupcam_WaitImageV4's
                // nRowPitch is the destination row stride in OUR buffer and must
                // hold a whole delivered row — a pitch shorter than the frame's
                // row (the requested width against a one-wider frame) is
                // E_INVALIDARG on hardware. build_image_array reads the first
                // active_num_x columns of each stride_px-wide row.
                bool got = sdk_local.wait_image(handle, timeout_ms, pixel_buffer.data(), active_16bit ? 16 : 8,
                                                stride_px * static_cast<int>(bytes_per_pixel), got_w, got_h);

                if (!got || !exposure_active_.load()) {
                    const char* failure_message = got ? "A frame arrived after the exposure was no longer active"
                                                      : "The camera failed to deliver the exposure frame";
                    ALPACA_LOG_WARN(branding_.label, failure_message);
                    // Publish the false-transition under readout_mutex_ so the
                    // invariant "exposure_active_ only changes under readout_mutex_"
                    // holds on the exposure thread's own exit paths too (matching
                    // the disconnect and deadline paths). Taken alone here — the
                    // thread holds neither mutex_ nor the SDK lock — so it respects
                    // the mutex_ -> readout_mutex_ order, and no join site holds
                    // readout_mutex_, so this cannot deadlock the joining thread.
                    {
                        std::lock_guard<std::mutex> rlock(readout_mutex_);
                        exposure_active_.store(false);
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (abort_generation_.load() == abort_generation) {
                        exposure_failure_ = failure_message;
                    }
                    return;
                }

                if (got_w > static_cast<unsigned>(std::numeric_limits<int>::max()) ||
                    got_h > static_cast<unsigned>(std::numeric_limits<int>::max())) {
                    alpacacore::util::throw_invalid_camera_image("ToupTek frame dimensions exceed supported limits");
                }
                // The ROI math in start_exposure predicts the SDK's frame size
                // exactly (even-padded span, even-rounded binned output). A
                // frame below that is malformed (#913) and must not be padded
                // into a plausible image; a frame at it that is still one row or
                // column short of num is the sensor-edge fill (#977).
                if (::alpacacore::logging::get_log_level() <= ::alpacacore::logging::LogLevel::Debug) {
                    ALPACA_LOG_DEBUG("ToupTek", "Frame " + std::to_string(got_w) + "x" + std::to_string(got_h) +
                                                    " for ROI " + std::to_string(roi_w) + "x" + std::to_string(roi_h) +
                                                    " at bin " + std::to_string(active_bin) + " (expected " +
                                                    std::to_string(expected_w) + "x" + std::to_string(expected_h) +
                                                    ", requested " + std::to_string(active_num_x) + "x" +
                                                    std::to_string(active_num_y) + ")");
                }
                if (got_w < static_cast<unsigned>(expected_w) || got_h < static_cast<unsigned>(expected_h)) {
                    alpacacore::util::throw_invalid_camera_image(
                        "ToupTek frame is smaller than the frame the ROI predicts");
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ImageArray image =
                        build_image_array(pixel_buffer, active_16bit, stride_px, active_num_x, active_num_y,
                                          static_cast<int>(got_w), static_cast<int>(got_h));
                    alpacacore::util::validate_image_array(image);
                    last_image_ = std::move(image);
                    image_cached_ = true;
                    // image_ready_ is intentionally NOT set here — it is published
                    // AFTER exposure_active_ is cleared below, so a poller never
                    // observes ImageReady=true while camera_state still reports
                    // Exposing (an ASCOM-forbidden combination). The frame is fully
                    // captured, so remaining Exposing during this build is fine.
                }
                frame_ready = true;
            } catch (const ExposureAborted&) {
                // Aborted before the frame started: the abort already forced the
                // next exposure to re-init the stream; nothing to re-mark.
                ALPACA_LOG_DEBUG(branding_.label, "Exposure aborted before the frame started");
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(branding_.label, "Exposure failed: " + std::string(e.what()));
                // Re-mark only the stage that did NOT complete, so the next
                // exposure re-applies the failed reconfigure without needlessly
                // restarting the stream for one that already succeeded.
                std::lock_guard<std::mutex> lock(mutex_);
                if (abort_generation_.load() == abort_generation) {
                    exposure_failure_ = e.what();
                    image_ready_ = false;
                    image_cached_ = false;
                }
                if (dirty_format && !format_applied) format_dirty_ = true;
                if (dirty_roi && !roi_applied) roi_dirty_ = true;
            }
            // Clear exposure_active_ FIRST (under readout_mutex_, matching the
            // abort path) so the camera leaves the Exposing state, THEN publish
            // image_ready_ — never the reverse, or a poller could observe
            // ImageReady=true while camera_state still returns Exposing. This also
            // keeps every exposure_active_ transition on this thread under
            // readout_mutex_ (the documented invariant).
            {
                std::lock_guard<std::mutex> rlock(readout_mutex_);
                exposure_active_.store(false);
            }
            if (frame_ready) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (abort_generation_.load() == abort_generation && exposure_deadline_valid_ &&
                    clock_.now() < exposure_deadline_) {
                    last_exposure_.commit(exposure_us / 1'000'000.0, candidate_start);
                    image_ready_ = true;
                    stream_clean_ = true;
                } else {
                    image_cached_ = false;
                    last_image_ = {};
                    if (abort_generation_.load() == abort_generation) {
                        exposure_failure_ = "Exposure deadline exceeded before frame publication";
                    }
                }
            }
        });
    }

    void stop_exposure() override {
        ensure_connected();
        // Serialise this stop+join against start_exposure's spawn (join racing the
        // thread-assignment is UB on std::thread) and against the disconnect's
        // join+close. Taken before mutex_, preserving lock order.
        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        // A completed exposure leaves a joinable thread but a reusable pull-mode
        // stream. Join that thread without stopping the stream. For an active,
        // failed or timed-out exposure, stop() still wakes WaitImageV4 promptly
        // (a 10-min frame would otherwise take ~10 min to abort). Hold mutex_
        // across stop() so a concurrent disconnect can't close the handle in the
        // gap. Do NOT pre-clear exposure_active_: the worker owns that transition.
        bool stream_stopped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_stopped = stop_unclean_stream_locked();
        }
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        exposure_failure_.clear();  // a failure from an already completed exposure
        image_cached_ = false;
        exposure_deadline_valid_ = false;
        if (stream_stopped) {
            // Stream was stopped; force the next exposure to re-init it. Mark the
            // ROI dirty too as a defensive safety net in case a future SDK resets
            // ROI on restart.
            format_dirty_ = true;
            roi_dirty_ = true;
        }
    }

private:
    // Injected SDK seam (issue #104): production passes the singleton
    // wrapper; tests pass a scripted fake. Reference outlives the driver
    // (singleton, or test-scoped fake created before the driver).
    // Identity strings (ToupTek, or an OEM brand on the same SDK); fixed at
    // construction, so read without a lock.
    const ToupCameraBranding branding_;
    ToupTekSDK& sdk_;
    util::TaskClock& clock_;
    int device_number_;
    int camera_index_;
    HToupcam handle_;
    ToupCameraInfo camera_info_;
    bool camera_info_valid_;
    std::string serial_number_;
    std::string firmware_version_;

    std::atomic<bool> connected_;
    // Bumped by stop_exposure() under mutex_, together with its stop(). The
    // exposure worker compares it under mutex_ before restarting the stream and
    // before the trigger, so an abort landing mid-restart is not undone.
    std::atomic<uint64_t> abort_generation_{0};
    struct ExposureAborted {};
    mutable std::mutex mutex_;
    // Serialises the exposure thread's LIFECYCLE: spawn (start_exposure) vs
    // join + handle close (set_connected(false)) vs join (stop_exposure). Without
    // it, a start_exposure landing between the disconnect's join and its
    // Toupcam_Close could spawn a fresh thread whose handle snapshot is then
    // closed under it (use-after-close inside WaitImageV4) — and two joiners /
    // a joiner racing the spawn's thread-assignment is UB on std::thread.
    // Lock order: exposure_lifecycle_mutex_ -> mutex_ -> readout_mutex_ -> SDK.
    // The exposure thread itself never takes it, so joins under it can't deadlock.
    std::mutex exposure_lifecycle_mutex_;
    // Serialises the two-step CG+HFW apply in set_readout_mode against the
    // paired reads in get_readout_mode, so a reader never observes a
    // half-applied combination that isn't in the enumerated specs.
    mutable std::mutex readout_mutex_;

    int bin_;
    int start_x_;
    int start_y_;
    int num_x_;
    int num_y_;
    bool roi_dirty_{false};
    bool format_dirty_{false};

    mutable bool image_ready_;
    mutable bool image_cached_;
    mutable ImageArray last_image_;
    util::LastExposure last_exposure_;
    double pending_exposure_duration_{0.0};
    util::TaskClock::clock::time_point exposure_start_steady_{};
    bool exposure_triggered_{false};
    std::string exposure_failure_;
    // Guarded by mutex_. True only when pull mode is running and the last
    // exposure (if any) published a frame without an abort/failure/timeout.
    // A joinable std::thread does not imply a live exposure: completed threads
    // remain joinable until the next request reaps them.
    bool stream_clean_;

    mutable std::atomic<bool> exposure_active_;
    std::thread exposure_thread_;
    mutable util::TaskClock::clock::time_point exposure_deadline_{};
    mutable bool exposure_deadline_valid_{false};

    // Thermal cache fed by thermal_poll_loop (all guarded by mutex_).
    static constexpr std::chrono::milliseconds kThermalPollInterval{1000};
    static constexpr std::chrono::milliseconds kThermalCacheMaxAge{3000};
    mutable double temperature_cache_c_{0.0};
    mutable int tec_voltage_deciV_{0};
    mutable int tec_voltage_max_deciV_{0};
    mutable bool tec_voltage_max_valid_{false};
    mutable util::TaskClock::clock::time_point temperature_cache_time_{};
    mutable bool temperature_cache_valid_{false};
    mutable util::TaskClock::clock::time_point tec_cache_time_{};
    mutable bool tec_cache_valid_{false};
    // Poller lifecycle: member thread + stop flag/cv so disconnect and the
    // destructor can wake and join it (never detached).
    std::thread thermal_thread_;
    std::mutex thermal_mutex_;
    std::condition_variable thermal_cv_;
    bool thermal_stop_{false};

    mutable std::atomic<bool> pulse_guiding_;
    // Joinable timer that clears pulse_guiding_ after the pulse duration. Kept as a
    // member (not detached) with a cancel flag + cv so the destructor can wake and
    // join it — a detached thread writing pulse_guiding_ after destruction is UB.
    std::thread pulse_guide_thread_;
    std::mutex pulse_guide_mutex_;
    std::condition_variable pulse_guide_cv_;
    bool pulse_guide_cancel_{false};
    // Serialises the pulse-guide timer's lifecycle: concurrent pulse_guide
    // calls would double-join pulse_guide_thread_ / race its assignment (UB
    // on std::thread; M18). The destructor's stop_pulse_guide_thread runs
    // after shutdown, when no client call can be in flight, so it does not
    // take it (same exemption as exposure_lifecycle_mutex_).
    std::mutex pulse_guide_lifecycle_mutex_;

    static void on_event_static(unsigned /*event*/, void* /*ctx*/) {
        // Pull-mode callbacks fire on an SDK-owned thread. Do not touch the
        // handle here — close/stop from this context deadlocks (SDK contract).
        // Frame delivery is handled by Toupcam_WaitImageV4 on the exposure
        // thread, so this callback is intentionally a no-op.
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    HToupcam handle_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!handle_) {
            throw AlpacaException("Camera handle is null", AlpacaError::NotConnected);
        }
        return handle_;
    }

    // Runs fn(handle_) with mutex_ HELD across the SDK call. set_connected(false)
    // closes the handle under mutex_, so holding it here means a concurrent
    // disconnect cannot Toupcam_Close the handle mid-call — closing the
    // use-after-close window that the bare handle_copy() snapshot idiom left open
    // (it released mutex_ before the SDK call). Use this for every fast SDK option
    // read/write. Do NOT use it for blocking SDK calls (Toupcam_WaitImageV4) or
    // the exposure thread, which must not hold mutex_. Callers that also need
    // readout_mutex_ (set_gain/set_offset/get/set_readout_mode) take both locks
    // directly instead, to stay atomic against the two-step readout-mode apply.
    // Trailing return type (not bare `auto`) so the type is known at the call
    // sites above this definition — a deduced `auto` return can't be used before
    // the function's definition is seen.
    template <typename Fn>
    auto with_handle(Fn&& fn) const -> decltype(fn(handle_)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!handle_) {
            throw AlpacaException("Camera handle is null", AlpacaError::NotConnected);
        }
        return fn(handle_);
    }

    // Reject runtime sensor-register writes (readout mode / gain / offset) while
    // a frame is integrating: the exposure thread is blocked in wait_image
    // holding no lock, so a mid-exposure register write would race the live
    // integration and yield a mixed-state or stalled frame with no error.
    void ensure_not_exposing() const {
        if (exposure_active_.load()) {
            throw AlpacaException("Cannot change camera settings during an exposure", AlpacaError::InvalidOperation);
        }
    }

    void validate_exposure_roi_locked() const {
        const int max_w = (camera_info_.max_width & ~1) / bin_;
        const int max_h = (camera_info_.max_height & ~1) / bin_;
        if (num_x_ <= 0 || num_y_ <= 0) {
            throw AlpacaException("ROI not valid", AlpacaError::InvalidValue);
        }
        if (num_x_ > max_w || num_y_ > max_h) {
            throw AlpacaException("ROI size exceeds sensor dimensions", AlpacaError::InvalidValue);
        }
        if (start_x_ < 0 || start_y_ < 0 || start_x_ > max_w - num_x_ || start_y_ > max_h - num_y_) {
            throw AlpacaException("ROI extends beyond sensor bounds", AlpacaError::InvalidValue);
        }
    }

    bool supports_cooler_locked_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.supports_cooler;
    }

    // One ASCOM ReadoutMode entry. set_cg / set_hfw say whether this camera has
    // that axis (uniform across all specs for a given camera); cg / hfw are the
    // values that mode applies.
    struct ReadoutModeSpec {
        std::string name;
        bool set_cg{};
        int cg{};
        bool set_hfw{};
        bool hfw{};
    };

    // Build the readout-mode list from the camera's conversion-gain (HCG/LCG/HDR)
    // and High Full Well capabilities. Order is stable so an index means the same
    // mode across get/set. Always returns at least one entry ("Normal").
    std::vector<ReadoutModeSpec> readout_mode_specs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return readout_mode_specs_locked();
    }
    // Assumes mutex_ is held. Callers that also hold readout_mutex_ and read
    // handle_ (get/set_readout_mode) use this to derive the mode list from the
    // SAME connection snapshot as the handle, so a disconnect+reconnect to a
    // different model can't pair an old model's spec with the new handle.
    std::vector<ReadoutModeSpec> readout_mode_specs_locked() const {
        const bool cg = camera_info_valid_ && camera_info_.supports_cg;
        const bool cghdr = camera_info_valid_ && camera_info_.supports_cghdr;
        const bool hfw = camera_info_valid_ && camera_info_.supports_high_fullwell;
        std::vector<ReadoutModeSpec> specs;
        if (cg && hfw) {
            specs.push_back({"HCG", true, 1, true, false});
            specs.push_back({"LCG", true, 0, true, false});
            specs.push_back({"High Full Well", true, 0, true, true});
            if (cghdr) {
                specs.push_back({"HDR", true, 2, true, false});
            }
        } else if (cg) {
            specs.push_back({"HCG", true, 1, false, false});
            specs.push_back({"LCG", true, 0, false, false});
            if (cghdr) {
                specs.push_back({"HDR", true, 2, false, false});
            }
        } else if (hfw) {
            specs.push_back({"Normal", false, 0, true, false});
            specs.push_back({"High Full Well", false, 0, true, true});
        } else {
            specs.push_back({"Normal", false, 0, false, false});
        }
        return specs;
    }

    // Assumes mutex_ is held. Checked inside the SAME lock hold as the SDK
    // call/return it guards: a take-and-release helper left a TOCTOU where a
    // reconnect to a model without TOUPCAM_FLAG_BLACKLEVEL between the check and
    // the body surfaced an SDK error (DriverException) instead of the expected
    // PropertyNotImplemented.
    void ensure_blacklevel_supported_locked() const {
        if (!camera_info_valid_ || !camera_info_.supports_blacklevel) {
            throw AlpacaException("Offset (black level) not supported by this camera",
                                  AlpacaError::PropertyNotImplemented);
        }
    }

    int offset_max_value() const {
        auto& sdk = sdk_;
        // handle_ and camera_info_.bit_depth_max read together under one mutex_ hold
        // (via with_handle), so the SDK read uses a live handle and a consistent
        // bit depth.
        return with_handle([&](HToupcam h) {
            ensure_blacklevel_supported_locked();  // same mutex_ hold as the read
            return sdk.get_blacklevel_max(h, camera_info_.bit_depth_max);
        });
    }

    void reset_exposure_state_locked() {
        image_ready_ = false;
        exposure_failure_.clear();
        temperature_cache_valid_ = false;
        tec_cache_valid_ = false;
        tec_voltage_max_valid_ = false;
        image_cached_ = false;
        last_exposure_.clear();
        pending_exposure_duration_ = 0.0;
        exposure_triggered_ = false;
        stream_clean_ = false;
        exposure_active_.store(false);
        exposure_deadline_valid_ = false;
    }

    // Assumes mutex_ is held. Returns true when the pull-mode stream was not
    // reusable and therefore had to be stopped (or treated as stopped if the
    // best-effort SDK call failed).
    bool stop_unclean_stream_locked() {
        if (stream_clean_) return false;
        abort_generation_.fetch_add(1);
        if (handle_) {
            try {
                sdk_.stop(handle_);
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            }
        }
        return true;
    }

    // Callers must hold exposure_lifecycle_mutex_ (start_exposure, stop_exposure,
    // set_connected(false)) — the join must not race a concurrent spawn's
    // thread-assignment. Sole exception: the destructor, which runs after the
    // connection thread is joined and no client calls can be in flight.
    void stop_exposure_thread() {
        if (!exposure_thread_.joinable()) return;
        // A successfully completed worker leaves the pull-mode stream reusable;
        // join it without Toupcam_Stop. Any other joinable worker is active,
        // failed, timed out or aborted, so stop the stream to wake WaitImageV4 and
        // force a clean restart. stream_clean_ and abort_generation_ are checked
        // and changed under the same mutex used by the worker's publication tail.
        bool stream_stopped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_stopped = stop_unclean_stream_locked();
        }
        exposure_thread_.join();
        if (stream_stopped) {
            std::lock_guard<std::mutex> lock(mutex_);
            // Stream was stopped; re-init format + ROI on the next exposure (ROI
            // is a defensive safety net — see stop_exposure).
            format_dirty_ = true;
            roi_dirty_ = true;
        }
    }

    void preload_camera_info() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            auto cameras = sdk_.enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG(branding_.label, "Preload enumerate failed: " + std::string(e.what()));
        }
    }

    void refresh_cached_camera_info_if_needed() {
        if (connected_.load()) return;
        try {
            auto cameras = sdk_.enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                std::lock_guard<std::mutex> lock(mutex_);
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception&) {}
    }

    void set_bin_locked(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // Check the exposure state under readout_mutex_ (lock order mutex_ ->
        // readout_mutex_) so it is atomic with the geometry mutation below and with
        // start_exposure publishing exposure_active_ — a bare pre-lock check is a
        // TOCTOU (matches set_gain / set_readout_mode).
        std::lock_guard<std::mutex> rlock(readout_mutex_);
        ensure_not_exposing();
        if (!camera_info_valid_) {
            throw AlpacaException("Camera info not valid", AlpacaError::DriverException);
        }
        if (std::find(camera_info_.supported_bins.begin(),
                      camera_info_.supported_bins.end(),
                      bin_x) == camera_info_.supported_bins.end()) {
            throw AlpacaException("Bin value not supported", AlpacaError::InvalidValue);
        }
        if (bin_ == bin_x) return;
        bin_ = bin_x;
        // Bound by the EVEN sensor size, matching start_exposure's max_w/max_h
        // (Toupcam_put_Roi needs an even sensor span). Using the raw size here
        // would let num_x_ exceed what start_exposure allows on an odd-width
        // sensor and throw "ROI size exceeds sensor dimensions" at full frame.
        num_x_ = (camera_info_.max_width & ~1) / bin_;
        num_y_ = (camera_info_.max_height & ~1) / bin_;
        start_x_ = 0;
        start_y_ = 0;
        format_dirty_ = true;
        roi_dirty_ = true;
        image_cached_ = false;
    }

    // width/height (or sx/sy) of std::nullopt means "leave that axis unchanged",
    // resolved UNDER mutex_: each public setter passes only its own axis, so a
    // concurrent setter for the other axis can no longer be clobbered by a stale
    // pre-lock get_num_x()/get_num_y() snapshot (lost-update TOCTOU).
    void set_roi_size_locked(std::optional<int> width_opt, std::optional<int> height_opt) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        std::lock_guard<std::mutex> rlock(readout_mutex_);  // TOCTOU close — see set_bin_locked
        const int width = width_opt.value_or(num_x_);
        const int height = height_opt.value_or(num_y_);
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        ensure_not_exposing();
        if (num_x_ == width && num_y_ == height) return;
        num_x_ = width;
        num_y_ = height;
        roi_dirty_ = true;
        image_cached_ = false;
    }

    // NOTE: the ROI origin snaps DOWN to an even sensor-coordinate boundary at
    // exposure time (Toupcam_put_Roi requires an even offset, and on a colour
    // sensor even alignment is mandatory to preserve the Bayer phase — an odd
    // offset would swap the colour filter pattern and corrupt debayering). The
    // sensor offset is start * bin, so for even bin factors every StartX is
    // already on the grid; for odd bin factors (notably bin 1) an odd StartX
    // resolves to the next lower even sensor column, i.e. the origin can land one
    // pixel before the requested StartX. This snap is intentional and consistent
    // (a guider's relative centroids are unaffected); callers needing an exact
    // origin should align StartX/StartY to a 2-pixel boundary at bin 1.
    void set_start_pos_locked(std::optional<int> sx_opt, std::optional<int> sy_opt) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        std::lock_guard<std::mutex> rlock(readout_mutex_);  // TOCTOU close — see set_bin_locked
        const int sx = sx_opt.value_or(start_x_);
        const int sy = sy_opt.value_or(start_y_);
        if (sx < 0 || sy < 0) {
            throw AlpacaException("Start position must be non-negative",
                                  AlpacaError::InvalidValue);
        }
        ensure_not_exposing();
        if (start_x_ == sx && start_y_ == sy) return;
        start_x_ = sx;
        start_y_ = sy;
        roi_dirty_ = true;
    }

    // Buffer holds stride_px-wide rows at 1 (RAW8) or 2 (16-bit, little-endian
    // native) bytes per pixel — the same stride passed as WaitImageV4's
    // nRowPitch (M14); stride_px >= out_width, and the first out_width columns
    // of each row are the image.
    ImageArray build_image_array(const std::vector<std::uint8_t>& buffer, bool is_16bit, int stride_px, int out_width,
                                 int out_height, int actual_width, int actual_height) const {
        ImageArray image;
        image.width = out_width;
        image.height = out_height;
        image.rank = 2;
        if (out_width <= 0 || out_height <= 0 || actual_width <= 0 || actual_height <= 0 || stride_px < out_width) {
            alpacacore::util::throw_invalid_camera_image("invalid ToupTek frame dimensions");
        }
        // start_exposure() sizes the sensor-space ROI so the SDK delivers at
        // least num pixels per axis wherever the sensor has them. The one case
        // it cannot is an odd num at the sensor edge with bin >= 2: digital
        // binning rounds its output down to even, and the extra row or column
        // that would fix it is off the sensor (#977). That frame is exactly one
        // short; fill the missing edge from its neighbour rather than fail
        // every full-frame exposure at that bin. Anything shorter than that is
        // a malformed frame (#913), and the exposure worker has already checked
        // the frame against the size the ROI math predicted.
        if (actual_width + 1 < out_width || actual_height + 1 < out_height) {
            alpacacore::util::throw_invalid_camera_image(
                "ToupTek frame is more than one pixel smaller than the requested ROI");
        }
        const int last_col = std::min(actual_width, out_width) - 1;
        const int last_row = std::min(actual_height, out_height) - 1;
        const std::size_t bpp = is_16bit ? 2 : 1;
        const std::size_t output_pixels = static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height);
        const std::size_t stored_pixels = static_cast<std::size_t>(stride_px) * static_cast<std::size_t>(out_height);
        if (stored_pixels > buffer.size() / bpp) {
            alpacacore::util::throw_invalid_camera_image("ToupTek frame buffer is shorter than its dimensions");
        }
        image.data.resize(output_pixels);
        for (int row = 0; row < out_height; ++row) {
            for (int col = 0; col < out_width; ++col) {
                // Rows are laid out at stride_px (WaitImageV4's nRowPitch); a
                // delivered width above out_width is cropped by reading only the
                // first out_width columns, and one below it has only the last
                // column missing, which reads its neighbour.
                const std::size_t src_idx =
                    static_cast<std::size_t>(std::min(row, last_row)) * static_cast<std::size_t>(stride_px) +
                    static_cast<std::size_t>(std::min(col, last_col));
                const std::size_t dst_idx =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(out_width) + static_cast<std::size_t>(col);
                const std::size_t byte_idx = src_idx * bpp;
                if (byte_idx + bpp <= buffer.size()) {
                    image.data[dst_idx] =
                        is_16bit ? static_cast<std::int32_t>(static_cast<std::uint16_t>(buffer[byte_idx]) |
                                                             (static_cast<std::uint16_t>(buffer[byte_idx + 1]) << 8))
                                 : static_cast<std::int32_t>(buffer[byte_idx]);
                }
            }
        }
        alpacacore::util::validate_image_array(image);
        return image;
    }
};

std::unique_ptr<CameraDriver> create_touptek_camera(int device_number, int camera_index) {
    return create_touptek_camera(device_number, camera_index, ToupTekSDKWrapper::instance());
}

std::unique_ptr<CameraDriver> create_touptek_camera(int device_number, int camera_index, ToupTekSDK& sdk,
                                                    util::TaskClock& clock) {
    return create_toupcam_family_camera(device_number, camera_index, sdk, ToupCameraBranding{"ToupTek", "TOUPTEK"},
                                        clock);
}

std::unique_ptr<CameraDriver> create_toupcam_family_camera(int device_number, int camera_index, ToupTekSDK& sdk,
                                                           ToupCameraBranding branding, util::TaskClock& clock) {
    return std::make_unique<ToupTekCameraDriver>(device_number, camera_index, sdk, std::move(branding), clock);
}

} // namespace alpacacore::vendor::touptek
