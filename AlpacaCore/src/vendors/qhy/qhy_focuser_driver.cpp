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
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>
#include <alpacacore/vendor/qhy/qhy_qfocuser_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::qhy {

namespace {
constexpr const char* kLogTag = "QHY";
// INDI gates the 12 V hold settings on this supply voltage: below it the
// unit is USB-powered and cannot drive the hold current.
constexpr double kHoldVoltageThreshold = 11.5;
// The protocol has no "moving" flag. A move is considered in progress while
// the position is still short of the target AND has changed within this
// window; a stalled or limit-clamped move therefore reads as stopped after
// one grace period instead of forever.
constexpr auto kMotionGrace = std::chrono::milliseconds(1500);
// State poll (open-astro#1310, decision 0010), start to start. One poll is a
// position and a telemetry exchange (about 35 ms each at 9600 baud), so the
// period leaves the wire mostly free for Move and Halt. The freshness bound is
// two periods: a frame older than that is still served, and a frame older than
// util::kLinkStalenessBound is a lost link (decision 0009). The old caches
// refilled on a read after 100 ms (position) and 1 s (telemetry); every getter
// now reads memory, so a burst of reads costs no exchange at all.
// kFaultThreshold failed polls in a row latch the #237 "communications
// compromised" fault, which the getters report until a poll succeeds.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(250);
constexpr auto kSnapshotMaxAge = 2 * kStatePollPeriod;
constexpr int kFaultThreshold = 3;
static_assert(kSnapshotMaxAge < util::kLinkStalenessBound, "freshness must undercut the link-loss bound");
}  // namespace

class QhyFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    QhyFocuserDriver(int device_number, QFocuserConnectionConfig config, QFocuserSettings settings,
                     util::ConnectionResolver<QFocuserConnectionConfig> connection_resolver = {},
                     util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("QHY"),
          clock_(clock),
          device_number_(device_number),
          config_(std::move(config)),
          connection_resolver_(std::move(connection_resolver)),
          settings_(std::move(settings)),
          connected_(false) {
        if (settings_.max_step < 1) settings_.max_step = 1;
    }

    ~QhyFocuserDriver() override {
        shutdown_connection();
        // Joins the poll body before the members it touches go away; a driver
        // that never finished a connect has no body, so this is a no-op.
        state_poll_.cancel_all_and_join();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during Q-Focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "QHY Q-Focuser"; }
    DeviceType get_device_type() const override { return DeviceType::Focuser; }
    std::string fallback_unique_id() const override { return "QHY_QFOCUSER_" + std::to_string(device_number_); }
    std::string get_description() const override { return "QHY Q-Focuser Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore QHY Q-Focuser Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }

    // Firmware date + board revision from the connect handshake; web UI
    // only, never DriverInfo. Own mutex: set on the connection thread while
    // the base may be joining it.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) return std::nullopt;
        return firmware_;
    }

    // Connected follows the serial link (driver-lifecycle.instructions.md, issue #445 shape; #527):
    // once a transaction has seen the port die the wrapper latches link_lost_
    // and releases the fd, so this reads false without a reconnect. Both
    // reads are lock-free; this stays a FAST-timing getter.
    bool get_connected() const override { return connected_.load() && link_up(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override {
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogTag, "Q-Focuser disconnect error: " + std::string(e.what()));
        }
    }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Serialize whole transitions against each other, matching the sibling
        // serial drivers (Gemini focuser open-astro#333, WandererAstro). The
        // router serializes connection ops per device, but the [stress] harness
        // calls set_connected() concurrently from lifecycle threads; without
        // this, two interleaved transitions can leave the wrapper open while
        // the driver reports disconnected, after which every reconnect throws
        // InvalidOperation "already connected". Deliberately NOT the base's
        // connection mutex and not firmware_mutex_ — it guards set_connected()
        // against set_connected() only and leaves the getters free.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // A lost link counts as disconnected here too: Connected=true against
        // a dead port must reconnect instead of taking the idempotency return
        // (issue #527), and Connected=false must still tear the driver down.
        const bool live = connected_.load() && link_up();
        // The disconnect gate takes the raw connected_: a lost link with an
        // async connect in flight must still run the teardown below rather
        // than be recorded as pending and skipped (review of #531). The
        // connect gate takes the link-aware value, so a stale connection is
        // treated as down and reconnects.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(live)) return;
        // Idempotency is direction-specific: a connect is a no-op only while
        // the link is live, but a disconnect is a no-op only when nothing was
        // ever connected. A lost link leaves connected_ true with the
        // firmware cache populated (the management endpoint renders it
        // without a Connected check), so Connected=false must still tear down.
        if (connected ? live : !connected_.load()) return;

        if (connected) {
            if (connected_.load()) {
                // Stale connection over a lost link: drop the old state first
                // so the reconnect below starts from a clean driver.
                teardown_locked();
            }
            // An auto-detected focuser resolves its port here, not in the factory (#659).
            QFocuserDeviceInfo info;
            util::connect_resolved(
                config_, connection_resolved_, connection_resolver_,
                [this, &info](const QFocuserConnectionConfig& cfg) {
                    try {
                        info = protocol_.connect(cfg);
                    } catch (const AlpacaException& e) {
                        // Only a vanished node, or a port another connected device now holds (#1202), is stale; a
                        // handshake miss on a present port is not.
                        if (util::serial_endpoint_stale(cfg.serial_port))
                            throw util::StaleEndpoint(e.what(), e.error_code());
                        throw;
                    }
                },
                kLogTag);
            try {
                apply_settings();
            } catch (...) {
                protocol_.disconnect();
                throw;
            }
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_ = std::to_string(info.firmware);
                if (info.board_version > 0) firmware_ += " (board " + std::to_string(info.board_version) + ")";
            }
            // First frame before Connected reads true, so a getter has an
            // answer the moment Connect returns. A failure leaves the snapshot
            // empty; the getters then report it and the poll fills it later.
            reset_state();
            link_lost_.store(false);
            poll_state_once();
            connected_.store(true);
            start_state_poll();
            ALPACA_LOG_INFO(kLogTag, "Q-Focuser connected (firmware " + std::to_string(info.firmware) + ")");
        } else {
            // Driver state first, port close second (driver-lifecycle.instructions.md, issue #387): a
            // throwing close must not leave the driver reporting connected on a
            // closed port. disconnect() cannot throw today; the order is the
            // contract, not the current wrapper's behaviour. Clear firmware
            // first so the store(false) is never observable beside a stale
            // firmware string.
            teardown_locked();
            ALPACA_LOG_INFO(kLogTag, "Q-Focuser disconnected");
        }
    }

    // Disconnect body shared by the explicit disconnect and the stale-link
    // reconnect. Caller holds transition_mutex_.
    void teardown_locked() {
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_.clear();
        }
        connected_.store(false);
        // Join the poll before the transport closes; its body takes no lock
        // this thread holds (transition_mutex_, firmware_mutex_).
        state_poll_.cancel_all_and_join();
        protocol_.disconnect();
        reset_state();
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
        return settings_.max_step;
    }
    int get_max_increment() const override {
        ensure_connected();
        return settings_.max_step;
    }

    int get_position() const override { return served_frame().position; }

    double get_step_size() const override {
        // Step size in microns depends on the drawtube the motor drives; the
        // protocol does not expose it. Connection check first (open-astro#309).
        ensure_connected();
        throw AlpacaException("Step size not available for this focuser", AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        ensure_connected();
        return false;
    }
    bool get_temp_comp() const override {
        ensure_connected();
        return false;
    }
    void set_temp_comp(bool) override {
        ensure_connected();
        throw AlpacaException("Temperature compensation not supported", AlpacaError::NotImplemented);
    }

    double get_temperature() const override {
        const QFocuserTelemetry t = served_frame().telemetry;
        return settings_.temperature_source == "chip" ? t.chip_temperature_c : t.external_temperature_c;
    }

    void halt() override {
        ensure_connected();
        // Hold status_mutex_ across the wire command so the command and the
        // target_ update are atomic against a concurrent move()/halt() (Alpaca
        // does not serialize device methods). Lock order is status_mutex_ ->
        // wrapper mutex, and status_mutex_ -> the snapshot's leaf lock.
        // Halt is the safety stop: never refused for a link fault.
        std::lock_guard<std::mutex> lock(status_mutex_);
        protocol_.halt();
        target_.reset();  // no target => not moving, whatever position the motor stopped at
        // Write-through: IsMoving reads false right after Halt, not after the next poll.
        snapshot_.write([](StateFrame& frame) { frame.moving = false; });
    }

    void move(int position) override {
        ensure_connected();
        if (position < 0 || position > settings_.max_step) {
            throw AlpacaException("Focuser position out of range", AlpacaError::InvalidValue);
        }
        throw_if_faulted();
        // Hold status_mutex_ across move_to() so two concurrent moves cannot
        // reach the device in one order while target_ records the other (see
        // halt() for the lock-order note).
        std::lock_guard<std::mutex> lock(status_mutex_);
        protocol_.move_to(position);
        target_ = position;
        last_change_ = clock_.now();
        // Write-through (decision 0010, point 3): IsMoving reads true right
        // after a successful Move that has somewhere to go; Position stays the
        // measured one until the next poll.
        snapshot_.write([position](StateFrame& frame) { frame.moving = frame.position != position; });
    }

private:
    // What one poll measured. `telemetry` is refreshed with every poll.
    struct StateFrame {
        int position = 0;
        bool moving = false;
        QFocuserTelemetry telemetry;
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

    // True while the transport is up and the poll has not declared the link lost.
    bool link_up() const { return protocol_.link_alive() && !link_lost_.load(); }

    // "<device> communications compromised" (driver-lifecycle.instructions.md, issue #237) while the
    // poll has failed kFaultThreshold times in a row; Connected stays true.
    void throw_if_faulted() const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        if (health_.faulted()) {
            throw AlpacaException("QHY Q-Focuser communications compromised: " + health_.fault(),
                                  AlpacaError::DriverException);
        }
    }

    // The frame the getters serve. A stale frame is still served (decision
    // 0010), until it is older than util::kLinkStalenessBound: then the link
    // is lost (decision 0009) and the getter says NotConnected. A faulted
    // link is not served.
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

    // One poll: two exchanges with no driver lock held (the protocol wrapper
    // serialises them), then publish. A write that landed since begin_poll()
    // makes publish() drop the frame; the next poll publishes.
    void poll_state_once() {
        const auto token = snapshot_.begin_poll();
        StateFrame frame;
        std::int32_t pos = 0;
        try {
            pos = protocol_.get_position();
            frame.telemetry = protocol_.get_telemetry();
        } catch (const std::exception&) {
            // The exception text can carry the device's reply; AlpacaCore has
            // no log escaper (CWE-117), so the lines state the fact only.
            note_poll_failure();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.on_reply()) {
                ALPACA_LOG_INFO(kLogTag, "Q-Focuser communications restored");
            }
        }
        // status_mutex_ orders this decision against move()/halt(), which hold
        // it across their wire command and snapshot write. The motion state
        // (target_, last_change_) is committed only when the frame is
        // published: a frame a write overtook is dropped whole.
        std::lock_guard<std::mutex> lock(status_mutex_);
        const auto now = clock_.now();
        const auto previous = snapshot_.read();
        const bool changed = !previous.has_value() || pos != previous->value.position;
        const auto changed_at = changed ? now : last_change_;
        frame.position = pos;
        frame.moving = target_.has_value() && pos != *target_ && (now - changed_at) < kMotionGrace;
        if (snapshot_.publish(token, frame)) {
            last_change_ = changed_at;
            if (target_.has_value() && pos == *target_) {
                target_.reset();  // arrived
            }
        }
    }

    void note_poll_failure() {
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.note_failure("no valid reply to the state poll", kFaultThreshold, clock_.now())) {
                ALPACA_LOG_ERROR(kLogTag, "Q-Focuser communications compromised: " + health_.fault());
            } else {
                ALPACA_LOG_DEBUG(kLogTag, "Q-Focuser state poll failed; the frame keeps its age");
            }
            lost = health_.fault_stale(clock_.now());
        }
        if (!lost) {
            const auto reading = snapshot_.read();
            lost = reading.has_value() && clock_.now() - reading->measured_at >= util::kLinkStalenessBound;
        }
        if (lost && !link_lost_.exchange(true)) {
            ALPACA_LOG_ERROR(
                kLogTag, "Q-Focuser link lost: no answer to the state poll for " +
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
            ALPACA_LOG_ERROR(kLogTag, "Q-Focuser state poll did not start");
        }
    }

    // At connect and disconnect: drop the frame, the fault and the target.
    void reset_state() {
        snapshot_.reset();
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            health_.reset();
        }
        std::lock_guard<std::mutex> lock(status_mutex_);
        target_.reset();
        last_change_ = {};
    }

    // Push the user's settings to the firmware right after the handshake.
    // Reverse and speed always; the hold settings only on a 12 V supply.
    void apply_settings() {
        protocol_.set_reverse(settings_.reverse);
        protocol_.set_speed(settings_.speed);

        const QFocuserTelemetry t = protocol_.get_telemetry();
        if (t.voltage_v > kHoldVoltageThreshold) {
            protocol_.set_hold_current(settings_.hold_ihold, settings_.hold_irun);
            protocol_.set_hold_force(settings_.hold_force);
        } else if (settings_.hold_force) {
            ALPACA_LOG_WARN(kLogTag, "Q-Focuser supply is " + std::to_string(t.voltage_v) +
                                         " V; hold force needs a 12 V supply and was not enabled");
        }
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
        if (!link_up()) {
            throw AlpacaException("Focuser link lost; set Connected false then true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    util::TaskClock& clock_;
    int device_number_;
    QFocuserConnectionConfig config_;
    // Set by the by-index factory; empty for an explicit port. connection_resolved_
    // is true once a connect has run the resolver, so a later connect retries
    // that port before scanning again (#659).
    util::ConnectionResolver<QFocuserConnectionConfig> connection_resolver_;
    bool connection_resolved_ = false;
    QFocuserSettings settings_;
    std::atomic<bool> connected_;
    QFocuserProtocolWrapper protocol_;

    // Serializes whole connect/disconnect transitions; see set_connected().
    // Separate from firmware_mutex_/status_mutex_ and the base connection mutex.
    std::mutex transition_mutex_;
    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    // Orders the poll's motion decision against move()/halt(); guards target_
    // and last_change_. Held across a wire command, so never taken by a getter.
    std::mutex status_mutex_;
    std::optional<int> target_;
    util::TaskClock::clock::time_point last_change_{};
    // Latched when the state poll has gone unanswered past
    // util::kLinkStalenessBound (decision 0009); cleared at connect.
    mutable std::atomic<bool> link_lost_{false};
    // Position, motion and telemetry served to the getters (open-astro#1310).
    // Declared after clock_ and protocol_: state_poll_ is destroyed first and
    // joins its body while both are still alive.
    mutable util::StateSnapshot<StateFrame> snapshot_{clock_, kSnapshotMaxAge};
    // Link health of the poll (issue #237). health_mutex_ is narrow: held for a
    // counter or a string, never across an exchange.
    mutable std::mutex health_mutex_;
    util::PolledLinkHealth health_;
    util::OperationGeneration state_poll_generation_;
    util::AsyncOperation state_poll_{"QHY focuser state poll", state_poll_generation_, clock_};
};

std::unique_ptr<FocuserDriver> create_qhy_focuser(int device_number, const std::string& serial_port,
                                                  const QFocuserSettings& settings) {
    QFocuserConnectionConfig config;
    config.serial_port = serial_port;
    return std::make_unique<QhyFocuserDriver>(device_number, std::move(config), settings);
}

std::unique_ptr<FocuserDriver> create_qhy_focuser(int device_number, const std::string& serial_port,
                                                  const QFocuserSettings& settings, util::TaskClock& clock) {
    QFocuserConnectionConfig config;
    config.serial_port = serial_port;
    return std::make_unique<QhyFocuserDriver>(device_number, std::move(config), settings,
                                              util::ConnectionResolver<QFocuserConnectionConfig>{}, clock);
}

std::unique_ptr<FocuserDriver> create_qhy_focuser_deferred(int device_number,
                                                           util::ConnectionResolver<QFocuserConnectionConfig> resolver,
                                                           const QFocuserSettings& settings) {
    if (!resolver) {
        throw AlpacaException("QHY Q-Focuser: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<QhyFocuserDriver>(device_number, QFocuserConnectionConfig{}, settings, std::move(resolver));
}

QFocuserConnectionConfig resolve_qhy_focuser_by_index(int focuser_index) {
    auto ports = enumerate_qfocuser_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("QHY Q-Focuser"), AlpacaError::NotConnected);
    }
    if (focuser_index < 0 || focuser_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Focuser index " + std::to_string(focuser_index) + " out of range (detected " +
                                  std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }
    const auto& port = ports[static_cast<std::size_t>(focuser_index)];
    ALPACA_LOG_INFO(kLogTag, "Auto-detected Q-Focuser at " + port.port_path + " (firmware " +
                                 std::to_string(port.info.firmware) + ")");
    QFocuserConnectionConfig config;
    config.serial_port = port.port_path;
    return config;
}

std::unique_ptr<FocuserDriver> create_qhy_focuser_by_index(int device_number, int focuser_index,
                                                           const QFocuserSettings& settings) {
    // The serial scan runs at connect time (#659), not here.
    return create_qhy_focuser_deferred(
        device_number, [focuser_index] { return resolve_qhy_focuser_by_index(focuser_index); }, settings);
}

}  // namespace alpacacore::vendor::qhy
