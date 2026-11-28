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

// The remotealpaca Switch proxied device (decision 0023, issue #1753 slice 5, #2709) over UpstreamProxy
// (upstream_proxy.h), which holds every rule it shares with the other proxied devices. What is the Switch's own stays
// here: the frame (GetSwitch, GetSwitchValue and, for an asynchronous switch, StateChangeComplete, per switch id; a
// two-state switch's GetSwitch derived from its value, #2796), what the Connect reads once (MaxSwitch and each switch's
// name, description, range, step, CanWrite and CanAsync), and the writes.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_switch_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "upstream_proxy.h"

namespace alpacacore::vendor::remotealpaca {

namespace {

using detail::Answer;
using detail::answer_of;
using detail::number_text;

// ISwitchV3 is the first Switch with Connect and the asynchronous members (CanAsync, SetAsync, SetAsyncValue,
// StateChangeComplete, CancelAsync).
constexpr int kSwitchV3 = 3;

// One switch in the polled frame (decision 0010): what the upstream answered, a value or its error. `complete` is
// measured only for a switch whose CanAsync the Connect read true; any other switch's change is complete when its
// write returns.
struct SwitchReading {
    Answer<bool> on;
    Answer<double> value;
    Answer<bool> complete;
};

// The readings by switch id, MaxSwitch of them, shared and immutable: the module hands a getter a copy of the frame
// (PolledState::served()), and a copy of a pointer keeps a single-switch read, and so DeviceState over 64 switches,
// from copying every switch. A write-through copies the readings once and replaces them.
struct Frame {
    std::shared_ptr<const std::vector<SwitchReading>> switches = std::make_shared<const std::vector<SwitchReading>>();

    std::size_t size() const { return switches->size(); }
    const SwitchReading& at(std::size_t id) const { return (*switches)[id]; }

    // Write-through: `apply` changes switch `id`'s reading in a fresh copy of the readings, which then replaces them.
    // Nothing for an id the frame lacks.
    template <class Apply>
    void edit(int id, Apply apply) {
        const auto at = static_cast<std::size_t>(id);
        if (at >= switches->size()) return;
        auto copy = std::make_shared<std::vector<SwitchReading>>(*switches);
        apply((*copy)[at]);
        switches = std::move(copy);
    }
};

using Proxy = detail::UpstreamProxy<Frame>;

// What the Connect reads once for one switch, each a value or the upstream's error, served unchanged.
struct Info {
    Answer<std::string> name;
    Answer<std::string> description;
    Answer<double> min;
    Answer<double> max;
    Answer<double> step;
    Answer<bool> can_write;
    Answer<bool> can_async;  // false, not asked, for an upstream older than ISwitchV3

    bool polls_complete() const { return can_async.value.value_or(false); }

    // Two states only: MaxSwitchValue one step above MinSwitchValue, all three read at Connect. ASCOM defines on as
    // MaxSwitchValue and off as MinSwitchValue (SetSwitch), so such a switch's GetSwitch is exactly its value at the
    // maximum. A multi-state switch's GetSwitch between its ends is the upstream's choice, and the poll asks it.
    bool two_state() const {
        return min.value && max.value && step.value && *max.value > *min.value &&
               *step.value == *max.value - *min.value;
    }
};

// What the poll measures of one switch, from what the Connect read.
struct SwitchPlan {
    bool complete = false;   // StateChangeComplete: CanAsync read true
    bool two_state = false;  // Info::two_state()
    double min = 0.0;
    double max = 0.0;
};

// Where a switch that takes `value` puts it: the nearest step from the minimum, within the range (a device rounds a
// value between steps: step 10, value 54 reads 50). Unchanged when the range or the step is unknown, or the step is not
// positive.
double snapped(const Info& i, double value) {
    if (!i.min.value || !i.max.value || !i.step.value || !(*i.step.value > 0.0)) return value;
    const double min = *i.min.value;
    const double step = *i.step.value;
    return std::clamp(min + std::round((value - min) / step) * step, min, *i.max.value);
}

struct Limits {
    int max_switch = 0;
    bool async_members = false;  // the upstream is ISwitchV3 or later
    std::vector<Info> switches;  // by switch id
};

RemoteParams id_param(int id) { return {{"Id", std::to_string(id)}}; }

// Before the connection check: no switch has a negative id (ASCOM contract precedence, InvalidValue first).
void check_static_id(int id) {
    if (id < 0) {
        throw AlpacaException("Switch id " + std::to_string(id) + " is out of range", AlpacaError::InvalidValue);
    }
}

class RemoteAlpacaSwitchDriver final : public SwitchDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaSwitchDriver(int device_number, const RemoteDeviceConfig& config,
                             std::shared_ptr<util::HttpClient> http, ServerAddressSource self, util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          proxy_({"switch", "Switch", kSwitchV3, [this] { return sample(); }}, config, std::move(http), std::move(self),
                 clock) {}

    ~RemoteAlpacaSwitchDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_SW_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca Switch Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }  // ISwitchV3 (Platform 7)

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

