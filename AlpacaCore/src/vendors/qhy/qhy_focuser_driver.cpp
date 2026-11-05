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
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>
#include <alpacacore/vendor/qhy/qhy_qfocuser_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
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
// State poll (open-astro#1310, decision 0010), start to start. One poll is one
// position exchange (about 35 ms at 9600 baud); telemetry rides on its own
// cadence below (#2031). The period leaves the wire mostly free for Move and
// Halt. The freshness bound is two periods; the cadence, the fault latch and the
// link-loss rule live in util::PolledState. Every getter reads memory, so a
// burst of reads costs no exchange at all.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(250);
constexpr int kFaultThreshold = 3;
// Telemetry (cmd_id 4) is best effort, on its own cadence (#2031): the firmware answers position
// through a move but gives no telemetry reply for about 10 s after every move command, and a poll
// that needed both latched the fault during every long move. The poll asks for telemetry once per
// kTelemetryPeriod while no move is pending, with a short deadline, and after a miss waits
// kTelemetryBackoff before asking again. The frame keeps the last reading with its own
// measurement time; Temperature refuses only a reading older than the staleness bound.
constexpr auto kTelemetryPeriod = std::chrono::seconds(1);
constexpr auto kTelemetryBackoff = std::chrono::seconds(5);
constexpr int kTelemetryTimeoutMs = 500;
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
          connected_(false),
          state_(
              clock_, state_poll_generation_, {kStatePollPeriod, kFaultThreshold, 2 * kStatePollPeriod, kLogTag},
              [this] { return probe(); }, [this] { protocol_.disconnect(); }) {
        if (settings_.max_step < 1) settings_.max_step = 1;
    }

    ~QhyFocuserDriver() override {
        shutdown_connection();
        // Joins the poll body before the members it touches go away; a driver
        // that never finished a connect has no body, so this is a no-op.
        state_.stop_and_join();
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
    // once a transaction has seen the port die the wrapper latches its lost flag
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
            // answer the moment Connect returns.
            reset_state();
            // A lost or dropped first reply gets one retry. A connect whose
            // retry also publishes nothing fails: Connected true means the
            // getters have a frame to serve (decisions 0010 and 0016).
            if (!state_.start()) {
                state_.stop_and_join();
                reset_state();
                {
                    std::lock_guard<std::mutex> fw_lock(firmware_mutex_);
                    firmware_.clear();
                }
                protocol_.disconnect();
                throw AlpacaException("QHY Q-Focuser did not answer the first state poll",
                                      AlpacaError::DriverException);
            }
            connected_.store(true);
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
        state_.stop_and_join();
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
        const StateFrame frame = served_frame();
        const bool external = settings_.temperature_source != "chip";
        if (frame.telemetry_valid && external && !frame.telemetry.external_probe_present) {
            // The firmware reports about -111 C with nothing in the probe socket (#2032). Checked before the
            // age of the reading, so a client that asks once whether the property is implemented gets one answer.
            throw AlpacaException(
                "No external temperature probe is plugged into the Q-Focuser; plug one in, or set temperatureSource "
                "to chip to report the controller board's sensor",
                AlpacaError::PropertyNotImplemented);
        }
        // Decision 0010 rule 8: a value the device has not supplied is a DriverException, never a guess.
        if (!frame.telemetry_valid) {
            // The first request after the connect missed (a connect within about 10 s of a move); the poll
            // asks again after its hold-off.
            throw AlpacaException(
                "The Q-Focuser has not answered a telemetry request since the connect; it answers none for about "
                "10 s after a move, try again shortly",
                AlpacaError::DriverException);
        }
        // Stale: the firmware has had the bound to answer and did not. A move is exempt (the poll does not ask
        // during one, and a full-range move takes about 40 s), and the bound counts from the end of motion
        // when a move came between, so a long move is not reported as a silent unit the moment it ends.
        if (!frame.moving && clock_.now() - frame.telemetry_idle_since >= util::kLinkStalenessBound) {
            throw AlpacaException(
                "The Q-Focuser has not answered a telemetry request for " +
                    std::to_string(
                        std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) +
                    " s",
                AlpacaError::DriverException);
        }
        return external ? frame.telemetry.external_temperature_c : frame.telemetry.chip_temperature_c;
    }

    void halt() override {
        ensure_connected();
        // Hold status_mutex_ across the wire command so the command and the
        // frame's target update are ordered against a concurrent move()/halt()
        // (Alpaca does not serialize device methods). Lock order is
        // status_mutex_ -> wrapper mutex, and status_mutex_ -> the module's leaf lock.
        // Halt is the safety stop: never refused for a link fault.
        std::lock_guard<std::mutex> lock(status_mutex_);
        protocol_.halt();
        const auto now = clock_.now();
        // Write-through: IsMoving reads false right after Halt, not after the next poll.
        // No target => not moving, whatever position the motor stopped at. The Halt ends the motion
        // the way an arrival does, so Temperature's bound counts from here (the poll's own
        // moving-to-stopped stamp never fires: it reads this frame's moving as already false).
        state_.write([now](StateFrame& frame) {
            frame.target.reset();
            if (frame.moving) frame.telemetry_idle_since = now;
            frame.moving = false;
        });
    }

    void move(int position) override {
        ensure_connected();
        // A target outside 0..MaxStep goes to that limit: ConformU moves below 0 and above MaxStep and expects the
        // focuser to stop there, not an InvalidValue (ascom-contract.instructions.md, issue #2028). The clamp is
        // pure arithmetic, so a faulted link is still refused below with the fault text.
        position = std::clamp(position, 0, std::max(settings_.max_step, 0));
        (void)served_frame();  // refused while the link is faulted or lost
        // Hold status_mutex_ across move_to() so two concurrent moves cannot
        // reach the device in one order while the frame records the other (see
        // halt() for the lock-order note).
        std::lock_guard<std::mutex> lock(status_mutex_);
        protocol_.move_to(position);
        const auto now = clock_.now();
        // Write-through (decision 0010, point 3): IsMoving reads true right
        // after a successful Move that has somewhere to go; Position stays the
        // measured one until the next poll.
        state_.write([position, now](StateFrame& frame) {
            frame.target = position;
            frame.last_change = now;
            frame.moving = frame.position != position;
        });
    }

