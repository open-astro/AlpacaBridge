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

// The remotealpaca Focuser proxied device (decision 0023, issue #1753 slice 5) over UpstreamProxy (upstream_proxy.h),
// which holds every rule it shares with the other proxied devices. What is the Focuser's own stays here: the frame
// (IsMoving, Position, Temperature, TempComp), the limits and capabilities the Connect reads once (Absolute, MaxStep,
// MaxIncrement, StepSize, TempCompAvailable), and the three commands, Move, Halt and the TempComp write.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_focuser_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <chrono>
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

// The polled frame (decision 0010): what the upstream answered for each member that changes while connected, a value
// or its error.
struct Frame {
    Answer<bool> is_moving;
    Answer<int> position;
    Answer<double> temperature;
    Answer<bool> temp_comp;
};

using Proxy = detail::UpstreamProxy<Frame>;

// What the Connect reads once: the upstream's limits and capabilities, each a value or its error (an upstream that
// lacks one answers NotImplemented, served unchanged).
struct Limits {
    Answer<bool> absolute;
    Answer<int> max_step;
    Answer<int> max_increment;
    Answer<double> step_size;
    Answer<bool> temp_comp_available;
};

class RemoteAlpacaFocuserDriver final : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaFocuserDriver(int device_number, const RemoteDeviceConfig& config,
                              std::shared_ptr<util::HttpClient> http, ServerAddressSource self, util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          // IFocuserV4 is the first Focuser with Connect; an older upstream gets Connected=true.
          proxy_({"focuser", "Focuser", 4, [this] { return sample(); }}, config, std::move(http), std::move(self),
                 clock) {}

    ~RemoteAlpacaFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::Focuser; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_FOC_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca Focuser Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }  // IFocuserV4 (Platform 7)

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

    // Limits and capabilities read at Connect: they keep answering while the link is faulted (static metadata).
    // Absolute is a capability every focuser driver answers while disconnected (the contract sweep's Can getters):
    // then it is what the last Connect read, and true before any Connect read it (an absolute focuser, the common
    // kind; the ASCOM Alpaca Simulators' focuser is one).
    // One state-lock hold decides both, so a racing Disconnect cannot make the read throw NotConnected.
    bool get_absolute() const override {
        return proxy_.with_state([this](const Proxy::State&) {
            return proxy_.connected() ? limits_.absolute.get() : limits_.absolute.value.value_or(true);
        });
    }
    int get_max_step() const override {
        return proxy_.with_connected([this](const Proxy::State&) { return limits_.max_step.get(); });
    }
    int get_max_increment() const override {
        return proxy_.with_connected([this](const Proxy::State&) { return limits_.max_increment.get(); });
    }
    double get_step_size() const override {
        return proxy_.with_connected([this](const Proxy::State&) { return limits_.step_size.get(); });
    }
    bool get_temp_comp_available() const override {
        return proxy_.with_connected([this](const Proxy::State&) { return limits_.temp_comp_available.get(); });
    }

    // From the frame (a stale frame is served), or the upstream's error unchanged; a latched fault refuses the frame
    // with DriverException (decision 0009 rule 1).
    bool get_is_moving() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.is_moving.get(); });
    }
    int get_position() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.position.get(); });
    }
    double get_temperature() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.temperature.get(); });
    }
    bool get_temp_comp() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.temp_comp.get(); });
    }

    // Forwarded; the upstream decides whether it has temperature compensation, and its refusal passes through
    // unchanged. Refused while the link is faulted (decision 0009 rule 1). Written through on success.
    void set_temp_comp(bool temp_comp) override {
        (void)proxy_.with_connected([](const Proxy::State& state) { return state.served().measured_at; });
        proxy_.run_command([this, temp_comp](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("tempcomp", {{"TempComp", temp_comp ? "True" : "False"}});
            write_through(ctx, [temp_comp](Frame& frame) { frame.temp_comp = {temp_comp, 0, {}}; });
        });
    }

    // Forwarded even while the link is faulted: the safety stop is refused only for a lost link (NotConnected).
    // Written through, so IsMoving reads false right after Halt, not after the next poll. A Halt sent while a Move's
    // body runs waits for it (run_command() keeps caller order), so the upstream gets the Halt after the Move.
    void halt() override {
        proxy_.require_connected();
        proxy_.run_command([this](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("halt");
            write_through(ctx, [](Frame& frame) { frame.is_moving = {false, 0, {}}; });
        });
    }

    // An absolute target outside 0..MaxStep goes to that limit: ConformU moves below 0 and above MaxStep and expects
    // the focuser to stop there, not an InvalidValue (ascom-contract.instructions.md, #2028). A relative step goes to
    // the upstream as given. Refused while the link is faulted (decision 0009 rule 1).
    void move(int position) override {
        struct Seen {
            bool absolute = false;
            std::optional<int> max_step;
        };
        const Seen seen = proxy_.with_connected([this](const Proxy::State& state) {
            (void)state.served();  // refused while the link is faulted
            return Seen{limits_.absolute.value.value_or(false), limits_.max_step.value};
        });
        int target = position;
        if (seen.absolute && seen.max_step) target = std::clamp(target, 0, std::max(*seen.max_step, 0));
        // An absolute Move always moves as far as this driver knows: the frame's Position can be a poll period old (a
        // hand controller, another client or a coasting halt may have moved the focuser since), so it is no evidence
        // that the target is reached. Only a relative step of 0 has nowhere to go.
        const bool moves = seen.absolute || target != 0;
        proxy_.run_command([this, target, moves](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("move", {{"Position", std::to_string(target)}});
            // IsMoving reads true right after a Move; Position stays the measured one until the next poll, which also
            // drops any sample taken before the command (decision 0010 rule 4). A relative step of 0 writes nothing: an
            // earlier move may still run, so the polled IsMoving stands.
            if (moves) write_through(ctx, [](Frame& frame) { frame.is_moving = {true, 0, {}}; });
        });
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; a Move, Halt or TempComp body calls it before its PUT.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    // Set while no command runs; a command's caller that waits for an earlier command calls it first.
    void set_command_waiting_hook(std::function<void()> hook) { proxy_.set_command_waiting_hook(std::move(hook)); }

protected:
    // DeviceState's TimeStamp is when the served frame was measured, not the request time (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        return proxy_.with_state([](const util::PolledState<Frame>& state) { return state.age(); });
    }