    // --- What the Connect read: static metadata, answered while the link is faulted ---

    // The upstream's count, read at Connect, so NotConnected while disconnected.
    int get_max_switch() const override {
        return proxy_.with_connected([this](const Proxy::State&) { return limits_.max_switch; });
    }

    bool get_can_write(int id) const override {
        return info(id, [](const Info& i) { return i.can_write.get(); });
    }

    // An upstream older than ISwitchV3 has no asynchronous switch control: false, not asked.
    bool get_can_async(int id) const override {
        return info(id, [](const Info& i) { return i.can_async.get(); });
    }

    std::string get_switch_name(int id) const override {
        return info(id, [](const Info& i) { return i.name.get(); });
    }

    std::string get_switch_description(int id) const override {
        return info(id, [](const Info& i) { return i.description.get(); });
    }

    double get_min_switch_value(int id) const override {
        return info(id, [](const Info& i) { return i.min.get(); });
    }

    double get_max_switch_value(int id) const override {
        return info(id, [](const Info& i) { return i.max.get(); });
    }

    double get_switch_step(int id) const override {
        return info(id, [](const Info& i) { return i.step.get(); });
    }

    // --- The polled frame: a stale frame is served, a latched fault refuses it (decision 0009 rule 1) ---

    bool get_switch(int id) const override {
        return reading(id, [](const Info&, const SwitchReading& r) { return r.on.get(); });
    }

    double get_switch_value(int id) const override {
        return reading(id, [](const Info&, const SwitchReading& r) { return r.value.get(); });
    }

    // A switch that is not asynchronous changes within its write, so its change is complete once the write returned.
    bool get_state_change_complete(int id) const override {
        return reading(
            id, [](const Info& i, const SwitchReading& r) { return i.polls_complete() ? r.complete.get() : true; });
    }

    // --- The writes: forwarded in the command slot, refused while the link is faulted (decision 0009 rule 1) ---

    // Forwarded; the upstream decides whether the switch can be written, and its refusal passes through unchanged.
    // Written through: GetSwitch and GetSwitchValue (the range's end) read the write before the next poll.
    void set_switch(int id, bool state) override {
        const Info i = writable_info(id);
        proxy_.run_command([this, id, state, i](util::OperationContext& ctx) {
            RemoteParams params = id_param(id);
            params.emplace_back("State", state ? "True" : "False");
            if (command_hook_) command_hook_();
            proxy_.client().put("setswitch", params);
            write_through(ctx, [id, state, &i](Frame& frame) {
                frame.edit(id, [state, &i](SwitchReading& r) {
                    r.on = {state, 0, {}};
                    const auto& end = state ? i.max.value : i.min.value;
                    if (end) r.value = {end, 0, {}};
                });
            });
        });
    }

    // A non-finite value is InvalidValue before the connection check; a value outside the range the Connect read is
    // InvalidValue before the fault check. Written through: GetSwitchValue on the switch's step (snapped()), and
    // GetSwitch (true above the minimum).
    void set_switch_value(int id, double value) override {
        check_value_static(id, value);
        const Info i = writable_info(id, value);
        proxy_.run_command([this, id, value, i](util::OperationContext& ctx) {
            RemoteParams params = id_param(id);
            params.emplace_back("Value", number_text(value));
            if (command_hook_) command_hook_();
            proxy_.client().put("setswitchvalue", params);
            // The value the device takes, on its step (snapped()), not the value as sent.
            const double taken = snapped(i, value);
            write_through(ctx, [id, taken, &i](Frame& frame) {
                frame.edit(id, [taken, &i](SwitchReading& r) {
                    r.value = {taken, 0, {}};
                    if (i.min.value) r.on = {taken > *i.min.value, 0, {}};
                });
            });
        });
    }

