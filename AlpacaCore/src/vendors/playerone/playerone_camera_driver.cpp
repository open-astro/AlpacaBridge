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
#include <alpacacore/util/capture_session.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/image_validation.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/playerone/playerone_camera_driver.h>
#include <alpacacore/vendor/playerone/playerone_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
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
#include <vector>

namespace alpacacore::vendor::playerone {

namespace {

std::pair<int, int> bayer_offsets(PlayerOneBayerPattern pattern) {
    switch (pattern) {
        case PlayerOneBayerPattern::RG:
            return {0, 0};
        case PlayerOneBayerPattern::BG:
            return {1, 1};
        case PlayerOneBayerPattern::GR:
            return {1, 0};
        case PlayerOneBayerPattern::GB:
            return {0, 1};
        default:
            return {0, 0};
    }
}

SensorType bayer_to_sensor_type(PlayerOneBayerPattern pattern) {
    // AlpacaCore's SensorType enum only names RGGB — CMYG/LRGB don't apply to
    // Player One sensors, and there's no dedicated BGGR/GRBG/GBRG value. Report
    // the mono/color split accurately and fall back to RGGB for any color
    // pattern; the Bayer-offset properties carry the actual layout for clients
    // that need to debayer.
    if (pattern == PlayerOneBayerPattern::None) return SensorType::Monochrome;
    return SensorType::RGGB;
}

std::size_t bytes_per_pixel(PlayerOneImageFormat format) {
    switch (format) {
        case PlayerOneImageFormat::Raw16:
            return 2;
        case PlayerOneImageFormat::Raw8:
        case PlayerOneImageFormat::Mono8:
            return 1;
        case PlayerOneImageFormat::Rgb24:
            return 3;
        case PlayerOneImageFormat::Unknown:
            break;
    }
    alpacacore::util::throw_invalid_camera_image("unsupported Player One pixel format");
}

std::size_t checked_frame_bytes(int width, int height, std::size_t pixel_bytes) {
    if (width <= 0 || height <= 0) {
        alpacacore::util::throw_invalid_camera_image("frame dimensions must be positive");
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    if (w > max_size / h || w * h > max_size / pixel_bytes) {
        alpacacore::util::throw_invalid_camera_image("frame dimensions overflow the buffer size");
    }
    return w * h * pixel_bytes;
}

bool format_is_supported(const std::vector<PlayerOneImageFormat>& supported, PlayerOneImageFormat candidate) {
    return std::find(supported.begin(), supported.end(), candidate) != supported.end();
}

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Action parameters arrive as the raw HTTP form value; accept an optionally
// whitespace-padded integer and nothing else.
int parse_power_percent(std::string_view parameters, const char* action_name) {
    std::string trimmed(parameters);
    const auto first = trimmed.find_first_not_of(" \t\r\n");
    const auto last = trimmed.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        throw AlpacaException(std::string(action_name) + " requires an integer percent parameter",
                              AlpacaError::InvalidValue);
    }
    trimmed = trimmed.substr(first, last - first + 1);
    int value = 0;
    const auto* begin = trimmed.data();
    const auto* end = trimmed.data() + trimmed.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw AlpacaException(std::string(action_name) + " parameter must be an integer percent: '" + trimmed + "'",
                              AlpacaError::InvalidValue);
    }
    return value;
}

PlayerOneImageFormat choose_default_format(const PlayerOneCameraInfo& info) {
    // Prefer RAW16 for anything >8-bit so we keep the sensor's full dynamic
    // range; fall back to RAW8 on 8-bit-only sensors.
    if (info.bit_depth > 8 && format_is_supported(info.supported_formats, PlayerOneImageFormat::Raw16)) {
        return PlayerOneImageFormat::Raw16;
    }
    if (format_is_supported(info.supported_formats, PlayerOneImageFormat::Raw8)) {
        return PlayerOneImageFormat::Raw8;
    }
    // Last resort — take whatever the camera claims first.
    if (!info.supported_formats.empty()) return info.supported_formats.front();
    return PlayerOneImageFormat::Raw16;
}

}  // namespace

