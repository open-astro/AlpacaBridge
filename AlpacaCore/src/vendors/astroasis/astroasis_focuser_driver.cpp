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

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::astroasis {

namespace {

// State poll (open-astro#1327, decision 0010), start to start. One poll is one
// 0x32 exchange, which fills position, motion and temperature. Getters read the
// snapshot and make no exchange. The freshness bound is two periods: a frame
// older than that is still served, and one older than util::kLinkStalenessBound
// is a lost link (decision 0009). kFaultThreshold failed polls in a row latch
// the #237 "communications compromised" fault, which the getters report until a
// poll succeeds.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(100);
constexpr auto kSnapshotMaxAge = 2 * kStatePollPeriod;
constexpr int kFaultThreshold = 3;
static_assert(kSnapshotMaxAge < util::kLinkStalenessBound, "freshness must undercut the link-loss bound");

}  // namespace

class AstroasisFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    AstroasisFocuserDriver(int device_number, std::string hid_path,
                           util::ConnectionResolver<std::string> connection_resolver = {},
                           std::unique_ptr<AstroasisHidTransport> transport = nullptr,
                           util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("Astroasis"),
          clock_(clock),
          device_number_(device_number),
          hid_path_(std::move(hid_path)),
          connection_resolver_(std::move(connection_resolver)),
          connected_(false),
          protocol_(std::move(transport)) {}

    ~AstroasisFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // Joins the poll body before the members it touches go away; a driver
        // that never finished a connect has no body, so this is a no-op.
        state_poll_.cancel_all_and_join();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Astroasis", "Error during focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "Astroasis Oasis Focuser"; }

    DeviceType get_device_type() const override { return DeviceType::Focuser; }

    std::string get_unique_id() const override { return "ASTROASIS_FOCUSER_" + std::to_string(device_number_); }

    std::string get_description() const override { return "Astroasis Oasis Focuser Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore Astroasis Focuser Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }

    // Lock-free: two atomic reads, so a Connected poll landing during an
    // in-flight connect answers at once instead of waiting out the ~1.1 s HID
    // handshake. A poll that has gone unanswered for util::kLinkStalenessBound
    // latches link_lost_ (decision 0009), and Connected reads false without a
    // reconnect.
    bool get_connected() const override { return connected_.load() && !link_lost_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously — closing the HID handle is trivial and
        // ASCOM clients expect Connected to be false immediately after. The
        // close does take hid_global_mutex(), so it can queue behind a
        // concurrent by-index enumeration's bus scan (bounded; see accepted
        // cost (1) on that mutex in astroasis_protocol_wrapper.cpp) — but
        // connected_ is stored false before it, so the flip never waits on
        // THAT lock. It can still wait on this driver's transition_mutex_ behind a connect
        // handshake, which can itself be queued on hid_global_mutex() behind a
        // concurrent enumeration's bus scan. Getters never take
        // transition_mutex_ and never touch the device.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Astroasis", "Focuser disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Serializes whole transitions against each other (AsyncConnectable
        // obligations 4/5): this driver's disconnect() calls set_connected
        // synchronously, outside the task machinery, and without the lock two
        // set_connected(true) calls can both observe connected_==false and
        // both reach protocol_.connect(), leaking the first HID handle. Only
        // set_connected() takes it; no getter, move or halt does, and the poll
        // body takes no lock this thread holds across its join.
        std::lock_guard<std::mutex> lock(transition_mutex_);
        // A lost link counts as disconnected for a connect: Connected=true
        // against a lost link reconnects instead of taking the idempotency
        // return. The disconnect gate takes the raw connected_, so Connected=false
        // still tears down a lost link (QHY Q-Focuser shape, decision 0009).
        const bool live = connected_.load() && !link_lost_.load();
        // See AsyncConnectable's class comment for why these gates run before
        // the idempotency check.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(live)) {
            return;
        }
        if (connected ? live : !connected_.load()) {
            return;
        }

        if (connected) {
            if (connected_.load()) {
                // Stale connection over a lost link: start from a clean driver.
                teardown_locked();
            }
            // An auto-detected focuser resolves its HID path here, not in the factory (#659).
            util::connect_resolved(
                hid_path_, connection_resolved_, connection_resolver_,
                [this](const std::string& path) {
                    try {
                        protocol_.connect(path);
                    } catch (const AlpacaException& e) {
                        // Only a vanished hidraw node is stale; a handshake miss on a present one is not.
                        if (util::device_node_missing(path)) throw util::StaleEndpoint(e.what(), e.error_code());
                        throw;
                    }
                },
                "Astroasis");
            // MaxStep is a device setting that does not change while connected:
            // read it once here so the getters and move() never go to the device
            // for it. A failure here must not leave the handle open.
            try {
                max_step_.store(protocol_.get_max_step());
            } catch (...) {
                protocol_.disconnect();
                throw;
            }
            // First frame before Connected reads true, so a getter has an
            // answer the moment Connect returns. A failure leaves the snapshot
            // empty; the getters then report it and the poll fills it later.
            reset_state();
            link_lost_.store(false);
            poll_state_once();
            connected_.store(true);
            start_state_poll();
            ALPACA_LOG_INFO("Astroasis", "Focuser connected");
        } else {
            teardown_locked();
            ALPACA_LOG_INFO("Astroasis", "Focuser disconnected");
        }
    }

    // The disconnect body, shared by the explicit disconnect and the
    // stale-link reconnect. Caller holds transition_mutex_.
    void teardown_locked() {
        // Clear driver state BEFORE the SDK close (AGENTS.md concurrency
        // checklist): a throwing close must not leave the driver
        // half-connected.
        connected_.store(false);
        max_step_.store(0);
        // Join the poll before the transport closes; its body takes no lock
        // this thread holds.
        state_poll_.cancel_all_and_join();
        reset_state();
        link_lost_.store(false);
        protocol_.disconnect();
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
        return max_step_.load();
    }

    int get_max_increment() const override {
        ensure_connected();
        return max_step_.load();
    }

    int get_position() const override { return served_frame().position; }

    double get_step_size() const override {
        // No transition_mutex_: touches nothing it guards -- ensure_connected() is
        // already a lock-free atomic read, and every path here throws
        // unconditionally, so taking the lock would only add a stall behind
        // an in-flight connect's full HID handshake for no protection.
        ensure_connected();
        // TODO: the vendor protocol does not expose step size in microns
        // (mechanical, varies by focuser model).
        throw AlpacaException("Step size not available for this focuser", AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        // open-astro#309: the connection check comes first. AGENTS.md's ASCOM
        // contract precedence rule is that only parameter/range validation
        // precedes it -- every other property throws NotConnected when
        // disconnected, with no early return that skips it. ensure_connected()
        // is a lock-free atomic read, so this costs a getter nothing.
        ensure_connected();
        return false;
    }

    bool get_temp_comp() const override {
        ensure_connected();
        return false;
    }

    void set_temp_comp(bool) override {
        // No transition_mutex_: same reasoning as get_step_size() above.
        ensure_connected();
        throw AlpacaException("Temperature compensation not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_temperature() const override { return served_frame().temperature; }

    void halt() override {
        // No transition_mutex_: a command may reach the device while a disconnect runs. Accepted:
        // the wrapper's Impl::mutex_ and require_connected() order it before the close or make it
        // throw NotConnected.
        ensure_connected();
        // Never refused for a link fault: Halt is the safety stop, and the wrapper
        // serialises the exchange. The write drops a poll in flight whatever the
        // outcome: a halt whose reply was lost may still have stopped the device,
        // and the next poll measures which.
        try {
            protocol_.stop_move();
        } catch (...) {
            snapshot_.write([](StateFrame&) {});
            throw;
        }
        // Write-through (decision 0010, point 3): IsMoving reads false now, not after the next poll.
        snapshot_.write([](StateFrame& frame) { frame.moving = false; });
    }

    void move(int position) override {
        // No transition_mutex_: see halt().
        ensure_connected();
        throw_if_faulted();
        const int max_step = max_step_.load();
        // ConformU requires graceful clamping, not exceptions.
        if (position < 0) {
            position = 0;
        } else if (position > max_step) {
            position = max_step;
        }
        try {
            protocol_.move_to(position);
        } catch (...) {
            snapshot_.write([](StateFrame&) {});  // the device may have moved; the next poll measures it
            throw;
        }
        // Write-through: IsMoving must read true right after a successful Move,
        // not after the next poll. Position stays the measured one.
        snapshot_.write([position](StateFrame& frame) { frame.moving = frame.position != position; });
    }

private:
    // What one poll measured: position, motion and the temperature the getter reports.
    struct StateFrame {
        int position = 0;
        bool moving = false;
        double temperature = 0.0;
    };

    // The single fault text (decision 0009, point 5): the latched poll fault,
    // kept after the link is lost so the management listing says why.
    std::string get_link_fault() const override {
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.faulted()) return health_.fault();
        }
        return link_lost_.load() ? "no answer to the state poll" : std::string();
    }

    // The TimeStamp of DeviceState is when the frame was measured (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        const auto reading = snapshot_.read();
        if (!reading.has_value()) return std::nullopt;
        return clock_.now() - reading->measured_at;
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
        if (link_lost_.load()) {
            throw AlpacaException("Focuser link lost; set Connected false then true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    // "<device> communications compromised" (AGENTS.md, issue #237) while the
    // poll has failed kFaultThreshold times in a row; Connected stays true.
    void throw_if_faulted() const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        if (health_.faulted()) {
            throw AlpacaException("Astroasis focuser communications compromised: " + health_.fault(),
                                  AlpacaError::DriverException);
        }
    }

    // The frame the getters serve. A stale frame is still served (decision
    // 0010), until it is older than util::kLinkStalenessBound: then the link is
    // lost (decision 0009) and the getter says NotConnected. A faulted link is
    // not served.
    StateFrame served_frame() const {
        ensure_connected();
        const auto reading = snapshot_.read();
        if (reading.has_value() && clock_.now() - reading->measured_at >= util::kLinkStalenessBound) {
            link_lost_.store(true);
            ensure_connected();
        }
        throw_if_faulted();
        if (!reading.has_value()) {
            // A disconnect between the check and the read dropped the frame.
            ensure_connected();
            throw AlpacaException("Focuser state not available yet", AlpacaError::DriverException);
        }
        return reading->value;
    }

    // One poll: one exchange with no driver lock held (the protocol wrapper
    // serialises it), then publish. A write that landed since begin_poll()
    // makes publish() drop the frame; the next poll publishes.
    void poll_state_once() {
        const auto token = snapshot_.begin_poll();
        StateFrame frame;
        try {
            const auto status = protocol_.get_status();
            frame.position = status.position;
            frame.moving = status.moving;
            frame.temperature =
                status.temperature_external_valid ? status.temperature_external : status.temperature_internal;
        } catch (const std::exception&) {
            // The exception text can carry the device's reply; AlpacaCore has
            // no log escaper (CWE-117), so the lines state the fact only.
            note_poll_failure();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.on_reply()) {
                ALPACA_LOG_INFO("Astroasis", "Focuser communications restored");
            }
        }
        snapshot_.publish(token, frame);
    }

    void note_poll_failure() {
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.note_failure("no valid reply to the state poll", kFaultThreshold, clock_.now())) {
                ALPACA_LOG_ERROR("Astroasis", "Focuser communications compromised: " + health_.fault());
            } else {
                ALPACA_LOG_DEBUG("Astroasis", "Focuser state poll failed; the frame keeps its age");
            }
            lost = health_.fault_stale(clock_.now());
        }
        if (!lost) {
            const auto reading = snapshot_.read();
            lost = reading.has_value() && clock_.now() - reading->measured_at >= util::kLinkStalenessBound;
        }
        if (lost && !link_lost_.exchange(true)) {
            ALPACA_LOG_ERROR(
                "Astroasis",
                "Focuser link lost: no answer to the state poll for " +
                    std::to_string(
                        std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) +
                    " s; set Connected false then true to reconnect");
        }
    }

    void run_state_poll(util::OperationContext& ctx) {
        if (!ctx.wait_for(kStatePollPeriod)) {
            return;
        }
        for (;;) {
            const auto started = clock_.now();
            poll_state_once();
            if (link_lost_.load()) {
                // Lost: release the transport (decision 0009, point 3) and end;
                // no autonomous reconnect (point 4).
                protocol_.disconnect();
                return;
            }
            // Start to start: skip any tick a slow poll ran over (no catch-up).
            const auto spent = clock_.now() - started;
            const auto ticks = spent / kStatePollPeriod + 1;
            if (!ctx.wait_for(ticks * kStatePollPeriod - spent)) {
                return;
            }
        }
    }

    // After the store(true), with no driver lock held (AsyncOperation rule 10).
    void start_state_poll() {
        try {
            state_poll_.start([this](util::OperationContext& ctx) { run_state_poll(ctx); });
        } catch (const std::exception&) {
            ALPACA_LOG_ERROR("Astroasis", "Focuser state poll did not start");
        }
    }

    // At connect and disconnect: drop the frame and the fault.
    void reset_state() {
        snapshot_.reset();
        std::lock_guard<std::mutex> lock(health_mutex_);
        health_.reset();
    }

    util::TaskClock& clock_;
    int device_number_;
    std::string hid_path_;
    // Set by the by-index factory; empty for an explicit path. connection_resolved_
    // is true once a connect has run the resolver, so a later connect retries
    // that path before scanning again (#659).
    util::ConnectionResolver<std::string> connection_resolver_;
    bool connection_resolved_ = false;
    std::atomic<bool> connected_;
    // Latched when the state poll has gone unanswered past
    // util::kLinkStalenessBound (decision 0009); cleared at connect and disconnect.
    mutable std::atomic<bool> link_lost_{false};
    AstroasisProtocolWrapper protocol_;
    std::atomic<int> max_step_{0};  // read at connect, cleared at disconnect
    // Position, motion and temperature served to the getters (open-astro#1327).
    // Declared after clock_ and protocol_: state_poll_ is destroyed first and
    // joins its body while both are still alive.
    mutable util::StateSnapshot<StateFrame> snapshot_{clock_, kSnapshotMaxAge};
    // Link health of the poll (issue #237). health_mutex_ is narrow: held for a
    // counter or a string, never across an exchange.
    mutable std::mutex health_mutex_;
    util::PolledLinkHealth health_;
    util::OperationGeneration state_poll_generation_;
    util::AsyncOperation state_poll_{"Astroasis focuser state poll", state_poll_generation_, clock_};
    // Serializes whole connect/disconnect transitions; see set_connected().
    std::mutex transition_mutex_;
};