    // Forwarded to an ISwitchV3 upstream; the change runs there, and the poll measures its StateChangeComplete. Written
    // through: StateChangeComplete reads false at once for an asynchronous switch. MethodNotImplemented for an older
    // upstream, which has no SetAsync.
    void set_async(int id, bool state) override {
        const Info i = writable_info(id, std::nullopt, true);
        proxy_.run_command([this, id, state, i](util::OperationContext& ctx) {
            RemoteParams params = id_param(id);
            params.emplace_back("State", state ? "True" : "False");
            if (command_hook_) command_hook_();
            proxy_.client().put("setasync", params);
            started_through(ctx, id, i);
        });
    }

    void set_async_value(int id, double value) override {
        check_value_static(id, value);
        const Info i = writable_info(id, value, true);
        proxy_.run_command([this, id, value, i](util::OperationContext& ctx) {
            RemoteParams params = id_param(id);
            params.emplace_back("Value", number_text(value));
            if (command_hook_) command_hook_();
            proxy_.client().put("setasyncvalue", params);
            started_through(ctx, id, i);
        });
    }

    // Forwarded; an upstream whose names are fixed refuses it with its own error, unchanged. Written through: the name
    // the Connect read is replaced on success.
    void set_switch_name(int id, const std::string& name) override {
        (void)writable_info(id);
        proxy_.run_command([this, id, name](util::OperationContext& ctx) {
            RemoteParams params = id_param(id);
            params.emplace_back("Name", name);
            if (command_hook_) command_hook_();
            proxy_.client().put("setswitchname", params);
            proxy_.with_state([this, &ctx, id, &name](Proxy::State&) {
                if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
                if (static_cast<std::size_t>(id) < limits_.switches.size()) {
                    limits_.switches[static_cast<std::size_t>(id)].name = {name, 0, {}};
                }
            });
        });
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; a command body calls it before its PUT.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    void set_command_waiting_hook(std::function<void()> hook) { proxy_.set_command_waiting_hook(std::move(hook)); }

protected:
    // DeviceState's TimeStamp is when the served frame was measured, not the request time (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        return proxy_.with_state([](const util::PolledState<Frame>& state) { return state.age(); });
    }

private:
    // Under the state lock, with Connected checked: an id the upstream does not have is InvalidValue.
    void check_id_locked(int id) const {
        if (id >= limits_.max_switch) {
            throw AlpacaException("Switch id " + std::to_string(id) + " is out of range (MaxSwitch is " +
                                      std::to_string(limits_.max_switch) + ")",
                                  AlpacaError::InvalidValue);
        }
    }

    // A value no switch can take is InvalidValue before the connection check.
    static void check_value_static(int id, double value) {
        check_static_id(id);
        if (!std::isfinite(value)) {
            throw AlpacaException("Switch value must be a finite number", AlpacaError::InvalidValue);
        }
    }

    // What the Connect read for switch `id`: the static id check, NotConnected, then the connected upstream's count.
    template <class Fn>
    auto info(int id, Fn&& fn) const -> std::invoke_result_t<Fn&, const Info&> {
        check_static_id(id);
        return proxy_.with_connected([this, id, &fn](const Proxy::State&) {
            check_id_locked(id);
            return fn(limits_.switches[static_cast<std::size_t>(id)]);
        });
    }

