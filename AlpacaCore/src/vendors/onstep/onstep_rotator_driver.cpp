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

// The OnStepX rotator (#2221), over the controller's shared link (OnStepProtocolWrapper::acquire(), the iOptron iFR
// precedent of decision 0017). Commands are the :r...# forms and :GX98# (OnStepX Rotator.command.cpp, af1d26bf); see
// .github/instructions/onstep.instructions.md. The derotator is the firmware's own: this driver never enables,
// disables, reverses or aims it (:r+#, :r-#, :rR#, :rP#), and refuses a move while it runs.

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_rotator_driver.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace alpacacore::vendor::onstep {

namespace {

constexpr const char* kLogTag = "OnStep rotator";
// The state poll (decision 0010), start to start: :rT# and :rG#, two short exchanges on the link the telescope
// shares. The freshness bound is two periods; the fault latch (three failed polls) and the loss rule live in
// util::PolledState.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(500);
constexpr int kFaultThreshold = 3;
// A move within this of where the rotator is has nowhere to go: IsMoving is not written true for it.
constexpr double kSameAngleDegrees = 1e-6;

// The :GE# codes a rotator goto gets (OnStepX Rotator.cpp gotoTarget() and Axis::autoGoto(), af1d26bf; the table in
// AlpacaCore/external/OnStep/COMMAND_REFERENCE.md).
std::string describe_rotator_error(int code) {
    switch (code) {
        case 2:
            return "unknown command";
        case 4:
            return "parameter out of range";
        case 5:
            return "bad parameter format";
        case 9:
            return "the rotator is parked";
        case 19:
            return "a goto is already running";
        case 20:
            return "outside the configured limits";
        case 21:
            return "hardware fault";
        case 22:
            return "the rotator is already moving";
        default:
            return code < 0 ? std::string("no reason given") : "error code " + std::to_string(code);
    }
}

void validate_angle(double degrees) {
    if (!std::isfinite(degrees)) {
        throw AlpacaException("Rotator position must be a finite number of degrees", AlpacaError::InvalidValue);
    }
}

double wrap(double degrees) { return util::wrap_degrees(degrees); }

}  // namespace

namespace detail {

std::optional<double> rotator_angle_within_travel(double mechanical, double current, double min, double max) {
    if (!std::isfinite(mechanical) || !std::isfinite(current) || !std::isfinite(min) || !std::isfinite(max) ||
        max < min) {
        return std::nullopt;
    }
    constexpr double kEdge = 1e-9;  // a target on a limit stays inside it
    // mechanical + 360 k for the turns k that land inside [min, max]; the one nearest `current` is the turn nearest
    // (current - mechanical) / 360, clamped into that range, or one of its neighbours.
    const double first = std::ceil((min - kEdge - mechanical) / 360.0);
    const double last = std::floor((max + kEdge - mechanical) / 360.0);
    if (first > last) {
        return std::nullopt;
    }
    const double nearest = std::clamp(std::round((current - mechanical) / 360.0), first, last);
    std::optional<double> best;
    for (const double k : {nearest - 1.0, nearest, nearest + 1.0}) {
        if (k < first || k > last) {
            continue;
        }
        const double candidate = std::clamp(mechanical + 360.0 * k, min, max);
        if (!best || std::fabs(candidate - current) < std::fabs(*best - current)) {
            best = candidate;
        }
    }
    return best;
}

}  // namespace detail

class OnStepRotatorDriver : public RotatorDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    OnStepRotatorDriver(int device_number, ConnectionInfo info, util::ConnectionResolver<ConnectionInfo> resolver,
                        util::TaskClock& clock)
        : AsyncConnectable("OnStep"),
          clock_(clock),
          device_number_(device_number),
          connection_info_(std::move(info)),
          connection_resolver_(std::move(resolver)),
          state_(
              clock_, state_poll_generation_, {kStatePollPeriod, kFaultThreshold, 2 * kStatePollPeriod, kLogTag},
              [this] { return probe(); }, [this] { lose_shared_link(); }) {}

    ~OnStepRotatorDriver() override {
        shutdown_connection();
        // The poll body goes before the members it reads.
        state_.stop_and_join();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall): most-derived class
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during OnStep rotator destruction: " + std::string(e.what()));
            }
        }
        release_link();
    }

    OnStepRotatorDriver(const OnStepRotatorDriver&) = delete;
    OnStepRotatorDriver& operator=(const OnStepRotatorDriver&) = delete;

    int get_device_number() const override { return device_number_; }
    // Decision 0024: the brand, then the device type (the controller has no model name for a rotator).
    std::string get_name() const override { return "OnStep Rotator"; }
    DeviceType get_device_type() const override { return DeviceType::Rotator; }
    std::string fallback_unique_id() const override { return "OnStep_Rotator_" + std::to_string(device_number_); }
    std::string get_description() const override { return "OnStepX rotator (LX200 protocol)"; }
    std::string get_driver_info() const override { return "AlpacaCore OnStep Rotator Driver"; }
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
        // Orders the transition against a move, Halt or Sync still on its way to the wire.
        std::lock_guard<std::mutex> io(io_mutex_);
        if (connected) {
            if (connected_.load()) {
                teardown_locked();  // a lost link: start from a clean driver
            }
            open_locked();
            ALPACA_LOG_INFO(kLogTag, "OnStep rotator connected");
        } else {
            teardown_locked();
            ALPACA_LOG_INFO(kLogTag, "OnStep rotator disconnected");
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

    // --- Rotator interface ---

    // IRotatorV4: CanReverse must be true and Reverse must be implemented. The firmware has no reverse for its gotos
    // (:rR# reverses only the derotator), so Reverse is the driver's: it mirrors the angle the rotator reports and
    // the angle each move sends, and never reaches the wire.
    bool get_can_reverse() const override { return true; }

    bool get_reverse() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return reverse_;
    }

    void set_reverse(bool reverse) override {
        (void)served_frame();  // NotConnected, or refused while the link is faulted (decision 0009 rule 1)
        std::lock_guard<std::mutex> lock(mutex_);
        reverse_ = reverse;
    }

    bool get_is_moving() const override { return served_frame().moving; }

    double get_mechanical_position() const override {
        const Frame frame = served_frame();
        std::lock_guard<std::mutex> lock(mutex_);
        return mechanical_of_locked(frame.angle);
    }

    double get_position() const override {
        const Frame frame = served_frame();
        std::lock_guard<std::mutex> lock(mutex_);
        return wrap(mechanical_of_locked(frame.angle) + sync_offset_);
    }

    double get_step_size() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!degrees_per_step_) {
            throw AlpacaException("The OnStepX rotator did not report its step size (:rD#)",
                                  AlpacaError::PropertyNotImplemented);
        }
        return *degrees_per_step_;
    }

    double get_target_position() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return has_target_position_ ? target_position_ : 0.0;
    }

    void set_target_position(double position) override {
        validate_angle(position);  // InvalidValue before NotConnected
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = wrap(position);
        has_target_position_ = true;
    }

    // :rQ#. The safety stop: never refused for a link fault, only for a lost link (NotConnected).
    void halt() override {
        ensure_connected();
        std::lock_guard<std::mutex> io(io_mutex_);
        live_link()->rotator_halt();
        // Write-through: IsMoving reads false right after Halt, not after the next poll.
        state_.write([](Frame& frame) { frame.moving = false; });
    }

    // IRotator.Move: relative to the current Position, so relative to the current mechanical angle too.
    void move(double position) override {
        validate_angle(position);
        const Frame frame = served_frame();
        double mechanical = 0.0;
        double target = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mechanical = wrap(mechanical_of_locked(frame.angle) + position);
            target = wrap(mechanical + sync_offset_);
        }
        move_mechanical_to(mechanical, target);
    }

    void move_absolute(double position) override {
        validate_angle(position);
        (void)served_frame();
        double mechanical = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mechanical = wrap(position - sync_offset_);
        }
        move_mechanical_to(mechanical, wrap(position));
    }

    void move_mechanical(double position) override {
        validate_angle(position);
        (void)served_frame();
        double target = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target = wrap(position + sync_offset_);
        }
        move_mechanical_to(wrap(position), target);
    }

    // IRotatorV4 Sync: the offset (Position - MechanicalPosition) that makes Position read `position` here, kept
    // across reconnects and restarts by util::RotatorSyncOffsetStore under the stored UniqueID (decision 0008).
    void sync(double position) override {
        validate_angle(position);
        (void)served_frame();                     // refused while the link is faulted or lost
        const std::string key = get_unique_id();  // not under mutex_: it takes the base's own lock
        std::lock_guard<std::mutex> io(io_mutex_);
        // The angle the rotator is at now, not a frame up to a poll old.
        const auto angle = live_link()->rotator_angle();
        if (!angle) {
            throw AlpacaException("The OnStepX rotator did not answer :rG# for the Sync", AlpacaError::DriverException);
        }
        double offset = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            offset = wrap(wrap(position) - mechanical_of_locked(*angle));
        }
        // IRotatorV4: the offset must persist. A Sync that cannot store it has failed, so it throws and keeps the
        // previous offset (decision 0008).
        if (!util::RotatorSyncOffsetStore::save(key, offset)) {
            throw AlpacaException(
                "OnStep rotator: Sync could not save the offset, so it was not applied. Check that the service can "
                "write config/rotator_sync_offsets.json.",
                AlpacaError::DriverException);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sync_offset_ = offset;
        }
        state_.write([angle = *angle](Frame& frame) { frame.angle = angle; });
    }

    // Test seams.
    void set_poll_paused_for_testing(bool paused) { state_.pause_for_testing(paused); }
    void poll_now_for_testing() { state_.poll_now_for_testing(); }

