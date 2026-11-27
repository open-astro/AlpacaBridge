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

// The remotealpaca Camera proxied device (decision 0023, issue #1753 slice 5, #2703) over UpstreamProxy
// (upstream_proxy.h), which holds every rule it shares with the other proxied devices. What is the Camera's own stays
// here: the frame (what the camera changes by itself), what the Connect reads once (the sensor's description and the
// client's settings, which only a client changes and this driver writes through), the members an older ICamera
// upstream lacks (by the InterfaceVersion the proxy read), the commands and the image download.
//
// The upstream runs the exposure: StartExposure is a command that returns once the upstream took it, as an ASCOM
// StartExposure does, and the poll brings ImageReady. No exposure body runs here, so there is no capture session
// (decision 0020 is a camera whose exposure runs in this process).

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/camera_frame_request.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_camera_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "upstream_proxy.h"

namespace alpacacore::vendor::remotealpaca {

namespace {

using detail::Answer;
using detail::answer_of;
using detail::number_text;

// ICameraV2 added BayerOffsetX/Y, CanFastReadout, ExposureMax/Min/Resolution, FastReadout, Gain, GainMax/Min, Gains,
// PercentCompleted, ReadoutMode(s), SensorName and SensorType; ICameraV3 added Offset, OffsetMax/Min, Offsets and
// SubExposureDuration. An older upstream has none of them: each answers NotImplemented and is never called.
constexpr int kFirstVersionV2 = 2;
constexpr int kFirstVersionV3 = 3;

constexpr int kCameraStateExposing = static_cast<int>(CameraState::Exposing);

// The texts of the frame request checks (util/camera_frame_request.h); the module's defaults.
constexpr util::CameraRequestTexts kTexts{};
static_assert(util::all_texts_set(kTexts));

// The polled frame (decision 0010): what the camera changes by itself, each a value or the upstream's error.
struct Frame {
    Answer<int> camera_state;
    Answer<bool> image_ready;
    // Asked only while the sampled CameraState is Exposing; otherwise the InvalidOperation ICameraV4 gives it.
    Answer<double> percent_completed;
    Answer<double> ccd_temperature;
    Answer<double> cooler_power;
    Answer<double> heat_sink_temperature;
    Answer<bool> is_pulse_guiding;
    Answer<double> last_exposure_duration;
    Answer<std::chrono::system_clock::time_point> last_exposure_start;
    // LastExposure* were read since ImageReady last turned true, so a poll copies them instead of asking again.
    bool last_exposure_read = false;
};

using Proxy = detail::UpstreamProxy<Frame>;

// What the Connect reads once: the sensor and its limits. ElectronsPerADU and FullWellCapacity follow the gain and the
// readout mode, so a write of either reads them again.
struct Sensor {
    Answer<int> bayer_offset_x;
    Answer<int> bayer_offset_y;
    Answer<int> camera_x_size;
    Answer<int> camera_y_size;
    Answer<bool> can_abort_exposure;
    Answer<bool> can_asymmetric_bin;
    Answer<bool> can_fast_readout;
    Answer<bool> can_get_cooler_power;
    Answer<bool> can_pulse_guide;
    Answer<bool> can_set_ccd_temperature;
    Answer<bool> can_stop_exposure;
    Answer<double> electrons_per_adu;
    Answer<double> exposure_max;
    Answer<double> exposure_min;
    Answer<double> exposure_resolution;
    Answer<double> full_well_capacity;
    Answer<int> gain_max;
    Answer<int> gain_min;
    Answer<std::vector<std::string>> gains;
    Answer<bool> has_shutter;
    Answer<int> max_adu;
    Answer<int> max_bin_x;
    Answer<int> max_bin_y;
    Answer<int> offset_max;
    Answer<int> offset_min;
    Answer<std::vector<std::string>> offsets;
    Answer<double> pixel_size_x;
    Answer<double> pixel_size_y;
    Answer<std::vector<std::string>> readout_modes;
    Answer<std::string> sensor_name;
    Answer<int> sensor_type;
};

// The client's settings: read at Connect and written through by this server's writes. A change another client of
// the upstream makes is not seen before the next Connect (the poll stays small).
struct Settings {
    Answer<int> bin_x;
    Answer<int> bin_y;
    Answer<int> num_x;
    Answer<int> num_y;
    Answer<int> start_x;
    Answer<int> start_y;
    Answer<int> gain;
    Answer<int> offset;
    Answer<int> readout_mode;
    Answer<bool> fast_readout;
    Answer<bool> cooler_on;
    Answer<double> set_ccd_temperature;
    Answer<double> sub_exposure_duration;
};

// Which telemetry members the poll asks: the Connect asked each once, and an upstream NotImplemented (0x400) leaves
// it out of the poll, its getter answering that error unchanged.
struct Polled {
    bool ccd_temperature = true;
    bool cooler_power = true;
    bool heat_sink_temperature = true;
    bool is_pulse_guiding = true;
};

// What the Connect stores, under the proxy's state lock with the identity.
struct Held {
    Sensor sensor;
    Settings settings;
    Polled polled;
    Frame missing;  // the telemetry answers of the members the poll leaves out
    ImageFetchBounds bounds;
};

template <class T>
bool answered_not_implemented(const Answer<T>& answer) {
    return !answer.value && answer.error_number == AlpacaError::NotImplemented;
}

std::string bool_text(bool value) { return value ? "True" : "False"; }

class RemoteAlpacaCameraDriver final : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaCameraDriver(int device_number, const RemoteDeviceConfig& config,
                             std::shared_ptr<util::HttpClient> http, ServerAddressSource self, util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          // ICameraV4 is the first Camera with Connect; an older upstream gets Connected=true.
          proxy_({"camera", "Camera", 4, [this] { return sample(); }}, config, std::move(http), std::move(self),
                 clock) {}

    ~RemoteAlpacaCameraDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_CAM_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca Camera Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }  // ICameraV4 (Platform 7)