    // The frame's reading of switch `id`, after the id checks: a latched fault refuses it with DriverException.
    template <class Fn>
    auto reading(int id, Fn&& fn) const -> std::invoke_result_t<Fn&, const Info&, const SwitchReading&> {
        check_static_id(id);
        return proxy_.with_connected([this, id, &fn](const Proxy::State& state) {
            check_id_locked(id);
            const auto served = state.served();
            const auto at = static_cast<std::size_t>(id);
            if (at >= served.value.size()) {
                // A frame of another Connect: cannot happen while connected_ holds (a Connect resets the frame).
                throw AlpacaException("Device not connected", AlpacaError::NotConnected);
            }
            return fn(limits_.switches[at], served.value.at(at));
        });
    }

    // The order every write refuses in: the id, Connected, the id against MaxSwitch, the value against the range the
    // Connect read (when given and known), an asynchronous write to an upstream without the asynchronous members
    // (older than ISwitchV3: MethodNotImplemented, decision 0023), then a latched fault (decision 0009 rule 1). Returns
    // the switch's Info.
    Info writable_info(int id, std::optional<double> value = std::nullopt, bool async = false) const {
        check_static_id(id);
        return proxy_.with_connected([this, id, value, async](const Proxy::State& state) {
            check_id_locked(id);
            const Info& i = limits_.switches[static_cast<std::size_t>(id)];
            if (value && i.min.value && i.max.value && (*value < *i.min.value || *value > *i.max.value)) {
                throw AlpacaException("Switch " + std::to_string(id) + " value " + number_text(*value) +
                                          " is out of range (" + number_text(*i.min.value) + " to " +
                                          number_text(*i.max.value) + ")",
                                      AlpacaError::InvalidValue);
            }
            if (async && !limits_.async_members) {
                throw AlpacaException(
                    "The upstream Switch is older than ISwitchV3 and has no asynchronous switch control",
                    AlpacaError::MethodNotImplemented);
            }
            (void)state.served();
            return i;
        });
    }

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

    // An asynchronous change the upstream took: StateChangeComplete reads false until a poll measures it done. A
    // switch the poll does not measure it for (CanAsync false) writes nothing: the upstream refused or finished it.
    void started_through(util::OperationContext& ctx, int id, const Info& i) {
        if (!i.polls_complete()) return;
        write_through(ctx,
                      [id](Frame& frame) { frame.edit(id, [](SwitchReading& r) { r.complete = {false, 0, {}}; }); });
    }

    // One measurement of the upstream, with no driver lock held while it calls (decision 0016 rule 1): per switch
    // GetSwitchValue, GetSwitch unless it derives from the value (a two-state switch at either end of its range,
    // #2796), and StateChangeComplete for an asynchronous one. Throws RemoteTransportError when a call gets no usable
    // reply; an upstream error is kept in the frame. What to measure is what the last Connect read, copied under the
    // state lock (only a Connect changes it, after it joined the poll). A poll longer than the poll period logs one
    // WARN per Connect.
    Frame sample() {
        const std::vector<SwitchPlan> plan = proxy_.with_state([this](const Proxy::State&) {
            std::vector<SwitchPlan> out;
            out.reserve(limits_.switches.size());
            for (const Info& i : limits_.switches) {
                out.push_back(
                    {i.polls_complete(), i.two_state(), i.min.value.value_or(0.0), i.max.value.value_or(0.0)});
            }
            return out;
        });
        util::TaskClock& clock = proxy_.clock();
        const auto started = clock.now();
        RemoteAlpacaClient& client = proxy_.client();
        auto readings = std::make_shared<std::vector<SwitchReading>>(plan.size());
        for (std::size_t at = 0; at < plan.size(); ++at) {
            const SwitchPlan& p = plan[at];
            const RemoteParams params = id_param(static_cast<int>(at));
            SwitchReading& r = (*readings)[at];
            r.value = answer_of([&] { return client.get_double("getswitchvalue", params); });
            const std::optional<double>& v = r.value.value;
            if (p.two_state && v && (*v == p.min || *v == p.max)) {
                r.on = {*v == p.max, 0, {}};
            } else {
                r.on = answer_of([&] { return client.get_bool("getswitch", params); });
            }
            if (p.complete) r.complete = answer_of([&] { return client.get_bool("statechangecomplete", params); });
        }
        warn_if_slow(clock.now() - started, plan.size());
        Frame frame;
        frame.switches = std::move(readings);
        return frame;
    }

