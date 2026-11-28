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

// The remotealpaca Dome proxied device (decision 0023, issue #1753 slice 5, #2705) over UpstreamProxy
// (upstream_proxy.h), which holds every rule it shares with the other proxied devices. What is the Dome's own stays
// here: the frame (Slewing, Azimuth, Altitude, AtHome, AtPark, ShutterStatus, Slaved), what the Connect reads once
// (the eight Can* capabilities, and the one answer of each frame member the poll skips), and the commands. The
// upstream judges every command: a capability the upstream lacks answers its own NotImplemented, unchanged (decision
// 0023 rule 5), and a finite angle goes to it as given.
//
// Dome motion moves hardware, so nothing here reads it settled before the upstream says so. A command writes only its
// in-progress state through (Slewing true after a slew, FindHome or Park; ShutterStatus Opening or Closing); only a
// poll, which samples after the command (decision 0010 rule 3 drops one that sampled before), writes Slewing false or
// the shutter Open or Closed. A motion or shutter PUT with no usable reply writes the in-progress state too: the
// upstream may have acted. AbortSlew writes nothing: a dome coasts after the stop, and the next poll has the
// upstream's Slewing. While the link is faulted the frame is refused (DriverException), and once it is lost every
// member is NotConnected (decision 0009 rule 3), so no stale "not slewing" is served across a link failure; a relink's
// Connect measures a fresh frame before any getter answers. Nothing is stopped on a relink: the upstream's own driver
// runs the motion it was given, and the first frame says whether it still moves.
//
// From the start of a Disconnect no command but AbortSlew reaches the upstream: a body checks it before its PUT, so a
// command queued behind a running one, or one whose caller passed its check before the Disconnect, never sends after
// it.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_dome_driver.h>
#include <alpacacore/version.h>

#include <atomic>
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
using detail::number_text;

// IDome ShutterState (ShutterStatus): what an accepted OpenShutter or CloseShutter writes through.
constexpr int kShutterOpening = 2;
constexpr int kShutterClosing = 3;

// The polled frame (decision 0010): what the upstream answered for each member that changes while connected, a value
// or its error. A member the poll skips (see Plan) holds the answer the Connect read.
struct Frame {
    Answer<bool> slewing;
    Answer<double> azimuth;
    Answer<double> altitude;
    Answer<bool> at_home;
    Answer<bool> at_park;
    Answer<int> shutter_status;
    Answer<bool> slaved;
};

using Proxy = detail::UpstreamProxy<Frame>;

// Which frame members the poll measures. Slewing always; each other one unless the Connect read its capability as
// false (CanSetAzimuth, CanSetAltitude, CanFindHome, CanPark, CanSetShutter, CanSlave): a dome with no azimuth control
// has no Azimuth to follow, and one that cannot slave reads Slaved false for ever. A capability that answered an error
// keeps its member polled.
struct Plan {
    bool azimuth = true;
    bool altitude = true;
    bool at_home = true;
    bool at_park = true;
    bool shutter = true;
    bool slaved = true;
};

// What the Connect reads once: the eight capabilities, each a value or its error, which members to poll, and the one
// answer of each member the poll skips (served unchanged, a value or the upstream's NotImplemented).
struct Limits {
    Answer<bool> can_find_home;
    Answer<bool> can_park;
    Answer<bool> can_set_altitude;
    Answer<bool> can_set_azimuth;
    Answer<bool> can_set_park;
    Answer<bool> can_set_shutter;
    Answer<bool> can_slave;
    Answer<bool> can_sync_azimuth;
    Plan plan;
    Frame fixed;
};

// InvalidValue before NotConnected (ascom-contract.instructions.md): NaN and infinity are invalid whatever the dome
// is. The range is the upstream's to judge: a finite angle goes to it as given, and its InvalidValue passes through.
void validate_angle(double degrees, const char* what) {
    if (!std::isfinite(degrees)) {
        throw AlpacaException(std::string("Dome ") + what + " must be a finite number of degrees",
                              AlpacaError::InvalidValue);
    }
}

bool polled_unless_false(const Answer<bool>& capability) { return !capability.value || *capability.value; }