private:
    // What one poll measured. `telemetry` is the last reading the firmware
    // gave, carried from frame to frame with its own measurement time (#2031).
    // `target` and `last_change` are motion state: they live in the frame so a
    // frame that a move()/halt() write overtook is dropped whole, with its
    // position and these, by PolledState.
    struct StateFrame {
        int position = 0;
        bool moving = false;
        std::optional<int> target;
        util::TaskClock::clock::time_point last_change{};
        QFocuserTelemetry telemetry;
        bool telemetry_valid = false;
        util::TaskClock::clock::time_point telemetry_measured_at{};
        // The later of the reading and the end of the last motion: Temperature's staleness counts from here,
        // because the firmware answers no telemetry during a move and the poll does not ask for it then.
        util::TaskClock::clock::time_point telemetry_idle_since{};
    };

    // The single fault text (decision 0009, point 5): the latched poll fault,
    // kept after the link is lost so the management listing says why.
    std::string get_link_fault() const override {
        auto fault = state_.fault();
        if (!fault.empty()) return fault;
        return state_.link_lost() ? "no answer to the state poll" : std::string();
    }

    // The TimeStamp of DeviceState is when the frame was measured (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override { return state_.age(); }

    // True while the transport is up and the poll has not declared the link lost.
    bool link_up() const { return protocol_.link_alive() && !state_.link_lost(); }

    // The frame the getters serve (decisions 0009 and 0010): stale is served,
    // a faulted link throws DriverException, a lost one NotConnected.
    StateFrame served_frame() const {
        ensure_connected();
        return state_.served().value;
    }

    // The poll's exchanges, on the poll thread with no driver lock held (the
    // protocol wrapper serialises them). A dead transport is a failed probe.
    StateFrame probe() {
        if (!protocol_.link_alive()) {
            throw AlpacaException("Q-Focuser transport is down", AlpacaError::NotConnected);
        }
        StateFrame frame;
        // The position exchange is the poll: its failure is what the fault latch counts.
        const std::int32_t pos = protocol_.get_position();
        // The motion decision reads the target and last change from the held
        // frame and writes the arrival into the new one. A frame a move()/halt()
        // write overtook is dropped whole by PolledState, so a target set
        // meanwhile survives and the next poll decides again from it.
        const auto now = clock_.now();
        const auto previous = state_.read();
        const bool changed = !previous.has_value() || pos != previous->value.position;
        frame.position = pos;
        if (previous.has_value()) {
            frame.target = previous->value.target;
            frame.last_change = changed ? now : previous->value.last_change;
            frame.telemetry = previous->value.telemetry;
            frame.telemetry_valid = previous->value.telemetry_valid;
            frame.telemetry_measured_at = previous->value.telemetry_measured_at;
            frame.telemetry_idle_since = previous->value.telemetry_idle_since;
        } else {
            frame.last_change = now;
        }
        frame.moving = frame.target.has_value() && pos != *frame.target && (now - frame.last_change) < kMotionGrace;
        if (frame.target.has_value() && pos == *frame.target) {
            frame.target.reset();  // arrived
        }
        if (previous.has_value() && previous->value.moving && !frame.moving) {
            frame.telemetry_idle_since = now;  // motion ended: the firmware can answer telemetry again from here
        }
        // Telemetry, best effort (#2031): asked once per period while the motor is not moving (IsMoving,
        // not the target: a stalled or clamped move keeps its target and reads as stopped after the grace)
        // and no miss is being waited out; otherwise the last reading rides along. A miss is not a failed poll.
        const bool due = !frame.telemetry_valid || now - frame.telemetry_measured_at >= kTelemetryPeriod;
        if (due && !frame.moving && now >= telemetry_hold_off_until_) {
            try {
                frame.telemetry = protocol_.get_telemetry(kTelemetryTimeoutMs);
                frame.telemetry_valid = true;
                frame.telemetry_measured_at = now;
                frame.telemetry_idle_since = now;
            } catch (const AlpacaException& e) {
                if (e.error_code() == AlpacaError::NotConnected) throw;  // the link is gone: a failed poll
                telemetry_hold_off_until_ = now + kTelemetryBackoff;
                ALPACA_LOG_DEBUG(kLogTag,
                                 "Q-Focuser telemetry not answered; the position poll goes on and the "
                                 "last reading is kept: " +
                                     std::string(e.what()));
            }
        }
        return frame;
    }

    // At connect and disconnect: drop the frame, the fault and the target. The poll is stopped here.
    void reset_state() {
        state_.reset();  // the target and last change go with the frame
        telemetry_hold_off_until_ = {};
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

    // Orders move() and halt() on the wire. Held across a wire command, so
    // never taken by a getter or the poll.
    std::mutex status_mutex_;
    util::OperationGeneration state_poll_generation_;
    // When the poll may ask for telemetry again after a miss (#2031). Touched by the poll thread and,
    // with the poll stopped, by reset_state(); never read by a getter.
    util::TaskClock::clock::time_point telemetry_hold_off_until_{};
    // Position, motion and telemetry served to the getters (open-astro#1310).
    // Declared after clock_ and protocol_: it is destroyed first and joins its
    // poll body while both are still alive.
    util::PolledState<StateFrame> state_;
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