    // Issue #2796: the poll calls the upstream one switch at a time, so a large bank on a slow link can take longer
    // than the poll period. The poll then skips the ticks it ran over; this says so, once per Connect.
    void warn_if_slow(std::chrono::nanoseconds spent, std::size_t switches) {
        const auto period = proxy_.config().poll_period;
        if (spent <= period || slow_poll_warned_.exchange(true)) return;
        const RemoteDeviceConfig& c = proxy_.config();
        ALPACA_LOG_WARN(detail::kComponent,
                        "Upstream " + c.host + ":" + std::to_string(c.port) + " device " +
                            std::to_string(c.upstream_device_number) + ": one poll of " + std::to_string(switches) +
                            " switches took " +
                            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(spent).count()) +
                            " ms, longer than the " + std::to_string(period.count()) +
                            " s poll period; set a longer poll period");
    }

    // At Connect, with no lock held, after the upstream's identity: MaxSwitch (an error from the upstream, or more than
    // kMaxProxiedSwitches, fails the Connect: there is nothing to index) and each switch's metadata, read once into
    // read_limits_, and what to store under the state lock with the identity, before the first frame is measured.
    std::function<void()> read_limits() {
        RemoteAlpacaClient& client = proxy_.client();
        slow_poll_warned_.store(false);  // the poll is joined: this Connect's polls warn afresh
        const int count = client.get_int("maxswitch");
        if (count < 0 || count > kMaxProxiedSwitches) {
            throw AlpacaException("Upstream MaxSwitch " + std::to_string(count) + " is outside 0 to " +
                                      std::to_string(kMaxProxiedSwitches) + ", the switches one proxied Switch serves",
                                  AlpacaError::DriverException);
        }
        read_limits_.max_switch = count;
        read_limits_.async_members = proxy_.upstream_interface_version() >= kSwitchV3;
        read_limits_.switches.assign(static_cast<std::size_t>(count), Info{});
        for (int id = 0; id < count; ++id) {
            const RemoteParams params = id_param(id);
            Info& i = read_limits_.switches[static_cast<std::size_t>(id)];
            i.name = answer_of([&] { return client.get_string("getswitchname", params); });
            i.description = answer_of([&] { return client.get_string("getswitchdescription", params); });
            i.min = answer_of([&] { return client.get_double("minswitchvalue", params); });
            i.max = answer_of([&] { return client.get_double("maxswitchvalue", params); });
            i.step = answer_of([&] { return client.get_double("switchstep", params); });
            i.can_write = answer_of([&] { return client.get_bool("canwrite", params); });
            i.can_async = read_limits_.async_members ? answer_of([&] { return client.get_bool("canasync", params); })
                                                     : Answer<bool>{false, 0, {}};
        }
        return [this] { limits_ = read_limits_; };
    }

    const int device_number_;

    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()

    // Under the proxy's state lock (with_connected(), with_state(), the Connect's store).
    Limits limits_;
    // The Connect's reads before the store, under transition_mutex_ (only the connect path touches it).
    Limits read_limits_;

    // A poll of this Connect logged the slow-poll WARN (warn_if_slow()); the poll and a Connect write it.
    std::atomic<bool> slow_poll_warned_{false};

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so it is destroyed (its poll and command bodies joined) before the members the bodies touch die.
    Proxy proxy_;
};

}  // namespace

std::unique_ptr<SwitchDriver> create_remotealpaca_switch(int device_number, const RemoteDeviceConfig& config,
                                                         ServerAddressSource self) {
    return create_remotealpaca_switch(device_number, config,
                                      util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"), std::move(self),
                                      util::default_task_clock());
}

std::unique_ptr<SwitchDriver> create_remotealpaca_switch(int device_number, const RemoteDeviceConfig& config,
                                                         std::shared_ptr<util::HttpClient> http,
                                                         ServerAddressSource self, util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaSwitchDriver>(device_number, config, std::move(http), std::move(self), clock);
}

void set_poll_paused_for_testing(SwitchDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaSwitchDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(SwitchDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaSwitchDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

void set_command_waiting_hook_for_testing(SwitchDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaSwitchDriver*>(&driver)) {
        remote->set_command_waiting_hook(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::remotealpaca