class RemoteAlpacaDomeDriver final : public DomeDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaDomeDriver(int device_number, const RemoteDeviceConfig& config, std::shared_ptr<util::HttpClient> http,
                           ServerAddressSource self, util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          // IDomeV3 is the first Dome with Connect; an older upstream gets Connected=true.
          proxy_({"dome", "Dome", 3, [this] { return sample(); }}, config, std::move(http), std::move(self), clock) {}

    ~RemoteAlpacaDomeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::Dome; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_DOME_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca Dome Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }  // IDomeV3 (Platform 7)

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
            return;
        }
        // Under transition_mutex_, so no other set_connected() sees the flag; cleared on every way out. From here no
        // command body but AbortSlew's sends its PUT (checked_put()).
        struct ClearDisconnecting {
            std::atomic<bool>& flag;
            ~ClearDisconnecting() { flag.store(false); }
        } clear_disconnecting{disconnecting_};
        disconnecting_.store(true);
        if (disconnect_hook_) disconnect_hook_();
        proxy_.disconnect();
    }

    // The capabilities answer while disconnected (the contract sweep's Can getters): then each is what the last
    // Connect read, and false before any Connect read it (a dome this server has not reached can do nothing).
    // Connected, the upstream's answer or its error, which keeps answering while the link is faulted (static
    // metadata).
    bool get_can_find_home() const override { return capability(limits_.can_find_home); }
    bool get_can_park() const override { return capability(limits_.can_park); }
    bool get_can_set_altitude() const override { return capability(limits_.can_set_altitude); }
    bool get_can_set_azimuth() const override { return capability(limits_.can_set_azimuth); }
    bool get_can_set_park() const override { return capability(limits_.can_set_park); }
    bool get_can_set_shutter() const override { return capability(limits_.can_set_shutter); }
    bool get_can_slave() const override { return capability(limits_.can_slave); }
    bool get_can_sync_azimuth() const override { return capability(limits_.can_sync_azimuth); }

    // From the frame (a stale frame is served), or the upstream's error unchanged; a latched fault refuses the frame
    // with DriverException (decision 0009 rule 1).
    bool get_slewing() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.slewing.get(); });
    }
    double get_azimuth() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.azimuth.get(); });
    }
    double get_altitude() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.altitude.get(); });
    }
    bool get_at_home() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.at_home.get(); });
    }
    bool get_at_park() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.at_park.get(); });
    }
    int get_shutter_status() const override {
        return proxy_.with_connected(
            [](const Proxy::State& state) { return state.served().value.shutter_status.get(); });
    }
    bool get_slaved() const override {
        return proxy_.with_connected([](const Proxy::State& state) { return state.served().value.slaved.get(); });
    }

    // Forwarded; refused while the link is faulted (decision 0009 rule 1). Written through when the poll follows
    // Slaved.
    void set_slaved(bool slaved) override {
        require_frame();
        run([this, slaved](util::OperationContext& ctx) {
            checked_put("slaved", {{"Slaved", slaved ? "True" : "False"}});
            write_through(ctx, [slaved](const Plan& plan, Frame& frame) {
                if (plan.slaved) frame.slaved = {slaved, 0, {}};
            });
        });
    }

    // Forwarded even while the link is faulted: the safety stop is refused only for a lost link (NotConnected). It
    // writes nothing: the dome may coast, so Slewing stays as written until a poll after the stop reads it. Sent
    // while a Disconnect runs too: a stop is always safe.
    void abort_slew() override {
        proxy_.require_connected();
        run([this](util::OperationContext&) {
            if (command_hook_) command_hook_();
            proxy_.client().put("abortslew");
        });
    }

    // The shutter commands write the transition through, never the end state: the frame's ShutterStatus can be a poll
    // period old, so it cannot say the shutter is already there. Refused while the link is faulted.
    void open_shutter() override { start_shutter("openshutter", kShutterOpening); }
    void close_shutter() override { start_shutter("closeshutter", kShutterClosing); }

    // The motions: Slewing is written through true, whatever the frame says (it can be a poll period old).
    void find_home() override {
        require_frame();
        start_motion("findhome", {});
    }

    void park() override {
        require_frame();
        start_motion("park", {});
    }

    void slew_to_azimuth(double azimuth) override {
        validate_angle(azimuth, "azimuth");
        require_frame();
        start_motion("slewtoazimuth", {{"Azimuth", number_text(azimuth)}});
    }

    void slew_to_altitude(double altitude) override {
        validate_angle(altitude, "altitude");
        require_frame();
        start_motion("slewtoaltitude", {{"Altitude", number_text(altitude)}});
    }

    // The upstream's park position becomes where it is now; nothing in the frame changes.
    void set_park() override {
        require_frame();
        run([this](util::OperationContext&) { checked_put("setpark", {}); });
    }

    // The upstream keeps the new azimuth; when the poll follows Azimuth, the upstream's Azimuth is read back and
    // written through (the upstream, not this server, says what the synced angle reads as). A read with no usable
    // reply leaves the frame to the next poll and does not fail the sync the upstream took.
    void sync_to_azimuth(double azimuth) override {
        validate_angle(azimuth, "azimuth");
        require_frame();
        run([this, azimuth](util::OperationContext& ctx) {
            checked_put("synctoazimuth", {{"Azimuth", number_text(azimuth)}});
            if (!proxy_.with_state([this](const Proxy::State&) { return limits_.plan.azimuth; })) return;
            Answer<double> read;
            try {
                read = answer_of([this] { return proxy_.client().get_double("azimuth"); });
            } catch (const RemoteTransportError&) {  // the next poll reads it
                return;
            }
            write_through(ctx, [&read](const Plan&, Frame& frame) { frame.azimuth = read; });
        });
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; a command body calls it first, before its PUT.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    // Set while no command runs; a caller that waits for an earlier command calls it first.
    void set_command_waiting_hook(std::function<void()> hook) { proxy_.set_command_waiting_hook(std::move(hook)); }

    // Set while no Disconnect runs; a Disconnect calls it once no command can send, before it joins the slot.
    void set_disconnect_hook(std::function<void()> hook) { disconnect_hook_ = std::move(hook); }

protected:
    // DeviceState's TimeStamp is when the served frame was measured, not the request time (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        return proxy_.with_state([](const util::PolledState<Frame>& state) { return state.age(); });
    }

