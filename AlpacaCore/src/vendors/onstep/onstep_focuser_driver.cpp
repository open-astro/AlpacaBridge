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

// An OnStepX focuser (#2223): one of the up to six focusers the controller drives, over the controller's shared
// link (OnStepProtocolWrapper::acquire(), the iOptron iFR precedent of decision 0017). Commands are the addressed
// raw-step forms :F<n>...# (OnStepX Focuser.command.cpp, af1d26bf); see .github/instructions/onstep.instructions.md.

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/onstep/onstep_focuser_driver.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::onstep {

namespace {

constexpr const char* kLogTag = "OnStep focuser";
// The state poll (decision 0010), start to start: :F<n>T# and :F<n>g# on every tick, two short exchanges on the link
// the telescope shares, so the line stays mostly free for the telescope's 1 s poll and its commands. The temperature
// and the compensation flag ride every kSlowProbeEvery-th tick. The freshness bound is two periods; the cadence, the
// fault latch (three failed polls) and the loss rule live in util::PolledState.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(500);
constexpr int kFaultThreshold = 3;
constexpr int kSlowProbeEvery = 4;

// The :GE# codes a focuser command gets (OnStepX Focuser.cpp gotoTarget()/setTcfEnable(), af1d26bf; the table in
// AlpacaCore/external/OnStep/COMMAND_REFERENCE.md).
std::string describe_focuser_error(int code) {
    switch (code) {
        case 2:
            return "unknown command";
        case 4:
            return "parameter out of range";
        case 5:
            return "bad parameter format";
        case 9:
            return "the focuser is parked";
        default:
            return code < 0 ? std::string("no reason given") : "error code " + std::to_string(code);
    }
}

// OnStepX addresses focusers 1-6 (:F1...# to :F6...#, Focuser.command.cpp).
int checked_focuser_number(int focuser_number) {
    if (focuser_number < 1 || focuser_number > 6) {
        throw AlpacaException("OnStep focuser number must be 1-6, not " + std::to_string(focuser_number),
                              AlpacaError::InvalidValue);
    }
    return focuser_number;
}

}  // namespace

class OnStepFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    OnStepFocuserDriver(int device_number, int focuser_number, ConnectionInfo info,
                        util::ConnectionResolver<ConnectionInfo> resolver, util::TaskClock& clock)
        : AsyncConnectable("OnStep"),
          clock_(clock),
          device_number_(device_number),
          focuser_number_(checked_focuser_number(focuser_number)),
          connection_info_(std::move(info)),
          connection_resolver_(std::move(resolver)),
          state_(
              clock_, state_poll_generation_, {kStatePollPeriod, kFaultThreshold, 2 * kStatePollPeriod, kLogTag},
              [this] { return probe(); }, [this] { lose_shared_link(); }) {}

    ~OnStepFocuserDriver() override {
        shutdown_connection();
        // The poll body goes before the members it reads.
        state_.stop_and_join();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall): most-derived class
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during OnStep focuser destruction: " + std::string(e.what()));
            }
        }
        release_link();
    }

    OnStepFocuserDriver(const OnStepFocuserDriver&) = delete;
    OnStepFocuserDriver& operator=(const OnStepFocuserDriver&) = delete;

    int get_device_number() const override { return device_number_; }
    // Decision 0024: the brand, then the device type (the controller has no model name for a focuser).
    std::string get_name() const override { return "OnStep Focuser"; }
    DeviceType get_device_type() const override { return DeviceType::Focuser; }
    std::string fallback_unique_id() const override { return "OnStep_Focuser_" + std::to_string(device_number_); }
    std::string get_description() const override {
        return "OnStepX focuser " + std::to_string(focuser_number_) + " (LX200 protocol)";
    }
    std::string get_driver_info() const override { return "AlpacaCore OnStep Focuser Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }

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
        // Orders the transition against a Move, Halt or TempComp write still on its way to the wire.
        std::lock_guard<std::mutex> io(io_mutex_);
        if (connected) {
            if (connected_.load()) {
                teardown_locked();  // a lost link: start from a clean driver
            }
            open_locked();
            ALPACA_LOG_INFO(kLogTag, "OnStep focuser " + std::to_string(focuser_number_) + " connected");
        } else {
            teardown_locked();
            ALPACA_LOG_INFO(kLogTag, "OnStep focuser " + std::to_string(focuser_number_) + " disconnected");
        }
    }

    // DeviceState's TimeStamp is when the served frame was measured, not the request time (decision 0010): the frame
    // can be a poll period or two old on a healthy link, and older while a fault stands.
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

    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }
    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }
    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    // --- Focuser interface ---

    bool get_absolute() const override { return true; }

    bool get_is_moving() const override { return served_frame().moving; }

    int get_max_step() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connect_state_locked();
        return max_step_;
    }
    int get_max_increment() const override { return get_max_step(); }

    int get_position() const override { return served_frame().position; }

    double get_step_size() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connect_state_locked();
        if (!microns_per_step_) {
            throw AlpacaException("OnStepX focuser " + std::to_string(focuser_number_) +
                                      " did not report its step size (:F" + std::to_string(focuser_number_) + "u#)",
                                  AlpacaError::PropertyNotImplemented);
        }
        return *microns_per_step_;
    }

    // OnStepX keeps temperature compensation off without a temperature probe: setTcfEnable() turns a request on into
    // off while the sensor reads NAN (Focuser.cpp, af1d26bf). So it is available only when :F<n>c# answers 0 or 1
    // and :F<n>t# reads a temperature, both at connect.
    bool get_temp_comp_available() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_connect_state_locked();
        return temp_comp_available_;
    }

    bool get_temp_comp() const override {
        if (!get_temp_comp_available()) {
            return false;  // IFocuserV4: always false when not available
        }
        return served_frame().temp_comp;
    }

    void set_temp_comp(bool temp_comp) override {
        if (!get_temp_comp_available()) {
            throw AlpacaException("OnStepX focuser " + std::to_string(focuser_number_) +
                                      " has no temperature compensation (no temperature probe)",
                                  AlpacaError::NotImplemented);
        }
        (void)served_frame();  // refused while the link is faulted or lost (decision 0009 rule 1)
        std::lock_guard<std::mutex> io(io_mutex_);
        const auto link = live_link();
        bool accepted = false;
        int code = -1;
        {
            // The reply and the :GE# that says why, with no telescope exchange between them (#2223).
            const auto sequence = link->hold_sequence();
            accepted = link->set_focuser_temp_comp(focuser_number_, temp_comp);
            if (!accepted) {
                code = link->get_last_command_error().value_or(-1);
            }
        }
        if (!accepted && code != 0 && code != 25) {
            throw AlpacaException("TempComp refused by the controller (" + describe_focuser_error(code) + ")",
                                  AlpacaError::InvalidOperation);
        }
        // Write-through (decision 0010): the next read sees it before the next poll.
        state_.write([temp_comp](Frame& frame) { frame.temp_comp = temp_comp; });
    }

    double get_temperature() const override {
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            require_connect_state_locked();
            if (!temperature_probe_) {
                throw AlpacaException("OnStepX focuser " + std::to_string(focuser_number_) +
                                          " has no temperature probe (:F" + std::to_string(focuser_number_) +
                                          "t# reads none)",
                                      AlpacaError::PropertyNotImplemented);
            }
        }
        const Frame frame = served_frame();
        if (!frame.temperature) {
            // Decision 0010 rule 8: a value only the device supplies, not answered (the probe stopped reading).
            throw AlpacaException("The OnStepX focuser temperature probe gave no reading",
                                  AlpacaError::DriverException);
        }
        return *frame.temperature;
    }

    // :F<n>Q#. The safety stop: never refused for a link fault, only for a lost link (NotConnected).
    void halt() override {
        ensure_connected();
        std::lock_guard<std::mutex> io(io_mutex_);
        live_link()->focuser_halt(focuser_number_);
        // Write-through: IsMoving reads false right after Halt, not after the next poll.
        state_.write([](Frame& frame) { frame.moving = false; });
    }

    void move(int position) override {
        ensure_connected();
        (void)served_frame();  // refused while the link is faulted or lost
        std::lock_guard<std::mutex> io(io_mutex_);
        // The travel is read under io_mutex_, which a Disconnect+Connect holds, so the goto uses this connect's limits
        // (decision 0016 rule 6), not a previous connect's.
        int max_step = 0;
        long min_position = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            require_connect_state_locked();
            max_step = max_step_;
            min_position = min_position_;
        }
        // A target outside 0..MaxStep goes to that limit: ConformU moves below 0 and above MaxStep and expects the
        // focuser to stop there, not an InvalidValue (ascom-contract.instructions.md, #2028).
        position = std::clamp(position, 0, max_step);
        const auto link = live_link();
        bool accepted = false;
        int code = -1;
        {
            const auto sequence = link->hold_sequence();  // the reply and its :GE# together (#2223)
            accepted = link->focuser_goto(focuser_number_, min_position + position);
            if (!accepted) {
                code = link->get_last_command_error().value_or(-1);
            }
        }
        if (!accepted && code != 0 && code != 25) {
            throw AlpacaException("Move refused by the controller (" + describe_focuser_error(code) + ")",
                                  AlpacaError::InvalidOperation);
        }
        // Write-through (decision 0010): IsMoving reads true right after a Move that has somewhere to go; Position
        // stays the measured one until the next poll, which also drops any sample taken before the command.
        state_.write([position](Frame& frame) { frame.moving = frame.position != position; });
    }

    // Test seams.
    void set_poll_paused_for_testing(bool paused) { state_.pause_for_testing(paused); }
    void poll_now_for_testing() { state_.poll_now_for_testing(); }