std::unique_ptr<FocuserDriver> create_astroasis_focuser(int device_number, const std::string& hid_path) {
    return std::make_unique<AstroasisFocuserDriver>(device_number, hid_path);
}

std::unique_ptr<FocuserDriver> create_astroasis_focuser(int device_number, const std::string& hid_path,
                                                        std::unique_ptr<AstroasisHidTransport> transport) {
    return std::make_unique<AstroasisFocuserDriver>(device_number, hid_path, util::ConnectionResolver<std::string>{},
                                                    std::move(transport));
}

std::unique_ptr<FocuserDriver> create_astroasis_focuser(int device_number, const std::string& hid_path,
                                                        std::unique_ptr<AstroasisHidTransport> transport,
                                                        util::TaskClock& clock) {
    return std::make_unique<AstroasisFocuserDriver>(device_number, hid_path, util::ConnectionResolver<std::string>{},
                                                    std::move(transport), clock);
}

std::unique_ptr<FocuserDriver> create_astroasis_focuser_deferred(int device_number,
                                                                 util::ConnectionResolver<std::string> resolver) {
    if (!resolver) {
        throw AlpacaException("Astroasis focuser: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<AstroasisFocuserDriver>(device_number, std::string{}, std::move(resolver));
}

std::string resolve_astroasis_focuser_by_index(int focuser_index) {
    auto ports = enumerate_astroasis_focusers();
    if (ports.empty()) {
        throw AlpacaException("No Astroasis Oasis Focuser detected on the USB bus", AlpacaError::NotConnected);
    }
    if (focuser_index < 0 || focuser_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Focuser index " + std::to_string(focuser_index) + " out of range (detected " +
                                  std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }

    const auto& port = ports[static_cast<std::size_t>(focuser_index)];
    ALPACA_LOG_INFO("Astroasis", "Auto-detected focuser at " + port.hid_path);
    return port.hid_path;
}

std::unique_ptr<FocuserDriver> create_astroasis_focuser_by_index(int device_number, int focuser_index) {
    // The USB HID scan runs at connect time (#659), not here.
    return create_astroasis_focuser_deferred(
        device_number, [focuser_index] { return resolve_astroasis_focuser_by_index(focuser_index); });
}

}  // namespace alpacacore::vendor::astroasis