    // A loss the module latched (a frame or a fault past the bound) reads false at once, before on_lost() runs.
    bool get_connected() const override { return proxy_.connected_flag().load() && !proxy_.link_lost(); }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    // Decision 0009 rule 5: the latched fault, or after a loss the reason it was lost, kept until the next Connect or
    // Disconnect.
    std::string get_link_fault() const override { return proxy_.link_fault(); }

    void set_connected(bool connected) override {
        // Issue #528: the connect makes HTTP calls, so a sync disconnect must not slip in between its checks and its
        // store. Guards set_connected() against set_connected() only; no getter takes it.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(proxy_.connected_flag().load())) {
            return;
        }
        if (connected && consume_pending_disconnect(proxy_.connected_flag().load())) {
            return;
        }
        if (connected) {
            if (get_connected()) {
                return;  // idempotent: connected, and the poll is running
            }
            proxy_.connect([this] { return read_at_connect(); });
        } else {
            proxy_.disconnect();
            // The held image belongs to the connection that fetched it.
            proxy_.with_state([this](Proxy::State&) { image_.reset(); });
        }
    }

    // --- Capabilities: answered while disconnected (the last Connect's value, false before any), the upstream's
    // answer or its error once connected. One state-lock hold picks the answer, so a racing Disconnect never makes
    // the read throw NotConnected.
    bool get_can_abort_exposure() const override { return from_capability(&Sensor::can_abort_exposure); }
    bool get_can_asymmetric_bin() const override { return from_capability(&Sensor::can_asymmetric_bin); }
    bool get_can_fast_readout() const override { return from_capability(&Sensor::can_fast_readout); }
    bool get_can_get_cooler_power() const override { return from_capability(&Sensor::can_get_cooler_power); }
    bool get_can_pulse_guide() const override { return from_capability(&Sensor::can_pulse_guide); }
    bool get_can_set_ccd_temperature() const override { return from_capability(&Sensor::can_set_ccd_temperature); }
    bool get_can_stop_exposure() const override { return from_capability(&Sensor::can_stop_exposure); }

    // --- The sensor, read at Connect: keeps answering while the link is faulted (static metadata).
    int get_bayer_offset_x() const override { return from_sensor(&Sensor::bayer_offset_x); }
    int get_bayer_offset_y() const override { return from_sensor(&Sensor::bayer_offset_y); }
    int get_camera_x_size() const override { return from_sensor(&Sensor::camera_x_size); }
    int get_camera_y_size() const override { return from_sensor(&Sensor::camera_y_size); }
    double get_electrons_per_adu() const override { return from_sensor(&Sensor::electrons_per_adu); }
    double get_exposure_max() const override { return from_sensor(&Sensor::exposure_max); }
    double get_exposure_min() const override { return from_sensor(&Sensor::exposure_min); }
    double get_exposure_resolution() const override { return from_sensor(&Sensor::exposure_resolution); }
    double get_full_well_capacity() const override { return from_sensor(&Sensor::full_well_capacity); }
    int get_gain_max() const override { return from_sensor(&Sensor::gain_max); }
    int get_gain_min() const override { return from_sensor(&Sensor::gain_min); }
    std::vector<std::string> get_gains() const override { return from_sensor(&Sensor::gains); }
    bool get_has_shutter() const override { return from_sensor(&Sensor::has_shutter); }
    int get_max_adu() const override { return from_sensor(&Sensor::max_adu); }
    int get_max_bin_x() const override { return from_sensor(&Sensor::max_bin_x); }
    int get_max_bin_y() const override { return from_sensor(&Sensor::max_bin_y); }
    int get_offset_max() const override { return from_sensor(&Sensor::offset_max); }
    int get_offset_min() const override { return from_sensor(&Sensor::offset_min); }
    std::vector<std::string> get_offsets() const override { return from_sensor(&Sensor::offsets); }
    double get_pixel_size_x() const override { return from_sensor(&Sensor::pixel_size_x); }
    double get_pixel_size_y() const override { return from_sensor(&Sensor::pixel_size_y); }
    std::vector<std::string> get_readout_modes() const override { return from_sensor(&Sensor::readout_modes); }
    std::string get_sensor_name() const override { return from_sensor(&Sensor::sensor_name); }

    SensorType get_sensor_type() const override {
        const int type = from_sensor(&Sensor::sensor_type);
        if (type < static_cast<int>(SensorType::Monochrome) || type > static_cast<int>(SensorType::LRGB)) {
            throw AlpacaException("Upstream SensorType " + std::to_string(type) + " is not an ASCOM SensorType",
                                  AlpacaError::DriverException);
        }
        return static_cast<SensorType>(type);
    }

