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
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace alpacacore::vendor::gemini {

namespace {

// State poll (open-astro#1229, decision 0010), start to start. One poll is
// three request/response exchanges at 9600 baud, so the period leaves the wire
// mostly free for Move/Halt. The freshness bound is two periods; a stale frame
// is still served. Link health follows the #237 rule: kFaultThreshold failed
// polls in a row latch a fault that the getters report until a poll succeeds.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(250);
constexpr auto kSnapshotMaxAge = 2 * kStatePollPeriod;
constexpr int kFaultThreshold = 3;
// A failed temperature read can cost the whole serial timeout while holding the
// wrapper, which Move and Halt would queue behind; ask again after this.
constexpr auto kTemperatureRetry = std::chrono::seconds(30);

}  // namespace

class GeminiFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    GeminiFocuserDriver(int device_number, ConnectionConfig config,
                        util::ConnectionResolver<ConnectionConfig> connection_resolver = {},
                        util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("Gemini"),
          clock_(clock),
          device_number_(device_number),
          config_(std::move(config)),
          connection_resolver_(std::move(connection_resolver)),
          connected_(false),
          protocol_() {}

    ~GeminiFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // Joins the poll body before the members it touches go away; a driver
        // that never finished a connect has no body, so this is a no-op.
        state_poll_.cancel_all_and_join();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Error during focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        return "Gemini Automatic Astro Focuser Pro";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Focuser;
    }

    std::string fallback_unique_id() const override { return "GEMINI_FOCUSER_" + std::to_string(device_number_); }

    std::string get_description() const override {
        return "Gemini Automatic Astro Focuser Pro Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore Gemini Focuser Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Focuser firmware captured at connect; surfaced in the web UI only.
    // Guarded SOLELY by firmware_mutex_ and never consults connected_: firmware_
    // is non-empty only while connected (set at connect, cleared at disconnect,
    // both under firmware_mutex_), so there is no atomic-vs-mutex ordering to get
    // wrong. The only window is the sub-microsecond one on connect (firmware_
    // set before the store(true)); on disconnect it is cleared before the
    // store(false), so a disconnected driver never shows a stale string.
    // Visible to nothing but the web UI's firmware read.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override {
        return connected_.load();
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        // Disconnect synchronously — the operation is trivial (close fd + set flag)
        // and ASCOM clients expect Connected to be false immediately after.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Gemini", "Focuser disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // open-astro#333: the idempotency check below and the protocol_ call
        // that follows it must be one atomic transition. Without this, two
        // HTTP workers could both observe connected_ == false and both reach
        // protocol_.connect(), which (before #1202) assigned the fd unconditionally, so
        // the first descriptor leaks for the life of the process. (It does
        // NOT reset the MCU: the tty is still open, connect_serial() clears
        // HUPCL so DTR stays asserted, and Linux runs port activation only on
        // the 0->1 open count, so a second open() on a live tty raises no DTR
        // edge -- see the DTR/HUPCL note in .github/instructions/gemini.instructions.md.) The window is wide,
        // not a single instruction -- the handshake retry ladder is ~9.1 s in the worst case.
        //
        // Deliberately NOT the base's connection mutex, and not shared with
        // firmware_mutex_ (see its comment below): this serializes
        // set_connected() against other set_connected() calls only, and
        // leaves every getter free rather than parking them behind the
        // handshake. Both sibling Gemini drivers make the same split.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;
        }

        if (connected) {
            // An auto-detected focuser resolves its port here, not in the factory (#659).
            util::connect_resolved(
                config_, connection_resolved_, connection_resolver_,
                [this](const ConnectionConfig& cfg) {
                    try {
                        protocol_.connect(cfg);
                    } catch (const AlpacaException& e) {
                        // Only a vanished node, or a port another connected device now holds (#1202), is stale; a
                        // handshake miss on a present port must not trigger the probe, which resets every CH340 MCU.
                        if (util::serial_endpoint_stale(cfg.serial_port))
                            throw util::StaleEndpoint(e.what(), e.error_code());
                        throw;
                    }
                },
                "Gemini");
            // Cache the focuser firmware once (web UI only — never DriverInfo).
            // firmware_ is exactly what get_device_firmware() reports; a failed
            // query must not fail the connect.
            try {
                int fw = protocol_.get_firmware_version();
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_ = fw > 0 ? std::to_string(fw) : std::string();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Focuser firmware query failed: " + std::string(e.what()));
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            // First frame before Connected reads true, so a getter has an
            // answer the moment Connect returns. A failure leaves the snapshot
            // empty; the getters then report it and the poll fills it later.
            snapshot_.reset();
            {
                std::lock_guard<std::mutex> lock(health_mutex_);
                health_.reset();
                temperature_retry_at_.reset();
            }
            poll_state_once();
            connected_.store(true);
            start_state_poll();
            ALPACA_LOG_INFO("Gemini", "Focuser connected");
        } else {
            // Driver state first, SDK close second (driver-lifecycle.instructions.md, issue #387): a
            // throwing close must not leave the driver reporting connected on
            // a closed port. disconnect() cannot throw today; the order is
            // the contract, not the current wrapper's behaviour.
            // The cached firmware goes first, so the store(false) is never
            // observable beside a stale firmware string.
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
            // Join the poll before the transport closes; its body takes no
            // lock this thread holds (transition_mutex_, firmware_mutex_).
            state_poll_.cancel_all_and_join();
            snapshot_.reset();
            {
                std::lock_guard<std::mutex> lock(health_mutex_);
                health_.reset();
            }
            protocol_.disconnect();
            ALPACA_LOG_INFO("Gemini", "Focuser disconnected");
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name),
                              AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override {
        return false;
    }

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

    bool get_absolute() const override {
        return true;
    }

    bool get_is_moving() const override { return served_frame().moving; }

    int get_max_step() const override {
        ensure_connected();
        return const_cast<GeminiFocuserDriver*>(this)->protocol_.get_max_position();
    }

    int get_max_increment() const override {
        ensure_connected();
        return get_max_step();
    }

    int get_position() const override { return served_frame().position; }

    double get_step_size() const override {
        ensure_connected();
        // TODO: The MyFocuserPro2 protocol does not expose step size in microns.
        // This may vary by mechanical configuration. Return a reasonable default
        // or throw if the focuser cannot report this.
        throw AlpacaException("Step size not available for this focuser",
                              AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        // open-astro#309: the connection check comes first. driver-lifecycle.instructions.md's ASCOM
        // contract precedence rule is that only parameter/range validation
        // precedes it -- every other property throws NotConnected when
        // disconnected, with no early return that skips it. ensure_connected()
        // is a lock-free atomic read, so this costs a getter nothing.
        ensure_connected();
        return true;
    }

    bool get_temp_comp() const override {
        // Was an early return of false while disconnected, which answered a
        // property that should have refused. The try/catch below still covers
        // a transport failure on a CONNECTED focuser.
        ensure_connected();
        try {
            return const_cast<GeminiFocuserDriver*>(this)->protocol_.get_temp_comp_enabled();
        } catch (const std::exception&) {
            return false;
        }
    }

    void set_temp_comp(bool temp_comp) override {
        ensure_connected();
        protocol_.set_temp_comp_enabled(temp_comp);
    }

    double get_temperature() const override {
        const auto frame = served_frame();
        if (!frame.temperature.has_value()) {
            throw AlpacaException("Focuser temperature not available", AlpacaError::DriverException);
        }
        return *frame.temperature;
    }

    void halt() override {
        ensure_connected();
        // Never refused for a link fault: Halt is the safety stop and the
        // blind command costs nothing if the link is down.
        protocol_.halt();
        snapshot_.write([](StateFrame& frame) { frame.moving = false; });
    }

    void move(int position) override {
        ensure_connected();
        throw_if_faulted();
        int max_step = protocol_.get_max_position();
        // ConformU requires graceful clamping, not exceptions
        if (position < 0) {
            position = 0;
        } else if (position > max_step) {
            position = max_step;
        }
        protocol_.move_to(position);
        // Write-through: IsMoving must read true right after a successful
        // Move, not after the next poll (decision 0010, point 3).
        snapshot_.write([](StateFrame& frame) { frame.moving = true; });
    }

private:
    // What one poll measured; position, motion and temperature only.
    struct StateFrame {
        int position = 0;
        bool moving = false;
        std::optional<double> temperature;  // empty: the probe did not answer
    };

    // "<device> communications compromised" (driver-lifecycle.instructions.md, issue #237) while the
    // poll has failed kFaultThreshold times in a row; Connected stays true.
    void throw_if_faulted() const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        if (health_.faulted()) {
            throw AlpacaException("Gemini focuser communications compromised: " + health_.fault(),
                                  AlpacaError::DriverException);
        }
    }

    // The frame the getters serve. A stale frame is still served (decision
    // 0010); a faulted link is not.
    StateFrame served_frame() const {
        ensure_connected();
        throw_if_faulted();
        const auto reading = snapshot_.read();
        if (!reading.has_value()) {
            // A disconnect between the check and the read dropped the frame.
            ensure_connected();
            throw AlpacaException("Focuser state not available yet", AlpacaError::DriverException);
        }
        return reading->value;
    }

    // One poll: three exchanges with no driver lock held (the protocol wrapper
    // serialises them), then publish. A write that landed since begin_poll()
    // makes publish() drop the frame; the next poll publishes.
    void poll_state_once() {
        const auto token = snapshot_.begin_poll();
        StateFrame frame;
        try {
            frame.position = protocol_.get_position();
            frame.moving = protocol_.is_moving();
        } catch (const std::exception&) {
            // The exception text can carry the device's reply; AlpacaCore has
            // no log escaper (CWE-117), so the lines state the fact only.
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.note_failure("no valid reply to the state poll", kFaultThreshold, clock_.now())) {
                ALPACA_LOG_ERROR("Gemini", "Focuser communications compromised: " + health_.fault());
            } else {
                ALPACA_LOG_DEBUG("Gemini", "Focuser state poll failed; the frame keeps its age");
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.on_reply()) {
                ALPACA_LOG_INFO("Gemini", "Focuser communications restored");
            }
        }
        // The temperature is optional: a failed read leaves it empty (the getter
        // reports it) without costing Position and IsMoving, and is not retried
        // for kTemperatureRetry.
        const auto now = clock_.now();
        if (!temperature_retry_at_ || now >= *temperature_retry_at_) {
            try {
                frame.temperature = protocol_.get_temperature();
                temperature_retry_at_.reset();
            } catch (const std::exception&) {
                temperature_retry_at_ = now + kTemperatureRetry;
                ALPACA_LOG_DEBUG("Gemini", "Focuser temperature read failed; retrying in 30 s");
            }
        } else if (const auto last = snapshot_.read(); last && last->value.temperature) {
            frame.temperature = last->value.temperature;  // keep the last good value meanwhile
        }
        snapshot_.publish(token, frame);
    }

    void run_state_poll(util::OperationContext& ctx) {
        if (!ctx.wait_for(kStatePollPeriod)) {
            return;
        }
        for (;;) {
            const auto started = clock_.now();
            poll_state_once();
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
            ALPACA_LOG_ERROR("Gemini", "Focuser state poll did not start");
        }
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
    }

    util::TaskClock& clock_;
    int device_number_;
    ConnectionConfig config_;
    // Set by the by-index factory; empty for an explicit port. connection_resolved_
    // is true once a connect has run the resolver, so a later connect retries
    // that port before scanning again (#659).
    util::ConnectionResolver<ConnectionConfig> connection_resolver_;
    bool connection_resolved_ = false;
    std::atomic<bool> connected_;
    GeminiProtocolWrapper protocol_;
    // Position, motion and temperature served to the getters (open-astro#1229).
    // Declared after clock_ and protocol_: state_poll_ is destroyed first and
    // joins its body while both are still alive.
    mutable util::StateSnapshot<StateFrame> snapshot_{clock_, kSnapshotMaxAge};
    // Link health of the poll (issue #237). health_mutex_ is narrow: held for a
    // counter or a string, never across an exchange, so a getter never waits
    // behind the wire.
    mutable std::mutex health_mutex_;
    util::PolledLinkHealth health_;
    // Poll thread, and connect before it starts; never concurrent.
    std::optional<util::TaskClock::clock::time_point> temperature_retry_at_;
    util::OperationGeneration state_poll_generation_;
    util::AsyncOperation state_poll_{"Gemini focuser state poll", state_poll_generation_, clock_};
    // firmware_ has its OWN mutex, NOT the base's connection mutex:
    // set_connected() runs on the connection thread and caches firmware, while
    // the base joins that thread — sharing one mutex would deadlock.
    mutable std::mutex firmware_mutex_;
    std::string firmware_;  // captured at connect; web-UI only (guarded by firmware_mutex_)

    std::mutex transition_mutex_;  // serializes set_connected() transitions (open-astro#333)
};

