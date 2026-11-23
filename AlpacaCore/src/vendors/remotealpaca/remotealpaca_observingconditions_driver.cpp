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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/device_name.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_observingconditions_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace alpacacore::vendor::remotealpaca {

namespace {

constexpr const char* kComponent = "remotealpaca";

// Decision 0009 rule 1: three failed polls in a row latch the link fault, the count every polled driver uses.
constexpr int kFailuresToLatch = 3;

// The longest the Connect waits for an upstream's own Platform 7 Connect to finish (Connecting false), and how
// often it asks. The upstream's connect is its device's business; this only bounds how long ours waits for it.
constexpr std::chrono::seconds kUpstreamConnectBound{30};
constexpr std::chrono::milliseconds kUpstreamConnectingPoll{100};

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

// One upstream answer: its value, or the upstream's error, kept to be thrown unchanged (decision 0023 rule 5).
template <class T>
struct Answer {
    std::optional<T> value;
    int error_number = 0;
    std::string error_message;

    T get() const {
        if (!value) throw AlpacaException(error_message, error_number);
        return *value;
    }
};

// What the upstream answered; a call with no usable reply (RemoteTransportError) is not an answer and is rethrown.
template <class Fn>
auto answer_of(Fn&& call) -> Answer<decltype(call())> {
    try {
        return {call(), 0, {}};
    } catch (const RemoteTransportError&) {
        throw;
    } catch (const AlpacaException& e) {
        return {std::nullopt, e.error_code(), e.what()};
    }
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

// Decision 0024 rules 1 and 2: the brand, then the upstream's Name as the model, without its " (...)" qualifiers and
// cut at its first comma, so no '(' or ',' reaches Name. An empty result keeps the default Name.
constexpr const char* kBrand = "Remote Alpaca";

std::string name_from_upstream(const std::string& upstream_name) {
    std::string model = util::without_qualifiers(upstream_name);
    if (const auto comma = model.find(','); comma != std::string::npos) model.resize(comma);
    model.erase(std::find_if(model.rbegin(), model.rend(), [](unsigned char c) { return std::isspace(c) == 0; }).base(),
                model.end());
    model.erase(model.begin(),
                std::find_if(model.begin(), model.end(), [](unsigned char c) { return std::isspace(c) == 0; }));
    if (model.empty() || model.find('(') != std::string::npos) return {};
    return util::branded_name(kBrand, model);
}

// A double in its shortest round-trip form, for a form body.
std::string number_text(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

std::uint32_t new_client_id() {
    std::random_device seed;
    std::uniform_int_distribution<std::uint32_t> pick(1, 0x7FFFFFFF);
    return pick(seed);
}

class RemoteAlpacaObservingConditionsDriver final : public ObservingConditionsDriver,
                                                    protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaObservingConditionsDriver(int device_number, RemoteObservingConditionsConfig config,
                                          std::shared_ptr<util::HttpClient> http, ServerAddressSource self,
                                          util::TaskClock& clock)
        : AsyncConnectable(kComponent),
          device_number_(device_number),
          config_(validated(std::move(config))),
          self_(std::move(self)),
          clock_(clock),
          client_(std::move(http),
                  UpstreamDevice{config_.host, config_.port, "observingconditions", config_.upstream_device_number},
                  new_client_id()),
          upstream_label_(config_.host + ":" + std::to_string(config_.port) + " device " +
                          std::to_string(config_.upstream_device_number)),
          name_("Remote Alpaca ObservingConditions"),
          description_(default_description()),
          state_(
              clock, poll_generation_,
              {config_.poll_period, kFailuresToLatch, 2 * config_.poll_period,
               std::string(kComponent) + " " + upstream_label_, loss_bound(config_.poll_period)},
              [this] { return probe(); }, [this] { on_lost(); }),
          command_op_("remotealpaca command", command_generation_, clock) {}

    ~RemoteAlpacaObservingConditionsDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        state_.stop_and_join();
        command_op_.cancel_all_and_join();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return name_;
    }

    std::string get_description() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return description_;
    }

    DeviceType get_device_type() const override { return DeviceType::ObservingConditions; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_OC_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca ObservingConditions Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 2; }

    // A loss the module latched (a frame or a fault past the bound) reads false at once, before on_lost() runs.
    bool get_connected() const override { return connected_.load() && !state_.link_lost(); }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    // Decision 0009 rule 5: the latched fault, or after a loss the reason it was lost, kept until the next Connect or
    // Disconnect.
    std::string get_link_fault() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string lost = loss_reason_locked();
        return lost.empty() ? state_.fault() : lost;
    }

    void set_connected(bool connected) override {
        // Issue #528: the connect makes HTTP calls, so a sync disconnect must not slip in between its checks and its
        // store. Guards set_connected() against set_connected() only; no getter takes it.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected) {
            if (get_connected()) {
                return;  // idempotent: connected, and the poll is running
            }
            connect_upstream();
        } else {
            disconnect_upstream();
        }
    }

    double get_average_period() const override {
        return served([](const Frame& frame) { return frame.average_period.get(); });
    }

    // InvalidValue precedes NotConnected; the upstream decides which periods it supports and its refusal passes
    // through unchanged.
    void set_average_period(double period) override {
        if (!std::isfinite(period) || period < 0.0) {
            throw AlpacaException("AveragePeriod must be a number of hours of 0 or more", AlpacaError::InvalidValue);
        }
        require_connected();
        if (caller_hook_) caller_hook_();
        run_command([this, period](util::OperationContext& ctx) {
            client_.put("averageperiod", {{"AveragePeriod", number_text(period)}});
            if (command_hook_) command_hook_();
            // Write-through (decision 0010 rule 2): the next read sees the period before the next poll. A superseded
            // body writes nothing (decision 0006 rule 6): the newer command's value stands. Nor does a body whose
            // caller passed its check before a Disconnect: with no connection there is no frame, and the write's epoch
            // bump would drop the next Connect's first frame (decision 0010 rule 8, #2446).
            std::lock_guard<std::mutex> lock(mutex_);
            if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
            state_.write([period](Frame& frame) { frame.average_period = {period, 0, {}}; });
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
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected_locked();
        if (index && !supported_[*index]) (void)probe_[*index].get();  // the upstream lacks it: its own error
        const auto reading = state_.served();
        const double since = index ? reading.value.since_update[*index].get() : reading.value.since_any_update.get();
        if (since < 0.0) return since;  // the upstream's own "never updated" answer, as it gave it
        return since + std::chrono::duration<double>(clock_.now() - reading.measured_at).count();
    }

    // Static metadata read at Connect: it keeps answering while the link is faulted.
    std::string get_sensor_description(std::string_view property_name) const override {
        const std::optional<std::size_t> index = sensor_index(property_name);
        if (!index) {
            throw AlpacaException("Unknown sensor property", AlpacaError::InvalidValue);
        }
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected_locked();
        if (!supported_[*index]) (void)probe_[*index].get();
        return descriptions_[*index].get();
    }

    // Forwards Refresh, then measures a new frame and publishes it through the module (#2496), so measured_at is the
    // Refresh's. Decision 0009 rule 1 as WeeWX: a failed Refresh reaches the caller and does not count towards the
    // latch; only the poll's probes feed the latch, so a standing fault clears at the next good poll.
    void refresh() override {
        require_connected();
        run_command([this](util::OperationContext& ctx) {
            client_.put("refresh");
            if (command_hook_) command_hook_();
            // A superseded body sends nothing more and publishes nothing (decision 0006 rule 6), also when the
            // supersede lands during the measurement.
            const auto superseded = [&ctx] { return ctx.stop_reason() == util::StopReason::Superseded; };
            if (superseded()) return;
            util::PolledState<Frame>::Token token;
            std::array<bool, kSensorCount> supported{};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                token = state_.begin_poll();
                supported = supported_;
            }
            Frame frame = sample(supported, superseded);
            if (superseded()) return;
            std::lock_guard<std::mutex> lock(mutex_);
            if (get_connected()) (void)state_.publish(token, std::move(frame));  // lost meanwhile: serve nothing
        });
    }

    void set_poll_paused(bool paused) { state_.pause_for_testing(paused); }

    // Set while no command runs; an AveragePeriod or Refresh body calls it after its PUT, before it touches state.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    // Set while no command runs; set_average_period() calls it after its NotConnected check, before its body starts.
    void set_caller_hook(std::function<void()> hook) { caller_hook_ = std::move(hook); }

    // Set while no command runs; a Refresh or AveragePeriod caller calls it after its body has ended, before it
    // answers.
    void set_command_done_hook(std::function<void()> hook) { command_done_hook_ = std::move(hook); }

    std::uint64_t command_generation() const { return command_generation_.current(); }

private:
    static RemoteObservingConditionsConfig validated(RemoteObservingConditionsConfig config) {
        if (config.poll_period < kMinPollPeriod || config.poll_period > kMaxPollPeriod) {
            throw AlpacaException("Poll period must be from 1 to 60 seconds", AlpacaError::InvalidValue);
        }
        return config;
    }

    std::string default_description() const {
        return "ObservingConditions of the upstream Alpaca server " + upstream_label_;
    }

    // NotConnected before any other answer (ASCOM contract precedence); after a loss the text says why.
    void require_connected() const {
        if (!get_connected()) {
            std::lock_guard<std::mutex> lock(mutex_);
            throw AlpacaException(not_connected_text_locked(), AlpacaError::NotConnected);
        }
    }

    // The re-check under mutex_: a loss drops connected_ under this lock.
    void require_connected_locked() const {
        if (!get_connected()) {
            throw AlpacaException(not_connected_text_locked(), AlpacaError::NotConnected);
        }
    }

    std::string not_connected_text_locked() const {
        const std::string lost = loss_reason_locked();
        if (!lost.empty()) {
            return "Upstream " + upstream_label_ + " lost (" + lost + "); Connect again when it answers";
        }
        return "Device not connected";
    }

    // Why the link was lost, empty while it is not: the refusal, the latched fault, or the frame's age.
    std::string loss_reason_locked() const {
        if (!loss_text_.empty() || !state_.link_lost()) return loss_text_;
        std::string fault = state_.fault();
        if (!fault.empty()) return fault;
        return "no answer for " +
               std::to_string(
                   std::chrono::duration_cast<std::chrono::seconds>(loss_bound(config_.poll_period)).count()) +
               " s";
    }

    // A getter served from the frame (decisions 0010 and 0016): no upstream call. A stale frame is served; a latched
    // fault refuses it with DriverException (decision 0009 rule 1).
    template <class Fn>
    double served(Fn&& pick) const {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected_locked();
        return pick(state_.served().value);
    }

    // A sensor the upstream lacks answers the upstream's own error from the Connect's probe (NotImplemented).
    double sensor_value(std::size_t index) const {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected_locked();
        if (!supported_[index]) return probe_[index].get();
        return state_.served().value.sensors[index].get();
    }

    std::array<bool, kSensorCount> supported_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return supported_;
    }

    // One measurement of the upstream, with no lock held (decision 0016 rule 1). Throws RemoteTransportError when a
    // call gets no usable reply; an upstream error is kept in the frame. When `stop` answers true before a call, it
    // returns the partial frame at once and the caller must not publish it.
    Frame sample(const std::array<bool, kSensorCount>& supported, const std::function<bool()>& stop = {}) {
        const auto stopped = [&stop] { return stop && stop(); };
        Frame frame;
        for (std::size_t i = 0; i < kSensorCount; ++i) {
            if (!supported[i]) continue;
            if (stopped()) return frame;
            frame.sensors[i] = answer_of([&] { return client_.get_double(kSensors[i].member); });
            if (stopped()) return frame;
            frame.since_update[i] = answer_of(
                [&] { return client_.get_double("timesincelastupdate", {{"SensorName", kSensors[i].name}}); });
        }
        if (stopped()) return frame;
        frame.since_any_update =
            answer_of([&] { return client_.get_double("timesincelastupdate", {{"SensorName", ""}}); });
        if (stopped()) return frame;
        frame.average_period = answer_of([&] { return client_.get_double("averageperiod"); });
        return frame;
    }

    // Decision 0023 rule 4, then the forwarded Connect, the upstream's identity, which sensors it has, and the first
    // frame, so the getters answer when Connect returns. Decision 0009 rule 4: this is the only relink of a lost link.
    void connect_upstream() {
        state_.stop_and_join();  // a lost link's poll has returned or is returning; join it so a fresh one starts
        // A command body from before (an AveragePeriod write-through) must not land on this Connect's frame.
        command_op_.cancel_all_and_join();
        // A loss a getter latched while the poll waited its tick never ran on_lost(): without this, reset() would clear
        // the latch and the link read connected before this Connect has a frame.
        connected_.store(false);
        state_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loss_text_.clear();  // rule 5: the kept text ends at the next Connect
            probe_error_ = nullptr;
        }
        if (self_) {
            if (auto refusal = self_loop_refusal(config_.host, config_.port, self_())) {
                throw AlpacaException(*refusal, AlpacaError::DriverException);
            }
        }
        try {
            const int version = client_.get_int("interfaceversion");
            forward_connect(version);
            if (!client_.get_bool("connected")) {
                throw AlpacaException("Upstream " + upstream_label_ + " still reports Connected false after Connect",
                                      AlpacaError::DriverException);
            }
            std::string name = client_.get_string("name");
            std::string description = client_.get_string("description");
            std::array<bool, kSensorCount> supported{};
            std::array<Answer<double>, kSensorCount> probe;
            std::array<Answer<std::string>, kSensorCount> descriptions;
            for (std::size_t i = 0; i < kSensorCount; ++i) {
                probe[i] = answer_of([&] { return client_.get_double(kSensors[i].member); });
                // Members the upstream lacks map to NotImplemented: its own answer. Any other error is a sensor it
                // has that could not be read just now.
                supported[i] = probe[i].value.has_value() || probe[i].error_number != AlpacaError::NotImplemented;
                if (supported[i]) {
                    descriptions[i] = answer_of(
                        [&] { return client_.get_string("sensordescription", {{"SensorName", kSensors[i].name}}); });
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Decision 0024 rule 4: what the brand-and-model Name leaves out of the upstream's Name goes in the
                // Description.
                const std::string branded = name_from_upstream(name);
                if (!branded.empty()) name_ = branded;
                // Rebuilt on each Connect, so a reconnect does not append the upstream Name again.
                description_ = description.empty() ? default_description() : std::move(description);
                if (!name.empty() && branded != util::branded_name(kBrand, name)) {
                    description_ += " (upstream Name: " + name + ")";
                }
                supported_ = supported;
                probe_ = std::move(probe);
                descriptions_ = std::move(descriptions);
            }
            // The first frame, then the poll: the getters answer when Connect returns. No frame fails the Connect
            // with the last probe's own error.
            if (!state_.start()) {
                std::exception_ptr error;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error = std::exchange(probe_error_, nullptr);
                }
                if (error) std::rethrow_exception(error);
                throw AlpacaException("Upstream " + upstream_label_ + " state poll did not start",
                                      AlpacaError::DriverException);
            }
            connected_.store(true);
        } catch (...) {
            connected_.store(false);
            if (forwarded_connect_) {
                forward_disconnect();  // best effort: leave the upstream as this Connect found it
            }
            throw;
        }
    }

    // Platform 7 (ObservingConditions 2 and later): Connect, then Connecting until false. Before it, Connected=true.
    void forward_connect(int version) {
        forwarded_connect_ = true;
        forwarded_version_ = version;
        if (version < 2) {
            client_.put("connected", {{"Connected", "True"}});
            return;
        }
        client_.put("connect");
        const auto deadline = clock_.now() + kUpstreamConnectBound;
        while (client_.get_bool("connecting")) {
            if (clock_.now() >= deadline) {
                throw AlpacaException("Upstream " + upstream_label_ + " was still connecting after " +
                                          std::to_string(kUpstreamConnectBound.count()) + " s",
                                      AlpacaError::DriverException);
            }
            clock_.sleep_for(kUpstreamConnectingPoll);
        }
    }

    // Best effort: a Disconnect completes here whatever the upstream answers. Called under transition_mutex_.
    void forward_disconnect() {
        forwarded_connect_ = false;
        try {
            if (forwarded_version_ < 2) {
                client_.put("connected", {{"Connected", "False"}});
            } else {
                client_.put("disconnect");
            }
        } catch (const std::exception& e) {
            // Decision 0007 rule 9: a failure a client can cause is not logged at WARNING or above.
            ALPACA_LOG_INFO(kComponent, "Upstream " + upstream_label_ + " did not take the Disconnect: " + e.what());
        }
    }

    void disconnect_upstream() {
        state_.stop_and_join();
        command_op_.cancel_all_and_join();
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lost = !loss_reason_locked().empty();
            connected_.store(false);
            loss_text_.clear();  // rule 5: the kept text ends at the next Disconnect
        }
        state_.reset();
        if (forwarded_connect_) {
            if (lost) {
                forwarded_connect_ = false;  // the upstream is gone: nothing to tell it, and the call would only wait
            } else {
                forward_disconnect();
            }
        }
    }

    // A command (Refresh, an AveragePeriod write) runs as a body in its own slot (decision 0023, decision 0006 rule
    // 1); the caller waits for the body's outcome, which the per-call timeout bounds, and gets its error unchanged. A
    // refused connection is a lost link at once (NotConnected); another call with no reply is a DriverException, or
    // NotConnected when the poll lost the link meanwhile. Both are decided inside the body: a Connect joins the slot
    // first, so no reconnect lands between the failure and its answer (#2456).
    void run_command(std::function<void(util::OperationContext&)> body) {
        struct Outcome {
            std::mutex mutex;
            std::condition_variable done_cv;
            bool done = false;
            std::exception_ptr error;
        };
        auto outcome = std::make_shared<Outcome>();
        command_op_.start([this, outcome, body = std::move(body)](util::OperationContext& ctx) {
            std::exception_ptr error;
            try {
                try {
                    body(ctx);
                } catch (const RemoteConnectRefused& e) {
                    lose_link(e.what());
                    require_connected();
                    throw;
                } catch (const RemoteTransportError&) {
                    require_connected();
                    throw;
                }
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(outcome->mutex);
                outcome->error = std::move(error);  // the caller becomes the error's only owner
                outcome->done = true;
            }
            outcome->done_cv.notify_all();
        });
        {
            // A completion wait, like a join: the body ends after its calls, each bounded by kRemoteCallTimeout.
            std::unique_lock<std::mutex> lock(outcome->mutex);
            outcome->done_cv.wait(lock, [&outcome] { return outcome->done; });
        }
        if (command_done_hook_) command_done_hook_();
        // Taken out of the shared outcome, so the body thread never frees the exception the caller is reading.
        if (auto error = std::move(outcome->error)) std::rethrow_exception(error);
    }

    // The poll's probe, on the poll thread with no driver lock held (decision 0016 rule 1): one measurement, which the
    // module publishes. A call with no usable reply throws (a failed exchange for the module's latch); a refused
    // connection is a lost link at once. A link a command lost is not called again: the cycle ends the poll.
    std::optional<Frame> probe() {
        if (state_.link_lost()) return std::nullopt;
        try {
            return sample(supported_copy());
        } catch (const RemoteConnectRefused& e) {
            note_probe_error();
            lose_link(e.what());
            throw;
        } catch (const std::exception&) {
            note_probe_error();
            throw;
        }
    }

    // Kept for the Connect, which answers a first frame that never came with the probe's own error.
    void note_probe_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        probe_error_ = std::current_exception();
    }

    // Decision 0009 point 2(b): the bound is three poll periods (#1753 maintainer decision, 2026-11-16), never
    // shorter than util::kLinkStalenessBound. A fault standing that long, or a frame no poll refreshed for it, is a
    // lost link (the module's verdict).
    static std::chrono::nanoseconds loss_bound(std::chrono::seconds period) {
        return std::max<std::chrono::nanoseconds>(util::kLinkStalenessBound, 3 * period);
    }

    // The module's loss, on the poll thread with no driver lock held (decision 0009 rule 3): Connected false and the
    // reason kept for get_link_fault() (rule 5); there is no transport to close, each call has its own.
    void on_lost() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_.load()) return;  // a refusal already took it down
        loss_text_ = loss_reason_locked();
        connected_.store(false);
    }

    // Decision 0009 point 2(a), decision 0023: a refused connection says nothing listens there, a lost link at once.
    // Called from the probe or a command body, both of the connection the last reset() opened (a Connect joins both
    // first); while the Connect runs, connected_ is false and the refusal only fails it.
    void lose_link(const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!get_connected()) return;
            connected_.store(false);
            loss_text_ = reason;
        }
        state_.lose_link();
        ALPACA_LOG_ERROR(kComponent, "Upstream " + upstream_label_ + " lost: " + reason +
                                         "; Connected is false until the client connects again");
    }

    const int device_number_;
    const RemoteObservingConditionsConfig config_;
    const ServerAddressSource self_;
    // The poll's wait, the fault stamp and the loss bound (decision 0005).
    util::TaskClock& clock_;
    // The I/O lock is the client's own: one call at a time per upstream device.
    RemoteAlpacaClient client_;
    const std::string upstream_label_;  // "<host>:<port> device <n>"

    // Decision 0009 rule 3: false on a loss, so every member throws NotConnected and DeviceState is empty. No
    // LinkLostStamp: nothing here moves on a relink (no #521 consumer).
    std::atomic<bool> connected_{false};
    std::function<void()> command_hook_;       // test seam: set_command_hook_for_testing()
    std::function<void()> caller_hook_;        // test seam: set_caller_hook_for_testing()
    std::function<void()> command_done_hook_;  // test seam: set_command_done_hook_for_testing()
    // Under transition_mutex_: the upstream was told Connect and not yet Disconnect, and its InterfaceVersion then.
    bool forwarded_connect_ = false;
    int forwarded_version_ = 0;

    // The state lock, a leaf of the driver's own locks (decision 0016): the snapshot mutex nests under it. Never held
    // across an upstream call.
    mutable std::mutex mutex_;
    std::string loss_text_;           // why the link was lost; empty while it is not
    std::exception_ptr probe_error_;  // the last failed probe's error, for the Connect
    std::string name_;
    std::string description_;
    std::array<bool, kSensorCount> supported_{};
    std::array<Answer<double>, kSensorCount> probe_;  // the Connect's answer, served for a sensor the upstream lacks
    std::array<Answer<std::string>, kSensorCount> descriptions_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so their bodies are joined before the members the bodies touch die. state_ (decision 0010 rule 7, #2496)
    // holds the frame, the poll and the link's fault and loss latches. The command slot comes after it, so the
    // command bodies, which publish to the frame, are joined before state_ dies.
    util::OperationGeneration poll_generation_;
    util::PolledState<Frame> state_;
    util::OperationGeneration command_generation_;
    util::AsyncOperation command_op_;
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

std::uint64_t command_generation_for_testing(ObservingConditionsDriver& driver) {
    auto* remote = dynamic_cast<RemoteAlpacaObservingConditionsDriver*>(&driver);
    return remote ? remote->command_generation() : 0;
}

}  // namespace alpacacore::vendor::remotealpaca
