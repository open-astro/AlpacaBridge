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

// The remotealpaca Rotator proxied device (decision 0023, issue #1753 slice 5, #2708) over UpstreamProxy
// (upstream_proxy.h), which holds every rule it shares with the other proxied devices. What is the Rotator's own stays
// here: the frame (IsMoving, Position, MechanicalPosition, TargetPosition, Reverse), what the Connect reads once
// (CanReverse, StepSize and the upstream's InterfaceVersion), the members an IRotatorV1 or V2 upstream lacks, and the
// commands: Move, MoveAbsolute, MoveMechanical, Sync, Halt and the Reverse write. The upstream keeps its own Sync
// offset: this driver stores none (decision 0018 rule 4 has nothing to move here).

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_rotator_driver.h>
#include <alpacacore/version.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
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

// IRotatorV3 added MechanicalPosition, MoveMechanical and Sync; an older upstream has none of them.
constexpr int kFirstVersionWithMechanical = 3;

// The polled frame (decision 0010): what the upstream answered for each member that changes while connected, a value
// or its error.
struct Frame {
    Answer<bool> is_moving;
    Answer<double> position;
    Answer<double> mechanical_position;  // not sampled from an upstream older than IRotatorV3
    Answer<double> target_position;
    Answer<bool> reverse;
};

using Proxy = detail::UpstreamProxy<Frame>;

// What the Connect reads once: the upstream's capability and step size, each a value or its error (an upstream that
// lacks one answers NotImplemented, served unchanged).
struct Limits {
    Answer<bool> can_reverse;
    Answer<double> step_size;
};

