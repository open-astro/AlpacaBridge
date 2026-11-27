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

// The remotealpaca ObservingConditions proxied device (decision 0023, issue #1753 slice 3) over UpstreamProxy
// (upstream_proxy.h), which holds every rule a proxied device shares: the self-loop check, the forwarded Connect and
// Disconnect, the upstream's Name and Description, the polled frame through util::PolledState, the link's fault and
// loss, the command slot and the pass-through of upstream errors. What is the ObservingConditions device's own stays
// here: the sensors and which of them the upstream has, the frame they make, Refresh and the AveragePeriod write.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_observingconditions_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "upstream_proxy.h"

namespace alpacacore::vendor::remotealpaca {

namespace {

using detail::Answer;
using detail::answer_of;

// The sensors IObservingConditions defines: the ASCOM name (SensorName parameter, DeviceState) and the member path.
struct Sensor {
    const char* name;
    const char* member;
};
constexpr std::size_t kSensorCount = 13;
constexpr std::array<Sensor, kSensorCount> kSensors{{{"CloudCover", "cloudcover"},
                                                     {"DewPoint", "dewpoint"},
                                                     {"Humidity", "humidity"},
                                                     {"Pressure", "pressure"},
                                                     {"RainRate", "rainrate"},
                                                     {"SkyBrightness", "skybrightness"},
                                                     {"SkyQuality", "skyquality"},
                                                     {"SkyTemperature", "skytemperature"},
                                                     {"StarFWHM", "starfwhm"},
                                                     {"Temperature", "temperature"},
                                                     {"WindDirection", "winddirection"},
                                                     {"WindGust", "windgust"},
                                                     {"WindSpeed", "windspeed"}}};
enum SensorIndex : std::uint8_t {
    kCloudCover,
    kDewPoint,
    kHumidity,
    kPressure,
    kRainRate,
    kSkyBrightness,
    kSkyQuality,
    kSkyTemperature,
    kStarFwhm,
    kTemperature,
    kWindDirection,
    kWindGust,
    kWindSpeed
};

// The index of an ASCOM sensor name, compared without case; nullopt for a name IObservingConditions does not define.
std::optional<std::size_t> sensor_index(std::string_view name) {
    for (std::size_t i = 0; i < kSensorCount; ++i) {
        const std::string_view known = kSensors[i].name;
        if (known.size() == name.size() && std::equal(known.begin(), known.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
            return i;
        }
    }
    return std::nullopt;
}

// The polled frame (decision 0010): what the upstream answered for each sensor the Connect found, its
// TimeSinceLastUpdate, the empty-name TimeSinceLastUpdate and AveragePeriod. A sensor the upstream lacks is not
// sampled; its getters answer the Connect's probe.
struct Frame {
    std::array<Answer<double>, kSensorCount> sensors;
    std::array<Answer<double>, kSensorCount> since_update;
    Answer<double> since_any_update;
    Answer<double> average_period;
};

using Proxy = detail::UpstreamProxy<Frame>;

class RemoteAlpacaObservingConditionsDriver final : public ObservingConditionsDriver,
                                                    protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaObservingConditionsDriver(int device_number, const RemoteObservingConditionsConfig& config,
                                          std::shared_ptr<util::HttpClient> http, ServerAddressSource self,
                                          util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          proxy_({"observingconditions", "ObservingConditions", 2, [this] { return sample(supported_copy()); }}, config,
                 std::move(http), std::move(self), clock) {}

    ~RemoteAlpacaObservingConditionsDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::ObservingConditions; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_OC_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca ObservingConditions Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 2; }

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
            proxy_.connect([this] { return probe_sensors(); });
        } else {
            proxy_.disconnect();
        }
    }

    double get_average_period() const override {
        return proxy_.with_connected(
            [](const Proxy::State& state) { return state.served().value.average_period.get(); });
    }

    // InvalidValue precedes NotConnected; the upstream decides which periods it supports and its refusal passes
    // through unchanged.
    void set_average_period(double period) override {
        if (!std::isfinite(period) || period < 0.0) {
            throw AlpacaException("AveragePeriod must be a number of hours of 0 or more", AlpacaError::InvalidValue);
        }
        proxy_.require_connected();
        if (caller_hook_) caller_hook_();
        proxy_.run_command([this, period](util::OperationContext& ctx) {
            proxy_.client().put("averageperiod", {{"AveragePeriod", detail::number_text(period)}});
            if (command_hook_) command_hook_();
            // Write-through (decision 0010 rule 2): the next read sees the period before the next poll. A superseded
            // body writes nothing (decision 0006 rule 6): the newer command's value stands. Nor does a body whose
            // caller passed its check before a Disconnect: with no connection there is no frame, and the write's epoch
            // bump would drop the next Connect's first frame (decision 0010 rule 8, #2446).
            proxy_.with_state([this, &ctx, period](Proxy::State& state) {
                if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
                state.write([period](Frame& frame) { frame.average_period = {period, 0, {}}; });
            });
        });
    }