private:
    // One state-lock hold picks the disconnected or the connected answer, so a racing Disconnect never makes the read
    // throw NotConnected.
    // `answer` is a member of limits_, read only under that hold.
    bool capability(const Answer<bool>& answer) const {
        return proxy_.with_state([this, &answer](const Proxy::State&) {
            if (!proxy_.connected()) return answer.value.value_or(false);
            return answer.get();
        });
    }

    // A command that needs the link: NotConnected, or refused while the link is faulted (decision 0009 rule 1).
    void require_frame() const {
        (void)proxy_.with_connected([](const Proxy::State& state) { return state.served().measured_at; });
    }

    // A slew, FindHome or Park: forwarded in the command slot, then Slewing written through true, so a client reads it
    // true at once. Azimuth, Altitude, AtHome and AtPark stay the measured ones until the next poll.
    void start_motion(const char* member, RemoteParams params) {
        run([this, member, params = std::move(params)](util::OperationContext& ctx) {
            in_progress_put(ctx, member, params, [](const Plan&, Frame& frame) { frame.slewing = {true, 0, {}}; });
        });
    }

    void start_shutter(const char* member, int transition) {
        require_frame();
        run([this, member, transition](util::OperationContext& ctx) {
            in_progress_put(ctx, member, {}, [transition](const Plan& plan, Frame& frame) {
                if (plan.shutter) frame.shutter_status = {transition, 0, {}};
            });
        });
    }

    // A motion or shutter PUT, then its in-progress state written through. A PUT with no usable reply writes it too
    // before the error reaches the caller: the upstream may have started the motion, and the frame may say it is
    // still. An upstream error answer (it refused) writes nothing.
    template <class Apply>
    void in_progress_put(util::OperationContext& ctx, const char* member, const RemoteParams& params, Apply apply) {
        try {
            checked_put(member, params);
        } catch (const RemoteTransportError&) {
            write_through(ctx, apply);
            throw;
        }
        write_through(ctx, apply);
    }

    // Every command PUT but AbortSlew's, in its body: NotConnected once a Disconnect has begun or ended, so a body
    // queued behind another, or one whose caller passed its check before the Disconnect, sends nothing after it.
    void checked_put(const char* member, const RemoteParams& params) {
        if (command_hook_) command_hook_();
        if (disconnecting_.load()) {
            throw AlpacaException("Device not connected: a Disconnect is in progress", AlpacaError::NotConnected);
        }
        proxy_.require_connected();  // after a Disconnect or a loss; the text says which
        proxy_.client().put(member, params);
    }

    void run(std::function<void(util::OperationContext&)> body) { proxy_.run_command(std::move(body)); }

    // Write-through (decision 0010 rule 2): the next read sees the command before the next poll. A superseded body
    // writes nothing (decision 0006 rule 6): the newer command's state stands. Nor does a body whose caller passed its
    // check before a Disconnect: with no connection there is no frame, and the write's epoch bump would drop the next
    // Connect's first frame (decision 0010 rule 8, #2446). `apply` sees which members the poll follows.
    template <class Apply>
    void write_through(util::OperationContext& ctx, Apply apply) {
        proxy_.with_state([this, &ctx, &apply](Proxy::State& state) {
            if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
            const Plan plan = limits_.plan;
            state.write([&apply, &plan](Frame& frame) { apply(plan, frame); });
        });
    }

    // One measurement of the upstream, with no lock held across a call (decision 0016 rule 1): Slewing, and each
    // member the plan follows, so a poll makes one to seven calls. Throws RemoteTransportError when a call gets no
    // usable reply; an upstream error is kept in the frame. A member the plan skips is the Connect's answer.
    Frame sample() {
        const Limits limits = proxy_.with_state([this](const Proxy::State&) { return limits_; });
        const Plan& plan = limits.plan;
        RemoteAlpacaClient& client = proxy_.client();
        Frame frame = limits.fixed;
        frame.slewing = answer_of([&] { return client.get_bool("slewing"); });
        if (plan.azimuth) frame.azimuth = answer_of([&] { return client.get_double("azimuth"); });
        if (plan.altitude) frame.altitude = answer_of([&] { return client.get_double("altitude"); });
        if (plan.at_home) frame.at_home = answer_of([&] { return client.get_bool("athome"); });
        if (plan.at_park) frame.at_park = answer_of([&] { return client.get_bool("atpark"); });
        if (plan.shutter) frame.shutter_status = answer_of([&] { return client.get_int("shutterstatus"); });
        if (plan.slaved) frame.slaved = answer_of([&] { return client.get_bool("slaved"); });
        return frame;
    }

    // At Connect, with no lock held, after the upstream's identity: the eight capabilities, then once each member the
    // poll will skip, read into read_limits_, and what to store under the state lock with the identity. The store
    // captures only `this`, so the std::function holds it with no allocation.
    std::function<void()> read_limits() {
        RemoteAlpacaClient& client = proxy_.client();
        Limits& l = read_limits_;
        l.can_find_home = answer_of([&] { return client.get_bool("canfindhome"); });
        l.can_park = answer_of([&] { return client.get_bool("canpark"); });
        l.can_set_altitude = answer_of([&] { return client.get_bool("cansetaltitude"); });
        l.can_set_azimuth = answer_of([&] { return client.get_bool("cansetazimuth"); });
        l.can_set_park = answer_of([&] { return client.get_bool("cansetpark"); });
        l.can_set_shutter = answer_of([&] { return client.get_bool("cansetshutter"); });
        l.can_slave = answer_of([&] { return client.get_bool("canslave"); });
        l.can_sync_azimuth = answer_of([&] { return client.get_bool("cansyncazimuth"); });
        l.plan = Plan{polled_unless_false(l.can_set_azimuth), polled_unless_false(l.can_set_altitude),
                      polled_unless_false(l.can_find_home),   polled_unless_false(l.can_park),
                      polled_unless_false(l.can_set_shutter), polled_unless_false(l.can_slave)};
        l.fixed = Frame{};
        if (!l.plan.azimuth) l.fixed.azimuth = answer_of([&] { return client.get_double("azimuth"); });
        if (!l.plan.altitude) l.fixed.altitude = answer_of([&] { return client.get_double("altitude"); });
        if (!l.plan.at_home) l.fixed.at_home = answer_of([&] { return client.get_bool("athome"); });
        if (!l.plan.at_park) l.fixed.at_park = answer_of([&] { return client.get_bool("atpark"); });
        if (!l.plan.shutter) l.fixed.shutter_status = answer_of([&] { return client.get_int("shutterstatus"); });
        if (!l.plan.slaved) l.fixed.slaved = answer_of([&] { return client.get_bool("slaved"); });
        return [this] { limits_ = read_limits_; };
    }

    const int device_number_;

    std::function<void()> command_hook_;     // test seam: set_command_hook_for_testing()
    std::function<void()> disconnect_hook_;  // test seam: set_disconnect_hook_for_testing()
    // Set by set_connected(false) for the length of the Disconnect: no command but AbortSlew sends (checked_put()).
    std::atomic<bool> disconnecting_{false};

    // Under the proxy's state lock (with_connected(), with_state(), the Connect's store, the poll's copy).
    Limits limits_;
    // The Connect's reads before the store, under transition_mutex_ (only the connect path touches it).
    Limits read_limits_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so it is destroyed (its poll and command bodies joined) before the members the bodies touch die.
    Proxy proxy_;
};

}  // namespace

std::unique_ptr<DomeDriver> create_remotealpaca_dome(int device_number, const RemoteDeviceConfig& config,
                                                     ServerAddressSource self) {
    return create_remotealpaca_dome(device_number, config, util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"),
                                    std::move(self), util::default_task_clock());
}

std::unique_ptr<DomeDriver> create_remotealpaca_dome(int device_number, const RemoteDeviceConfig& config,
                                                     std::shared_ptr<util::HttpClient> http, ServerAddressSource self,
                                                     util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaDomeDriver>(device_number, config, std::move(http), std::move(self), clock);
}

void set_poll_paused_for_testing(DomeDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaDomeDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(DomeDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaDomeDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

void set_command_waiting_hook_for_testing(DomeDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaDomeDriver*>(&driver)) {
        remote->set_command_waiting_hook(std::move(hook));
    }
}

void set_disconnect_hook_for_testing(DomeDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaDomeDriver*>(&driver)) {
        remote->set_disconnect_hook(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::remotealpaca
