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
#include <alpacacore/util/state_snapshot.h>
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
          snapshot_(clock, 2 * config_.poll_period),
          name_("Remote Alpaca ObservingConditions"),
          description_(default_description()),
          poll_op_("remotealpaca poll", poll_generation_, clock),
          command_op_("remotealpaca command", command_generation_, clock) {}

    ~RemoteAlpacaObservingConditionsDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        stop_polling();
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

    bool get_connected() const override { return connected_.load(); }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    // Decision 0009 rule 5: the latched fault, or after a loss the reason it was lost, kept until the next Connect or
    // Disconnect.
    std::string get_link_fault() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return loss_text_.empty() ? link_health_.fault() : loss_text_;
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
            if (connected_.load()) {
                return;  // idempotent: connected, and the poll is running
            }
            connect_upstream();
        } else {
            disconnect_upstream();
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view, std::string_view) override {
        throw AlpacaException("Action not supported", AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override { return false; }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::NotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::NotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::NotImplemented);
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
        run_command([this, period](util::OperationContext& ctx) {
            client_.put("averageperiod", {{"AveragePeriod", number_text(period)}});
            if (command_hook_) command_hook_();
            // Write-through (decision 0010 rule 2): the next read sees the period before the next poll. A superseded
            // body writes nothing (decision 0006 rule 6): the newer command's value stands.
            std::lock_guard<std::mutex> lock(mutex_);
            if (ctx.stop_reason() == util::StopReason::Superseded) return;
            snapshot_.write([period](Frame& frame) { frame.average_period = {period, 0, {}}; });
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
        require_not_faulted_locked();
        const auto reading = frame_locked();
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

    // Forwards Refresh, then measures a new frame. Decision 0009 rule 1 as WeeWX: a failed Refresh reaches the caller
    // and does not count towards the latch; a good measurement clears a standing fault like a good poll.
    void refresh() override {
        require_connected();
        run_command([this](util::OperationContext& ctx) {
            client_.put("refresh");
            if (command_hook_) command_hook_();
            // A superseded body sends nothing more and publishes nothing (decision 0006 rule 6), also when the
            // supersede lands during the measurement.
            const auto superseded = [&ctx] { return ctx.stop_reason() == util::StopReason::Superseded; };
            if (superseded()) return;
            const auto token = snapshot_.begin_poll();
            Frame frame = sample(supported_copy(), superseded);
            if (superseded()) return;
            if (note_reply(token, std::move(frame))) {
                ALPACA_LOG_INFO(kComponent, "Upstream " + upstream_label_ + " restored: Refresh answered");
            }
        });
    }

    void set_poll_paused(bool paused) { poll_paused_.store(paused); }

    // Set while no command runs; an AveragePeriod or Refresh body calls it after its PUT, before it touches state.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

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
        if (!connected_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            throw AlpacaException(not_connected_text_locked(), AlpacaError::NotConnected);
        }
    }

    // The re-check under mutex_: a loss drops connected_ under this lock.
    void require_connected_locked() const {
        if (!connected_.load()) {
            throw AlpacaException(not_connected_text_locked(), AlpacaError::NotConnected);
        }
    }

    std::string not_connected_text_locked() const {
        if (!loss_text_.empty()) {
            return "Upstream " + upstream_label_ + " lost (" + loss_text_ + "); Connect again when it answers";
        }
        return "Device not connected";
    }

    // Decision 0009 rule 1: while the fault is latched the last frame is not served.
    void require_not_faulted_locked() const {
        if (link_health_.faulted()) {
            throw AlpacaException(
                "Upstream " + upstream_label_ + " communications compromised: " + link_health_.fault(),
                AlpacaError::DriverException);
        }
    }

    // Decision 0010 rule 8: a connected driver holds a frame from the Connect on; none is a device that has not
    // answered, never InvalidOperation.
    util::StateSnapshot<Frame>::Reading frame_locked() const {
        auto reading = snapshot_.read();
        if (!reading) {
            throw AlpacaException("Upstream " + upstream_label_ + " has not answered since the Connect",
                                  AlpacaError::DriverException);
        }
        return *reading;
    }

    // A getter served from the frame (decisions 0010 and 0016): no upstream call. A stale frame is served.
    template <class Fn>
    double served(Fn&& pick) const {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected_locked();
        require_not_faulted_locked();
        return pick(frame_locked().value);
    }

    // A sensor the upstream lacks answers the upstream's own error from the Connect's probe (NotImplemented).
    double sensor_value(std::size_t index) const {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected_locked();
        if (!supported_[index]) return probe_[index].get();
        require_not_faulted_locked();
        return frame_locked().value.sensors[index].get();
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
        stop_polling();  // a lost link's poll has returned or is returning; join it so a fresh one starts
        // A command body from before (an AveragePeriod write-through) must not land on this Connect's frame.
        command_op_.cancel_all_and_join();
        snapshot_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            link_health_.reset();
            loss_text_.clear();  // rule 5: the kept text ends at the next Connect
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
            const auto token = snapshot_.begin_poll();
            Frame frame = sample(supported);
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
                snapshot_.publish(token, std::move(frame));
            }
            connected_.store(true);
            start_polling();
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
        stop_polling();
        command_op_.cancel_all_and_join();
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lost = !loss_text_.empty();
            connected_.store(false);
            snapshot_.reset();
            link_health_.reset();
            loss_text_.clear();  // rule 5: the kept text ends at the next Disconnect
        }
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
    // NotConnected when the poll lost the link meanwhile.
    void run_command(std::function<void(util::OperationContext&)> body) {
        struct Outcome {
            std::mutex mutex;
            std::condition_variable done_cv;
            bool done = false;
            std::exception_ptr error;
        };
        auto outcome = std::make_shared<Outcome>();
        command_op_.start([outcome, body = std::move(body)](util::OperationContext& ctx) {
            std::exception_ptr error;
            try {
                body(ctx);
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(outcome->mutex);
                outcome->error = error;
                outcome->done = true;
            }
            outcome->done_cv.notify_all();
        });
        {
            // A completion wait, like a join: the body ends after its calls, each bounded by kRemoteCallTimeout.
            std::unique_lock<std::mutex> lock(outcome->mutex);
            outcome->done_cv.wait(lock, [&outcome] { return outcome->done; });
        }
        if (!outcome->error) return;
        try {
            std::rethrow_exception(outcome->error);
        } catch (const RemoteConnectRefused& e) {
            lose_link(e.what());
            require_connected();
            throw;
        } catch (const RemoteTransportError&) {
            require_connected();
            throw;
        }
    }

    // Decision 0006 rule 1: the poll is a slot body. set_connected() runs under transition_mutex_, so a start never
    // races a stop.
    void start_polling() {
        if (poll_op_.running()) return;
        poll_op_.start([this](util::OperationContext& ctx) { poll_loop(ctx); });
    }

    void stop_polling() { poll_op_.cancel_all_and_join(); }

    // The Connect measured the first frame; the poll measures one each period, start to end plus the period.
    void poll_loop(util::OperationContext& ctx) {
        while (ctx.wait_for(config_.poll_period)) {
            // A command that met a refused connection lost the link: this poll ends too (decision 0009 rule 4).
            if (!connected_.load()) return;
            if (poll_paused_.load()) continue;
            const auto token = snapshot_.begin_poll();
            std::optional<Frame> frame;
            std::string failure;
            bool refused = false;
            try {
                frame = sample(supported_copy());
            } catch (const RemoteConnectRefused& e) {
                failure = e.what();
                refused = true;
            } catch (const std::exception& e) {
                failure = e.what();
            }
            if (ctx.stop_reason() != util::StopReason::None) {
                return;  // a Disconnect stopped this poll during the sample: it changes nothing
            }
            if (frame) {
                if (note_reply(token, std::move(*frame))) {
                    ALPACA_LOG_INFO(kComponent, "Upstream " + upstream_label_ + " restored: the poll answered again");
                }
            } else if (refused) {
                lose_link(failure);
                return;  // decision 0009 rule 4: a lost link's poll ends; only the client's Connect starts another
            } else if (note_poll_failure(failure)) {
                return;
            }
        }
    }

    // Publishes a frame and clears a standing fault; true when it cleared one.
    bool note_reply(util::StateSnapshot<Frame>::Token token, Frame frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_.load()) return false;  // lost or disconnected while measuring: serve nothing
        snapshot_.publish(token, std::move(frame));
        return link_health_.on_reply();
    }

    // Decision 0009 point 2(b): the bound is three poll periods (#1753 maintainer decision, 2026-11-16), never
    // shorter than util::kLinkStalenessBound: with the 2 s default the three-poll latch is followed by 30 s.
    std::chrono::nanoseconds loss_bound() const {
        return std::max<std::chrono::nanoseconds>(util::kLinkStalenessBound, 3 * config_.poll_period);
    }

    // Decision 0009 rules 1 to 3: a poll with no usable reply (a timeout, a dropped connection, an HTTP error, a
    // body that is not an Alpaca reply) is one failed exchange; the third in a row latches the fault, stamped on the
    // task clock, and a fault standing past loss_bound() is a lost link. True when this failure lost it.
    bool note_poll_failure(const std::string& reason) {
        const auto now = clock_.now();
        std::optional<std::string> latched;
        int failures = 0;
        bool lost = false;
        std::string fault;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latched = link_health_.note_failure(reason, kFailuresToLatch, now);
            failures = link_health_.consecutive_failures();
            fault = link_health_.fault();
            if (link_health_.fault_stale(now, loss_bound())) {
                lose_link_locked(fault);
                lost = true;
            }
        }
        if (lost) {
            const auto bound_s = std::chrono::duration_cast<std::chrono::seconds>(loss_bound()).count();
            ALPACA_LOG_ERROR(kComponent, "Upstream " + upstream_label_ + " lost: no answer for " +
                                             std::to_string(bound_s) + " s after the fault latched (" + fault +
                                             "); Connected is false until the client connects again");
        } else if (latched) {
            ALPACA_LOG_ERROR(kComponent, "Upstream " + upstream_label_ + " faulted: " + *latched);
        } else {
            ALPACA_LOG_WARN(kComponent, "Upstream poll failed (" + std::to_string(failures) + " in a row): " + reason);
        }
        return lost;
    }

    // Decision 0009 point 2(a), decision 0023: a refused connection says nothing listens there, a lost link at once.
    void lose_link(const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_.load()) return;
            lose_link_locked(reason);
        }
        ALPACA_LOG_ERROR(kComponent, "Upstream " + upstream_label_ + " lost: " + reason +
                                         "; Connected is false until the client connects again");
    }

    // Decision 0009 rule 3: Connected false (every member then throws NotConnected and DeviceState is empty), the
    // frame dropped; there is no transport to close, each call has its own. The reason stays for get_link_fault()
    // (rule 5). No LinkLostStamp: nothing here moves on a relink (no #521 consumer).
    void lose_link_locked(const std::string& reason) {
        connected_.store(false);
        loss_text_ = reason;
        snapshot_.reset();
    }

    const int device_number_;
    const RemoteObservingConditionsConfig config_;
    const ServerAddressSource self_;
    // The poll's wait, the fault stamp and the loss bound (decision 0005).
    util::TaskClock& clock_;
    // The I/O lock is the client's own: one call at a time per upstream device.
    RemoteAlpacaClient client_;
    const std::string upstream_label_;  // "<host>:<port> device <n>"

    std::atomic<bool> connected_{false};
    std::atomic<bool> poll_paused_{false};
    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()
    // Under transition_mutex_: the upstream was told Connect and not yet Disconnect, and its InterfaceVersion then.
    bool forwarded_connect_ = false;
    int forwarded_version_ = 0;

    // The state lock, a leaf of the driver's own locks (decision 0016): the snapshot mutex nests under it. Never held
    // across an upstream call.
    mutable std::mutex mutex_;
    util::StateSnapshot<Frame> snapshot_;
    util::PolledLinkHealth link_health_;  // decision 0009: the poll's fault latch, stamped on clock_
    std::string loss_text_;               // why the link was lost; empty while it is not
    std::string name_;
    std::string description_;
    std::array<bool, kSensorCount> supported_{};
    std::array<Answer<double>, kSensorCount> probe_;  // the Connect's answer, served for a sensor the upstream lacks
    std::array<Answer<std::string>, kSensorCount> descriptions_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so the slots are destroyed (and their bodies joined) before anything the bodies touch.
    util::OperationGeneration poll_generation_;
    util::AsyncOperation poll_op_;
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

std::uint64_t command_generation_for_testing(ObservingConditionsDriver& driver) {
    auto* remote = dynamic_cast<RemoteAlpacaObservingConditionsDriver*>(&driver);
    return remote ? remote->command_generation() : 0;
}

}  // namespace alpacacore::vendor::remotealpaca