private:
    // What one poll measured. `angle` is the firmware's (:rG#, signed, inside the travel limits).
    struct Frame {
        double angle = 0.0;
        bool moving = false;
        bool derotating = false;
    };

    // The mechanical angle (0-360) of a firmware angle, mirrored while Reverse is set. Caller holds mutex_.
    double mechanical_of_locked(double angle) const { return wrap(reverse_ ? -angle : angle); }

    // The move every member above ends in: `mechanical` (0-360) mapped into the travel, refused with nothing sent
    // when no angle inside it reaches it, refused while the derotator runs, then :rS and its reply.
    void move_mechanical_to(double mechanical, double target_position) {
        ensure_connected();
        std::lock_guard<std::mutex> io(io_mutex_);
        // Read under io_mutex_, which a Disconnect+Connect holds, so the move uses this connect's limits (decision
        // 0016 rule 6), and the frame's angle as the start of the shorter way.
        const Frame frame = served_frame();
        double min_angle = 0.0;
        double max_angle = 0.0;
        double firmware_target = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            min_angle = min_angle_;
            max_angle = max_angle_;
            firmware_target = wrap(reverse_ ? -mechanical : mechanical);
        }
        const auto destination =
            detail::rotator_angle_within_travel(firmware_target, frame.angle, min_angle, max_angle);
        if (!destination) {
            throw AlpacaException("Rotator mechanical position " + std::to_string(mechanical) +
                                      " is outside the OnStepX rotator's travel (" + std::to_string(min_angle) +
                                      " to " + std::to_string(max_angle) + " degrees)",
                                  AlpacaError::InvalidValue);
        }
        const auto link = live_link();
        bool accepted = false;
        int code = -1;
        {
            // The derotation check, the goto and the :GE# that says why it was refused, with no other device's
            // exchange between them (#2223).
            const auto sequence = link->hold_sequence();
            const auto status = link->rotator_status();
            if (!status) {
                throw AlpacaException("The OnStepX rotator did not answer :rT# before the move",
                                      AlpacaError::DriverException);
            }
            if (status->derotating) {
                throw AlpacaException(
                    "The OnStepX rotator is derotating; turn derotation off on the controller before a move",
                    AlpacaError::InvalidOperation);
            }
            accepted = link->rotator_goto(*destination);
            if (!accepted) {
                code = link->get_last_command_error().value_or(-1);
            }
        }
        if (!accepted && code != 0 && code != 25) {
            throw AlpacaException("Move refused by the controller (" + describe_rotator_error(code) + ")",
                                  AlpacaError::InvalidOperation);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_position_ = target_position;
            has_target_position_ = true;
        }
        // Write-through (decision 0010): IsMoving reads true right after a move that has somewhere to go; the angle
        // stays the measured one until the next poll, which also drops any sample taken before the command.
        const double to = *destination;
        state_.write([to](Frame& f) { f.moving = std::fabs(f.angle - to) > kSameAngleDegrees; });
    }

    // The connect body. Caller holds transition_mutex_ and io_mutex_; no poll runs.
    void open_locked() {
        std::shared_ptr<OnStepProtocolWrapper> link;
        // An auto-detected controller resolves its port here, not in the factory (#659): the telescope's scan, where
        // a port a connected OnStep device holds keeps its place, so the rotator lands on the telescope's link.
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
            // The Sync offset this device saved, under its stored UniqueID (decision 0008); 0 when there is none.
            const double offset = util::RotatorSyncOffsetStore::load(get_unique_id());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sync_offset_ = offset;
            }
            link_.store(link);
            state_.reset();
            // The first frame before Connected reads true (decisions 0010 and 0016); a lost or dropped reply gets
            // one retry inside start().
            if (!state_.start()) {
                state_.stop_and_join();
                throw AlpacaException("The OnStepX rotator did not answer the first state poll",
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

    // Identity, firmware and the rotator itself, through the joined link. Throws the operator-facing refusal.
    void check_controller(OnStepProtocolWrapper& link) {
        const std::string product = link.get_product_name();
        if (product.find("OnStep") == std::string::npos && product.find("On-Step") == std::string::npos) {
            throw AlpacaException("The device on this port is not an OnStep controller (:GVP# answered \"" + product +
                                  "\")");
        }
        const std::string version = link.get_version_number();
        if (!is_onstepx_version(version)) {
            throw AlpacaException("The OnStep Rotator requires OnStepX firmware; the controller reports " +
                                  (version.empty() ? std::string("no version") : "version " + version));
        }
        if (!link.rotator_capability()) {
            throw AlpacaException("OnStepX reports no rotator (:GX98#)");
        }
        const auto min_angle = link.rotator_min_angle();
        const auto max_angle = link.rotator_max_angle();
        if (!min_angle || !max_angle || *max_angle <= *min_angle) {
            throw AlpacaException("The OnStepX rotator did not report its travel (:rI#, :rM#)",
                                  AlpacaError::DriverException);
        }
        const auto degrees_per_step = link.rotator_degrees_per_step();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            min_angle_ = static_cast<double>(*min_angle);
            max_angle_ = static_cast<double>(*max_angle);
            degrees_per_step_ = degrees_per_step;
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

    // Reverse is the client's setting and stays with the driver object across a reconnect; the Sync offset is
    // loaded again at the next connect.
    void clear_connect_state() {
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_.clear();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        min_angle_ = 0.0;
        max_angle_ = 0.0;
        degrees_per_step_.reset();
        sync_offset_ = 0.0;
        target_position_ = 0.0;
        has_target_position_ = false;
    }

    // Lets go of the shared link; the port closes when no other OnStep device holds it. Never under mutex_.
    void release_link() {
        std::shared_ptr<OnStepProtocolWrapper> link = link_.exchange(nullptr);
        link.reset();
    }

    // PolledState's on_lost (decision 0009 rule 2, the iFR precedent): a rotator whose poll has gone unanswered past
    // util::kLinkStalenessBound takes the link down for every device on the controller, as a telescope's stale
    // fault does.
    void lose_shared_link() {
        if (const auto link = link_.load()) {
            link->declare_link_lost(
                "OnStepX rotator has had no reply for " +
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
            throw AlpacaException("OnStep rotator not connected", AlpacaError::NotConnected);
        }
        if (!link_up()) {
            throw AlpacaException("OnStep rotator link lost; set Connected true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    // The held link, or NotConnected when it is gone (a Disconnect that won the race).
    std::shared_ptr<OnStepProtocolWrapper> live_link() const {
        auto link = link_.load();
        if (!link) {
            throw AlpacaException("OnStep rotator not connected", AlpacaError::NotConnected);
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
            throw AlpacaException("OnStep rotator link is down", AlpacaError::NotConnected);
        }
        try {
            const auto status = link->rotator_status();
            if (!status) {
                throw AlpacaException("OnStep rotator status did not read", AlpacaError::DriverException);
            }
            const auto angle = link->rotator_angle();
            if (!angle) {
                throw AlpacaException("OnStep rotator angle did not read", AlpacaError::DriverException);
            }
            return Frame{*angle, status->moving, status->derotating};
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
    // leaf held only to copy the values below, never across an exchange.
    std::mutex transition_mutex_;
    std::mutex io_mutex_;  // orders the moves, Halt and Sync on the wire; never taken by a getter or the poll
    mutable std::mutex mutex_;
    double min_angle_ = 0.0;                  // :rI# at connect; under mutex_
    double max_angle_ = 0.0;                  // :rM# at connect; under mutex_
    std::optional<double> degrees_per_step_;  // :rD# at connect; under mutex_
    double sync_offset_ = 0.0;                // Position - MechanicalPosition (decision 0008); under mutex_
    bool reverse_ = false;                    // the client's Reverse; under mutex_
    double target_position_ = 0.0;            // under mutex_
    bool has_target_position_ = false;        // under mutex_
    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    util::OperationGeneration state_poll_generation_;
    // Last: destroyed first, joining its poll body while the members it reads are alive.
    util::PolledState<Frame> state_;
};

std::unique_ptr<RotatorDriver> create_onstep_rotator(int device_number, const ConnectionInfo& connection_info,
                                                     util::TaskClock& clock) {
    return std::make_unique<OnStepRotatorDriver>(device_number, connection_info,
                                                 util::ConnectionResolver<ConnectionInfo>{}, clock);
}

std::unique_ptr<RotatorDriver> create_onstep_rotator_deferred(int device_number,
                                                              util::ConnectionResolver<ConnectionInfo> resolver,
                                                              util::TaskClock& clock) {
    if (!resolver) {
        throw AlpacaException("OnStep rotator: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<OnStepRotatorDriver>(device_number, ConnectionInfo{}, std::move(resolver), clock);
}

std::unique_ptr<RotatorDriver> create_onstep_rotator_auto(int device_number, int mount_index) {
    // The serial scan runs at connect time (#659), not here.
    return create_onstep_rotator_deferred(device_number,
                                          [mount_index] { return resolve_onstep_serial_auto(mount_index); });
}

void set_onstep_rotator_poll_paused_for_testing(RotatorDriver& driver, bool paused) {
    if (auto* onstep = dynamic_cast<OnStepRotatorDriver*>(&driver)) {
        onstep->set_poll_paused_for_testing(paused);
    }
}

void poll_onstep_rotator_now_for_testing(RotatorDriver& driver) {
    if (auto* onstep = dynamic_cast<OnStepRotatorDriver*>(&driver)) {
        onstep->poll_now_for_testing();
    }
}

}  // namespace alpacacore::vendor::onstep
