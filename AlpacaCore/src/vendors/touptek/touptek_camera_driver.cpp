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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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

class ToupTekCameraDriver : public CameraDriver {
public:
    ToupTekCameraDriver(int device_number, int camera_index)
        : device_number_(device_number)
        , camera_index_(camera_index)
        , handle_(nullptr)
        , camera_info_()
        , camera_info_valid_(false)
        , serial_number_()
        , firmware_version_()
        , connected_(false)
        , connecting_(false)
        , bin_(1)
        , start_x_(0)
        , start_y_(0)
        , num_x_(0)
        , num_y_(0)
        , image_ready_(false)
        , image_cached_(false)
        , last_image_()
        , last_exposure_duration_(0.0)
        , last_exposure_start_()
        , last_exposure_valid_(false)
        , exposure_active_(false)
        , pulse_guiding_(false)
    {
        preload_camera_info();
    }

    ~ToupTekCameraDriver() override {
        {
            // Block any new connection task from spawning a thread that would
            // outlive this object (destructor race -> std::terminate on an
            // unjoined connection_thread_).
            std::lock_guard<std::mutex> lock(connection_mutex_);
            shutting_down_ = true;
        }
        stop_connection_thread();
        stop_exposure_thread();
        stop_pulse_guide_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "Error during destruction: " + std::string(e.what()));
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
        return "ToupTek Camera";
    }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "TOUPTEK_SN_" + serial_number_;
        }
        return "TOUPTEK_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "ToupTek Camera Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore ToupTek Camera Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }  // ICameraV4 (Platform 7)

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected == connected_.load()) {
            if (connected) {
                // reset_exposure_state_locked() clears exposure_active_, so take
                // readout_mutex_ (after mutex_, preserving lock order) to uphold the
                // invariant that the flag only changes under readout_mutex_ — a
                // concurrent set_gain/set_readout_mode must not pass ensure_not_exposing
                // and write registers mid-frame during a redundant reconnect.
                std::lock_guard<std::mutex> rlock(readout_mutex_);
                reset_exposure_state_locked();
            }
            return;
        }

        auto& sdk = ToupTekSDKWrapper::instance();

        if (connected) {
            // Enumerate and resolve the target camera.
            auto cameras = sdk.enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No ToupTek cameras detected", AlpacaError::NotConnected);
            }
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range", AlpacaError::InvalidValue);
            }
            camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
            camera_info_valid_ = true;

            ALPACA_LOG_INFO("ToupTek", "SDK version: " + sdk.get_sdk_version());
            ALPACA_LOG_INFO("ToupTek", "Opening camera index " +
                std::to_string(camera_index_) + ": " + camera_info_.name);

            handle_ = sdk.open_camera_by_id(camera_info_.id);

            // Configure: disable auto-exposure, enable RAW output, select
            // 16-bit bitdepth when the sensor supports it, and arm software
            // trigger mode before starting pull-mode streaming.
            try {
                sdk.put_auto_exposure(handle_, false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "put_AutoExpoEnable failed: " + std::string(e.what()));
            }

            try {
                sdk.put_raw(handle_, 1);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "put_Option(RAW) failed: " + std::string(e.what()));
            }

            if (camera_info_.bit_depth_max > 8) {
                try {
                    sdk.put_bitdepth(handle_, 1);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ToupTek", "put_Option(BITDEPTH=1) failed: " +
                                               std::string(e.what()));
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
                    ALPACA_LOG_DEBUG("ToupTek", "get_Size failed: " + std::string(e.what()));
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
                    ALPACA_LOG_DEBUG("ToupTek", "get_RawFormat failed: " + std::string(e.what()));
                }

                try {
                    float px = 0.0f;
                    float py = 0.0f;
                    sdk.get_pixel_size(handle_, 0xffffffffu, px, py);
                    if (px > 0.0f) camera_info_.pixel_size_um_x = px;
                    if (py > 0.0f) camera_info_.pixel_size_um_y = py;
                } catch (const std::exception& e) {
                    // Best-effort: keep the preloaded pixel size on failure.
                    ALPACA_LOG_DEBUG("ToupTek", "get_PixelSize failed: " + std::string(e.what()));
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
                throw AlpacaException(std::string("Failed to configure ToupTek camera: ") + e.what(),
                                      AlpacaError::DriverException);
            }

            {
                // Same invariant as the redundant-reconnect path above: clear
                // exposure_active_ (inside reset_exposure_state_locked) under
                // readout_mutex_. No exposure thread runs yet here, but keeping every
                // caller consistent means the invariant holds by construction.
                std::lock_guard<std::mutex> rlock(readout_mutex_);
                reset_exposure_state_locked();
            }
            connected_.store(true);
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
        if (!connected_.load()) return CameraState::Idle;
        if (exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_deadline_valid_ &&
                std::chrono::steady_clock::now() >= exposure_deadline_) {
                ALPACA_LOG_WARN("ToupTek",
                    "Exposure deadline exceeded; forcing CameraState=Idle.");
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

    double get_ccd_temperature() const override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
        int deci_c = with_handle([&](HToupcam h) { return sdk.get_temperature_deciC(h); });
        return static_cast<double>(deci_c) / 10.0;
    }

    bool get_cooler_on() const override {
        ensure_connected();
        if (!get_can_get_cooler_power()) return false;
        auto& sdk = ToupTekSDKWrapper::instance();
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
        auto& sdk = ToupTekSDKWrapper::instance();
        with_handle([&](HToupcam h) { sdk.put_tec_enable(h, cooler_on); });
    }
    double get_cooler_power() const override {
        ensure_connected();
        if (!get_can_get_cooler_power()) return 0.0;
        auto& sdk = ToupTekSDKWrapper::instance();
        try {
            // Both reads under one with_handle so they come from the same open.
            int v = 0;
            int vmax = 0;
            with_handle([&](HToupcam h) {
                v = sdk.get_tec_voltage_deciV(h);
                vmax = sdk.get_tec_voltage_max_deciV(h);
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

    double get_electrons_per_adu() const override { return 1.0; }

    double get_exposure_max() const override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
        auto r = with_handle([&](HToupcam h) { return sdk.get_exposure_range(h); });
        return static_cast<double>(r.max_us) / 1'000'000.0;
    }
    double get_exposure_min() const override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.bit_depth_max <= 0) return 0.0;
        return static_cast<double>((1ULL << camera_info_.bit_depth_max) - 1ULL);
    }

    int get_gain() const override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
        return static_cast<int>(with_handle([&](HToupcam h) { return sdk.get_gain(h); }));
    }
    void set_gain(int gain) override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
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
        auto& sdk = ToupTekSDKWrapper::instance();
        return static_cast<int>(with_handle([&](HToupcam h) { return sdk.get_gain_range(h).max; }));
    }
    int get_gain_min() const override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
        return static_cast<int>(with_handle([&](HToupcam h) { return sdk.get_gain_range(h).min; }));
    }
    std::vector<std::string> get_gains() const override {
        throw AlpacaException("Gain descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_has_shutter() const override { return false; }

    double get_heat_sink_temperature() const override { return get_ccd_temperature(); }

    ImageArray get_image_array() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_ || !image_ready_ || !image_cached_) {
            throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
        }
        return last_image_;
    }
    std::string get_image_array_variant() const override { return "Int32"; }

    bool get_image_ready() const override {
        if (!connected_.load()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return last_exposure_valid_ && image_ready_ && image_cached_;
    }

    bool get_is_pulse_guiding() const override {
        if (!connected_.load()) return false;
        if (!pulse_guiding_.load()) return false;
        try {
            auto& sdk = ToupTekSDKWrapper::instance();
            bool guiding = with_handle([&](HToupcam h) { return sdk.is_guiding(h); });
            if (!guiding) pulse_guiding_.store(false);
            return guiding;
        } catch (const std::exception&) {
            return false;
        }
    }

    double get_last_exposure_duration() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("Last exposure duration not set", AlpacaError::ValueNotSet);
        }
        return last_exposure_duration_;
    }
    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("Last exposure start time not set", AlpacaError::ValueNotSet);
        }
        return last_exposure_start_;
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
    void set_num_x(int num_x) override { set_roi_size_locked(num_x, get_num_y()); }
    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }
    void set_num_y(int num_y) override { set_roi_size_locked(get_num_x(), num_y); }

    // ASCOM Offset maps to the ToupTek black level (TOUPCAM_OPTION_BLACKLEVEL),
    // available on cameras that report TOUPCAM_FLAG_BLACKLEVEL (e.g. the ATR2600M
    // / IMX571). Cameras without it throw PropertyNotImplemented, as before.
    int get_offset() const override {
        ensure_connected();
        ensure_blacklevel_supported();
        auto& sdk = ToupTekSDKWrapper::instance();
        return with_handle([&](HToupcam h) { return sdk.get_blacklevel(h); });
    }
    void set_offset(int offset) override {
        ensure_connected();
        ensure_blacklevel_supported();
        auto& sdk = ToupTekSDKWrapper::instance();
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
        const int max = sdk.get_blacklevel_max(handle_, camera_info_.bit_depth_max);
        if (offset < 0 || offset > max) {
            throw AlpacaException("Offset out of range", AlpacaError::InvalidValue);
        }
        ensure_not_exposing();
        sdk.put_blacklevel(handle_, offset);
    }
    int get_offset_max() const override {
        ensure_connected();
        ensure_blacklevel_supported();
        return offset_max_value();
    }
    int get_offset_min() const override {
        ensure_connected();
        ensure_blacklevel_supported();
        return 0;  // TOUPCAM_BLACKLEVEL_MIN
    }
    std::vector<std::string> get_offsets() const override {
        // Integer offset mode (OffsetMin/OffsetMax), so the named-offsets list is
        // deliberately not implemented, matching the other camera drivers.
        throw AlpacaException("Offset descriptions not supported",
                              AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        if (!connected_.load()) return 0.0;
        if (!exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            return image_ready_ ? 100.0 : 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_exposure_duration_ <= 0.0) return 0.0;
        auto now = std::chrono::system_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_exposure_start_).count();
        double pct = (elapsed / last_exposure_duration_) * 100.0;
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
        const auto specs = readout_mode_specs();
        if (specs.size() == 1) {
            return 0;  // Only "Normal".
        }
        auto& sdk = ToupTekSDKWrapper::instance();
        int cur_cg = 0;
        bool cur_hfw = false;
        {
            // Capture the handle, the capability flags (which axes exist), AND read
            // both SDK registers under mutex_ + readout_mutex_ held together, so all
            // three come from ONE connection: a disconnect (or disconnect+reconnect
            // to a different model) cannot swap the handle out from under a stale
            // has_cg/has_hfw pair captured before the lock. readout_mutex_ also makes
            // the two register reads atomic w.r.t. set_readout_mode's two-step
            // CG+HFW apply. Deriving the caps here from camera_info_ (rather than the
            // pre-lock specs snapshot) is what makes them consistent with handle_.
            std::lock_guard<std::mutex> lock(mutex_);
            std::lock_guard<std::mutex> rlock(readout_mutex_);
            if (!handle_) {
                throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
            }
            const bool has_cg = camera_info_valid_ && camera_info_.supports_cg;
            const bool has_hfw = camera_info_valid_ && camera_info_.supports_high_fullwell;
            cur_cg = has_cg ? sdk.get_cg(handle_) : 0;
            cur_hfw = has_hfw && sdk.get_high_fullwell(handle_) != 0;
        }
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const auto& s = specs[i];
            if ((!s.set_cg || s.cg == cur_cg) && (!s.set_hfw || s.hfw == cur_hfw)) {
                return static_cast<int>(i);
            }
        }
        return 0;  // Current combination not enumerated; report the first mode.
    }
    void set_readout_mode(int mode) override {
        const auto specs = readout_mode_specs();
        // Range check first so an out-of-range index is InvalidValue even while
        // disconnected (same precedence as the switch driver's validate_switch_id).
        if (mode < 0 || mode >= static_cast<int>(specs.size())) {
            throw AlpacaException("Readout mode index out of range", AlpacaError::InvalidValue);
        }
        // Then require a connection, so a Normal-only camera still throws
        // NotConnected here rather than silently succeeding on the early-return.
        ensure_connected();
        const auto& s = specs[static_cast<std::size_t>(mode)];
        if (!s.set_cg && !s.set_hfw) {
            // Single "Normal" mode: no SDK write, but still reject a mid-exposure
            // ReadoutMode change per the ASCOM contract. Take readout_mutex_ alone
            // (no handle_copy, so no mutex_ -> readout_mutex_ ordering concern) so
            // the check is consistent with the write path below.
            std::lock_guard<std::mutex> lock(readout_mutex_);
            ensure_connected();
            ensure_not_exposing();
            return;
        }
        auto& sdk = ToupTekSDKWrapper::instance();
        // Hold mutex_ + readout_mutex_ together and read handle_ directly (same fix
        // as set_gain/set_offset) — the old handle_copy() snapshot released mutex_
        // before the write, so a disconnect+reconnect in the gap left the captured
        // handle closed while ensure_connected() still passed. readout_mutex_ keeps
        // the two-axis apply atomic w.r.t. get_readout_mode's paired reads, and the
        // exposure check under it closes the TOCTOU where an exposure could begin
        // between the check and the CG/HFW writes. Lock order mutex_ -> readout_mutex_.
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> rlock(readout_mutex_);
        if (!handle_) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        ensure_not_exposing();
        if (s.set_cg) {
            sdk.put_cg(handle_, s.cg);
        }
        if (s.set_hfw) {
            sdk.put_high_fullwell(handle_, s.hfw);
        }
    }
    std::vector<std::string> get_readout_modes() const override {
        ensure_connected();  // ASCOM: properties throw NotConnected when disconnected
                             // (consistent with the get_readout_mode index getter)
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
        return "ToupTek Sensor";
    }
    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) return SensorType::Monochrome;
        return SensorType::RGGB;
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        auto& sdk = ToupTekSDKWrapper::instance();
        int t = with_handle([&](HToupcam h) { return sdk.get_tec_target_deciC(h); });
        return static_cast<double>(t) / 10.0;
    }
    void set_set_ccd_temperature(double temperature) override {
        ensure_connected();
        int deci = static_cast<int>(std::lround(temperature * 10.0));
        auto& sdk = ToupTekSDKWrapper::instance();
        with_handle([&](HToupcam h) { sdk.put_tec_target_deciC(h, deci); });
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }
    void set_start_x(int start_x) override { set_start_pos_locked(start_x, get_start_y()); }
    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }
    void set_start_y(int start_y) override { set_start_pos_locked(get_start_x(), start_y); }

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
        auto& sdk = ToupTekSDKWrapper::instance();
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
            const bool cancelled = pulse_guide_cv_.wait_for(plock, std::chrono::milliseconds(duration),
                                                            [this] { return pulse_guide_cancel_; });
            if (!cancelled) {
                pulse_guiding_.store(false);  // natural completion
            }
        });
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
        ensure_connected();
        (void)light;

        if (duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative", AlpacaError::InvalidValue);
        }

        auto& sdk = ToupTekSDKWrapper::instance();
        HToupcam handle = handle_copy();
        auto range = sdk.get_exposure_range(handle);

        long exposure_us_long = static_cast<long>(std::lround(duration * 1'000'000.0));
        if (exposure_us_long < static_cast<long>(range.min_us)) exposure_us_long = range.min_us;
        if (exposure_us_long > static_cast<long>(range.max_us)) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }
        unsigned exposure_us = static_cast<unsigned>(exposure_us_long);

        // Stop any still-running prior exposure BEFORE snapshotting state: it may
        // force format_dirty_ (see stop_exposure_thread) to restart the stream it
        // had to stop, and we want that reflected in this exposure's snapshot.
        stop_exposure_thread();

        int active_bin = 0;
        int active_start_x = 0;
        int active_start_y = 0;
        int active_num_x = 0;
        int active_num_y = 0;
        // Sensor-coordinate ROI actually programmed into the SDK (see below).
        unsigned roi_x = 0;
        unsigned roi_y = 0;
        unsigned roi_w = 0;
        unsigned roi_h = 0;
        bool dirty_format = false;
        bool dirty_roi = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!camera_info_valid_) {
                throw AlpacaException("Camera info not valid", AlpacaError::DriverException);
            }
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
            int max_w = even_max_w / active_bin;
            int max_h = even_max_h / active_bin;
            if (active_num_x <= 0 || active_num_y <= 0) {
                throw AlpacaException("ROI not valid", AlpacaError::InvalidValue);
            }
            if (active_num_x > max_w || active_num_y > max_h) {
                throw AlpacaException("ROI size exceeds sensor dimensions", AlpacaError::InvalidValue);
            }
            if (active_start_x < 0 || active_start_y < 0 ||
                active_start_x + active_num_x > max_w ||
                active_start_y + active_num_y > max_h) {
                throw AlpacaException("ROI extends beyond sensor bounds", AlpacaError::InvalidValue);
            }

            // Toupcam_put_Roi requires an even sensor-coordinate offset, width,
            // and height, and the ROI must fit the sensor. The binned span is
            // (num * bin); for odd bin factors that product can be odd (e.g.
            // 3x3 → 4167), which the SDK rejects — no frame is delivered and
            // ImageReady never sets. Round the span UP to even so the SDK
            // floor-bins it back to exactly num output pixels (the +1 pad is
            // < bin for any bin >= 2), and round the offset DOWN to even. If
            // padding the span pushes the right/bottom edge past the sensor
            // (an edge-touching sub-frame where num*bin already reached the
            // limit), shift the even offset left/up by the overflow instead of
            // shrinking the span — that keeps the output count at num, so the
            // pixel buffer sized from active_num_x/y stays correct.
            int span_w = active_num_x * active_bin;
            int span_h = active_num_y * active_bin;
            span_w += (span_w & 1);
            span_h += (span_h & 1);
            // NB: at bin 1 an odd NumX/NumY makes the SDK ROI one pixel wider/taller
            // than requested, so got_w/got_h from WaitImageV4 can exceed
            // active_num_x/active_num_y — this is NOT an off-by-one. We pass
            // rowPitch = active_num_x * bytes, which is SMALLER than the ROI's
            // (active_num_x+1)-pixel row, so each row's extra trailing pixel is
            // written at the start of the next row's stride slot and is then
            // overwritten by that row's first pixel; columns 0..active_num_x-1 of
            // every row stay intact. build_image_array_16bit reads exactly
            // active_num_x columns per row at the same stride, and the +2-row
            // buffer margin absorbs the final row's unreclaimed extra pixel.
            // Reported NumX/NumY stay = active_num_x/y.
            // Defensive only: with max_w/max_h derived from the even sensor size
            // above, ceil_even(num*bin) <= even_max, so these clamps never fire.
            if (span_w > even_max_w) span_w = even_max_w;
            if (span_h > even_max_h) span_h = even_max_h;
            int roi_x_i = (active_start_x * active_bin) & ~1;
            int roi_y_i = (active_start_y * active_bin) & ~1;
            if (roi_x_i + span_w > even_max_w) roi_x_i = (even_max_w - span_w) & ~1;
            if (roi_y_i + span_h > even_max_h) roi_y_i = (even_max_h - span_h) & ~1;
            if (roi_x_i < 0) roi_x_i = 0;
            if (roi_y_i < 0) roi_y_i = 0;
            roi_x = static_cast<unsigned>(roi_x_i);
            roi_y = static_cast<unsigned>(roi_y_i);
            roi_w = static_cast<unsigned>(span_w);
            roi_h = static_cast<unsigned>(span_h);

            // Clear the dirty flags here — AFTER all validation that can throw, but
            // still under the same lock as the snapshot above. Clearing before the
            // validation would lose the flags if an invalid ROI threw (the thread
            // that restores them on failure is never spawned). Because snapshot and
            // clear share this lock, a concurrent set_num_x/set_roi cannot interleave
            // between them; one that lands after the lock releases re-dirties the
            // flag for the NEXT exposure. The catch below re-marks on SDK failure.
            format_dirty_ = false;
            roi_dirty_ = false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_exposure_duration_ = duration;
            last_exposure_start_ = std::chrono::system_clock::now();
            last_exposure_valid_ = true;
            image_ready_ = false;
            image_cached_ = false;
            exposure_deadline_ = std::chrono::steady_clock::now() +
                std::chrono::microseconds(exposure_us) +
                std::chrono::seconds(15);
            exposure_deadline_valid_ = true;
        }

        {
            // Publish exposure_active_ under readout_mutex_ so set_readout_mode's
            // exposure check (also under readout_mutex_) can't race the start of an
            // integration and write CG/HFW mid-frame (TOCTOU close).
            std::lock_guard<std::mutex> rlock(readout_mutex_);
            exposure_active_.store(true);
        }

        exposure_thread_ = std::thread([this, handle, exposure_us, active_bin, active_num_x, active_num_y, roi_x, roi_y,
                                        roi_w, roi_h, dirty_format, dirty_roi]() {
            auto& sdk_local = ToupTekSDKWrapper::instance();
            // Track which reconfigure stages actually completed, so the catch
            // re-marks ONLY the stage that failed — re-marking an
            // already-applied stage would trigger a spurious stream restart on
            // the next exposure.
            bool format_applied = false;
            bool roi_applied = false;
            bool frame_ready = false;
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
                    sdk_local.start_pull_mode(handle, &ToupTekCameraDriver::on_event_static, this);
                    format_applied = true;  // flag cleared at snapshot time
                }

                if (dirty_roi) {
                    sdk_local.put_roi(handle, roi_x, roi_y, roi_w, roi_h);
                    roi_applied = true;  // flag cleared at snapshot time
                }

                sdk_local.put_exposure_us(handle, exposure_us);
                sdk_local.trigger(handle, 1);

                unsigned timeout_ms = (exposure_us / 1000) + 10000;
                // Buffer is num_x * num_y; the even-up ROI floor-bins to exactly
                // that. Add a two-row margin as crash-insurance in case a model
                // ever delivers one extra padded row from the rounded ROI span.
                std::vector<std::uint16_t> pixel_buffer(static_cast<std::size_t>(active_num_x) *
                                                        static_cast<std::size_t>(active_num_y + 2));

                unsigned got_w = 0;
                unsigned got_h = 0;
                // rowPitch = active_num_x * 2 bytes: this is the SDK contract for
                // Toupcam_WaitImageV4's nRowPitch arg — the destination row stride
                // in OUR buffer (not the ROI width). We deliberately set it to the
                // requested (unpadded) width so the image is laid out at active_num_x
                // stride regardless of the even-padded ROI (see the odd-NumX/bin-1
                // note at the ROI computation above). got_w/got_h are the SDK's
                // delivered dimensions and MAY exceed active_num_x/y by one at bin 1;
                // build_image_array_16bit reads exactly active_num_x columns per row
                // at this same stride. If a future SDK ignores nRowPitch and packs at
                // got_w, this assumption breaks — revalidate against the SDK docs.
                bool got = sdk_local.wait_image(handle, timeout_ms,
                                                 pixel_buffer.data(),
                                                 16,
                                                 static_cast<int>(active_num_x * 2),
                                                 got_w, got_h);

                if (!got || !exposure_active_.load()) {
                    ALPACA_LOG_WARN("ToupTek",
                        "Exposure failed or aborted before frame arrived");
                    // Publish the false-transition under readout_mutex_ so the
                    // invariant "exposure_active_ only changes under readout_mutex_"
                    // holds on the exposure thread's own exit paths too (matching
                    // the disconnect and deadline paths). Taken alone here — the
                    // thread holds neither mutex_ nor the SDK lock — so it respects
                    // the mutex_ -> readout_mutex_ order, and no join site holds
                    // readout_mutex_, so this cannot deadlock the joining thread.
                    std::lock_guard<std::mutex> rlock(readout_mutex_);
                    exposure_active_.store(false);
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_image_ = build_image_array_16bit(pixel_buffer,
                                                          active_num_x, active_num_y,
                                                          static_cast<int>(got_w),
                                                          static_cast<int>(got_h));
                    image_cached_ = true;
                    // image_ready_ is intentionally NOT set here — it is published
                    // AFTER exposure_active_ is cleared below, so a poller never
                    // observes ImageReady=true while camera_state still reports
                    // Exposing (an ASCOM-forbidden combination). The frame is fully
                    // captured, so remaining Exposing during this build is fine.
                }
                frame_ready = true;
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ToupTek", "Exposure failed: " + std::string(e.what()));
                // Re-mark only the stage that did NOT complete, so the next
                // exposure re-applies the failed reconfigure without needlessly
                // restarting the stream for one that already succeeded.
                std::lock_guard<std::mutex> lock(mutex_);
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
                image_ready_ = true;
            }
        });
    }

    void stop_exposure() override {
        ensure_connected();
        // Unblock a thread parked in Toupcam_WaitImageV4 so abort returns promptly
        // (a 10-min frame would otherwise take ~10 min to abort). Hold mutex_ across
        // stop() so a concurrent disconnect can't Toupcam_Close the handle in the gap
        // (use-after-close). Do NOT pre-clear exposure_active_: the thread exits on
        // the stopped stream (wait_image returns got=false) and clears it itself, so
        // clearing it here before the join would let a concurrent set_readout_mode /
        // set_gain (which check the flag under readout_mutex_) write registers while
        // the frame is still live.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_) {
                try {
                    ToupTekSDKWrapper::instance().stop(handle_);
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                }
            }
        }
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        image_cached_ = false;
        exposure_deadline_valid_ = false;
        // Stream was stopped; force the next exposure to re-init it. Mark the ROI
        // dirty too as a defensive safety net: the SDK preserves ROI across a
        // StartPullMode restart today, but re-applying it costs one put_roi and
        // avoids silently capturing full-frame data into a sub-frame buffer if a
        // future SDK version resets ROI on restart.
        format_dirty_ = true;
        roi_dirty_ = true;
    }