// A double in its shortest round-trip form, for a form body.
std::string number_text(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

// InvalidValue before NotConnected (ascom-contract.instructions.md): NaN and infinity are invalid whatever the
// rotator is. The range is the upstream's to judge: a finite angle goes to it as given, and its InvalidValue passes
// through unchanged.
void validate_angle(double degrees) {
    if (!std::isfinite(degrees)) {
        throw AlpacaException("Rotator position must be a finite number of degrees", AlpacaError::InvalidValue);
    }
}

class RemoteAlpacaRotatorDriver final : public RotatorDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaRotatorDriver(int device_number, const RemoteDeviceConfig& config,
                              std::shared_ptr<util::HttpClient> http, ServerAddressSource self, util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          // IRotatorV4 is the first Rotator with Connect; an older upstream gets Connected=true.
          proxy_({"rotator", "Rotator", 4, [this] { return sample(); }}, config, std::move(http), std::move(self),
                 clock) {}

    ~RemoteAlpacaRotatorDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::Rotator; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_ROT_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca Rotator Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }  // IRotatorV4 (Platform 7)

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

    // A capability every rotator driver answers while disconnected (the contract sweep's Can getters): then it is
    // what the last Connect read, and true before any Connect read it (IRotatorV3 and later must answer true).
    // Connected, the upstream's answer or its error, which keeps answering while the link is faulted (static
    // metadata).
    bool get_can_reverse() const override {
        if (!proxy_.connected()) {
            return proxy_.with_state([this](const Proxy::State&) { return limits_.can_reverse.value.value_or(true); });
        }
        return proxy_.with_connected([this](const Proxy::State&) { return limits_.can_reverse.get(); });
    }

    // Read at Connect: keeps answering while the link is faulted (static metadata).
    double get_step_size() const override {
        return proxy_.with_connected([this](const Proxy::State&) { return limits_.step_size.get(); });
    }

    // From the frame (a stale frame is served), or the upstream's error unchanged; a latched fault refuses the frame
    // with DriverException (decision 0009 rule 1).
    bool get_is_moving() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.is_moving.get(); });
    }
    double get_position() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.position.get(); });
    }
    double get_target_position() const override {
        return proxy_.with_connected(
            [](const Proxy::State& state) { return state.served().value.target_position.get(); });
    }
    bool get_reverse() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.reverse.get(); });
    }
    double get_mechanical_position() const override {
        proxy_.require_connected();
        require_mechanical("MechanicalPosition");
        return proxy_.with_connected(
            [](const Proxy::State& state) { return state.served().value.mechanical_position.get(); });
    }

    // TargetPosition is read-only in IRotatorV4 (the PUT is this server's extension), and the upstream has no member
    // to take it: MoveAbsolute sets it.
    void set_target_position(double position) override {
        validate_angle(position);
        proxy_.require_connected();
        throw AlpacaException("TargetPosition of a Remote Alpaca rotator is read-only; MoveAbsolute sets it",
                              AlpacaError::PropertyNotImplemented);
    }

    // Forwarded; refused while the link is faulted (decision 0009 rule 1). Written through on success.
    void set_reverse(bool reverse) override {
        (void)proxy_.with_connected([](const Proxy::State& state) { return state.served().measured_at; });
        run([this, reverse](util::OperationContext& ctx) {
            proxy_.client().put("reverse", {{"Reverse", reverse ? "True" : "False"}});
            if (command_hook_) command_hook_();
            write_through(ctx, [reverse](Frame& frame) { frame.reverse = {reverse, 0, {}}; });
        });
    }

    // Forwarded even while the link is faulted: the safety stop is refused only for a lost link (NotConnected).
    // Written through, so IsMoving reads false right after Halt, not after the next poll.
    void halt() override {
        proxy_.require_connected();
        run([this](util::OperationContext& ctx) {
            proxy_.client().put("halt");
            if (command_hook_) command_hook_();
            write_through(ctx, [](Frame& frame) { frame.is_moving = {false, 0, {}}; });
        });
    }

    // IRotator.Move: relative to the current Position; the upstream adds it. Refused while the link is faulted.
    void move(double position) override {
        validate_angle(position);
        const Seen seen = seen_frame();
        std::optional<double> target;
        if (seen.position) target = util::wrap_degrees(*seen.position + position);
        start_move("move", position, position != 0.0, target);
    }

    void move_absolute(double position) override {
        validate_angle(position);
        const Seen seen = seen_frame();
        const double target = util::wrap_degrees(position);
        start_move("moveabsolute", position, !seen.position || util::wrap_degrees(*seen.position) != target, target);
    }

    // To a raw mechanical angle; TargetPosition is that angle plus the upstream's Sync offset (Position minus
    // MechanicalPosition, both from the frame).
    void move_mechanical(double position) override {
        validate_angle(position);
        proxy_.require_connected();
        require_mechanical("MoveMechanical");
        const Seen seen = seen_frame();
        std::optional<double> target;
        if (seen.position && seen.mechanical) target = util::wrap_degrees(position + *seen.position - *seen.mechanical);
        start_move("movemechanical", position,
                   !seen.mechanical || util::wrap_degrees(*seen.mechanical) != util::wrap_degrees(position), target);
    }

    // The upstream keeps the offset (IRotatorV4: it persists); Position is written through as the synced angle.
    void sync(double position) override {
        validate_angle(position);
        proxy_.require_connected();
        require_mechanical("Sync");
        (void)seen_frame();  // refused while the link is faulted (decision 0009 rule 1)
        run([this, position](util::OperationContext& ctx) {
            proxy_.client().put("sync", {{"Position", number_text(position)}});
            if (command_hook_) command_hook_();
            write_through(ctx, [position](Frame& frame) { frame.position = {util::wrap_degrees(position), 0, {}}; });
        });
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; a command body calls it after its PUT, before it touches state.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

protected:
    // DeviceState's TimeStamp is when the served frame was measured, not the request time (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        return proxy_.with_state([](const util::PolledState<Frame>& state) { return state.age(); });
    }

