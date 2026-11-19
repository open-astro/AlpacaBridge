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

// The OnStepX auxiliary features (#2222) as one Switch device, over the controller's shared link
// (OnStepProtocolWrapper::acquire(), the iOptron iFR precedent of decision 0017). Commands are :GXY#, :GXX# and :SXX#
// (OnStepX Features.command.cpp, af1d26bf; purpose codes from Constants.h); see
// .github/instructions/onstep.instructions.md.

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_switch_driver.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::onstep {

namespace {

constexpr const char* kLogTag = "OnStep switch";
// The state poll (decision 0010), start to start: one :GXX<n># per included slot, at most eight short exchanges on the
// link the telescope shares. The freshness bound is two periods; the fault latch (three failed polls) and the loss rule
// live in util::PolledState.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(1000);
constexpr int kFaultThreshold = 3;
// OnStepX has eight auxiliary feature slots (:GXY1# to :GXY8#), so no switch id reaches 8 on any controller.
constexpr int kMaxSlots = 8;

// The :GE# codes a feature write gets (OnStepX Features.command.cpp, af1d26bf).
std::string describe_feature_error(int code) {
    switch (code) {
        case 2:
            return "unknown command";
        case 4:
            return "parameter out of range";
        case 5:
            return "bad parameter format";
        default:
            return code < 0 ? std::string("no reason given") : "error code " + std::to_string(code);
    }
}

}  // namespace

namespace detail {

std::optional<AuxSwitchKind> aux_switch_kind(int purpose) {
    switch (purpose) {
        case 1:  // SWITCH (and MOMENTARY_SWITCH, COVER_SWITCH, which :GXY reports as 1)
            return AuxSwitchKind{1.0, "on/off output"};
        case 2:  // ANALOG_OUTPUT: :SXX<n>,V takes 0-255 for 0-100 % power
            return AuxSwitchKind{255.0, "PWM output, 0-255"};
        case 3:  // DEW_HEATER: :SXX<n>,V 0/1 enables or disables its regulation
            return AuxSwitchKind{1.0, "dew heater, enabled or disabled"};
        default:  // 4 INTERVALOMETER, 6 HIDDEN_SWITCH, anything else
            return std::nullopt;
    }
}

}  // namespace detail

class OnStepSwitchDriver : public SwitchDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    OnStepSwitchDriver(int device_number, ConnectionInfo info, util::ConnectionResolver<ConnectionInfo> resolver,
                       util::TaskClock& clock)
        : AsyncConnectable("OnStep"),
          clock_(clock),
          device_number_(device_number),
          connection_info_(std::move(info)),
          connection_resolver_(std::move(resolver)),
          state_(
              clock_, state_poll_generation_, {kStatePollPeriod, kFaultThreshold, 2 * kStatePollPeriod, kLogTag},
              [this] { return probe(); }, [this] { lose_shared_link(); }) {}

    ~OnStepSwitchDriver() override {
        shutdown_connection();
        // The poll body goes before the members it reads.
        state_.stop_and_join();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall): most-derived class
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during OnStep switch destruction: " + std::string(e.what()));
            }
        }
        release_link();
    }

    OnStepSwitchDriver(const OnStepSwitchDriver&) = delete;
    OnStepSwitchDriver& operator=(const OnStepSwitchDriver&) = delete;

    int get_device_number() const override { return device_number_; }
    // Decision 0024: the brand, then the device type (the controller has no model name for its features).
    std::string get_name() const override { return "OnStep Switch"; }
    DeviceType get_device_type() const override { return DeviceType::Switch; }
    std::string fallback_unique_id() const override { return "OnStep_Switch_" + std::to_string(device_number_); }
    std::string get_description() const override { return "OnStepX auxiliary features (LX200 protocol)"; }
    std::string get_driver_info() const override { return "AlpacaCore OnStep Switch Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 3; }

    // The controller's product and firmware from the connect (:GVP#, :GVN#); web UI only, never DriverInfo.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) return std::nullopt;
        return firmware_;
    }

    // Lock-free (decision 0016): the shared link is atomic, its liveness is a stat()/poll() with no wait, and the
    // poll's loss latch is atomic. A loss of the link, found by any device on it, reads here at once (decision 0009).
    bool get_connected() const override { return connected_.load() && link_up(); }
    ALPACA_ASYNC_CONNECT_FORWARDERS()

    void set_connected(bool connected) override {
        // Serializes whole transitions (the connect is a multi-exchange handshake, #528); no getter takes it.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        const bool live = connected_.load() && link_up();
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(live)) return;
        // A connect is a no-op only on a live link; a lost one reconnects (decision 0009 rule 4). A disconnect is a
        // no-op only when nothing is held.
        if (connected ? live : !connected_.load()) return;
        // Orders the transition against a write still on its way to the wire.
        std::lock_guard<std::mutex> io(io_mutex_);
        if (connected) {
            if (connected_.load()) {
                teardown_locked();  // a lost link: start from a clean driver
            }
            open_locked();
            ALPACA_LOG_INFO(kLogTag, "OnStep switch connected");
        } else {
            teardown_locked();
            ALPACA_LOG_INFO(kLogTag, "OnStep switch disconnected");
        }
    }

    // DeviceState's TimeStamp is when the served frame was measured, not the request time (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override { return state_.age(); }

    std::string get_link_fault() const override {
        const auto link = link_.load();
        if (link) {
            (void)link->link_alive();  // notice an unplug the listing would otherwise miss
            const std::string reason = link->link_lost_reason();
            if (!reason.empty()) {
                return "Link to the controller lost (" + reason + "); reconnect";
            }
        }
        std::string fault = state_.fault();
        if (!fault.empty()) return fault;
        return state_.link_lost() ? "no answer to the state poll" : std::string();
    }

    // --- Switch interface ---

    // The slots are read at connect (:GXY0#, :GXY<n>#), so the count is not known while disconnected.
    int get_max_switch() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_slots_locked();
        return static_cast<int>(slots_.size());
    }

    bool get_can_write(int id) const override {
        (void)slot_at(id);
        return true;
    }

    // OnStepX applies a write at once (a momentary switch turns itself off after a fixed time), so nothing is async.
    bool get_can_async(int id) const override {
        (void)slot_at(id);
        return false;
    }

    bool get_switch(int id) const override { return value_of(id) > 0.0; }

    void set_switch(int id, bool state) override {
        const Slot slot = slot_at(id);
        write_value(id, slot, state ? slot.max : 0.0);
    }

    void set_async(int id, bool /*state*/) override {
        (void)slot_at(id);
        throw AlpacaException("Async switch control not supported", AlpacaError::MethodNotImplemented);
    }

    double get_switch_value(int id) const override { return value_of(id); }

    void set_switch_value(int id, double value) override {
        check_static_id(id);
        if (!std::isfinite(value)) {
            throw AlpacaException("Switch value must be a finite number", AlpacaError::InvalidValue);
        }
        const Slot slot = slot_at(id);
        write_value(id, slot, value);
    }

    void set_async_value(int id, double /*value*/) override {
        (void)slot_at(id);
        throw AlpacaException("Async switch control not supported", AlpacaError::MethodNotImplemented);
    }

    bool get_state_change_complete(int id) const override {
        (void)slot_at(id);
        return true;
    }

    // The firmware's name for the slot (its configuration, at most 10 characters).
    std::string get_switch_name(int id) const override {
        const Slot slot = slot_at(id);
        return slot.name.empty() ? "Feature " + std::to_string(slot.slot) : slot.name;
    }

    // The name is the firmware's; OnStepX has no command that sets it.
    void set_switch_name(int id, const std::string& /*name*/) override {
        (void)slot_at(id);
        throw AlpacaException("The OnStepX feature names are set in the firmware's configuration",
                              AlpacaError::MethodNotImplemented);
    }

    // Only what the connect read: the slot number and its purpose.
    std::string get_switch_description(int id) const override {
        const Slot slot = slot_at(id);
        return "OnStepX auxiliary feature " + std::to_string(slot.slot) + ": " + slot.description;
    }

    double get_min_switch_value(int id) const override {
        (void)slot_at(id);
        return 0.0;
    }

    double get_max_switch_value(int id) const override { return slot_at(id).max; }

    double get_switch_step(int id) const override {
        (void)slot_at(id);
        return 1.0;
    }

    // Test seams.
    void set_poll_paused_for_testing(bool paused) { state_.pause_for_testing(paused); }
    void poll_now_for_testing() { state_.poll_now_for_testing(); }

