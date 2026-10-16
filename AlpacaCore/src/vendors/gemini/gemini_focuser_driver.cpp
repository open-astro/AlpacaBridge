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
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/async_operation.h>
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

// State poll (open-astro#1229, decision 0010). One poll is three request/
// response exchanges at 9600 baud, so the period leaves the wire mostly free
// for Move/Halt. The freshness bound is two periods; a stale frame is still
// served, only one older than util::kLinkStalenessBound is refused.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(250);
constexpr auto kSnapshotMaxAge = 2 * kStatePollPeriod;

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

    std::string get_unique_id() const override {
        return "GEMINI_FOCUSER_" + std::to_string(device_number_);
    }

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
        // protocol_.connect(), which assigns serial_fd_ unconditionally, so
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
                        // Only a vanished node is stale; a handshake miss on a present
                        // port must not trigger the probe, which resets every CH340 MCU.
                        if (util::device_node_missing(cfg.serial_port))
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
            temperature_unavailable_ = false;
            poll_state_once();
            connected_.store(true);
            start_state_poll();
            ALPACA_LOG_INFO("Gemini", "Focuser connected");
        } else {
            // Driver state first, SDK close second (AGENTS.md, issue #387): a
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

    bool get_is_moving() const override {
        return served_frame().moving;
    }

    int get_max_step() const override {
        ensure_connected();
        return const_cast<GeminiFocuserDriver*>(this)->protocol_.get_max_position();
    }

    int get_max_increment() const override {
        ensure_connected();
        return get_max_step();
    }

    int get_position() const override {
        return served_frame().position;
    }

    double get_step_size() const override {
        ensure_connected();
        // TODO: The MyFocuserPro2 protocol does not expose step size in microns.
        // This may vary by mechanical configuration. Return a reasonable default
        // or throw if the focuser cannot report this.
        throw AlpacaException("Step size not available for this focuser",
                              AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        // open-astro#309: the connection check comes first. AGENTS.md's ASCOM
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
        protocol_.halt();
        snapshot_.write([](StateFrame& frame) { frame.moving = false; });
    }

    void move(int position) override {
        ensure_connected();
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

    // The frame the getters serve. Stale is served (decision 0010); a frame
    // older than the link-loss bound means the poll has not measured the
    // device for that long, which is a fault, not a value.
    StateFrame served_frame() const {
        ensure_connected();
        const auto reading = snapshot_.read();
        if (!reading.has_value()) {
            // A disconnect between the check and the read dropped the frame.
            ensure_connected();
            throw AlpacaException("Focuser state not available yet", AlpacaError::DriverException);
        }
        if (clock_.now() - reading->measured_at >= util::kLinkStalenessBound) {
            throw AlpacaException("Gemini focuser communications compromised: no state from the device for "
                                      "30 s",
                                  AlpacaError::DriverException);
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
            // no log escaper (CWE-117), so the line states the fact only.
            ALPACA_LOG_DEBUG("Gemini", "Focuser state poll failed; the frame keeps its age");
            return;
        }
        // A focuser without a probe never answers a temperature read. Ask
        // once per session after a failure: a timed-out read holds the wrapper
        // for the whole serial timeout, which Move and Halt would queue behind.
        if (!temperature_unavailable_) {
            try {
                frame.temperature = protocol_.get_temperature();
            } catch (const std::exception&) {
                temperature_unavailable_ = true;
                ALPACA_LOG_DEBUG("Gemini", "Focuser temperature read failed; not polling it again this session");
            }
        }
        snapshot_.publish(token, frame);
    }

    void run_state_poll(util::OperationContext& ctx) {
        for (;;) {
            // Start to start: a tick a slow poll ran over is skipped.
            const auto started = clock_.now();
            if (!ctx.wait_for(kStatePollPeriod)) {
                return;
            }
            poll_state_once();
            const auto spent = clock_.now() - started - kStatePollPeriod;
            if (spent > kStatePollPeriod && !ctx.wait_for(kStatePollPeriod - (spent % kStatePollPeriod))) {
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
    bool temperature_unavailable_ = false;  // poll thread and connect only, never concurrent
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

std::unique_ptr<FocuserDriver> create_gemini_focuser(int device_number,
                                                      const std::string& serial_port,
                                                      int baud_rate,
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
    ALPACA_LOG_INFO("Gemini", "Auto-detected focuser at " + port.port_path +
                    " (firmware " + std::to_string(port.firmware_version) + ")");

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