private:
    // What a move needs from the frame, read under the state lock; refused while the link is faulted.
    struct Seen {
        std::optional<double> position;
        std::optional<double> mechanical;
    };

    Seen seen_frame() const {
        return proxy_.with_connected([](const Proxy::State& state) {
            const auto reading = state.served();
            return Seen{reading.value.position.value, reading.value.mechanical_position.value};
        });
    }

    // NotImplemented for a member an IRotatorV1 or V2 upstream lacks: it is never called, so an upstream that answers
    // an unknown member with HTTP 400 does not read as a failed poll or a lost link.
    void require_mechanical(const char* member) const {
        const int version = upstream_version_.load();
        if (version >= kFirstVersionWithMechanical) return;
        const RemoteDeviceConfig& config = proxy_.config();
        throw AlpacaException("Upstream " + config.host + ":" + std::to_string(config.port) + " device " +
                                  std::to_string(config.upstream_device_number) + " is an IRotatorV" +
                                  std::to_string(version) + " and has no " + member,
                              AlpacaError::NotImplemented);
    }

    // The three moves: forwarded in the command slot, IsMoving written through (true when the move has somewhere to
    // go, so a client reads it true at once) and TargetPosition when the frame gave what it takes. Position stays the
    // measured one until the next poll, which also drops any sample taken before the command (decision 0010 rule 4).
    void start_move(const char* member, double position, bool moves, std::optional<double> target) {
        run([this, member, position, moves, target](util::OperationContext& ctx) {
            proxy_.client().put(member, {{"Position", number_text(position)}});
            if (command_hook_) command_hook_();
            write_through(ctx, [moves, target](Frame& frame) {
                frame.is_moving = {moves, 0, {}};
                if (target) frame.target_position = {target, 0, {}};
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

    // One measurement of the upstream, with no lock held (decision 0016 rule 1). Throws RemoteTransportError when a
    // call gets no usable reply; an upstream error is kept in the frame. MechanicalPosition is not asked of an
    // upstream that lacks it.
    Frame sample() {
        RemoteAlpacaClient& client = proxy_.client();
        Frame frame;
        frame.is_moving = answer_of([&] { return client.get_bool("ismoving"); });
        frame.position = answer_of([&] { return client.get_double("position"); });
        if (upstream_version_.load() >= kFirstVersionWithMechanical) {
            frame.mechanical_position = answer_of([&] { return client.get_double("mechanicalposition"); });
        } else {
            frame.mechanical_position = {std::nullopt, AlpacaError::NotImplemented, "no MechanicalPosition"};
        }
        frame.target_position = answer_of([&] { return client.get_double("targetposition"); });
        frame.reverse = answer_of([&] { return client.get_bool("reverse"); });
        return frame;
    }

    // At Connect, with no lock held, after the upstream's identity: its InterfaceVersion (which members it has, set
    // before the first frame is sampled), CanReverse and StepSize, read once into read_limits_, and what to store under
    // the state lock with the identity. The store captures only `this`, so the std::function holds it with no
    // allocation.
    std::function<void()> read_limits() {
        RemoteAlpacaClient& client = proxy_.client();
        upstream_version_.store(client.get_int("interfaceversion"));
        read_limits_.can_reverse = answer_of([&] { return client.get_bool("canreverse"); });
        read_limits_.step_size = answer_of([&] { return client.get_double("stepsize"); });
        return [this] { limits_ = read_limits_; };
    }

    const int device_number_;

    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()

    // The upstream's InterfaceVersion, written by the Connect before the first sample and read by the poll, the
    // getters and the commands.
    std::atomic<int> upstream_version_{0};
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

std::unique_ptr<RotatorDriver> create_remotealpaca_rotator(int device_number, const RemoteDeviceConfig& config,
                                                           ServerAddressSource self) {
    return create_remotealpaca_rotator(device_number, config,
                                       util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"), std::move(self),
                                       util::default_task_clock());
}

std::unique_ptr<RotatorDriver> create_remotealpaca_rotator(int device_number, const RemoteDeviceConfig& config,
                                                           std::shared_ptr<util::HttpClient> http,
                                                           ServerAddressSource self, util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaRotatorDriver>(device_number, config, std::move(http), std::move(self), clock);
}

void set_poll_paused_for_testing(RotatorDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaRotatorDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(RotatorDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaRotatorDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::remotealpaca