class PlayerOneCameraDriver : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    PlayerOneCameraDriver(int device_number, int camera_index, PlayerOneSDK& sdk,
                          std::chrono::steady_clock::duration completion_grace, util::TaskClock& clock)
        : AsyncConnectable("PlayerOne"),
          device_number_(device_number),
          camera_index_(camera_index),
          sdk_(sdk),
          completion_grace_(completion_grace),
          clock_(clock),
          pulse_slot_("PlayerOne pulse guide", pulse_generation_, clock_),
          session_("PlayerOne", clock_, completion_grace_) {
        preload_camera_info();
    }

    ~PlayerOneCameraDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // The SDK stop first, then the join: the body must be gone before the camera closes.
        const std::string stop_error = stop_sdk_exposure();
        if (!stop_error.empty()) {
            ALPACA_LOG_WARN("PlayerOne", "Error stopping the exposure during destruction: " + stop_error);
        }
        session_.disconnect_and_join();
        if (connected_.load()) {
            try {
                apply_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Error during destruction: " + std::string(e.what()));
                force_release_camera();
            }
        } else {
            try {
                stop_pulse_guide_thread();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Error stopping pulse guide during destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        const_cast<PlayerOneCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.name.empty()) {
            return camera_info_.name;
        }
        return "Player One Camera";
    }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.serial_number.empty()) {
            return "PLAYERONE_SN_" + camera_info_.serial_number;
        }
        return "PLAYERONE_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "Player One Camera Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore Player One Camera Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only (never in DriverInfo).
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = sdk_.get_sdk_version();
        if (version.empty()) {
            return std::nullopt;
        }
        return version;
    }

    int get_interface_version() const override { return 4; }  // ICameraV4 (Platform 7)

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override { apply_connected(connected); }

    // The body of set_connected(), non-virtual so the destructor can run it without a virtual call.
    void apply_connected(bool connected) {
        std::lock_guard<std::mutex> transition_lock(transition_mutex_);
        std::unique_lock<std::mutex> pulse_lifecycle_lock(pulse_guide_lifecycle_mutex_, std::defer_lock);
        // A disconnect that leaves the camera connected (a gate below, a pulse stop that throws) must not
        // leave the session closed: a StartExposure would raise NotConnected on a connected camera.
        struct ReopenIfConnected {
            PlayerOneCameraDriver& driver;
            bool closed = false;
            ~ReopenIfConnected() {
                if (closed && driver.connected_.load()) {
                    try {
                        driver.session_.open();
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("PlayerOne", "Could not reopen the exposure session: " + std::string(e.what()));
                    }
                }
            }
        } reopen{*this};
        std::string exposure_stop_error;
        if (!connected) {
            // Stop the SDK exposure, then join the body BEFORE taking mutex_ and closing the camera, so the
            // disconnect never tears down the SDK session under a live exposure body (the sync path and the
            // async connection task both come here). The join is outside mutex_: the body takes mutex_ to
            // publish. disconnect_and_join() closes the session first, so a StartExposure that passed its
            // connection check before this disconnect raises NotConnected and runs no body. A failed stop
            // leaves the exposure running: fail() keeps the error for ImageReady, nothing is joined and the
            // camera stays connected.
            exposure_stop_error = stop_sdk_exposure();
            if (!exposure_stop_error.empty()) {
                session_.fail(exposure_stop_error);
            } else {
                // The SDK stop discarded the frame, so a live body would poll to its deadline. Cancel and join
                // it now (a no-op when nothing runs: a Ready frame and a kept failure stay) so a pulse stop
                // that throws below leaves the camera idle, not Exposing.
                session_.abort();
            }
            // The pulse stop comes before the join: it can throw and leave the camera connected, and the
            // join clears the Ready frame, the kept failure and LastExposure*.
            pulse_lifecycle_lock.lock();
            stop_pulse_guide_thread_locked();
            if (exposure_stop_error.empty()) {
                reopen.closed = true;
                session_.disconnect_and_join();
            }
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
        if (!connected && !exposure_stop_error.empty()) {
            throw AlpacaException(exposure_stop_error, AlpacaError::DriverException);
        }
        if (connected == connected_.load()) {
            // Idempotent (ASCOM): a redundant Connect/Disconnect is a no-op and must
            // NOT reset exposure state. The Platform-7 `connect` endpoint calls
            // connect() unconditionally, so wiping here would abort an in-flight
            // exposure or discard a just-completed image. Matches the QHY driver.
            return;
        }

        auto& sdk = sdk_;

        if (connected) {
            auto cameras = sdk.enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No Player One cameras detected", AlpacaError::NotConnected);
            }
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range", AlpacaError::InvalidValue);
            }
            camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
            camera_info_valid_ = true;

            ALPACA_LOG_INFO("PlayerOne", "SDK version: " + sdk.get_sdk_version());
            ALPACA_LOG_INFO("PlayerOne", "Opening camera index " + std::to_string(camera_index_) + " (id=" +
                                             std::to_string(camera_info_.camera_id) + "): " + camera_info_.name);

            sdk.open_camera(camera_info_.camera_id);

            try {
                sdk.init_camera(camera_info_.camera_id);
            } catch (const std::exception& e) {
                try {
                    sdk.close_camera(camera_info_.camera_id);
                } catch (const std::exception& close_error) {
                    ALPACA_LOG_WARN("PlayerOne",
                                    "close_camera after init failure failed: " + std::string(close_error.what()));
                } catch (...) {
                    ALPACA_LOG_WARN("PlayerOne", "close_camera after init failure threw a non-standard exception");
                }
                throw AlpacaException(std::string("POAInitCamera failed: ") + e.what(), AlpacaError::DriverException);
            }

            // Probe capabilities from POAGetConfigAttributes — this gives us
            // ranges and writability for every POAConfig this camera exposes.
            try {
                caps_ = sdk.probe_config_caps(camera_info_.camera_id);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "probe_config_caps failed: " + std::string(e.what()));
                caps_ = PlayerOneConfigCaps{};
            }

            // Refresh properties post-init (Player One fills in some fields
            // only after init — most notably localPath).
            try {
                auto fresh = sdk.get_camera_properties_by_id(camera_info_.camera_id);
                // Keep our enumeration index but take the fresh identity fields.
                int idx = camera_info_.index;
                camera_info_ = fresh;
                camera_info_.index = idx;
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("PlayerOne", "get_camera_properties_by_id failed: " + std::string(e.what()));
            }

            // Select default image format (prefer RAW16 on ≥10-bit sensors).
            active_format_ = choose_default_format(camera_info_);
            try {
                sdk.set_image_format(camera_info_.camera_id, active_format_);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "set_image_format failed: " + std::string(e.what()));
            }
            bool format_verified = false;
            try {
                static_cast<void>(bytes_per_pixel(active_format_));
                format_verified = sdk.get_image_format(camera_info_.camera_id) == active_format_;
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("PlayerOne", "image format readback failed: " + std::string(e.what()));
            }
            if (!format_verified) {
                const int failed_id = camera_info_.camera_id;
                try {
                    sdk.close_camera(failed_id);
                } catch (const std::exception& close_error) {
                    ALPACA_LOG_WARN("PlayerOne",
                                    "close_camera after format failure failed: " + std::string(close_error.what()));
                } catch (...) {
                    ALPACA_LOG_WARN("PlayerOne", "close_camera after format failure threw a non-standard exception");
                }
                throw AlpacaException("Unable to configure a supported Player One image format",
                                      AlpacaError::DriverException);
            }

            // Default ROI = full sensor at bin 1. POASetImageBin resets the
            // size/start, so do it first then push the full frame.
            try {
                sdk.set_image_bin(camera_info_.camera_id, 1);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "set_image_bin(1) failed: " + std::string(e.what()));
            }
            try {
                sdk.set_image_start_pos(camera_info_.camera_id, 0, 0);
                sdk.set_image_size(camera_info_.camera_id, camera_info_.max_width, camera_info_.max_height);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "initial ROI setup failed: " + std::string(e.what()));
            }

            // Re-query so our cache matches what the SDK actually accepted
            // (width%4==0 / height%2==0 alignment may have trimmed values).
            int w = camera_info_.max_width;
            int h = camera_info_.max_height;
            int sx = 0;
            int sy = 0;
            int bin = 1;
            try {
                sdk.get_image_size(camera_info_.camera_id, w, h);
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("PlayerOne", "initial image-size readback failed: " + std::string(e.what()));
            } catch (...) {
                ALPACA_LOG_WARN("PlayerOne", "initial image-size readback threw a non-standard exception");
            }
            try {
                sdk.get_image_start_pos(camera_info_.camera_id, sx, sy);
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("PlayerOne", "initial start-position readback failed: " + std::string(e.what()));
            } catch (...) {
                ALPACA_LOG_WARN("PlayerOne", "initial start-position readback threw a non-standard exception");
            }
            try {
                bin = sdk.get_image_bin(camera_info_.camera_id);
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("PlayerOne", "initial bin readback failed: " + std::string(e.what()));
            } catch (...) {
                ALPACA_LOG_WARN("PlayerOne", "initial bin readback threw a non-standard exception");
            }

            const bool supported_bin =
                bin > 0 && std::find(camera_info_.supported_bins.begin(), camera_info_.supported_bins.end(), bin) !=
                               camera_info_.supported_bins.end();
            const int binned_width = supported_bin ? camera_info_.max_width / bin : 0;
            const int binned_height = supported_bin ? camera_info_.max_height / bin : 0;
            if (!supported_bin || camera_info_.max_width <= 0 || camera_info_.max_height <= 0 || w <= 0 || h <= 0 ||
                sx < 0 || sy < 0 || w > binned_width || h > binned_height || sx > binned_width - w ||
                sy > binned_height - h) {
                const int failed_id = camera_info_.camera_id;
                try {
                    sdk.close_camera(failed_id);
                } catch (const std::exception& close_error) {
                    ALPACA_LOG_WARN("PlayerOne",
                                    "close_camera after invalid geometry failed: " + std::string(close_error.what()));
                } catch (...) {
                    ALPACA_LOG_WARN("PlayerOne", "close_camera after invalid geometry threw a non-standard exception");
                }
                throw AlpacaException("Player One SDK returned invalid sensor or ROI geometry",
                                      AlpacaError::DriverException);
            }

            bin_ = bin;
            num_x_ = w;
            num_y_ = h;
            start_x_ = sx;
            start_y_ = sy;
            roi_dirty_ = false;
            format_dirty_ = false;

            reset_exposure_state_locked();
            session_.open();
            connected_.store(true);
            return;
        }

        // Disconnecting. Clear driver state and publish disconnected BEFORE
        // the SDK close so a racing operational call fails fast at its
        // connection check instead of hitting a just-closed camera, and a
        // throwing close can't trap the driver half-connected — same order as
        // the switch/EFW siblings, per the driver-lifecycle.instructions.md disconnect rule (#116).
        // The exposure body is already joined (disconnect_and_join() above).
        const int close_id = (camera_info_valid_ && camera_info_.camera_id >= 0) ? camera_info_.camera_id : -1;
        camera_info_ = {};
        camera_info_valid_ = false;
        caps_ = {};
        reset_exposure_state_locked();
        connected_.store(false);
        if (close_id >= 0) {
            sdk.close_camera(close_id);
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        // Static driver capability list per the ASCOM spec; whether the
        // connected model actually has a heater/fan is reported at call time.
        return {"GetHeaterPower", "SetHeaterPower", "GetFanPower", "SetFanPower"};
    }
    std::string action(std::string_view action_name, std::string_view parameters) override {
        const std::string name = to_lower_copy(action_name);
        if (name == "getheaterpower") {
            ensure_connected();
            ensure_heater_supported();
            return std::to_string(with_camera([this](int id) { return sdk_.get_heater_power_percent(id); }));
        }
        if (name == "setheaterpower") {
            ensure_connected();
            ensure_heater_supported();
            int percent = parse_power_percent(parameters, "SetHeaterPower");
            validate_heater_range(percent);
            with_camera([&](int id) { sdk_.set_heater_power_percent(id, percent); });
            return "";
        }
        if (name == "getfanpower") {
            ensure_connected();
            ensure_fan_supported();
            return std::to_string(with_camera([this](int id) { return sdk_.get_fan_power_percent(id); }));
        }
        if (name == "setfanpower") {
            ensure_connected();
            ensure_fan_supported();
            int percent = parse_power_percent(parameters, "SetFanPower");
            validate_fan_range(percent);
            with_camera([&](int id) { sdk_.set_fan_power_percent(id, percent); });
            return "";
        }
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }
    bool can_action(std::string_view action_name) const override {
        const std::string name = to_lower_copy(action_name);
        return name == "getheaterpower" || name == "setheaterpower" || name == "getfanpower" || name == "setfanpower";
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
    void set_bin_x(int bin_x) override { set_bin_common(bin_x, bin_x); }
    int get_bin_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_;
    }
    void set_bin_y(int bin_y) override { set_bin_common(bin_y, bin_y); }

    CameraState get_camera_state() const override {
        ensure_connected();
        return session_.camera_state();
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
    bool get_can_get_cooler_power() const override { return cooler_available_copy(); }
    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_st4_port;
    }
    bool get_can_set_ccd_temperature() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_cooler && caps_.has_target_temp;
    }
    bool get_can_stop_exposure() const override {
        // StopExposure would have to hand back the frame acquired so far, but
        // POAStopExposure discards it: on a Ceres 462M (SDK 3.10.0) the camera
        // is back in STATE_OPENED 60 ms after the stop, POAImageReady never
        // turns true and POAGetImageData fails (issue #1023). AbortExposure is
        // the supported way to end an exposure early.
        return false;
    }

    double get_ccd_temperature() const override {
        ensure_connected();
        return with_camera([this](int id) { return sdk_.get_temperature_c(id); });
    }

    bool get_cooler_on() const override {
        ensure_connected();
        if (!cooler_available_copy()) {
            throw AlpacaException("This Player One camera has no cooler", AlpacaError::PropertyNotImplemented);
        }
        return with_camera([this](int id) { return sdk_.get_cooler_on(id); });
    }
    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (!cooler_available_copy()) {
            throw AlpacaException("This Player One camera has no cooler", AlpacaError::PropertyNotImplemented);
        }
        with_camera([&](int id) { sdk_.set_cooler_on(id, cooler_on); });
    }
    double get_cooler_power() const override {
        ensure_connected();
        if (!cooler_available_copy()) {
            throw AlpacaException("This Player One camera has no cooler", AlpacaError::PropertyNotImplemented);
        }
        // An SDK failure propagates like get_cooler_on() does; it is never reported as 0 % power.
        int pct = with_camera([this](int id) { return sdk_.get_cooler_power_percent(id); });
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        return static_cast<double>(pct);
    }

    double get_electrons_per_adu() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        // POA_EGAIN is the per-gain e-/ADU; without it, or when the read fails, there is no honest value
        // (issue #1277), never a 1.0 stand-in.
        if (!caps_.has_egain) {
            throw AlpacaException("ElectronsPerADU is not available for this Player One camera",
                                  AlpacaError::PropertyNotImplemented);
        }
        try {
            return sdk_.get_egain(camera_info_.camera_id);
        } catch (const std::exception&) {
            throw AlpacaException("ElectronsPerADU could not be read from this Player One camera",
                                  AlpacaError::PropertyNotImplemented);
        }
    }

    double get_exposure_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_exposure) {
            throw AlpacaException("Exposure not supported", AlpacaError::NotImplemented);
        }
        return static_cast<double>(caps_.exposure_max_us) / 1'000'000.0;
    }
    double get_exposure_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_exposure) {
            throw AlpacaException("Exposure not supported", AlpacaError::NotImplemented);
        }
        return static_cast<double>(caps_.exposure_min_us) / 1'000'000.0;
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
        // The Player One SDK has no full-well figure; the ADU saturation is not electrons (issue #1277).
        throw AlpacaException("FullWellCapacity is not available for Player One cameras",
                              AlpacaError::PropertyNotImplemented);
    }

    int get_gain() const override {
        ensure_connected();
        return static_cast<int>(with_camera([this](int id) {
            return sdk_.get_config_int(id, /*config_id=*/1);  // POA_GAIN
        }));
    }
    void set_gain(int gain) override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_gain) {
            throw AlpacaException("Gain not supported", AlpacaError::PropertyNotImplemented);
        }
        if (!caps_.gain_writable) {
            throw AlpacaException("Gain is read-only", AlpacaError::NotImplemented);
        }
        if (gain < caps_.gain_min || gain > caps_.gain_max) {
            throw AlpacaException("Gain out of range", AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();  // M15 — sensor register, rejected mid-exposure
        sdk_.set_config_int(camera_info_.camera_id, /*config_id=*/1, static_cast<long>(gain), false);  // POA_GAIN
    }
    int get_gain_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_gain) {
            throw AlpacaException("Gain not supported", AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(caps_.gain_max);
    }
    int get_gain_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_gain) {
            throw AlpacaException("Gain not supported", AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(caps_.gain_min);
    }
    std::vector<std::string> get_gains() const override {
        throw AlpacaException("Gain descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_has_shutter() const override { return false; }
    double get_heat_sink_temperature() const override {
        throw AlpacaException("Heat sink temperature not supported", AlpacaError::NotImplemented);
    }

    ImageArray get_image_array() const override {
        ensure_connected();
        std::shared_ptr<const ImageArray> image;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected_locked();
            image = session_.image();
        }
        return *image;
    }
    std::string get_image_array_variant() const override { return "Int32"; }

    bool get_image_ready() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        return session_.image_ready();
    }

    bool get_is_pulse_guiding() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pulse_guide_failure_.empty()) {
            throw AlpacaException(pulse_guide_failure_, AlpacaError::DriverException);
        }
        return pulse_guiding_.load();
    }

    double get_last_exposure_duration() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        return session_.last_exposure_duration();
    }
    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        return session_.last_exposure_start_time();
    }

    int get_max_adu() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.bit_depth <= 0) return 0;
        return static_cast<int>((1ULL << camera_info_.bit_depth) - 1ULL);
    }

    int get_max_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_.supported_bins.empty()) return 1;
        return *std::max_element(camera_info_.supported_bins.begin(), camera_info_.supported_bins.end());
    }
    int get_max_bin_y() const override { return get_max_bin_x(); }

    int get_num_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_x_;
    }
    void set_num_x(int num_x) override { set_roi_size_common(num_x, std::nullopt); }
    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }
    void set_num_y(int num_y) override { set_roi_size_common(std::nullopt, num_y); }

    int get_offset() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_offset) {
            throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(sdk_.get_config_int(camera_info_.camera_id, /*config_id=*/7));  // POA_OFFSET
    }
    void set_offset(int offset) override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_offset) {
            throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
        }
        if (!caps_.offset_writable) {
            throw AlpacaException("Offset is read-only", AlpacaError::NotImplemented);
        }
        if (offset < caps_.offset_min || offset > caps_.offset_max) {
            throw AlpacaException("Offset out of range", AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();  // M15 — sensor register, rejected mid-exposure
        sdk_.set_config_int(camera_info_.camera_id, /*config_id=*/7, static_cast<long>(offset), false);  // POA_OFFSET
    }
    int get_offset_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_offset) {
            throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(caps_.offset_max);
    }
    int get_offset_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_offset) {
            throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
        }
        return static_cast<int>(caps_.offset_min);
    }
    std::vector<std::string> get_offsets() const override {
        throw AlpacaException("Offset descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        ensure_connected();
        return session_.percent_completed();
    }

    double get_pixel_size_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_um : 0.0;
    }
    double get_pixel_size_y() const override {
        // Player One reports a single square pixel size, so X == Y.
        return get_pixel_size_x();
    }

    int get_readout_mode() const override { return 0; }
    void set_readout_mode(int mode) override {
        if (mode != 0) {
            throw AlpacaException("Readout mode not supported", AlpacaError::NotImplemented);
        }
    }
    std::vector<std::string> get_readout_modes() const override { return {"Normal"}; }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.sensor_model.empty()) {
            return camera_info_.sensor_model;
        }
        return std::string();  // unknown: "" (ICameraV4 SensorName)
    }
    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) return SensorType::Monochrome;
        return bayer_to_sensor_type(camera_info_.bayer);
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        if (!get_can_set_ccd_temperature()) {
            throw AlpacaException("Set CCD temperature not supported", AlpacaError::NotImplemented);
        }
        return static_cast<double>(with_camera([this](int id) { return sdk_.get_target_temp_c(id); }));
    }
    void set_set_ccd_temperature(double temperature) override {
        ensure_connected();
        if (!get_can_set_ccd_temperature()) {
            throw AlpacaException("Set CCD temperature not supported", AlpacaError::NotImplemented);
        }
        if (!std::isfinite(temperature)) {
            throw AlpacaException("Target temperature must be finite", AlpacaError::InvalidValue);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected_locked();
            if (temperature < static_cast<double>(caps_.target_temp_min) ||
                temperature > static_cast<double>(caps_.target_temp_max) ||
                temperature < static_cast<double>(std::numeric_limits<int>::min()) ||
                temperature > static_cast<double>(std::numeric_limits<int>::max())) {
                throw AlpacaException("Target temperature out of range", AlpacaError::InvalidValue);
            }
        }
        const int target_c = static_cast<int>(std::lround(temperature));
        with_camera([&](int id) { sdk_.set_target_temp_c(id, target_c); });
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }
    void set_start_x(int start_x) override { set_start_pos_common(start_x, std::nullopt); }
    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }
    void set_start_y(int start_y) override { set_start_pos_common(std::nullopt, start_y); }

    double get_sub_exposure_duration() const override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }
    void set_sub_exposure_duration(double) override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void abort_exposure() override {
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected_locked();
            const CameraState state = session_.camera_state();
            if (state != CameraState::Exposing && state != CameraState::Download) return;
            if (state == CameraState::Download || (sdk_exposure_started_ && sdk_.image_ready(camera_info_.camera_id))) {
                throw AlpacaException("AbortExposure is not possible while the image is being downloaded",
                                      AlpacaError::InvalidOperation);
            }
            if (sdk_exposure_started_) {
                try {
                    sdk_.stop_exposure(camera_info_.camera_id);
                } catch (...) {
                    if (sdk_.image_ready(camera_info_.camera_id)) {
                        throw AlpacaException("AbortExposure is no longer possible because image readout has begun",
                                              AlpacaError::InvalidOperation);
                    }
                    throw;
                }
                sdk_exposure_started_ = false;
            }
        }
        // Cancels and joins the body; bounded by one readiness poll step or one get_image_data timeout.
        session_.abort();
    }

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
        PlayerOneGuideDirection dir = PlayerOneGuideDirection::North;
        switch (direction) {
            case 0:
                dir = PlayerOneGuideDirection::North;
                break;
            case 1:
                dir = PlayerOneGuideDirection::South;
                break;
            case 2:
                dir = PlayerOneGuideDirection::East;
                break;
            case 3:
                dir = PlayerOneGuideDirection::West;
                break;
        }

        // Serialize timer replacement with disconnect/destruction. The timer
        // body is joined by the slot, so the injected SDK remains alive through pulse-off.
        std::unique_lock<std::mutex> pulse_lifecycle_lock(pulse_guide_lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pulse_guiding_.load()) {
                throw AlpacaException("A pulse guide is already in progress", AlpacaError::InvalidOperation);
            }
        }
        const int pulse_camera_id = [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!camera_info_valid_ || camera_info_.camera_id < 0) {
                throw AlpacaException("Camera ID not available", AlpacaError::NotConnected);
            }
            if (pulse_guiding_.load()) {
                throw AlpacaException("A pulse guide is already in progress", AlpacaError::InvalidOperation);
            }
            const int id = camera_info_.camera_id;
            sdk_.pulse_guide_on(id, dir);
            pulse_guide_failure_.clear();
            active_pulse_direction_ = dir;
            pulse_guiding_.store(true);
            return id;
        }();
        auto pulse_task = [this, pulse_camera_id, dir, duration](util::OperationContext& ctx) {
            // A cancel (disconnect, destruction) ends the wait early and the pulse still turns off.
            static_cast<void>(ctx.wait_for(std::chrono::milliseconds(duration)));
            try {
                sdk_.pulse_guide_off(pulse_camera_id, dir);
                std::lock_guard<std::mutex> state_lock(mutex_);
                pulse_guiding_.store(false);
                pulse_guide_failure_.clear();
                active_pulse_direction_.reset();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "pulse_guide_off failed: " + std::string(e.what()));
                std::lock_guard<std::mutex> state_lock(mutex_);
                pulse_guide_failure_ = std::string("Player One pulse guide could not be stopped: ") + e.what();
            } catch (...) {
                ALPACA_LOG_WARN("PlayerOne", "pulse_guide_off threw a non-standard exception");
                std::lock_guard<std::mutex> state_lock(mutex_);
                pulse_guide_failure_ = "Player One pulse guide could not be stopped";
            }
        };
        try {
            pulse_slot_.start(std::move(pulse_task));
        } catch (const std::exception& e) {
            try {
                sdk_.pulse_guide_off(pulse_camera_id, dir);
            } catch (const std::exception& off_error) {
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guide_failure_ = std::string("Player One pulse guide could not be stopped: ") + off_error.what();
                throw AlpacaException(
                    "Player One pulse timer could not start and the guide output could not be "
                    "stopped; keep the camera connected and retry disconnect",
                    AlpacaError::DriverException);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guide_failure_ = "Player One pulse guide could not be stopped";
                throw AlpacaException(
                    "Player One pulse timer could not start and the guide output could not be "
                    "stopped; keep the camera connected and retry disconnect",
                    AlpacaError::DriverException);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guiding_.store(false);
                pulse_guide_failure_.clear();
                active_pulse_direction_.reset();
            }
            throw AlpacaException("Could not start Player One pulse timer: " + std::string(e.what()),
                                  AlpacaError::DriverException);
        }
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        (void)light;  // Player One has no mechanical shutter; dark zero-duration frames use ExposureMin.

        // The session's start() turns a NaN, an infinite or a huge duration into an undefined duration_cast, so
        // none of them may reach it. A finite duration below this bound is range-checked against ExposureMax.
        if (!std::isfinite(duration) || duration < 0.0 ||
            duration >= static_cast<double>(std::numeric_limits<long>::max()) / 1'000'000.0) {
            throw AlpacaException("Exposure duration must be non-negative", AlpacaError::InvalidValue);
        }

        long exposure_us_long = static_cast<long>(std::lround(duration * 1'000'000.0));
        int id = 0;
        int active_bin = 0;
        int active_start_x = 0;
        int active_start_y = 0;
        int active_num_x = 0;
        int active_num_y = 0;
        int sensor_width = 0;
        int sensor_height = 0;
        bool dirty_format = false;
        bool dirty_roi = false;
        PlayerOneImageFormat active_format = PlayerOneImageFormat::Raw16;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            validate_exposure_request_locked(exposure_us_long);
            const CameraState state = session_.camera_state();
            if (exposure_starting_ || state == CameraState::Exposing || state == CameraState::Download) {
                throw AlpacaException("Camera is finishing a previous exposure; try again shortly",
                                      AlpacaError::InvalidOperation);
            }
            // Retries the stop a failed earlier exposure left running.
            const std::string exposure_stop_error = stop_sdk_exposure_locked();
            if (!exposure_stop_error.empty()) {
                throw AlpacaException(exposure_stop_error, AlpacaError::DriverException);
            }
            id = camera_info_.camera_id;
            active_bin = bin_;
            active_start_x = start_x_;
            active_start_y = start_y_;
            active_num_x = num_x_;
            active_num_y = num_y_;
            sensor_width = camera_info_.max_width;
            sensor_height = camera_info_.max_height;
            dirty_format = format_dirty_;
            dirty_roi = roi_dirty_;
            active_format = active_format_;
            // Holds the settings (ensure_not_exposing_locked) from this snapshot until session_.start() has
            // published Exposing, so a gain/offset/geometry write cannot slip in between (M15).
            exposure_starting_ = true;
        }
        struct StartingDone {
            PlayerOneCameraDriver& driver;
            ~StartingDone() {
                std::lock_guard<std::mutex> lock(driver.mutex_);
                driver.exposure_starting_ = false;
            }
        } starting_done{*this};

        auto body = [this, id, exposure_us_long, active_bin, active_start_x, active_start_y, active_num_x, active_num_y,
                     sensor_width, sensor_height, dirty_format, dirty_roi, active_format](util::CaptureControl& c) {
            auto& sdk = sdk_;
            const auto cancelled = [&c] { return c.stop_reason() != util::StopReason::None; };
            // A body that ends early leaves the SDK exposure running: a failure, or a cancel (the watchdog, or a
            // disconnect that raced the driver's own stop) with no stop before it. Stop it before returning.
            const auto stop_running_exposure = [this, &sdk, id] {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!sdk_exposure_started_) return;
                try {
                    sdk.stop_exposure(id);
                    sdk_exposure_started_ = false;
                } catch (const std::exception& e) {
                    ALPACA_LOG_DEBUG("PlayerOne", "stop_exposure on body exit failed: " + std::string(e.what()));
                } catch (...) {
                    ALPACA_LOG_WARN("PlayerOne", "stop_exposure on body exit threw a non-standard exception");
                }
            };
            try {
                if (cancelled()) return;
                // Reconfiguring bin/format requires the camera be in STATE_OPENED
                // (not exposing). Stop defensively in case a prior exposure was
                // still running.
                if (dirty_format || dirty_roi) {
                    try {
                        sdk.stop_exposure(id);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_DEBUG("PlayerOne", "pre-exposure stop failed: " + std::string(e.what()));
                    }
                }

                if (dirty_format) {
                    try {
                        sdk.set_image_format(id, active_format);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("PlayerOne", std::string("set_image_format failed: ") + e.what());
                    }
                    if (sdk.get_image_format(id) != active_format) {
                        alpacacore::util::throw_invalid_camera_image("camera did not apply the requested pixel format");
                    }
                    sdk.set_image_bin(id, active_bin);
                    std::lock_guard<std::mutex> lock(mutex_);
                    format_dirty_ = false;
                }

                // Player One SDK requires width%4==0, height%2==0. Align DOWN
                // for the SDK call — the returned ImageArray is built at the
                // user's requested dims so clients see what they set.
                int sdk_w = active_num_x - (active_num_x % 4);
                int sdk_h = active_num_y - (active_num_y % 2);
                if (sdk_w <= 0) sdk_w = 4;
                if (sdk_h <= 0) sdk_h = 2;
                const int minimum_sdk_w = sdk_w;
                const int minimum_sdk_h = sdk_h;
                int sdk_start_x = active_start_x;
                int sdk_start_y = active_start_y;

                if (dirty_roi) {
                    // Order: size first (inside sensor at current bin), then
                    // start. POASetImageBin resets size/start so we always
                    // push both again.
                    sdk.set_image_size(id, sdk_w, sdk_h);
                    sdk.set_image_start_pos(id, active_start_x, active_start_y);
                    // Read back — SDK may have further aligned values.
                    try {
                        sdk.get_image_size(id, sdk_w, sdk_h);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_DEBUG("PlayerOne", "ROI size readback failed: " + std::string(e.what()));
                    } catch (...) {
                        ALPACA_LOG_WARN("PlayerOne", "ROI size readback threw a non-standard exception");
                    }
                    if (sdk_w < minimum_sdk_w || sdk_h < minimum_sdk_h) {
                        alpacacore::util::throw_invalid_camera_image(
                            "camera applied an ROI smaller than the aligned requested dimensions");
                    }
                    int rx = active_start_x;
                    int ry = active_start_y;
                    try {
                        sdk.get_image_start_pos(id, rx, ry);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_DEBUG("PlayerOne", "ROI start-position readback failed: " + std::string(e.what()));
                    } catch (...) {
                        ALPACA_LOG_WARN("PlayerOne", "ROI start-position readback threw a non-standard exception");
                    }
                    const int max_start_x = sensor_width / active_bin - sdk_w;
                    const int max_start_y = sensor_height / active_bin - sdk_h;
                    if (rx < 0 || ry < 0 || rx > max_start_x || ry > max_start_y) {
                        alpacacore::util::throw_invalid_camera_image(
                            "camera applied an ROI start outside the binned sensor bounds");
                    }
                    sdk_start_x = rx;
                    sdk_start_y = ry;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        start_x_ = rx;
                        start_y_ = ry;
                        roi_dirty_ = false;
                    }
                }

                // Set the exposure — POA_EXPOSURE's value is microseconds (long).
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (cancelled()) return;
                    sdk.set_config_int(id, /*config_id=*/0, exposure_us_long, false);  // POA_EXPOSURE
                    c.exposure_started();
                    sdk_exposure_started_ = true;
                    sdk.start_exposure(id, /*single_frame=*/true);
                }

                // Snapshot ROI (after any SDK alignment) so the buffer size
                // matches the image the SDK will deliver.
                int sdk_frame_w = sdk_w;
                int sdk_frame_h = sdk_h;
                sdk.get_image_size(id, sdk_frame_w, sdk_frame_h);
                if (sdk.get_image_format(id) != active_format) {
                    alpacacore::util::throw_invalid_camera_image(
                        "camera pixel format does not match the selected decoder");
                }
                const std::size_t bytes_per_pixel_value = bytes_per_pixel(active_format);
                if (sdk_frame_w < sdk_w || sdk_frame_h < sdk_h || sdk_frame_w < minimum_sdk_w ||
                    sdk_frame_h < minimum_sdk_h) {
                    alpacacore::util::throw_invalid_camera_image("frame dimensions are smaller than the aligned ROI");
                }
                const int max_frame_w = sensor_width / active_bin - sdk_start_x;
                const int max_frame_h = sensor_height / active_bin - sdk_start_y;
                if (sdk_frame_w > max_frame_w || sdk_frame_h > max_frame_h) {
                    alpacacore::util::throw_invalid_camera_image("frame dimensions exceed the binned sensor bounds");
                }

                const std::size_t buffer_bytes = checked_frame_bytes(sdk_frame_w, sdk_frame_h, bytes_per_pixel_value);

                // Poll for image readiness so a cancel short-circuits without a long blocking get_image_data.
                bool ready = false;
                std::string readiness_failure;
                for (;;) {
                    try {
                        if (sdk.image_ready(id)) {
                            ready = true;
                            break;
                        }
                    } catch (const std::exception& e) {
                        readiness_failure = std::string("Player One readiness check failed: ") + e.what();
                        ALPACA_LOG_WARN("PlayerOne", std::string("image_ready failed: ") + e.what());
                        break;
                    }
                    if (!c.wait_for(std::chrono::milliseconds(50))) break;
                }
                if (!ready) {
                    // Cancelled or past the deadline (the session already holds its own state), or the readiness
                    // check failed.
                    stop_running_exposure();
                    if (!cancelled() && !readiness_failure.empty()) {
                        throw AlpacaException(readiness_failure, AlpacaError::DriverException);
                    }
                    return;
                }

                c.reading_out();
                std::vector<std::uint8_t> raw(buffer_bytes, 0);
                const bool got = !cancelled() && sdk.get_image_data(id, raw.data(), raw.size(), kDownloadTimeoutMs);
                if (!got) {
                    stop_running_exposure();
                    if (!cancelled()) {
                        ALPACA_LOG_WARN("PlayerOne", "get_image_data timed out after ready=true");
                        throw AlpacaException("Player One image download timed out before the exposure deadline",
                                              AlpacaError::DriverException);
                    }
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    sdk_exposure_started_ = false;
                }
                ImageArray img =
                    build_image_array(raw, sdk_frame_w, sdk_frame_h, active_num_x, active_num_y, active_format);
                static_cast<void>(c.image_ready(std::make_shared<const ImageArray>(std::move(img))));
            } catch (const std::exception& e) {
                stop_running_exposure();
                if (cancelled()) {
                    ALPACA_LOG_DEBUG("PlayerOne", "Exposure body cancelled: " + std::string(e.what()));
                    return;
                }
                ALPACA_LOG_WARN("PlayerOne", "Exposure failed: " + std::string(e.what()));
                throw;
            } catch (...) {
                stop_running_exposure();
                if (cancelled()) return;
                ALPACA_LOG_WARN("PlayerOne", "Exposure failed with a non-standard exception");
                throw AlpacaException("Player One exposure failed with a non-standard SDK error",
                                      AlpacaError::DriverException);
            }
        };
        session_.start(static_cast<double>(exposure_us_long) / 1'000'000.0, std::move(body));
    }

    void stop_exposure() override {
        ensure_connected();
        throw AlpacaException("StopExposure is not supported (CanStopExposure is false); use AbortExposure",
                              AlpacaError::MethodNotImplemented);
    }