private:
    // What one poll measured. Position is counted from the minimum (:F<n>g# - :F<n>i#), so it is 0..MaxStep.
    struct Frame {
        int position = 0;
        bool moving = false;
        std::optional<double> temperature;  // the last :F<n>t# reading; nullopt when it read none
        bool temp_comp = false;             // the last :F<n>c# answer, or the driver's own write
    };

    // The connect body. Caller holds transition_mutex_ and io_mutex_; no poll runs.
    void open_locked() {
        std::shared_ptr<OnStepProtocolWrapper> link;
        // An auto-detected controller resolves its port here, not in the factory (#659): the telescope's scan, where
        // a port a connected OnStep device holds keeps its place, so the focuser lands on the telescope's link.
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
            probe_count_ = 0;
            // The first frame before Connected reads true (decisions 0010 and 0016); a lost or dropped reply gets
            // one retry inside start().
            if (!state_.start()) {
                state_.stop_and_join();
                throw AlpacaException(
                    "OnStepX focuser " + std::to_string(focuser_number_) + " did not answer the first state poll",
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

    // Identity, firmware and the focuser itself, through the joined link. Throws the operator-facing refusal.
    void check_controller(OnStepProtocolWrapper& link) {
        const std::string product = link.get_product_name();
        if (product.find("OnStep") == std::string::npos && product.find("On-Step") == std::string::npos) {
            throw AlpacaException("The device on this port is not an OnStep controller (:GVP# answered \"" + product +
                                  "\")");
        }
        const std::string version = link.get_version_number();
        if (!is_onstepx_version(version)) {
            throw AlpacaException("The OnStep Focuser requires OnStepX firmware; the controller reports " +
                                  (version.empty() ? std::string("no version") : "version " + version));
        }
        const std::string n = std::to_string(focuser_number_);
        if (!link.focuser_status(focuser_number_)) {
            throw AlpacaException("OnStepX reports no focuser " + n);
        }
        const auto min_position = link.focuser_min_position(focuser_number_);
        const auto max_position = link.focuser_max_position(focuser_number_);
        if (!min_position || !max_position || *max_position < *min_position) {
            throw AlpacaException("OnStepX focuser " + n + " did not report its travel (:F" + n + "i#, :F" + n + "m#)",
                                  AlpacaError::DriverException);
        }
        const auto microns = link.focuser_microns_per_step(focuser_number_);
        const bool probe = link.focuser_temperature(focuser_number_).has_value();
        const bool tcf_answers = link.focuser_temp_comp(focuser_number_).has_value();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connect_state_ = true;  // the travel, step size and probe flags of this connect are in place
            min_position_ = *min_position;
            const long span = *max_position - *min_position;
            max_step_ = static_cast<int>(std::min<long>(span, std::numeric_limits<int>::max()));
            microns_per_step_ = microns;
            temperature_probe_ = probe;
            temp_comp_available_ = probe && tcf_answers;
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
        connect_state_ = false;
        min_position_ = 0;
        max_step_ = 0;
        microns_per_step_.reset();
        temperature_probe_ = false;
        temp_comp_available_ = false;
    }

    // Lets go of the shared link; the port closes when no other OnStep device holds it. Never under mutex_.
    void release_link() {
        std::shared_ptr<OnStepProtocolWrapper> link = link_.exchange(nullptr);
        link.reset();
    }

    // PolledState's on_lost (decision 0009 rule 2, the iFR precedent): a focuser whose poll has gone unanswered past
    // util::kLinkStalenessBound takes the link down for every device on the controller, as a telescope's stale
    // fault does. One controller, one link: the telescope's next exchange then reads the loss.
    void lose_shared_link() {
        if (const auto link = link_.load()) {
            link->declare_link_lost(
                "OnStepX focuser " + std::to_string(focuser_number_) + " has had no reply for " +
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
            throw AlpacaException("OnStep focuser not connected", AlpacaError::NotConnected);
        }
        if (!link_up()) {
            throw AlpacaException("OnStep focuser link lost; set Connected true to reconnect",
                                  AlpacaError::NotConnected);
        }
        detail::run_connection_check_hook();  // a test lands a Disconnect here
    }

    // Caller holds mutex_. The connect-time values (travel, step size, probe flags) belong to a live connection: a
    // Disconnect that ran after the caller's connection check has cleared them, so the member answers NotConnected,
    // never 0 or PropertyNotImplemented computed from the cleared values (#2222 TSan finding).
    void require_connect_state_locked() const {
        if (!connect_state_) {
            throw AlpacaException("OnStep focuser not connected", AlpacaError::NotConnected);
        }
    }

    // The held link, or NotConnected when it is gone (a Disconnect that won the race).
    std::shared_ptr<OnStepProtocolWrapper> live_link() const {
        auto link = link_.load();
        if (!link) {
            throw AlpacaException("OnStep focuser not connected", AlpacaError::NotConnected);
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
    // own lock serialises it with the telescope's exchanges. A lost link is positive evidence (decision 0009 rule 2a).
    Frame probe() {
        const auto link = link_.load();
        if (!link || !link->link_alive()) {
            state_.lose_link();
            throw AlpacaException("OnStep focuser link is down", AlpacaError::NotConnected);
        }
        try {
            Frame frame;
            const auto previous = state_.read();
            if (previous.has_value()) {
                frame.temperature = previous->value.temperature;
                frame.temp_comp = previous->value.temp_comp;
            }
            const auto status = link->focuser_status(focuser_number_);
            if (!status) {
                throw AlpacaException("OnStep focuser status did not read", AlpacaError::DriverException);
            }
            const auto raw = link->focuser_position(focuser_number_);
            if (!raw) {
                throw AlpacaException("OnStep focuser position did not read", AlpacaError::DriverException);
            }
            long min_position = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                min_position = min_position_;
            }
            frame.moving = status->moving;
            const long position = *raw - min_position;
            frame.position = static_cast<int>(
                std::clamp<long>(position, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
            if (probe_count_++ % kSlowProbeEvery == 0) {
                frame.temperature = link->focuser_temperature(focuser_number_);
                if (const auto tcf = link->focuser_temp_comp(focuser_number_)) {
                    frame.temp_comp = *tcf;
                }
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
    const int focuser_number_;
    ConnectionInfo connection_info_;
    // Empty for a configured endpoint; set by the auto-detect factory. Under transition_mutex_.
    util::ConnectionResolver<ConnectionInfo> connection_resolver_;
    bool connection_resolved_ = false;
    std::atomic<bool> connected_{false};
    // The controller's shared link (#2223). Atomic: get_connected() and the poll read it lock-free; replaced only
    // under transition_mutex_ and io_mutex_.
    std::atomic<std::shared_ptr<OnStepProtocolWrapper>> link_;

    // Decision 0016 lock order: transition_mutex_ -> io_mutex_ -> the link's sequence and wire locks. mutex_ is a
    // leaf, held only to copy the connect-time values below, never across an exchange.
    std::mutex transition_mutex_;
    std::mutex io_mutex_;  // orders Move, Halt and the TempComp write on the wire; never taken by a getter or the poll
    mutable std::mutex mutex_;
    bool connect_state_ = false;              // the values below are a live connect's; under mutex_
    long min_position_ = 0;                   // :F<n>i# at connect; under mutex_
    int max_step_ = 0;                        // :F<n>m# - :F<n>i# at connect; under mutex_
    std::optional<double> microns_per_step_;  // :F<n>u# at connect; under mutex_
    bool temperature_probe_ = false;          // :F<n>t# read a temperature at connect; under mutex_
    bool temp_comp_available_ = false;        // and :F<n>c# answered 0 or 1; under mutex_
    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    // The poll's tick count, for the slow reads. Touched by the poll thread, or the connect with the poll stopped.
    int probe_count_ = 0;
    util::OperationGeneration state_poll_generation_;
    // Last: destroyed first, joining its poll body while the members it reads are alive.
    util::PolledState<Frame> state_;
};

std::unique_ptr<FocuserDriver> create_onstep_focuser(int device_number, const ConnectionInfo& connection_info,
                                                     int focuser_number, util::TaskClock& clock) {
    return std::make_unique<OnStepFocuserDriver>(device_number, focuser_number, connection_info,
                                                 util::ConnectionResolver<ConnectionInfo>{}, clock);
}

std::unique_ptr<FocuserDriver> create_onstep_focuser_deferred(int device_number,
                                                              util::ConnectionResolver<ConnectionInfo> resolver,
                                                              int focuser_number, util::TaskClock& clock) {
    if (!resolver) {
        throw AlpacaException("OnStep focuser: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<OnStepFocuserDriver>(device_number, focuser_number, ConnectionInfo{}, std::move(resolver),
                                                 clock);
}

std::unique_ptr<FocuserDriver> create_onstep_focuser_auto(int device_number, int mount_index, int focuser_number) {
    // The serial scan runs at connect time (#659), not here.
    return create_onstep_focuser_deferred(
        device_number, [mount_index] { return resolve_onstep_serial_auto(mount_index); }, focuser_number);
}

void set_onstep_focuser_poll_paused_for_testing(FocuserDriver& driver, bool paused) {
    if (auto* onstep = dynamic_cast<OnStepFocuserDriver*>(&driver)) {
        onstep->set_poll_paused_for_testing(paused);
    }
}

void poll_onstep_focuser_now_for_testing(FocuserDriver& driver) {
    if (auto* onstep = dynamic_cast<OnStepFocuserDriver*>(&driver)) {
        onstep->poll_now_for_testing();
    }
}

}  // namespace alpacacore::vendor::onstep