private:
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

    // One measurement of the upstream, with no lock held (decision 0016 rule 1). Throws RemoteTransportError when a
    // call gets no usable reply; an upstream error is kept in the frame.
    Frame sample() {
        RemoteAlpacaClient& client = proxy_.client();
        Frame frame;
        frame.is_moving = answer_of([&] { return client.get_bool("ismoving"); });
        frame.position = answer_of([&] { return client.get_int("position"); });
        frame.temperature = answer_of([&] { return client.get_double("temperature"); });
        frame.temp_comp = answer_of([&] { return client.get_bool("tempcomp"); });
        return frame;
    }

    // At Connect, with no lock held, after the upstream's identity: the limits and capabilities, read once into
    // read_limits_, and what to store under the state lock with the identity, before the first frame is measured. The
    // store captures only `this`, so the std::function holds it with no allocation.
    std::function<void()> read_limits() {
        RemoteAlpacaClient& client = proxy_.client();
        read_limits_.absolute = answer_of([&] { return client.get_bool("absolute"); });
        read_limits_.max_step = answer_of([&] { return client.get_int("maxstep"); });
        read_limits_.max_increment = answer_of([&] { return client.get_int("maxincrement"); });
        read_limits_.step_size = answer_of([&] { return client.get_double("stepsize"); });
        read_limits_.temp_comp_available = answer_of([&] { return client.get_bool("tempcompavailable"); });
        return [this] { limits_ = read_limits_; };
    }

    const int device_number_;

    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()

    // Under the proxy's state lock (with_connected(), with_state(), the Connect's store).
    Limits limits_;
    // The Connect's reads before the store, under transition_mutex_ (only the connect path touches it). A member, not a
    // capture of the store lambda: a by-value capture made clang-analyzer report NewDeleteLeaks in std::function.
    Limits read_limits_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so it is destroyed (its poll and command bodies joined) before the members the bodies touch die.
    Proxy proxy_;
};

}  // namespace

std::unique_ptr<FocuserDriver> create_remotealpaca_focuser(int device_number, const RemoteDeviceConfig& config,
                                                           ServerAddressSource self) {
    return create_remotealpaca_focuser(device_number, config,
                                       util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"), std::move(self),
                                       util::default_task_clock());
}

std::unique_ptr<FocuserDriver> create_remotealpaca_focuser(int device_number, const RemoteDeviceConfig& config,
                                                           std::shared_ptr<util::HttpClient> http,
                                                           ServerAddressSource self, util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaFocuserDriver>(device_number, config, std::move(http), std::move(self), clock);
}

void set_poll_paused_for_testing(FocuserDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaFocuserDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(FocuserDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaFocuserDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

void set_command_waiting_hook_for_testing(FocuserDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaFocuserDriver*>(&driver)) {
        remote->set_command_waiting_hook(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::remotealpaca