    double get_cloud_cover() const override { return sensor_value(kCloudCover); }
    double get_dew_point() const override { return sensor_value(kDewPoint); }
    double get_humidity() const override { return sensor_value(kHumidity); }
    double get_pressure() const override { return sensor_value(kPressure); }
    double get_rain_rate() const override { return sensor_value(kRainRate); }
    double get_sky_brightness() const override { return sensor_value(kSkyBrightness); }
    double get_sky_quality() const override { return sensor_value(kSkyQuality); }
    double get_sky_temperature() const override { return sensor_value(kSkyTemperature); }
    double get_star_fwhm() const override { return sensor_value(kStarFwhm); }
    double get_temperature() const override { return sensor_value(kTemperature); }
    double get_wind_direction() const override { return sensor_value(kWindDirection); }
    double get_wind_gust() const override { return sensor_value(kWindGust); }
    double get_wind_speed() const override { return sensor_value(kWindSpeed); }

    // The upstream's TimeSinceLastUpdate when the frame was measured, plus the frame's age on the task clock. An empty
    // name asks for the latest update of any sensor.
    double get_time_since_last_update(std::string_view property_name) const override {
        const std::optional<std::size_t> index = sensor_index(property_name);
        if (!property_name.empty() && !index) {
            throw AlpacaException("Unknown sensor property", AlpacaError::InvalidValue);
        }
        return proxy_.with_connected([this, &index](const Proxy::State& state) {
            if (index && !supported_[*index]) (void)probe_[*index].get();  // the upstream lacks it: its own error
            const auto reading = state.served();
            const double since =
                index ? reading.value.since_update[*index].get() : reading.value.since_any_update.get();
            if (since < 0.0) return since;  // the upstream's own "never updated" answer, as it gave it
            return since + std::chrono::duration<double>(proxy_.clock().now() - reading.measured_at).count();
        });
    }

    // Static metadata read at Connect: it keeps answering while the link is faulted.
    std::string get_sensor_description(std::string_view property_name) const override {
        const std::optional<std::size_t> index = sensor_index(property_name);
        if (!index) {
            throw AlpacaException("Unknown sensor property", AlpacaError::InvalidValue);
        }
        return proxy_.with_connected([this, &index](const Proxy::State&) {
            if (!supported_[*index]) (void)probe_[*index].get();
            return descriptions_[*index].get();
        });
    }