private:
    int device_number_;
    int camera_index_;
    PlayerOneSDK& sdk_;
    const std::chrono::steady_clock::duration completion_grace_;
    util::TaskClock& clock_;
    static constexpr int kDownloadTimeoutMs = 5000;  // one get_image_data call; the session watchdog bounds the rest
    PlayerOneCameraInfo camera_info_{};
    bool camera_info_valid_{false};
    PlayerOneConfigCaps caps_{};

    std::atomic<bool> connected_{false};
    std::mutex transition_mutex_;
    mutable std::mutex mutex_;

    int bin_{1};
    int start_x_{0};
    int start_y_{0};
    int num_x_{0};
    int num_y_{0};
    bool roi_dirty_{false};
    bool format_dirty_{false};
    PlayerOneImageFormat active_format_{PlayerOneImageFormat::Raw16};

    // True from a StartExposure's settings snapshot until session_.start() has published Exposing.
    bool exposure_starting_{false};
    // An SDK exposure is running that the SDK has not yet been told to stop.
    bool sdk_exposure_started_{false};

    mutable std::atomic<bool> pulse_guiding_{false};
    std::string pulse_guide_failure_;
    std::optional<PlayerOneGuideDirection> active_pulse_direction_;
    std::mutex pulse_guide_lifecycle_mutex_;
    util::OperationGeneration pulse_generation_;
    util::AsyncOperation pulse_slot_;  // the pulse-off timer body; destroyed (joined) before the members it uses

    // Declared after every member the exposure body touches: destroyed first, so the body is joined before they die.
    util::CaptureSession session_;

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    void ensure_connected_locked() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    // Reject runtime sensor-register / geometry writes while a frame is
    // integrating: the exposure body polls/downloads holding no lock, so a
    // mid-exposure write would race the live integration. Requires mutex_
    // held — the same lock start_exposure sets exposure_starting_ under,
    // closing the check/publish TOCTOU (M15; driver-lifecycle.instructions.md rule, matching
    // the ToupTek driver's ensure_not_exposing).
    // A cancelled or failed body can still be configuring the camera (ROI, format, start position), so
    // the settings stay locked while the session is busy, except once the frame is Ready (the body is
    // only returning then).
    void ensure_not_exposing_locked() const {
        const CameraState state = session_.camera_state();
        bool frame_ready = false;
        try {
            frame_ready = session_.image_ready();
        } catch (const AlpacaException&) {
            frame_ready = false;  // a kept failure: not ready
        }
        if (exposure_starting_ || sdk_exposure_started_ || state == CameraState::Exposing ||
            state == CameraState::Download || (session_.busy() && !frame_ready)) {
            throw AlpacaException("Cannot change camera settings during an exposure", AlpacaError::InvalidOperation);
        }
    }

    void validate_exposure_request_locked(long& exposure_us) const {
        ensure_connected_locked();
        if (!camera_info_valid_) {
            throw AlpacaException("Camera info not valid", AlpacaError::DriverException);
        }
        if (!caps_.has_exposure) {
            throw AlpacaException("Exposure not supported", AlpacaError::NotImplemented);
        }
        if (exposure_us < caps_.exposure_min_us) exposure_us = caps_.exposure_min_us;
        if (exposure_us > caps_.exposure_max_us) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }

        const int bin = bin_;
        if (bin <= 0 || camera_info_.max_width <= 0 || camera_info_.max_height <= 0) {
            throw AlpacaException("Camera returned invalid sensor geometry", AlpacaError::DriverException);
        }
        const int max_w = camera_info_.max_width / bin;
        const int max_h = camera_info_.max_height / bin;
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

    // Runs fn(camera_id) while holding mutex_, so a concurrent disconnect
    // cannot close the camera underneath the SDK call (driver-lifecycle.instructions.md shape (a)).
    // Fast register/control calls only — the exposure body keeps its bare
    // id snapshot because it must not hold mutex_ across the blocking image
    // wait. Must NOT be called with mutex_ already held (non-recursive).
    template <typename Fn>
    auto with_camera(Fn&& fn) const -> decltype(fn(0)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_.load() || !camera_info_valid_ || camera_info_.camera_id < 0) {
            throw AlpacaException("Camera ID not available", AlpacaError::NotConnected);
        }
        return fn(camera_info_.camera_id);
    }

    bool cooler_available_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_cooler && caps_.has_cooler;
    }

    void ensure_heater_supported() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_heater_power) {
            throw AlpacaException("Dew heater not supported by this camera", AlpacaError::NotImplemented);
        }
    }

    void ensure_fan_supported() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.has_fan_power) {
            throw AlpacaException("Radiator fan not supported by this camera", AlpacaError::NotImplemented);
        }
    }

    void validate_heater_range(int percent) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.heater_power_writable) {
            throw AlpacaException("Dew heater is read-only", AlpacaError::InvalidOperation);
        }
        if (percent < caps_.heater_power_min || percent > caps_.heater_power_max) {
            throw AlpacaException("Heater power out of range [" + std::to_string(caps_.heater_power_min) + ", " +
                                      std::to_string(caps_.heater_power_max) + "]",
                                  AlpacaError::InvalidValue);
        }
    }

    void validate_fan_range(int percent) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!caps_.fan_power_writable) {
            throw AlpacaException("Radiator fan is read-only", AlpacaError::InvalidOperation);
        }
        if (percent < caps_.fan_power_min || percent > caps_.fan_power_max) {
            throw AlpacaException("Fan power out of range [" + std::to_string(caps_.fan_power_min) + ", " +
                                      std::to_string(caps_.fan_power_max) + "]",
                                  AlpacaError::InvalidValue);
        }
    }

    void reset_exposure_state_locked() {
        // A disconnect racing an in-flight pulse must not leave
        // IsPulseGuiding=true for a freshly reconnected client.
        pulse_guiding_.store(false);
        pulse_guide_failure_.clear();
        active_pulse_direction_.reset();
        sdk_exposure_started_ = false;
    }

    // Stops the SDK exposure a body started and returns the error ("" on success). The body is joined
    // separately (session_.abort() / disconnect_and_join()).
    std::string stop_sdk_exposure() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_sdk_exposure_locked();
    }

    std::string stop_sdk_exposure_locked() {
        if (!connected_.load() || !camera_info_valid_ || camera_info_.camera_id < 0 || !sdk_exposure_started_) {
            return {};
        }
        std::string stop_error;
        try {
            sdk_.stop_exposure(camera_info_.camera_id);
            sdk_exposure_started_ = false;
        } catch (const std::exception& e) {
            stop_error = "Player One could not stop the camera exposure: " + std::string(e.what());
            ALPACA_LOG_WARN("PlayerOne", stop_error);
        } catch (...) {
            stop_error = "Player One could not stop the camera exposure";
            ALPACA_LOG_WARN("PlayerOne", stop_error + " (non-standard SDK exception)");
        }
        return stop_error;
    }

    void stop_pulse_guide_thread() {
        std::lock_guard<std::mutex> lifecycle_lock(pulse_guide_lifecycle_mutex_);
        stop_pulse_guide_thread_locked();
    }

    void stop_pulse_guide_thread_locked() {
        pulse_slot_.cancel_all_and_join();
        int camera_id = -1;
        std::optional<PlayerOneGuideDirection> direction;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pulse_guide_failure_.empty() || !active_pulse_direction_) return;
            camera_id = camera_info_.camera_id;
            direction = active_pulse_direction_;
        }
        try {
            sdk_.pulse_guide_off(camera_id, *direction);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guide_failure_ = std::string("Player One pulse guide could not be stopped: ") + e.what();
            throw AlpacaException(
                "Player One pulse guide could not be stopped; keep the camera connected and "
                "retry disconnect",
                AlpacaError::DriverException);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guide_failure_ = "Player One pulse guide could not be stopped";
            throw AlpacaException(
                "Player One pulse guide could not be stopped; keep the camera connected and "
                "retry disconnect",
                AlpacaError::DriverException);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guiding_.store(false);
            pulse_guide_failure_.clear();
            active_pulse_direction_.reset();
        }
    }

    void force_release_camera() noexcept {
        std::lock_guard<std::mutex> transition_lock(transition_mutex_);
        std::lock_guard<std::mutex> pulse_lock(pulse_guide_lifecycle_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_.load()) return;
        const int camera_id = camera_info_valid_ ? camera_info_.camera_id : -1;
        camera_info_ = {};
        camera_info_valid_ = false;
        caps_ = {};
        reset_exposure_state_locked();
        connected_.store(false);
        if (camera_id >= 0) {
            try {
                sdk_.close_camera(camera_id);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR("PlayerOne", "Forced camera close after shutdown failure: " + std::string(e.what()));
            } catch (...) {
                ALPACA_LOG_ERROR("PlayerOne",
                                 "Forced camera close after shutdown failure threw a non-standard exception");
            }
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
            ALPACA_LOG_DEBUG("PlayerOne", "Preload enumerate failed: " + std::string(e.what()));
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
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("PlayerOne", "camera-info refresh failed: " + std::string(e.what()));
        }
    }

    void set_bin_common(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!camera_info_valid_) {
            throw AlpacaException("Camera info not valid", AlpacaError::DriverException);
        }
        if (bin_x <= 0) {
            throw AlpacaException("Bin value must be positive", AlpacaError::InvalidValue);
        }
        if (std::find(camera_info_.supported_bins.begin(), camera_info_.supported_bins.end(), bin_x) ==
            camera_info_.supported_bins.end()) {
            throw AlpacaException("Bin value not supported", AlpacaError::InvalidValue);
        }
        // Geometry write: rejected mid-exposure, checked under the same
        // mutex_ hold that start_exposure sets exposure_starting_ (M15).
        ensure_not_exposing_locked();
        if (bin_ == bin_x) return;
        bin_ = bin_x;
        // Binning in the Player One SDK resets size/start — mirror that in
        // our cache so the driver's next set_num_x/y uses the right base.
        // Align to SDK requirements (width%4==0, height%2==0).
        int w = camera_info_.max_width / bin_;
        int h = camera_info_.max_height / bin_;
        num_x_ = w - (w % 4);
        num_y_ = h - (h % 2);
        start_x_ = 0;
        start_y_ = 0;
        format_dirty_ = true;
        roi_dirty_ = true;
        session_.discard_image();
    }

    // width/height (or sx/sy) of std::nullopt means "leave that axis unchanged",
    // resolved UNDER mutex_: each public setter passes only its own axis, so a
    // concurrent setter for the other axis can no longer be clobbered by a stale
    // pre-lock get_num_x()/get_num_y() snapshot (lost-update TOCTOU).
    void set_roi_size_common(std::optional<int> width_opt, std::optional<int> height_opt) {
        ensure_connected();
        // ASCOM convention: setters are lenient, StartExposure does the
        // sensor-bounds validation. ConformU's "Reject Bad XSize/YSize" tests
        // set values beyond sensor dimensions and expect the error at
        // StartExposure rather than here.
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        const int width = width_opt.value_or(num_x_);
        const int height = height_opt.value_or(num_y_);
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();  // M15 — see set_bin_common
        if (num_x_ == width && num_y_ == height) return;
        num_x_ = width;
        num_y_ = height;
        roi_dirty_ = true;
        session_.discard_image();
    }

    void set_start_pos_common(std::optional<int> sx_opt, std::optional<int> sy_opt) {
        ensure_connected();
        // ASCOM convention: setters are lenient, StartExposure validates.
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        const int sx = sx_opt.value_or(start_x_);
        const int sy = sy_opt.value_or(start_y_);
        if (sx < 0 || sy < 0) {
            throw AlpacaException("Start position must be non-negative", AlpacaError::InvalidValue);
        }
        ensure_not_exposing_locked();  // M15 — see set_bin_common
        if (start_x_ == sx && start_y_ == sy) return;
        start_x_ = sx;
        start_y_ = sy;
        roi_dirty_ = true;
        session_.discard_image();
    }

    ImageArray build_image_array(const std::vector<std::uint8_t>& buffer, int src_w, int src_h, int out_w, int out_h,
                                 PlayerOneImageFormat format) const {
        ImageArray image;
        image.width = out_w;
        image.height = out_h;
        image.rank = format == PlayerOneImageFormat::Rgb24 ? 3 : 2;
        const auto output_shape = alpacacore::util::validate_image_shape(image);
        // The SDK buffer is src_w × src_h (aligned). The returned ImageArray is
        // out_w × out_h (what the client requested). Copy the overlapping
        // region; any trailing row/col is left zeroed.
        const int copy_w = std::min(src_w, out_w);
        const int copy_h = std::min(src_h, out_h);
        const std::size_t out_pixels = output_shape.width * output_shape.height;

        if (format == PlayerOneImageFormat::Raw16) {
            image.data.assign(out_pixels, 0);
            const std::size_t src_stride = static_cast<std::size_t>(src_w) * 2;
            for (int y = 0; y < copy_h; ++y) {
                const std::size_t row_off = static_cast<std::size_t>(y) * src_stride;
                for (int x = 0; x < copy_w; ++x) {
                    const std::size_t j = row_off + static_cast<std::size_t>(x) * 2;
                    std::uint16_t px =
                        static_cast<std::uint16_t>(buffer[j]) | (static_cast<std::uint16_t>(buffer[j + 1]) << 8);
                    image.data[static_cast<std::size_t>(y) * out_w + x] = static_cast<std::int32_t>(px);
                }
            }
        } else if (format == PlayerOneImageFormat::Raw8 || format == PlayerOneImageFormat::Mono8) {
            image.data.assign(out_pixels, 0);
            const std::size_t src_stride = static_cast<std::size_t>(src_w);
            for (int y = 0; y < copy_h; ++y) {
                const std::size_t row_off = static_cast<std::size_t>(y) * src_stride;
                for (int x = 0; x < copy_w; ++x) {
                    const std::size_t j = row_off + static_cast<std::size_t>(x);
                    image.data[static_cast<std::size_t>(y) * out_w + x] = static_cast<std::int32_t>(buffer[j]);
                }
            }
        } else if (format == PlayerOneImageFormat::Rgb24) {
            // Present RGB24 as rank-3 (H x W x 3). Pack each channel into its
            // plane as ASCOM clients expect.
            image.data.assign(output_shape.element_count, 0);
            const std::size_t src_stride = static_cast<std::size_t>(src_w) * 3;
            for (int y = 0; y < copy_h; ++y) {
                const std::size_t row_off = static_cast<std::size_t>(y) * src_stride;
                for (int x = 0; x < copy_w; ++x) {
                    const std::size_t j = row_off + static_cast<std::size_t>(x) * 3;
                    // Player One RGB24 is stored as B,G,R per pixel; transpose
                    // to R,G,B for Alpaca clients.
                    std::int32_t b = buffer[j];
                    std::int32_t g = buffer[j + 1];
                    std::int32_t r = buffer[j + 2];
                    const std::size_t out_i = (static_cast<std::size_t>(y) * out_w + x) * 3;
                    image.data[out_i + 0] = r;
                    image.data[out_i + 1] = g;
                    image.data[out_i + 2] = b;
                }
            }
        }
        alpacacore::util::validate_image_array(image);
        return image;
    }
};

std::unique_ptr<CameraDriver> create_playerone_camera(int device_number, int camera_index) {
    return create_playerone_camera(device_number, camera_index, PlayerOneSDKWrapper::instance());
}

std::unique_ptr<CameraDriver> create_playerone_camera(int device_number, int camera_index, PlayerOneSDK& sdk,
                                                      std::chrono::steady_clock::duration completion_grace,
                                                      util::TaskClock& clock) {
    return std::make_unique<PlayerOneCameraDriver>(device_number, camera_index, sdk, completion_grace, clock);
}

}  // namespace alpacacore::vendor::playerone