private:
    // One included feature: its OnStepX slot (1-8), the firmware's name, and how the Switch serves its purpose.
    struct Slot {
        int slot = 0;
        std::string name;
        int purpose = 0;
        double max = 1.0;
        std::string description;
    };

    // What one poll measured: the value of each included slot, in switch id order.
    struct Frame {
        std::vector<long> values;
    };

    // An id no OnStepX controller can have is InvalidValue before the connection check.
    static void check_static_id(int id) {
        if (id < 0 || id >= kMaxSlots) {
            throw AlpacaException(
                "Switch id " + std::to_string(id) + " is out of range (0-" + std::to_string(kMaxSlots - 1) + ")",
                AlpacaError::InvalidValue);
        }
    }

    // The slot behind a switch id: the static id check, the connection, then the connected controller's count.
    Slot slot_at(int id) const {
        check_static_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_slots_locked();
        if (id >= static_cast<int>(slots_.size())) {
            throw AlpacaException("Switch id " + std::to_string(id) + " is out of range (MaxSwitch is " +
                                      std::to_string(slots_.size()) + ")",
                                  AlpacaError::InvalidValue);
        }
        return slots_[static_cast<std::size_t>(id)];
    }

    double value_of(int id) const {
        (void)slot_at(id);
        const Frame frame = served_frame();
        if (id >= static_cast<int>(frame.values.size())) {
            // A Disconnect and a Connect to a controller with fewer slots between the two reads.
            throw AlpacaException("OnStep switch not connected", AlpacaError::NotConnected);
        }
        return static_cast<double>(frame.values[static_cast<std::size_t>(id)]);
    }

    // The write every setter ends in: the range of this slot (InvalidValue with nothing sent), the fault check, then
    // :SXX<n>,V<v># and its reply, with the :GE# that says why in the same sequence.
    void write_value(int id, const Slot& slot, double value) {
        if (value < 0.0 || value > slot.max) {
            throw AlpacaException("Switch " + std::to_string(id) + " value " + std::to_string(value) +
                                      " is out of range (0-" + std::to_string(static_cast<int>(slot.max)) + ")",
                                  AlpacaError::InvalidValue);
        }
        const long wire = std::lround(value);  // the firmware rounds too (lroundf); the step is 1
        (void)served_frame();                  // refused while the link is faulted or lost (decision 0009 rule 1)
        std::lock_guard<std::mutex> io(io_mutex_);
        // Under io_mutex_, which a Disconnect+Connect holds: the slot set of this connect (decision 0016 rule 6).
        if (slot_at(id).slot != slot.slot) {
            throw AlpacaException("OnStep switch reconnected to another feature set; read the switches again",
                                  AlpacaError::InvalidOperation);
        }
        const auto link = live_link();
        bool accepted = false;
        int code = -1;
        {
            const auto sequence = link->hold_sequence();  // the reply and its :GE# together (#2223)
            accepted = link->set_aux_feature_value(slot.slot, wire);
            if (!accepted) {
                code = link->get_last_command_error().value_or(-1);
            }
        }
        if (!accepted && code != 0 && code != 25) {
            throw AlpacaException("Switch " + std::to_string(id) + " write refused by the controller (" +
                                      describe_feature_error(code) + ")",
                                  AlpacaError::InvalidOperation);
        }
        // Write-through (decision 0010): the next read sees it before the next poll.
        state_.write([id, wire](Frame& frame) {
            if (id < static_cast<int>(frame.values.size())) {
                frame.values[static_cast<std::size_t>(id)] = wire;
            }
        });
    }

    // The connect body. Caller holds transition_mutex_ and io_mutex_; no poll runs.
    void open_locked() {
        std::shared_ptr<OnStepProtocolWrapper> link;
        // An auto-detected controller resolves its port here, not in the factory (#659): the telescope's scan, where
        // a port a connected OnStep device holds keeps its place, so the switch lands on the telescope's link.
        util::connect_resolved(
            connection_info_, connection_resolved_, connection_resolver_,
            [&link](const ConnectionInfo& info) {
                std::string reason;
                link = OnStepProtocolWrapper::acquire(info, &reason);
                if (!link) {
                    const std::string message = reason.empty()
                                                    ? "Failed to connect to the OnStep controller"
                                                    : "Failed to connect to the OnStep controller: " + reason;
                    // Refused TCP connect, vanished serial node, or a port another kind of device now holds (#1202).
                    if (info.type == ConnectionType::Network || util::serial_endpoint_stale(info.port_path)) {
                        throw util::StaleEndpoint(message);
                    }
                    throw AlpacaException(message);
                }
            },
            "OnStep");
        try {
            check_controller(*link);
            link_.store(link);
            state_.reset();
            // The first frame before Connected reads true (decisions 0010 and 0016); a lost or dropped reply gets
            // one retry inside start().
            if (!state_.start()) {
                state_.stop_and_join();
                throw AlpacaException("The OnStepX auxiliary features did not answer the first state poll",
                                      AlpacaError::DriverException);
            }
        } catch (...) {
            // Nothing half-connected stays: the hold on the shared link goes (the port closes when no other OnStep
            // device holds it).
            clear_connect_state();
            release_link();
            state_.reset();
            throw;
        }
        connected_.store(true);
    }

    // Identity, firmware and the feature slots, through the joined link. Throws the operator-facing refusal. The slot
    // set is fixed here until the next connect: switch ids 0..N-1 are the included slots in slot order.
    void check_controller(OnStepProtocolWrapper& link) {
        const std::string product = link.get_product_name();
        if (product.find("OnStep") == std::string::npos && product.find("On-Step") == std::string::npos) {
            throw AlpacaException("The device on this port is not an OnStep controller (:GVP# answered \"" + product +
                                  "\")");
        }
        const std::string version = link.get_version_number();
        if (!is_onstepx_version(version)) {
            throw AlpacaException("The OnStep Switch requires OnStepX firmware; the controller reports " +
                                  (version.empty() ? std::string("no version") : "version " + version));
        }
        const auto present = link.aux_feature_slots();
        if (!present) {
            throw AlpacaException("OnStepX did not report its auxiliary features (:GXY0#)",
                                  AlpacaError::DriverException);
        }
        std::vector<Slot> slots;
        for (std::size_t i = 0; i < present->size(); ++i) {
            if ((*present)[i] != '1') continue;
            const int n = static_cast<int>(i) + 1;
            const auto feature = link.aux_feature(n);
            if (!feature) {
                throw AlpacaException("OnStepX auxiliary feature " + std::to_string(n) +
                                          " did not report its name and purpose (:GXY" + std::to_string(n) + "#)",
                                      AlpacaError::DriverException);
            }
            const auto kind = detail::aux_switch_kind(feature->purpose);
            if (!kind) {
                if (feature->purpose != 4 && feature->purpose != 6) {
                    ALPACA_LOG_INFO(kLogTag, "Leaving out OnStepX auxiliary feature " + std::to_string(n) + " (\"" +
                                                 feature->name + "\"): purpose " + std::to_string(feature->purpose) +
                                                 " is not one a Switch serves");
                }
                continue;
            }
            slots.push_back(Slot{n, feature->name, feature->purpose, kind->max, kind->description});
        }
        if (slots.empty()) {
            throw AlpacaException(
                "OnStepX reports no auxiliary feature a Switch can drive (a switch, an analog output or a dew heater)");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            slots_ = std::move(slots);
        }
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        firmware_ = product + (version.empty() ? "" : " " + version);
    }

    // The disconnect body, shared with the reconnect over a lost link. Caller holds transition_mutex_ and io_mutex_.
    void teardown_locked() {
        connected_.store(false);
        // The poll ends before the hold on the link goes; its body takes no lock this thread holds.
        state_.stop_and_join();
        clear_connect_state();
        release_link();
        state_.reset();
    }

    void clear_connect_state() {
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_.clear();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.clear();
    }

    // Lets go of the shared link; the port closes when no other OnStep device holds it. Never under mutex_.
    void release_link() {
        std::shared_ptr<OnStepProtocolWrapper> link = link_.exchange(nullptr);
        link.reset();
    }

    // PolledState's on_lost (decision 0009 rule 2, the iFR precedent): a switch whose poll has gone unanswered past
    // util::kLinkStalenessBound takes the link down for every device on the controller.
    void lose_shared_link() {
        if (const auto link = link_.load()) {
            link->declare_link_lost(
                "OnStepX auxiliary features have had no reply for " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) +
                " s");
        }
    }

    bool link_up() const {
        const auto link = link_.load();
        return link && link->link_alive() && !state_.link_lost();
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("OnStep switch not connected", AlpacaError::NotConnected);
        }
        if (!link_up()) {
            throw AlpacaException("OnStep switch link lost; set Connected true to reconnect",
                                  AlpacaError::NotConnected);
        }
        detail::run_connection_check_hook();  // a test lands a Disconnect here
    }

    // Caller holds mutex_. The slot set belongs to a live connection: a connect never leaves it empty (it refuses a
    // controller with nothing to serve), and a Disconnect that ran after the caller's connection check has cleared it,
    // so the member answers NotConnected, never an id range against an empty set (#2222 TSan finding).
    void require_slots_locked() const {
        if (slots_.empty()) {
            throw AlpacaException("OnStep switch not connected", AlpacaError::NotConnected);
        }
    }

    // The held link, or NotConnected when it is gone (a Disconnect that won the race).
    std::shared_ptr<OnStepProtocolWrapper> live_link() const {
        auto link = link_.load();
        if (!link) {
            throw AlpacaException("OnStep switch not connected", AlpacaError::NotConnected);
        }
        return link;
    }

    // The frame the getters serve (decisions 0009 and 0010): stale is served, a faulted link throws DriverException,
    // a lost one NotConnected.
    Frame served_frame() const {
        ensure_connected();
        return state_.served().value;
    }

    // One poll, on the poll thread (or the connect's, with the poll stopped) with no driver lock held; the link's
    // own lock serialises it with the other devices' exchanges. A lost link is positive evidence (decision 0009
    // rule 2a).
    Frame probe() {
        const auto link = link_.load();
        if (!link || !link->link_alive()) {
            state_.lose_link();
            throw AlpacaException("OnStep switch link is down", AlpacaError::NotConnected);
        }
        std::vector<int> slots;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            slots.reserve(slots_.size());
            for (const auto& s : slots_) slots.push_back(s.slot);
        }
        try {
            Frame frame;
            frame.values.reserve(slots.size());
            for (const int slot : slots) {
                const auto value = link->aux_feature_value(slot);
                if (!value) {
                    throw AlpacaException("OnStep auxiliary feature " + std::to_string(slot) + " did not read",
                                          AlpacaError::DriverException);
                }
                frame.values.push_back(*value);
            }
            return frame;
        } catch (const AlpacaException& e) {
            if (e.error_code() == AlpacaError::NotConnected) {
                state_.lose_link();  // the link went during the exchange
            }
            throw;
        }
    }

    util::TaskClock& clock_;
    const int device_number_;
    ConnectionInfo connection_info_;
    // Empty for a configured endpoint; set by the auto-detect factory. Under transition_mutex_.
    util::ConnectionResolver<ConnectionInfo> connection_resolver_;
    bool connection_resolved_ = false;
    std::atomic<bool> connected_{false};
    // The controller's shared link (#2223). Atomic: get_connected() and the poll read it lock-free; replaced only
    // under transition_mutex_ and io_mutex_.
    std::atomic<std::shared_ptr<OnStepProtocolWrapper>> link_;

    // Decision 0016 lock order: transition_mutex_ -> io_mutex_ -> the link's sequence and wire locks -> mutex_, a
    // leaf held only to copy the slot set, never across an exchange.
    std::mutex transition_mutex_;
    std::mutex io_mutex_;  // orders the writes on the wire; never taken by a getter or the poll
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;  // the included slots of this connect, in switch id order; under mutex_
    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    util::OperationGeneration state_poll_generation_;
    // Last: destroyed first, joining its poll body while the members it reads are alive.
    util::PolledState<Frame> state_;
};

