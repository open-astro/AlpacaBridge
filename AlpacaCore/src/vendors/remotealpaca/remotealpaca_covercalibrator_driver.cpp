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

// The remotealpaca CoverCalibrator proxied device (decision 0023, issue #1753 slice 5, #2704) over UpstreamProxy
// (upstream_proxy.h), which holds every rule it shares with the other proxied devices. What is the CoverCalibrator's
// own stays here: the frame (CoverState, CoverMoving, CalibratorState, CalibratorChanging, Brightness), what the
// Connect reads once (MaxBrightness, and whether the cover and the calibrator are there: a CoverState or
// CalibratorState of NotPresent), the members an ICoverCalibratorV1 upstream lacks (CoverMoving and
// CalibratorChanging, by the InterfaceVersion the proxy read), and the commands: OpenCover, CloseCover, HaltCover,
// CalibratorOn and CalibratorOff.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_covercalibrator_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "upstream_proxy.h"

namespace alpacacore::vendor::remotealpaca {

namespace {

using detail::Answer;
using detail::answer_of;

// ICoverCalibratorV2 added CoverMoving and CalibratorChanging; an ICoverCalibratorV1 upstream has neither.
constexpr int kFirstVersionWithChanging = 2;

// CoverState and CalibratorState both run from NotPresent (0) to Error (5).
constexpr int kLastState = static_cast<int>(CoverState::Error);
static_assert(static_cast<int>(CalibratorState::Error) == kLastState);

// The polled frame (decision 0010): what the upstream answered for each member that changes while connected, a value
// or its error. A member of a part the Connect found NotPresent, or that an ICoverCalibratorV1 upstream lacks, is not
// sampled, and no getter reads it.
struct Frame {
    Answer<int> cover_state;
    Answer<bool> cover_moving;
    Answer<int> calibrator_state;
    Answer<bool> calibrator_changing;
    Answer<int> brightness;
};

using Proxy = detail::UpstreamProxy<Frame>;

// What the Connect reads once: MaxBrightness (a value or the upstream's error, served unchanged), and whether the
// cover and the calibrator are there. A CoverState or CalibratorState the upstream answered with an error counts as
// present: the poll asks it again and serves that answer.
struct Limits {
    Answer<int> max_brightness;
    bool cover_present = true;
    bool calibrator_present = true;
};

class RemoteAlpacaCoverCalibratorDriver final : public CoverCalibratorDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaCoverCalibratorDriver(int device_number, const RemoteDeviceConfig& config,
                                      std::shared_ptr<util::HttpClient> http, ServerAddressSource self,
                                      util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          // ICoverCalibratorV2 is the first CoverCalibrator with Connect; an older upstream gets Connected=true.
          proxy_({"covercalibrator", "CoverCalibrator", 2, [this] { return sample(); }}, config, std::move(http),
                 std::move(self), clock) {}

    ~RemoteAlpacaCoverCalibratorDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }

