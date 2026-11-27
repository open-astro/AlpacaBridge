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

// The remotealpaca FilterWheel proxied device (decision 0023, issue #1753 slice 5, #2706) over UpstreamProxy
// (upstream_proxy.h), which holds every rule it shares with the other proxied devices. What is the FilterWheel's own
// stays here: the frame (Position, -1 while the upstream's wheel moves), the slot data the Connect reads (Names and
// FocusOffsets, kept in util::FilterWheelSlots as every wheel keeps them), and the one command, the Position write.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/filterwheel_slots.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_filterwheel_driver.h>
#include <alpacacore/version.h>

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

// The polled frame (decision 0010): the upstream's Position, a value (-1 while it moves) or its error.
struct Frame {
    Answer<int> position;
};

using Proxy = detail::UpstreamProxy<Frame>;

class RemoteAlpacaFilterWheelDriver final : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaFilterWheelDriver(int device_number, const RemoteDeviceConfig& config,
                                  std::shared_ptr<util::HttpClient> http, ServerAddressSource self,
                                  util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          // IFilterWheelV3 is the first FilterWheel with Connect; an older upstream gets Connected=true.
          proxy_({"filterwheel", "FilterWheel", 3, [this] { return sample(); }}, config, std::move(http),
                 std::move(self), clock) {}

    ~RemoteAlpacaFilterWheelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_FW_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca FilterWheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }  // IFilterWheelV3 (Platform 7)

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
            try {
                proxy_.connect([this] { return read_slots(); });
            } catch (...) {
                // A Connect that failed after its store leaves no slot count behind: a disconnected wheel takes any
                // length (filterwheel_slots.h).
                proxy_.with_state([this](Proxy::State&) { slots_.clear_slot_count(); });
                throw;
            }
        } else {
            proxy_.disconnect();
            // The count is unknown again until the next Connect; names and offsets stay.
            proxy_.with_state([this](Proxy::State&) { slots_.clear_slot_count(); });
        }
    }

    // From the frame (a stale frame is served): the upstream's slot, -1 while its wheel moves, or its error unchanged.
    // A latched fault refuses the frame with DriverException (decision 0009 rule 1).
    int get_position() const override {
        return proxy_.with_connected(
            [](const util::PolledState<Frame>& state) { return state.served().value.position.get(); });
    }

    // A negative slot is InvalidValue before the connection check (ASCOM precedence); a slot past the upstream's
    // count is InvalidValue once connected. Refused while the link is faulted (decision 0009 rule 1). The body
    // forwards the write and writes Position -1 through, so a client reads the move at once, even to the slot the
    // last poll saw (another client may have turned the wheel since); the next poll reports where it landed.
    void set_position(int position) override {
        if (position < 0) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        // served() refuses a latched fault before the slot count is read.
        const std::optional<int> slots = proxy_.with_connected([this](const Proxy::State& state) {
            (void)state.served();
            return slots_.slot_count();
        });
        if (!slots || position >= *slots) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        proxy_.run_command([this, position](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            proxy_.client().put("position", {{"Position", std::to_string(position)}});
            write_through(ctx, [](Frame& frame) { frame.position = {-1, 0, {}}; });
        });
    }

    // Slot data: the upstream's, read at Connect, and a client's write on top (an AlpacaBridge extension the upstream
    // never sees). Readable and writable while disconnected, as on every wheel; while connected each takes one entry
    // per upstream slot (util::FilterWheelSlots).
    std::vector<int> get_focus_offsets() const override {
        return proxy_.with_state([this](const Proxy::State&) { return slots_.offsets(); });
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        proxy_.with_state([this, &offsets](Proxy::State&) { slots_.set_offsets(offsets); });
    }

    std::vector<std::string> get_names() const override {
        return proxy_.with_state([this](const Proxy::State&) { return slots_.names(); });
    }

    void set_names(const std::vector<std::string>& names) override {
        proxy_.with_state([this, &names](Proxy::State&) { slots_.set_names(names); });
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; a Position write body calls it before its PUT.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    void set_command_waiting_hook(std::function<void()> hook) { proxy_.set_command_waiting_hook(std::move(hook)); }

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

    // One measurement of the upstream, with no lock held (decision 0016 rule 1). Throws RemoteTransportError when the
    // call gets no usable reply; an upstream error is kept in the frame.
    Frame sample() {
        Frame frame;
        frame.position = answer_of([this] { return proxy_.client().get_int("position"); });
        return frame;
    }

    // At Connect, with no lock held, after the upstream's identity: its Names and FocusOffsets, read once into
    // read_names_ and read_offsets_, and what to store under the state lock with the identity. Every IFilterWheel has
    // both, so an upstream error on either fails the Connect with that error unchanged, as does a wheel with no
    // slot: there is no slot count to check a Position write against. The store captures only `this`, so the
    // std::function holds it with no allocation (a capture of the vectors trips clang-analyzer NewDeleteLeaks in
    // UpstreamProxy::connect()).
    std::function<void()> read_slots() {
        RemoteAlpacaClient& client = proxy_.client();
        read_names_ = client.get_string_list("names");
        read_offsets_ = client.get_int_list("focusoffsets");
        if (read_names_.empty()) {
            throw AlpacaException("Upstream " + proxy_.config().host + ":" + std::to_string(proxy_.config().port) +
                                      " reports a filter wheel with no slots",
                                  AlpacaError::DriverException);
        }
        return [this] { store_slots(); };
    }

    // Under the state lock. The upstream's slot data replaces what this server held (there is no local config to
    // keep: the upstream's lineup is the wheel's), and its slot count is published, so a write while connected must
    // match it (util::FilterWheelSlots). The reset drops the count a lost link kept, so a relink to a wheel whose
    // count changed takes the new lineup instead of refusing it.
    void store_slots() {
        const int count = static_cast<int>(read_names_.size());
        slots_ = util::FilterWheelSlots{};
        slots_.set_names(std::move(read_names_));
        slots_.set_offsets(std::move(read_offsets_));
        slots_.publish_slot_count(count, detail::kComponent);
    }

    const int device_number_;

    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()

    // Under the proxy's state lock (with_connected(), with_state(), the Connect's store).
    util::FilterWheelSlots slots_;
    // The Connect's reads before the store, under transition_mutex_ (only the connect path touches them).
    std::vector<std::string> read_names_;
    std::vector<int> read_offsets_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so it is destroyed (its poll and command bodies joined) before the members the bodies touch die.
    Proxy proxy_;
};

}  // namespace

std::unique_ptr<FilterWheelDriver> create_remotealpaca_filterwheel(int device_number, const RemoteDeviceConfig& config,
                                                                   ServerAddressSource self) {
    return create_remotealpaca_filterwheel(device_number, config,
                                           util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"),
                                           std::move(self), util::default_task_clock());
}

std::unique_ptr<FilterWheelDriver> create_remotealpaca_filterwheel(int device_number, const RemoteDeviceConfig& config,
                                                                   std::shared_ptr<util::HttpClient> http,
                                                                   ServerAddressSource self, util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaFilterWheelDriver>(device_number, config, std::move(http), std::move(self),
                                                           clock);
}

void set_poll_paused_for_testing(FilterWheelDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaFilterWheelDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(FilterWheelDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaFilterWheelDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

void set_command_waiting_hook_for_testing(FilterWheelDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaFilterWheelDriver*>(&driver)) {
        remote->set_command_waiting_hook(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::remotealpaca