std::unique_ptr<SwitchDriver> create_onstep_switch(int device_number, const ConnectionInfo& connection_info,
                                                   util::TaskClock& clock) {
    return std::make_unique<OnStepSwitchDriver>(device_number, connection_info,
                                                util::ConnectionResolver<ConnectionInfo>{}, clock);
}

std::unique_ptr<SwitchDriver> create_onstep_switch_deferred(int device_number,
                                                            util::ConnectionResolver<ConnectionInfo> resolver,
                                                            util::TaskClock& clock) {
    if (!resolver) {
        throw AlpacaException("OnStep switch: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<OnStepSwitchDriver>(device_number, ConnectionInfo{}, std::move(resolver), clock);
}

std::unique_ptr<SwitchDriver> create_onstep_switch_auto(int device_number, int mount_index) {
    // The serial scan runs at connect time (#659), not here.
    return create_onstep_switch_deferred(device_number,
                                         [mount_index] { return resolve_onstep_serial_auto(mount_index); });
}

void set_onstep_switch_poll_paused_for_testing(SwitchDriver& driver, bool paused) {
    if (auto* onstep = dynamic_cast<OnStepSwitchDriver*>(&driver)) {
        onstep->set_poll_paused_for_testing(paused);
    }
}

void poll_onstep_switch_now_for_testing(SwitchDriver& driver) {
    if (auto* onstep = dynamic_cast<OnStepSwitchDriver*>(&driver)) {
        onstep->poll_now_for_testing();
    }
}

}  // namespace alpacacore::vendor::onstep