    // --- The settings, read at Connect and written through: keep answering while the link is faulted.
    int get_bin_x() const override { return from_settings(&Settings::bin_x); }
    int get_bin_y() const override { return from_settings(&Settings::bin_y); }
    int get_num_x() const override { return from_settings(&Settings::num_x); }
    int get_num_y() const override { return from_settings(&Settings::num_y); }
    int get_start_x() const override { return from_settings(&Settings::start_x); }
    int get_start_y() const override { return from_settings(&Settings::start_y); }
    int get_gain() const override { return from_settings(&Settings::gain); }
    int get_offset() const override { return from_settings(&Settings::offset); }
    int get_readout_mode() const override { return from_settings(&Settings::readout_mode); }
    bool get_fast_readout() const override { return from_settings(&Settings::fast_readout); }
    bool get_cooler_on() const override { return from_settings(&Settings::cooler_on); }
    double get_set_ccd_temperature() const override { return from_settings(&Settings::set_ccd_temperature); }
    double get_sub_exposure_duration() const override { return from_settings(&Settings::sub_exposure_duration); }

    // --- The frame (a stale frame is served), or the upstream's error unchanged; a latched fault refuses it with
    // DriverException (decision 0009 rule 1).
    CameraState get_camera_state() const override {
        const int state = from_frame(&Frame::camera_state);
        if (state < static_cast<int>(CameraState::Idle) || state > static_cast<int>(CameraState::Error)) {
            throw AlpacaException("Upstream CameraState " + std::to_string(state) + " is not an ASCOM CameraState",
                                  AlpacaError::DriverException);
        }
        return static_cast<CameraState>(state);
    }
    bool get_image_ready() const override { return from_frame(&Frame::image_ready); }
    double get_percent_completed() const override { return from_frame(&Frame::percent_completed); }
    double get_ccd_temperature() const override { return from_frame(&Frame::ccd_temperature); }
    double get_cooler_power() const override { return from_frame(&Frame::cooler_power); }
    double get_heat_sink_temperature() const override { return from_frame(&Frame::heat_sink_temperature); }
    bool get_is_pulse_guiding() const override { return from_frame(&Frame::is_pulse_guiding); }
    // The upstream's InvalidOperation before its first image is the ICameraV4 "asked too early" answer (#2182).
    double get_last_exposure_duration() const override {
        return asked_too_early_if_refused([this] { return from_frame(&Frame::last_exposure_duration); });
    }
    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        return asked_too_early_if_refused([this] { return from_frame(&Frame::last_exposure_start); });
    }

    // The image of the last exposure, from the upstream's imagearray as ImageBytes, in the command path: the poll
    // never carries it. A downloaded image is held until the next StartExposure, a Disconnect, or a frame that reads
    // ImageReady false; until then a read answers it again without a call. Refused while the link is faulted. The
    // upstream's InvalidOperation (no image yet) is the ICameraV4 "asked too early" answer (issue #1167).
    ImageArray get_image_array() const override {
        proxy_.require_connected();
        std::uint64_t serial = 0;
        auto held = proxy_.with_connected([this, &serial](const Proxy::State& state) {
            serial = exposure_serial_;
            const bool ready = state.served().value.image_ready.value.value_or(false);  // a fault throws here
            return ready ? image_ : nullptr;
        });
        if (held) return *held;
        std::shared_ptr<const ImageArray> fetched;
        try {
            // ImageArray is const in the interface; the download is a command that changes only the held image.
            auto& self = const_cast<RemoteAlpacaCameraDriver&>(*this);
            self.run([&self, serial, &fetched](util::OperationContext& ctx) {
                if (self.command_hook_) self.command_hook_();
                // A reader queued behind another's download of the same exposure takes the image it held.
                const ImageFetchBounds bounds = self.proxy_.with_state([&self, serial, &fetched](Proxy::State& state) {
                    const bool ready = state.served().value.image_ready.value.value_or(false);
                    if (ready && self.exposure_serial_ == serial) fetched = self.image_;
                    return self.held_.bounds;
                });
                if (fetched) return;
                fetched = std::make_shared<const ImageArray>(
                    self.proxy_.client().get_image_array(bounds.max_body_bytes, bounds.timeout));
                self.proxy_.with_state([&self, &ctx, serial, &fetched](Proxy::State&) {
                    // A StartExposure since, a newer command or a Disconnect: this image is not the one to hold.
                    if (ctx.stop_reason() == util::StopReason::Superseded || !self.get_connected() ||
                        self.exposure_serial_ != serial) {
                        return;
                    }
                    self.image_ = fetched;
                });
            });
        } catch (const AlpacaException& e) {
            if (refused_too_early(e)) throw AlpacaException::asked_too_early(e.what());
            throw;
        }
        return *fetched;
    }

    // COM only: the HTTP route refuses ImageArrayVariant without asking the driver.
    std::string get_image_array_variant() const override {
        proxy_.require_connected();
        throw AlpacaException("ImageArrayVariant is not supported by Alpaca; use ImageArray",
                              AlpacaError::NotImplemented);
    }

    // --- StartExposure, in the camera argument order (ascom-contract.instructions.md, util/camera_frame_request.h):
    // (1) the duration, (2) the connection, (3) ExposureMin and ExposureMax as the Connect read them (a duration below
    // ExposureMin runs at it). (4) The frame against the sensor and (5) an exposure already running are the upstream's
    // to judge: the settings held here are what this server wrote, and another client of the upstream may have changed
    // them; its InvalidValue or InvalidOperation passes through. Refused while the link is faulted. The body forwards
    // the start and writes CameraState Exposing and ImageReady false through; only the poll writes ImageReady true.
    void start_exposure(double duration, bool light) override {
        const double requested_us = util::check_exposure_duration(duration, kTexts);
        proxy_.require_connected();
        double seconds = duration;
        proxy_.with_connected([&seconds, requested_us, this](const Proxy::State&) {
            const Sensor& s = held_.sensor;
            if (!s.exposure_min.value || !s.exposure_max.value) return;  // an older upstream judges it alone
            const long min_us = std::lround(*s.exposure_min.value * 1'000'000.0);
            const long max_us = std::lround(*s.exposure_max.value * 1'000'000.0);
            seconds = static_cast<double>(util::exposure_microseconds(requested_us, min_us, max_us, kTexts)) / 1e6;
        });
        require_frame();
        run([this, seconds, light](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("startexposure", {{"Duration", number_text(seconds)}, {"Light", bool_text(light)}});
            proxy_.with_state([this, &ctx](Proxy::State& state) {
                if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
                ++exposure_serial_;
                image_.reset();
                state.write([](Frame& frame) {
                    frame.camera_state = {kCameraStateExposing, 0, {}};
                    frame.image_ready = {false, 0, {}};
                    frame.percent_completed = {0.0, 0, {}};
                    frame.last_exposure_read = false;  // the next ImageReady reads LastExposure* again
                });
            });
        });
    }

    // Forwarded even while the link is faulted: refused only for a lost link (NotConnected). The upstream's
    // CameraState is read back after the PUT; a read with no usable reply leaves it to the next poll.
    void abort_exposure() override { stop_or_abort("abortexposure"); }
    void stop_exposure() override { stop_or_abort("stopexposure"); }

    // Direction 0 to 3 and a duration of 0 to kDefaultMaxPulseGuideMs (#2292: the upstream may set no bound), before
    // the connection check; then forwarded, and IsPulseGuiding written true through, as the Telescope's PulseGuide:
    // the next poll serves the upstream's own answer (a short pulse may already have ended).
    void pulse_guide(int direction, int duration) override {
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Guide direction must be 0 to 3", AlpacaError::InvalidValue);
        }
        if (duration < 0 || duration > kDefaultMaxPulseGuideMs) {
            throw AlpacaException("Guide duration must be 0 to " + std::to_string(kDefaultMaxPulseGuideMs) + " ms",
                                  AlpacaError::InvalidValue);
        }
        proxy_.require_connected();
        require_frame();
        run([this, direction, duration](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("pulseguide",
                                {{"Direction", std::to_string(direction)}, {"Duration", std::to_string(duration)}});
            write_frame(ctx, [](Frame& frame) { frame.is_pulse_guiding = {true, 0, {}}; });
        });
    }

    // --- The setters: each value check before the connection check, then the device check, then refused while the
    // link is faulted; forwarded and written through. The upstream refuses a write while it exposes
    // (InvalidOperation, passed through).
    void set_bin_x(int bin) override { set_bin("binx", "BinX", bin, &Sensor::max_bin_x, &Settings::bin_x); }
    void set_bin_y(int bin) override { set_bin("biny", "BinY", bin, &Sensor::max_bin_y, &Settings::bin_y); }

    void set_num_x(int num_x) override {
        util::check_frame_size(num_x, kTexts);
        write_int("numx", "NumX", num_x, &Settings::num_x, 1);
    }
    void set_num_y(int num_y) override {
        util::check_frame_size(num_y, kTexts);
        write_int("numy", "NumY", num_y, &Settings::num_y, 1);
    }
    void set_start_x(int start_x) override {
        util::check_frame_start(start_x, kTexts);
        write_int("startx", "StartX", start_x, &Settings::start_x, 1);
    }
    void set_start_y(int start_y) override {
        util::check_frame_start(start_y, kTexts);
        write_int("starty", "StartY", start_y, &Settings::start_y, 1);
    }

    // The upstream judges the range (GainMin to GainMax, or an index into Gains). ElectronsPerADU and
    // FullWellCapacity follow the gain: read again after the write.
    void set_gain(int gain) override { write_int("gain", "Gain", gain, &Settings::gain, kFirstVersionV2, true); }
    void set_offset(int offset) override { write_int("offset", "Offset", offset, &Settings::offset, kFirstVersionV3); }

    // An index into ReadoutModes: below 0 is invalid whatever the camera is, past the list the Connect read is
    // invalid once connected.
    void set_readout_mode(int mode) override {
        if (mode < 0) throw AlpacaException("ReadoutMode must be 0 or more", AlpacaError::InvalidValue);
        proxy_.require_connected();
        require_version(kFirstVersionV2, "ReadoutMode");
        proxy_.with_connected([this, mode](const Proxy::State&) {
            const auto& modes = held_.sensor.readout_modes.value;
            if (modes && static_cast<std::size_t>(mode) >= modes->size()) {
                throw AlpacaException("ReadoutMode " + std::to_string(mode) + " is not one of the " +
                                          std::to_string(modes->size()) + " ReadoutModes",
                                      AlpacaError::InvalidValue);
            }
        });
        write_int("readoutmode", "ReadoutMode", mode, &Settings::readout_mode, kFirstVersionV2, true);
    }

    void set_fast_readout(bool fast) override {
        proxy_.require_connected();
        require_version(kFirstVersionV2, "FastReadout");
        require_frame();
        run([this, fast](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("fastreadout", {{"FastReadout", bool_text(fast)}});
            write_held(ctx, [fast](Held& held) { held.settings.fast_readout = {fast, 0, {}}; });
        });
    }

    void set_cooler_on(bool on) override {
        proxy_.require_connected();
        require_frame();
        run([this, on](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("cooleron", {{"CoolerOn", bool_text(on)}});
            write_held(ctx, [on](Held& held) { held.settings.cooler_on = {on, 0, {}}; });
        });
    }

    // A setpoint that is not a number is invalid whatever the camera is (no static bound: the upstream reports
    // none); the upstream judges its range.
    void set_set_ccd_temperature(double temperature) override {
        util::check_ccd_setpoint(temperature, {}, kTexts);
        write_double("setccdtemperature", "SetCCDTemperature", temperature, &Settings::set_ccd_temperature, 1);
    }

    void set_sub_exposure_duration(double duration) override {
        if (!std::isfinite(duration) || duration < 0.0) {
            throw AlpacaException("SubExposureDuration must be finite and non-negative", AlpacaError::InvalidValue);
        }
        write_double("subexposureduration", "SubExposureDuration", duration, &Settings::sub_exposure_duration,
                     kFirstVersionV3);
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; a command body calls it first, before its call.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    // Set while no command runs; a caller that waits for an earlier command calls it first.
    void set_command_waiting_hook(std::function<void()> hook) { proxy_.set_command_waiting_hook(std::move(hook)); }

protected:
    // DeviceState's TimeStamp is when the served frame was measured, not the request time (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        return proxy_.with_state([](const util::PolledState<Frame>& state) { return state.age(); });
    }