std::unique_ptr<FocuserDriver> create_gemini_focuser(int device_number,
                                                      const std::string& serial_port,
                                                      int baud_rate) {
    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<GeminiFocuserDriver>(device_number, std::move(config));
}

std::unique_ptr<FocuserDriver> create_gemini_focuser(int device_number, const std::string& serial_port, int baud_rate,
                                                     util::TaskClock& clock) {
    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<GeminiFocuserDriver>(device_number, std::move(config),
                                                 util::ConnectionResolver<ConnectionConfig>{}, clock);
}

std::unique_ptr<FocuserDriver> create_gemini_focuser_deferred(int device_number,
                                                              util::ConnectionResolver<ConnectionConfig> resolver) {
    if (!resolver) {
        throw AlpacaException("Gemini focuser: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<GeminiFocuserDriver>(device_number, ConnectionConfig{}, std::move(resolver));
}

ConnectionConfig resolve_gemini_focuser_by_index(int focuser_index) {
    auto ports = enumerate_gemini_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("Gemini/MyFocuserPro2 focuser"),
                              AlpacaError::NotConnected);
    }
    if (focuser_index < 0 || focuser_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Focuser index " + std::to_string(focuser_index) +
                              " out of range (detected " + std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }

    const auto& port = ports[static_cast<std::size_t>(focuser_index)];
    // firmware_version is 0 for a port a connected Gemini focuser already
    // holds: the scan counts it without probing (#1202), and the connect that
    // follows is refused with a message naming the holder.
    ALPACA_LOG_INFO("Gemini",
                    "Auto-detected focuser at " + port.port_path +
                        (port.firmware_version > 0 ? " (firmware " + std::to_string(port.firmware_version) + ")"
                                                   : std::string(" (held by a connected focuser, not probed)")));

    ConnectionConfig config;
    config.type = ConnectionType::Serial;
    config.serial_port = port.port_path;
    return config;
}

std::unique_ptr<FocuserDriver> create_gemini_focuser_by_index(int device_number, int focuser_index) {
    // The serial scan runs at connect time (#659), not here.
    return create_gemini_focuser_deferred(device_number,
                                          [focuser_index] { return resolve_gemini_focuser_by_index(focuser_index); });
}

} // namespace alpacacore::vendor::gemini