    std::string fallback_unique_id() const override {
        return "REMOTEALPACA_COVERCAL_" + std::to_string(device_number_);
    }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca CoverCalibrator Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 2; }  // ICoverCalibratorV2 (Platform 7)

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
            proxy_.connect([this] { return read_limits(); });
        } else {
            proxy_.disconnect();
        }
    }

    // --- the cover ---

    // From the frame (a stale frame is served), or the upstream's error unchanged. NotPresent when the Connect read
    // it so: that is static and answers while the link is faulted. While a fault is latched the frame's state is
    // Unknown, the ASCOM word for it (driver-lifecycle "Cache-backed reads must track link health"), where the other
    // frame getters throw DriverException (decision 0009 rule 1).
    CoverState get_cover_state() const override {
        return proxy_.with_connected([this](const Proxy::State& state) {
            if (!limits_.cover_present) return CoverState::NotPresent;
            const auto reading = served_unless_faulted(state);
            if (!reading) return CoverState::Unknown;
            return static_cast<CoverState>(reading->value.cover_state.get());
        });
    }

    // From the frame; false with no cover (ICoverCalibratorV2). NotImplemented for an ICoverCalibratorV1 upstream.
    bool get_cover_moving() const override {
        proxy_.require_connected();
        require_changing_members("CoverMoving");
        return proxy_.with_connected([this](const Proxy::State& state) {
            if (!limits_.cover_present) return false;
            return state.served().value.cover_moving.get();
        });
    }

    // Forwarded; refused while the link is faulted (decision 0009 rule 1). CoverState reads Moving at once: the frame's
    // state can be a poll period old, so it cannot say the cover is already there; the next poll has the upstream's.
    void open_cover() override { start_cover("opencover", "OpenCover"); }

    void close_cover() override { start_cover("closecover", "CloseCover"); }

    // Forwarded even while the link is faulted: the safety stop is refused only for a lost link (NotConnected).
    // CoverMoving reads false right after it; CoverState is the upstream's, read after the PUT (the cover stops where
    // it is, which only the upstream knows): a failed read leaves it to the next poll and does not fail the Halt.
    void halt_cover() override {
        proxy_.require_connected();
        require_cover("HaltCover");
        run([this](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            RemoteAlpacaClient& client = proxy_.client();
            client.put("haltcover");
            std::optional<Answer<int>> state;
            try {
                state = state_answer(client, "coverstate");
            } catch (const RemoteTransportError&) {  // NOLINT(bugprone-empty-catch): the next poll reads it
            }
            const bool changing_members = has_changing_members();
            write_through(ctx, [changing_members, &state](Frame& frame) {
                if (changing_members) frame.cover_moving = {false, 0, {}};
                if (state) frame.cover_state = *state;
            });
        });
    }

    // --- the calibrator ---

    // As CoverState: from the frame, NotPresent when the Connect read it so, Unknown while a fault is latched.
    CalibratorState get_calibrator_state() const override {
        return proxy_.with_connected([this](const Proxy::State& state) {
            if (!limits_.calibrator_present) return CalibratorState::NotPresent;
            const auto reading = served_unless_faulted(state);
            if (!reading) return CalibratorState::Unknown;
            return static_cast<CalibratorState>(reading->value.calibrator_state.get());
        });
    }

    // From the frame; false with no calibrator (ICoverCalibratorV2). NotImplemented for an ICoverCalibratorV1 upstream.
    bool get_calibrator_changing() const override {
        proxy_.require_connected();
        require_changing_members("CalibratorChanging");
        return proxy_.with_connected([this](const Proxy::State& state) {
            if (!limits_.calibrator_present) return false;
            return state.served().value.calibrator_changing.get();
        });
    }

    // From the frame, or the upstream's error unchanged; NotImplemented with no calibrator (ICoverCalibratorV2), which
    // answers while the link is faulted.
    int get_brightness() const override {
        return proxy_.with_connected([this](const Proxy::State& state) {
            if (!limits_.calibrator_present) throw no_part("calibrator", "CalibratorState", "Brightness");
            return state.served().value.brightness.get();
        });
    }

    // Read at Connect: keeps answering while the link is faulted (static metadata).
    int get_max_brightness() const override {
        return proxy_.with_connected([this](const Proxy::State&) { return limits_.max_brightness.get(); });
    }

    // A negative brightness is InvalidValue before the connection check; one above the MaxBrightness the Connect read
    // is InvalidValue after it (an upstream that answered MaxBrightness with an error judges the range itself).
    // Refused while the link is faulted. CalibratorState reads NotReady and CalibratorChanging true at once, and
    // Brightness the value sent; the next poll has the upstream's state.
    void calibrator_on(int brightness) override {
        if (brightness < 0) {
            throw AlpacaException("Brightness " + std::to_string(brightness) + " is below 0",
                                  AlpacaError::InvalidValue);
        }
        proxy_.require_connected();
        require_calibrator("CalibratorOn");
        const Answer<int> max = proxy_.with_state([this](const Proxy::State&) { return limits_.max_brightness; });
        if (max.value && brightness > *max.value) {
            throw AlpacaException(
                "Brightness " + std::to_string(brightness) + " is above MaxBrightness " + std::to_string(*max.value),
                AlpacaError::InvalidValue);
        }
        require_frame();
        run([this, brightness](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("calibratoron", {{"Brightness", std::to_string(brightness)}});
            const bool changing_members = has_changing_members();
            write_through(ctx, [changing_members, brightness](Frame& frame) {
                frame.calibrator_state = {static_cast<int>(CalibratorState::NotReady), 0, {}};
                if (changing_members) frame.calibrator_changing = {true, 0, {}};
                frame.brightness = {brightness, 0, {}};
            });
        });
    }

    // Refused while the link is faulted. CalibratorState reads NotReady and CalibratorChanging true at once; Brightness
    // stays the polled one until the next poll, which also has the upstream's state.
    void calibrator_off() override {
        proxy_.require_connected();
        require_calibrator("CalibratorOff");
        require_frame();
        run([this](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("calibratoroff");
            const bool changing_members = has_changing_members();
            write_through(ctx, [changing_members](Frame& frame) {
                frame.calibrator_state = {static_cast<int>(CalibratorState::NotReady), 0, {}};
                if (changing_members) frame.calibrator_changing = {true, 0, {}};
            });
        });
    }

    // Not an ASCOM member (Brightness is set through CalibratorOn), kept for the interface: CalibratorOn.
    void set_brightness(int brightness) override { calibrator_on(brightness); }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; a command body calls it first, before its PUT.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    // Set while no command runs; a caller that waits for an earlier command calls it first.
    void set_command_waiting_hook(std::function<void()> hook) { proxy_.set_command_waiting_hook(std::move(hook)); }