private:
    int device_number_;
    int camera_index_;
    HToupcam handle_;
    ToupCameraInfo camera_info_;
    bool camera_info_valid_;
    std::string serial_number_;
    std::string firmware_version_;

    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    bool shutting_down_ = false;  // guarded by connection_mutex_
    // Serialises the two-step CG+HFW apply in set_readout_mode against the
    // paired reads in get_readout_mode, so a reader never observes a
    // half-applied combination that isn't in the enumerated specs.
    mutable std::mutex readout_mutex_;
    std::thread connection_thread_;

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
    double last_exposure_duration_;
    std::chrono::system_clock::time_point last_exposure_start_;
    bool last_exposure_valid_;

    mutable std::atomic<bool> exposure_active_;
    std::thread exposure_thread_;
    mutable std::chrono::steady_clock::time_point exposure_deadline_{};
    mutable bool exposure_deadline_valid_{false};

    mutable std::atomic<bool> pulse_guiding_;
    // Joinable timer that clears pulse_guiding_ after the pulse duration. Kept as a
    // member (not detached) with a cancel flag + cv so the destructor can wake and
    // join it — a detached thread writing pulse_guiding_ after destruction is UB.
    std::thread pulse_guide_thread_;
    std::mutex pulse_guide_mutex_;
    std::condition_variable pulse_guide_cv_;
    bool pulse_guide_cancel_{false};

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
        bool cg = false;
        bool cghdr = false;
        bool hfw = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cg = camera_info_valid_ && camera_info_.supports_cg;
            cghdr = camera_info_valid_ && camera_info_.supports_cghdr;
            hfw = camera_info_valid_ && camera_info_.supports_high_fullwell;
        }
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

    void ensure_blacklevel_supported() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.supports_blacklevel) {
            throw AlpacaException("Offset (black level) not supported by this camera",
                                  AlpacaError::PropertyNotImplemented);
        }
    }

    int offset_max_value() const {
        auto& sdk = ToupTekSDKWrapper::instance();
        // handle_ and camera_info_.bit_depth_max read together under one mutex_ hold
        // (via with_handle), so the SDK read uses a live handle and a consistent
        // bit depth.
        return with_handle([&](HToupcam h) { return sdk.get_blacklevel_max(h, camera_info_.bit_depth_max); });
    }

    void reset_exposure_state_locked() {
        image_ready_ = false;
        image_cached_ = false;
        last_exposure_duration_ = 0.0;
        last_exposure_start_ = std::chrono::system_clock::time_point{};
        last_exposure_valid_ = false;
        exposure_active_.store(false);
        exposure_deadline_valid_ = false;
    }

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (shutting_down_) return;  // Destruction in progress; never spawn a new thread.
        if (connecting_.load()) return;
        if (connection_thread_.joinable()) connection_thread_.join();
        connecting_.store(true);
        connection_thread_ = std::thread([this, connect]() {
            try {
                if (!connect) {
                    // Match the destructor's ordering: stop AND JOIN the exposure
                    // thread BEFORE set_connected(false) closes the SDK handle. The
                    // exposure thread runs Toupcam_WaitImageV4 holding none of our
                    // locks, and set_connected(false) closes the handle under
                    // mutex_ — so closing without joining first is a use-after-close
                    // if Stop unwinds the wait asynchronously. The join cannot live
                    // inside set_connected (it already holds mutex_, which
                    // stop_exposure_thread also takes → deadlock), so it goes here.
                    stop_exposure_thread();
                }
                set_connected(connect);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR("ToupTek", "Connection task failed: " + std::string(e.what()));
            }
            connecting_.store(false);
        });
    }

    void stop_connection_thread() {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connection_thread_.joinable()) connection_thread_.join();
    }

    void stop_exposure_thread() {
        if (!exposure_thread_.joinable()) return;
        // Unblock a thread parked in Toupcam_WaitImageV4 (the wrapper releases the
        // SDK lock across the wait for exactly this) so the join returns promptly
        // instead of waiting out the full wait_image timeout. Hold mutex_ across
        // stop() so a concurrent disconnect can't close the handle in the gap; the
        // thread exits on the stopped stream (got=false) and clears exposure_active_
        // itself, so we don't pre-clear it. Stopping the stream means the next
        // exposure must restart it, so mark format/ROI dirty.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_) {
                try {
                    ToupTekSDKWrapper::instance().stop(handle_);
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                }
            }
        }
        exposure_thread_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        // Stream was stopped; re-init format + ROI on the next exposure (ROI is a
        // defensive safety net — see stop_exposure).
        format_dirty_ = true;
        roi_dirty_ = true;
    }

    void preload_camera_info() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            auto cameras = ToupTekSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ToupTek",
                             "Preload enumerate failed: " + std::string(e.what()));
        }
    }

    void refresh_cached_camera_info_if_needed() {
        if (connected_.load()) return;
        try {
            auto cameras = ToupTekSDKWrapper::instance().enumerate_cameras();
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

    void set_roi_size_locked(int width, int height) {
        ensure_connected();
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> rlock(readout_mutex_);  // TOCTOU close — see set_bin_locked
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
    void set_start_pos_locked(int sx, int sy) {
        ensure_connected();
        if (sx < 0 || sy < 0) {
            throw AlpacaException("Start position must be non-negative",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> rlock(readout_mutex_);  // TOCTOU close — see set_bin_locked
        ensure_not_exposing();
        if (start_x_ == sx && start_y_ == sy) return;
        start_x_ = sx;
        start_y_ = sy;
        roi_dirty_ = true;
    }

    ImageArray build_image_array_16bit(const std::vector<std::uint16_t>& buffer,
                                        int out_width, int out_height,
                                        int actual_width, int actual_height) const {
        ImageArray image;
        image.width = out_width;
        image.height = out_height;
        image.rank = 2;
        if (out_width <= 0 || out_height <= 0) {
            image.rank = 0;
            return image;
        }
        const int copy_w = std::min(out_width, actual_width > 0 ? actual_width : out_width);
        const int copy_h = std::min(out_height, actual_height > 0 ? actual_height : out_height);
        image.data.assign(static_cast<std::size_t>(out_width) *
                          static_cast<std::size_t>(out_height), 0);
        for (int row = 0; row < copy_h; ++row) {
            for (int col = 0; col < copy_w; ++col) {
                std::size_t src_idx = static_cast<std::size_t>(row) *
                                      static_cast<std::size_t>(out_width) +
                                      static_cast<std::size_t>(col);
                if (src_idx < buffer.size()) {
                    image.data[src_idx] = static_cast<std::int32_t>(buffer[src_idx]);
                }
            }
        }
        return image;
    }
};

std::unique_ptr<CameraDriver> create_touptek_camera(int device_number, int camera_index) {
    return std::make_unique<ToupTekCameraDriver>(device_number, camera_index);
}

} // namespace alpacacore::vendor::touptek