    // Forwards Refresh, then measures a new frame and publishes it through the module (#2496), so measured_at is the
    // Refresh's. Decision 0009 rule 1 as WeeWX: a failed Refresh reaches the caller and does not count towards the
    // latch; only the poll's probes feed the latch, so a standing fault clears at the next good poll.
    void refresh() override {
        proxy_.require_connected();
        proxy_.run_command([this](util::OperationContext& ctx) {
            proxy_.client().put("refresh");
            if (command_hook_) command_hook_();
            // A superseded body sends nothing more and publishes nothing (decision 0006 rule 6), also when the
            // supersede lands during the measurement.
            const auto superseded = [&ctx] { return ctx.stop_reason() == util::StopReason::Superseded; };
            if (superseded()) return;
            util::PolledState<Frame>::Token token;
            std::array<bool, kSensorCount> supported{};
            proxy_.with_state([this, &token, &supported](Proxy::State& state) {
                token = state.begin_poll();
                supported = supported_;
            });
            Frame frame = sample(supported, superseded);
            if (superseded()) return;
            proxy_.with_state([this, &token, &frame](Proxy::State& state) {
                if (get_connected()) (void)state.publish(token, std::move(frame));  // lost meanwhile: serve nothing
            });
        });
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; an AveragePeriod or Refresh body calls it after its PUT, before it touches state.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    // Set while no command runs; set_average_period() calls it after its NotConnected check, before its body starts.
    void set_caller_hook(std::function<void()> hook) { caller_hook_ = std::move(hook); }

    // Set while no command runs; a Refresh or AveragePeriod caller calls it after its body has ended, before it
    // answers.
    void set_command_done_hook(std::function<void()> hook) { proxy_.set_command_done_hook(std::move(hook)); }

    // Set while no command runs; a caller that waits for an earlier command calls it first.
    void set_command_waiting_hook(std::function<void()> hook) { proxy_.set_command_waiting_hook(std::move(hook)); }

    std::uint64_t command_generation() const { return proxy_.command_generation(); }

private:
    // A sensor the upstream lacks answers the upstream's own error from the Connect's probe (NotImplemented). A stale
    // frame is served; a latched fault refuses it with DriverException (decision 0009 rule 1).
    double sensor_value(std::size_t index) const {
        return proxy_.with_connected([this, index](const Proxy::State& state) {
            if (!supported_[index]) return probe_[index].get();
            return state.served().value.sensors[index].get();
        });
    }

    std::array<bool, kSensorCount> supported_copy() const {
        return proxy_.with_state([this](const Proxy::State&) { return supported_; });
    }

    // One measurement of the upstream, with no lock held (decision 0016 rule 1). Throws RemoteTransportError when a
    // call gets no usable reply; an upstream error is kept in the frame. When `stop` answers true before a call, it
    // returns the partial frame at once and the caller must not publish it.
    Frame sample(const std::array<bool, kSensorCount>& supported, const std::function<bool()>& stop = {}) {
        RemoteAlpacaClient& client = proxy_.client();
        const auto stopped = [&stop] { return stop && stop(); };
        Frame frame;
        for (std::size_t i = 0; i < kSensorCount; ++i) {
            if (!supported[i]) continue;
            if (stopped()) return frame;
            frame.sensors[i] = answer_of([&] { return client.get_double(kSensors[i].member); });
            if (stopped()) return frame;
            frame.since_update[i] =
                answer_of([&] { return client.get_double("timesincelastupdate", {{"SensorName", kSensors[i].name}}); });
        }
        if (stopped()) return frame;
        frame.since_any_update =
            answer_of([&] { return client.get_double("timesincelastupdate", {{"SensorName", ""}}); });
        if (stopped()) return frame;
        frame.average_period = answer_of([&] { return client.get_double("averageperiod"); });
        return frame;
    }

    // At Connect, with no lock held, after the upstream's identity: asks each sensor once, and returns what to store
    // under the state lock with the identity, before the first frame is measured.
    std::function<void()> probe_sensors() {
        RemoteAlpacaClient& client = proxy_.client();
        std::array<bool, kSensorCount> supported{};
        std::array<Answer<double>, kSensorCount> probe;
        std::array<Answer<std::string>, kSensorCount> descriptions;
        for (std::size_t i = 0; i < kSensorCount; ++i) {
            probe[i] = answer_of([&] { return client.get_double(kSensors[i].member); });
            // Members the upstream lacks map to NotImplemented: its own answer. Any other error is a sensor it has
            // that could not be read just now.
            supported[i] = probe[i].value.has_value() || probe[i].error_number != AlpacaError::NotImplemented;
            if (supported[i]) {
                descriptions[i] = answer_of(
                    [&] { return client.get_string("sensordescription", {{"SensorName", kSensors[i].name}}); });
            }
        }
        return [this, supported, probe = std::move(probe), descriptions = std::move(descriptions)]() mutable {
            supported_ = supported;
            probe_ = std::move(probe);
            descriptions_ = std::move(descriptions);
        };
    }

    const int device_number_;

    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()
    std::function<void()> caller_hook_;   // test seam: set_caller_hook_for_testing()

    // Under the proxy's state lock (with_connected(), with_state(), the Connect's store): which sensors the upstream
    // has, the Connect's answer served for one it lacks, and each sensor's description.
    std::array<bool, kSensorCount> supported_{};
    std::array<Answer<double>, kSensorCount> probe_;
    std::array<Answer<std::string>, kSensorCount> descriptions_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so it is destroyed (its poll and command bodies joined) before the members the bodies touch die.
    Proxy proxy_;
};

}  // namespace

std::unique_ptr<ObservingConditionsDriver> create_remotealpaca_observingconditions(
    int device_number, const RemoteObservingConditionsConfig& config, ServerAddressSource self) {
    return create_remotealpaca_observingconditions(device_number, config,
                                                   util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"),
                                                   std::move(self), util::default_task_clock());
}

std::unique_ptr<ObservingConditionsDriver> create_remotealpaca_observingconditions(
    int device_number, const RemoteObservingConditionsConfig& config, std::shared_ptr<util::HttpClient> http,
    ServerAddressSource self, util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaObservingConditionsDriver>(device_number, config, std::move(http),
                                                                   std::move(self), clock);
}

void set_poll_paused_for_testing(ObservingConditionsDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaObservingConditionsDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(ObservingConditionsDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaObservingConditionsDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

void set_caller_hook_for_testing(ObservingConditionsDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaObservingConditionsDriver*>(&driver)) {
        remote->set_caller_hook(std::move(hook));
    }
}

void set_command_done_hook_for_testing(ObservingConditionsDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaObservingConditionsDriver*>(&driver)) {
        remote->set_command_done_hook(std::move(hook));
    }
}

void set_command_waiting_hook_for_testing(ObservingConditionsDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaObservingConditionsDriver*>(&driver)) {
        remote->set_command_waiting_hook(std::move(hook));
    }
}

std::uint64_t command_generation_for_testing(ObservingConditionsDriver& driver) {
    auto* remote = dynamic_cast<RemoteAlpacaObservingConditionsDriver*>(&driver);
    return remote ? remote->command_generation() : 0;
}

}  // namespace alpacacore::vendor::remotealpaca