private:
    // The frame, or nothing while a fault is latched; a lost link, or no frame, still throws NotConnected. Only the
    // module's own refusal is caught: an upstream error kept in the frame is thrown by the caller's get().
    static auto served_unless_faulted(const util::PolledState<Frame>& state)
        -> std::optional<decltype(state.served())> {
        try {
            return state.served();
        } catch (const AlpacaException& e) {
            if (e.error_code() != AlpacaError::DriverException) throw;
            return std::nullopt;
        }
    }

    std::string upstream_label() const {
        const RemoteDeviceConfig& config = proxy_.config();
        return "Upstream " + config.host + ":" + std::to_string(config.port) + " device " +
               std::to_string(config.upstream_device_number);
    }

    // MethodNotImplemented or PropertyNotImplemented (one code) for a member of a part the upstream does not have.
    AlpacaException no_part(const char* part, const char* state_member, const char* member) const {
        return AlpacaException(upstream_label() + " has no " + part + " (its " + state_member +
                                   " is NotPresent): " + member + " is not implemented",
                               AlpacaError::NotImplemented);
    }

    void require_cover(const char* member) const {
        if (proxy_.with_state([this](const Proxy::State&) { return limits_.cover_present; })) return;
        throw no_part("cover", "CoverState", member);
    }

    void require_calibrator(const char* member) const {
        if (proxy_.with_state([this](const Proxy::State&) { return limits_.calibrator_present; })) return;
        throw no_part("calibrator", "CalibratorState", member);
    }

    bool has_changing_members() const { return proxy_.upstream_interface_version() >= kFirstVersionWithChanging; }

    // NotImplemented for a member an ICoverCalibratorV1 upstream lacks: it is never called, so an upstream that answers
    // an unknown member with HTTP 400 does not read as a failed poll or a lost link.
    void require_changing_members(const char* member) const {
        if (has_changing_members()) return;
        throw AlpacaException(upstream_label() + " is an ICoverCalibratorV" +
                                  std::to_string(proxy_.upstream_interface_version()) + " and has no " + member,
                              AlpacaError::NotImplemented);
    }

    // A command that needs the link: NotConnected, or refused while the link is faulted (decision 0009 rule 1).
    void require_frame() const {
        (void)proxy_.with_connected([](const Proxy::State& state) { return state.served().measured_at; });
    }

    // OpenCover and CloseCover: forwarded in the command slot. CoverState is written through Moving, and CoverMoving
    // true from an upstream that has it, whatever the frame said: only the poll writes the cover settled.
    void start_cover(const char* path, const char* member) {
        proxy_.require_connected();
        require_cover(member);
        require_frame();
        run([this, path](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put(path);
            const bool changing_members = has_changing_members();
            write_through(ctx, [changing_members](Frame& frame) {
                frame.cover_state = {static_cast<int>(CoverState::Moving), 0, {}};
                if (changing_members) frame.cover_moving = {true, 0, {}};
            });
        });
    }

    void run(std::function<void(util::OperationContext&)> body) { proxy_.run_command(std::move(body)); }

    // Write-through (decision 0010 rule 2): the next read sees the command before the next poll. A superseded body
    // writes nothing (decision 0006 rule 6): the newer command's state stands. Nor does a body whose caller passed its
    // check before a Disconnect: with no connection there is no frame, and the write's epoch bump would drop the next
    // Connect's first frame (decision 0010 rule 8, #2446).
    template <class Apply>
    void write_through(util::OperationContext& ctx, Apply apply) {
        proxy_.with_state([this, &ctx, &apply](Proxy::State& state) {
            if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
            state.write(apply);
        });
    }

    // A CoverState or CalibratorState answer; a number outside the ASCOM enumeration is the upstream's error, not a
    // state this server serves.
    Answer<int> state_answer(RemoteAlpacaClient& client, const char* member) const {
        Answer<int> answer = answer_of([&] { return client.get_int(member); });
        if (answer.value && (*answer.value < 0 || *answer.value > kLastState)) {
            return {std::nullopt, AlpacaError::DriverException,
                    upstream_label() + " answered " + member + " " + std::to_string(*answer.value) +
                        ", which is not an ASCOM state"};
        }
        return answer;
    }

    // One measurement of the upstream, with no lock held (decision 0016 rule 1). Throws RemoteTransportError when a
    // call gets no usable reply; an upstream error is kept in the frame. A part the Connect found NotPresent, and the
    // members an ICoverCalibratorV1 upstream lacks, are not asked.
    Frame sample() {
        RemoteAlpacaClient& client = proxy_.client();
        const Limits limits = proxy_.with_state([this](const Proxy::State&) { return limits_; });
        const bool changing_members = has_changing_members();
        Frame frame;
        if (limits.cover_present) {
            frame.cover_state = state_answer(client, "coverstate");
            if (changing_members) frame.cover_moving = answer_of([&] { return client.get_bool("covermoving"); });
        }
        if (limits.calibrator_present) {
            frame.calibrator_state = state_answer(client, "calibratorstate");
            if (changing_members) {
                frame.calibrator_changing = answer_of([&] { return client.get_bool("calibratorchanging"); });
            }
            frame.brightness = answer_of([&] { return client.get_int("brightness"); });
        }
        return frame;
    }

    // At Connect, with no lock held, after the upstream's identity: which parts the upstream has and MaxBrightness,
    // read once into read_limits_, and what to store under the state lock with the identity. The store captures only
    // `this`, so the std::function holds it with no allocation.
    std::function<void()> read_limits() {
        RemoteAlpacaClient& client = proxy_.client();
        const Answer<int> cover = state_answer(client, "coverstate");
        const Answer<int> calibrator = state_answer(client, "calibratorstate");
        read_limits_.cover_present = cover.value != static_cast<int>(CoverState::NotPresent);
        read_limits_.calibrator_present = calibrator.value != static_cast<int>(CalibratorState::NotPresent);
        read_limits_.max_brightness = answer_of([&] { return client.get_int("maxbrightness"); });
        return [this] { limits_ = read_limits_; };
    }

    const int device_number_;

    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()

    // Under the proxy's state lock (with_connected(), with_state(), the Connect's store).
    Limits limits_;
    // The Connect's reads before the store, under transition_mutex_ (only the connect path touches it).
    Limits read_limits_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so it is destroyed (its poll and command bodies joined) before the members the bodies touch die.
    Proxy proxy_;
};

}  // namespace

std::unique_ptr<CoverCalibratorDriver> create_remotealpaca_covercalibrator(int device_number,
                                                                           const RemoteDeviceConfig& config,
                                                                           ServerAddressSource self) {
    return create_remotealpaca_covercalibrator(device_number, config,
                                               util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"),
                                               std::move(self), util::default_task_clock());
}

std::unique_ptr<CoverCalibratorDriver> create_remotealpaca_covercalibrator(int device_number,
                                                                           const RemoteDeviceConfig& config,
                                                                           std::shared_ptr<util::HttpClient> http,
                                                                           ServerAddressSource self,
                                                                           util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaCoverCalibratorDriver>(device_number, config, std::move(http), std::move(self),
                                                               clock);
}

void set_poll_paused_for_testing(CoverCalibratorDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaCoverCalibratorDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(CoverCalibratorDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaCoverCalibratorDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

void set_command_waiting_hook_for_testing(CoverCalibratorDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaCoverCalibratorDriver*>(&driver)) {
        remote->set_command_waiting_hook(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::remotealpaca