private:
    template <class T>
    bool from_capability(Answer<T> Sensor::*member) const {
        return proxy_.with_state([this, member](const Proxy::State&) {
            const Answer<T>& answer = held_.sensor.*member;
            if (!proxy_.connected()) return answer.value.value_or(false);
            return answer.get();
        });
    }

    template <class T>
    T from_sensor(Answer<T> Sensor::*member) const {
        return proxy_.with_connected([this, member](const Proxy::State&) { return (held_.sensor.*member).get(); });
    }

    template <class T>
    T from_settings(Answer<T> Settings::*member) const {
        return proxy_.with_connected([this, member](const Proxy::State&) { return (held_.settings.*member).get(); });
    }

    template <class T>
    T from_frame(Answer<T> Frame::*member) const {
        return proxy_.with_connected(
            [member](const Proxy::State& state) { return (state.served().value.*member).get(); });
    }

    // An upstream InvalidOperation as the "asked too early" answer, so the router logs it at DEBUG (issue #1167);
    // every other error unchanged.
    static bool refused_too_early(const AlpacaException& e) {
        return e.error_code() == AlpacaError::InvalidOperation && !e.is_asked_too_early();
    }

    template <class Fn>
    static auto asked_too_early_if_refused(Fn&& fn) -> decltype(fn()) {
        try {
            return fn();
        } catch (const AlpacaException& e) {
            if (refused_too_early(e)) throw AlpacaException::asked_too_early(e.what());
            throw;
        }
    }

    // A command that needs the link: NotConnected, or refused while the link is faulted (decision 0009 rule 1).
    void require_frame() const {
        (void)proxy_.with_connected([](const Proxy::State& state) { return state.served().measured_at; });
    }

    // NotImplemented for a member an older upstream lacks: it is never called, so an upstream that answers an
    // unknown member with HTTP 400 does not read as a failed command or poll.
    std::string lacks_text(int version, const char* member) const {
        const RemoteDeviceConfig& config = proxy_.config();
        return "Upstream " + config.host + ":" + std::to_string(config.port) + " device " +
               std::to_string(config.upstream_device_number) + " is an ICameraV" + std::to_string(version) +
               " and has no " + member;
    }

    template <class T>
    Answer<T> lacks(int version, const char* member) const {
        return {std::nullopt, AlpacaError::NotImplemented, lacks_text(version, member)};
    }

    void require_version(int first, const char* member) const {
        const int version = proxy_.upstream_interface_version();
        if (version < first) throw AlpacaException(lacks_text(version, member), AlpacaError::NotImplemented);
    }

    void stop_or_abort(const char* member) {
        proxy_.require_connected();
        run([this, member](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            RemoteAlpacaClient& client = proxy_.client();
            client.put(member);
            std::optional<Answer<int>> state;
            try {
                state = answer_of([&] { return client.get_int("camerastate"); });
            } catch (const RemoteTransportError&) {  // NOLINT(bugprone-empty-catch): the next poll reads it
            }
            if (state) write_frame(ctx, [&state](Frame& frame) { frame.camera_state = *state; });
        });
    }

    // BinX or BinY: (1) 0 or less, (2) the connection, (3) above the MaxBin the Connect read. The upstream may change
    // the other bin (a camera without asymmetric binning) and the frame with it, so they are read back after the PUT;
    // a read with no usable reply keeps the value held and ends the read-back.
    void set_bin(const char* member, const char* parameter, int bin, Answer<int> Sensor::*max,
                 Answer<int> Settings::*field) {
        util::check_bin(bin, kTexts);
        proxy_.require_connected();
        proxy_.with_connected([this, bin, max](const Proxy::State&) {
            const Answer<int>& limit = held_.sensor.*max;
            if (limit.value && bin > *limit.value)
                throw AlpacaException(kTexts.bin_not_supported, AlpacaError::InvalidValue);
        });
        require_frame();
        run([this, member, parameter, bin, field](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            RemoteAlpacaClient& client = proxy_.client();
            client.put(member, {{parameter, std::to_string(bin)}});
            write_held(ctx, [bin, field](Held& held) { held.settings.*field = {bin, 0, {}}; });
            static constexpr std::pair<const char*, Answer<int> Settings::*> kGeometry[] = {
                {"binx", &Settings::bin_x}, {"biny", &Settings::bin_y},     {"numx", &Settings::num_x},
                {"numy", &Settings::num_y}, {"startx", &Settings::start_x}, {"starty", &Settings::start_y}};
            for (const auto& [name, other] : kGeometry) {
                if (other == field) continue;
                Answer<int> read;
                try {
                    read = answer_of([&client, name = name] { return client.get_int(name); });
                } catch (const RemoteTransportError&) {
                    return;  // the rest keep their values
                }
                write_held(ctx, [&read, other = other](Held& held) { held.settings.*other = read; });
            }
        });
    }

    // An int setting the upstream judges: the connection, the version, refused while faulted, forwarded and written
    // through; `follows_gain` reads ElectronsPerADU and FullWellCapacity again after it.
    void write_int(const char* member, const char* parameter, int value, Answer<int> Settings::*field, int first,
                   bool follows_gain = false) {
        proxy_.require_connected();
        require_version(first, parameter);
        require_frame();
        run([this, member, parameter, value, field, follows_gain](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            RemoteAlpacaClient& client = proxy_.client();
            client.put(member, {{parameter, std::to_string(value)}});
            write_held(ctx, [value, field](Held& held) { held.settings.*field = {value, 0, {}}; });
            if (follows_gain) read_gain_figures(ctx);
        });
    }

    void write_double(const char* member, const char* parameter, double value, Answer<double> Settings::*field,
                      int first) {
        proxy_.require_connected();
        require_version(first, parameter);
        require_frame();
        run([this, member, parameter, value, field](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put(member, {{parameter, number_text(value)}});
            write_held(ctx, [value, field](Held& held) { held.settings.*field = {value, 0, {}}; });
        });
    }

    // After a gain or readout mode write: the upstream's figures for the new one. A read with no usable reply keeps
    // the figures held and does not fail the write the upstream took.
    void read_gain_figures(util::OperationContext& ctx) {
        RemoteAlpacaClient& client = proxy_.client();
        try {
            const Answer<double> electrons = answer_of([&] { return client.get_double("electronsperadu"); });
            write_held(ctx, [&electrons](Held& held) { held.sensor.electrons_per_adu = electrons; });
            const Answer<double> full_well = answer_of([&] { return client.get_double("fullwellcapacity"); });
            write_held(ctx, [&full_well](Held& held) { held.sensor.full_well_capacity = full_well; });
        } catch (const RemoteTransportError&) {  // NOLINT(bugprone-empty-catch): kept until the next Connect
        }
    }

    void run(std::function<void(util::OperationContext&)> body) { proxy_.run_command(std::move(body)); }

    // Write-through (decision 0010 rule 2): the next read sees the command before the next poll. A superseded body
    // writes nothing (decision 0006 rule 6): the newer command's state stands. Nor does a body whose caller passed its
    // check before a Disconnect: with no connection there is no frame, and the write's epoch bump would drop the next
    // Connect's first frame (decision 0010 rule 8, #2446).
    template <class Apply>
    void write_frame(util::OperationContext& ctx, Apply apply) {
        proxy_.with_state([this, &ctx, &apply](Proxy::State& state) {
            if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
            state.write(apply);
        });
    }

    // The same for what the Connect read (a setting, or a figure that follows the gain).
    template <class Apply>
    void write_held(util::OperationContext& ctx, Apply apply) {
        proxy_.with_state([this, &ctx, &apply](Proxy::State&) {
            if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
            apply(held_);
        });
    }

    // One measurement of the upstream, with no lock held (decision 0016 rule 1): CameraState and ImageReady always;
    // PercentCompleted while the sampled CameraState is Exposing; CCDTemperature, CoolerPower, HeatSinkTemperature and
    // IsPulseGuiding unless the Connect found them missing; LastExposureDuration and LastExposureStartTime once each
    // time ImageReady turns true (and in the Connect's first frame). Two to seven calls a poll. Throws
    // RemoteTransportError when a call gets no usable reply; an upstream error is kept in the frame.
    Frame sample() {
        RemoteAlpacaClient& client = proxy_.client();
        const int version = proxy_.upstream_interface_version();
        // What to ask and the frame before this one, read under the state lock; no call runs under it.
        Polled polled;
        Frame missing;
        std::optional<Frame> previous;
        proxy_.with_state([this, &polled, &missing, &previous](const Proxy::State& state) {
            polled = held_.polled;
            missing = held_.missing;
            if (const auto reading = state.read()) previous = reading->value;
        });
        Frame frame;
        frame.camera_state = answer_of([&] { return client.get_int("camerastate"); });
        frame.image_ready = answer_of([&] { return client.get_bool("imageready"); });
        if (version < kFirstVersionV2) {
            frame.percent_completed = lacks<double>(version, "PercentCompleted");
        } else if (frame.camera_state.value == kCameraStateExposing) {
            frame.percent_completed = answer_of([&] { return client.get_double("percentcompleted"); });
        } else {
            frame.percent_completed = {std::nullopt, AlpacaError::InvalidOperation,
                                       "PercentCompleted is only valid while an exposure is in progress"};
        }
        frame.ccd_temperature = polled.ccd_temperature ? answer_of([&] { return client.get_double("ccdtemperature"); })
                                                       : missing.ccd_temperature;
        frame.cooler_power =
            polled.cooler_power ? answer_of([&] { return client.get_double("coolerpower"); }) : missing.cooler_power;
        frame.heat_sink_temperature = polled.heat_sink_temperature
                                          ? answer_of([&] { return client.get_double("heatsinktemperature"); })
                                          : missing.heat_sink_temperature;
        frame.is_pulse_guiding = polled.is_pulse_guiding ? answer_of([&] { return client.get_bool("ispulseguiding"); })
                                                         : missing.is_pulse_guiding;
        const bool ready = frame.image_ready.value.value_or(false);
        if (previous && (!ready || (previous->last_exposure_read && previous->image_ready.value.value_or(false)))) {
            // Not ready, or read since ImageReady turned true: the values stand (a failed exposure never erases an
            // earlier success, and the upstream keeps them until a new image).
            frame.last_exposure_duration = previous->last_exposure_duration;
            frame.last_exposure_start = previous->last_exposure_start;
            frame.last_exposure_read = ready && previous->last_exposure_read;
        } else {
            frame.last_exposure_duration = answer_of([&] { return client.get_double("lastexposureduration"); });
            frame.last_exposure_start = answer_of([&] { return client.get_utc_time("lastexposurestarttime"); });
            frame.last_exposure_read = ready;
        }
        if (!ready) {
            // After the calls, under the state lock: an upstream that reads not ready has no image for the one held (a
            // StartExposure, maybe another client's of the shared upstream). Drop it, and bump the count so a download
            // already in flight does not hold it either.
            proxy_.with_state([this](Proxy::State&) {
                ++exposure_serial_;
                image_.reset();
            });
        }
        return frame;
    }

    // At Connect, with no lock held, after the upstream's identity: the sensor, the settings and the telemetry
    // members once, into read_ (a member an older upstream lacks is not asked), and what to store under the state
    // lock with the identity. The store captures only `this`, so the std::function holds it with no allocation.
    std::function<void()> read_at_connect() {
        RemoteAlpacaClient& client = proxy_.client();
        const int version = proxy_.upstream_interface_version();
        const auto i = [&client](const char* member) { return answer_of([&] { return client.get_int(member); }); };
        const auto d = [&client](const char* member) { return answer_of([&] { return client.get_double(member); }); };
        const auto b = [&client](const char* member) { return answer_of([&] { return client.get_bool(member); }); };
        const auto list = [&client](const char* member) {
            return answer_of([&] { return client.get_string_list(member); });
        };
        Held held;
        Sensor& s = held.sensor;
        const bool v2 = version >= kFirstVersionV2;
        const bool v3 = version >= kFirstVersionV3;
        s.bayer_offset_x = v2 ? i("bayeroffsetx") : lacks<int>(version, "BayerOffsetX");
        s.bayer_offset_y = v2 ? i("bayeroffsety") : lacks<int>(version, "BayerOffsetY");
        s.camera_x_size = i("cameraxsize");
        s.camera_y_size = i("cameraysize");
        s.can_abort_exposure = b("canabortexposure");
        s.can_asymmetric_bin = b("canasymmetricbin");
        s.can_fast_readout = v2 ? b("canfastreadout") : Answer<bool>{false, 0, {}};  // ICameraV1: no fast readout
        s.can_get_cooler_power = b("cangetcoolerpower");
        s.can_pulse_guide = b("canpulseguide");
        s.can_set_ccd_temperature = b("cansetccdtemperature");
        s.can_stop_exposure = b("canstopexposure");
        s.electrons_per_adu = d("electronsperadu");
        s.exposure_max = v2 ? d("exposuremax") : lacks<double>(version, "ExposureMax");
        s.exposure_min = v2 ? d("exposuremin") : lacks<double>(version, "ExposureMin");
        s.exposure_resolution = v2 ? d("exposureresolution") : lacks<double>(version, "ExposureResolution");
        s.full_well_capacity = d("fullwellcapacity");
        s.gain_max = v2 ? i("gainmax") : lacks<int>(version, "GainMax");
        s.gain_min = v2 ? i("gainmin") : lacks<int>(version, "GainMin");
        s.gains = v2 ? list("gains") : lacks<std::vector<std::string>>(version, "Gains");
        s.has_shutter = b("hasshutter");
        s.max_adu = i("maxadu");
        s.max_bin_x = i("maxbinx");
        s.max_bin_y = i("maxbiny");
        s.offset_max = v3 ? i("offsetmax") : lacks<int>(version, "OffsetMax");
        s.offset_min = v3 ? i("offsetmin") : lacks<int>(version, "OffsetMin");
        s.offsets = v3 ? list("offsets") : lacks<std::vector<std::string>>(version, "Offsets");
        s.pixel_size_x = d("pixelsizex");
        s.pixel_size_y = d("pixelsizey");
        s.readout_modes = v2 ? list("readoutmodes") : lacks<std::vector<std::string>>(version, "ReadoutModes");
        s.sensor_name =
            v2 ? answer_of([&] { return client.get_string("sensorname"); }) : lacks<std::string>(version, "SensorName");
        s.sensor_type = v2 ? i("sensortype") : lacks<int>(version, "SensorType");

        Settings& t = held.settings;
        t.bin_x = i("binx");
        t.bin_y = i("biny");
        t.num_x = i("numx");
        t.num_y = i("numy");
        t.start_x = i("startx");
        t.start_y = i("starty");
        t.gain = v2 ? i("gain") : lacks<int>(version, "Gain");
        t.offset = v3 ? i("offset") : lacks<int>(version, "Offset");
        t.readout_mode = v2 ? i("readoutmode") : lacks<int>(version, "ReadoutMode");
        t.fast_readout = v2 ? b("fastreadout") : lacks<bool>(version, "FastReadout");
        t.cooler_on = b("cooleron");
        t.set_ccd_temperature = d("setccdtemperature");
        t.sub_exposure_duration = v3 ? d("subexposureduration") : lacks<double>(version, "SubExposureDuration");

        // The telemetry members: asked once; NotImplemented leaves one out of the poll with that answer.
        Frame& m = held.missing;
        m.ccd_temperature = d("ccdtemperature");
        m.cooler_power = d("coolerpower");
        m.heat_sink_temperature = d("heatsinktemperature");
        m.is_pulse_guiding = b("ispulseguiding");
        held.polled.ccd_temperature = !answered_not_implemented(m.ccd_temperature);
        held.polled.cooler_power = !answered_not_implemented(m.cooler_power);
        held.polled.heat_sink_temperature = !answered_not_implemented(m.heat_sink_temperature);
        held.polled.is_pulse_guiding = !answered_not_implemented(m.is_pulse_guiding);

        // A Color sensor's image is three planes; an upstream that did not say (ICameraV1) may send them too.
        const bool color = !s.sensor_type.value || *s.sensor_type.value == static_cast<int>(SensorType::Color);
        held.bounds =
            image_fetch_bounds(s.camera_x_size.value.value_or(0), s.camera_y_size.value.value_or(0), color ? 3 : 1);
        read_ = std::move(held);
        return [this] {
            held_ = std::move(read_);
            image_.reset();
        };
    }

    const int device_number_;

    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()

    // Under the proxy's state lock (with_connected(), with_state(), the Connect's store): what the Connect read and
    // the commands wrote through, the downloaded image and the count of exposures it belongs to (a StartExposure, and a
    // polled frame that reads ImageReady false, each start a new one).
    Held held_;
    std::shared_ptr<const ImageArray> image_;
    std::uint64_t exposure_serial_ = 0;
    // The Connect's reads before the store, under transition_mutex_ (only the connect path touches it).
    Held read_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so it is destroyed (its poll and command bodies joined) before the members the bodies touch die.
    Proxy proxy_;
};

}  // namespace

ImageFetchBounds image_fetch_bounds(int width, int height, int planes) {
    constexpr std::size_t kElementBytes = 4;            // Int32 or UInt32, the widest type the client reads
    constexpr std::size_t kSlack = std::size_t{65536};  // the 44-byte metadata, or an error's text
    std::size_t cap = kImageBodyCeiling;
    if (width > 0 && height > 0 && planes > 0) {
        const auto w = static_cast<std::size_t>(width);
        const auto h = static_cast<std::size_t>(height);
        const auto p = static_cast<std::size_t>(planes);
        // Compared by division, so a huge claimed sensor takes the ceiling without overflowing.
        const std::size_t room = (kImageBodyCeiling - kSlack) / kElementBytes;
        if (w <= room / h && w * h <= room / p) cap = w * h * p * kElementBytes + kSlack;
    }
    cap = std::max(cap, util::kHttpMaxResponseBytes);
    const auto transfer = std::chrono::milliseconds(cap * 1000 / kImageTransferFloor);
    const auto timeout = std::min<std::chrono::milliseconds>(kRemoteCallTimeout + transfer, kImageFetchTimeoutCeiling);
    return {cap, timeout};
}

std::unique_ptr<CameraDriver> create_remotealpaca_camera(int device_number, const RemoteDeviceConfig& config,
                                                         ServerAddressSource self) {
    return create_remotealpaca_camera(device_number, config,
                                      util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"), std::move(self),
                                      util::default_task_clock());
}

std::unique_ptr<CameraDriver> create_remotealpaca_camera(int device_number, const RemoteDeviceConfig& config,
                                                         std::shared_ptr<util::HttpClient> http,
                                                         ServerAddressSource self, util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaCameraDriver>(device_number, config, std::move(http), std::move(self), clock);
}

void set_poll_paused_for_testing(CameraDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaCameraDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(CameraDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaCameraDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

void set_command_waiting_hook_for_testing(CameraDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaCameraDriver*>(&driver)) {
        remote->set_command_waiting_hook(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::remotealpaca
