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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace alpacacore::vendor::qhy {

namespace {
constexpr const char* kLogTag = "QHY";
}  // namespace

/**
 * Standalone QHYCFW3 over its own USB (CP2102) port.
 *
 * Position model: the wire protocol reports a position only when asked (NOW)
 * or when a goto arrives, and it is silent while the wheel turns, so the
 * driver never polls the wire for Position. The settled slot is cached from
 * the connect handshake and from each goto's arrival reply; while a goto is
 * in flight Position reads -1 (the ASCOM moving sentinel) from memory. That
 * keeps the FAST-classified reads (Position, DeviceState) off the serial line
 * in normal operation -- the one place the integrated CFW driver had to fight
 * ConformU's 0.1 s target. The one exception is the read after a FAILED move,
 * where the cache is empty and get_position() asks the wheel once.
 *
 * The goto's arrival wait (about 1 s per slot travelled) runs on a joinable
 * member thread so the Position PUT returns within the STANDARD budget;
 * disconnect and the destructor cancel and join it (AGENTS.md: never detach
 * a thread that touches `this`).
 */
class QhyCfw3FilterWheelDriver : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    QhyCfw3FilterWheelDriver(int device_number, Cfw3ConnectionConfig config,
                             util::ConnectionResolver<Cfw3ConnectionConfig> connection_resolver = {})
        : AsyncConnectable("QHY"),
          device_number_(device_number),
          config_(std::move(config)),
          connection_resolver_(std::move(connection_resolver)),
          connected_(false) {}

    ~QhyCfw3FilterWheelDriver() override {
        // Base contract: block new connection tasks and join the in-flight one
        // before any member the task touches is destroyed.
        shutdown_connection();
        if (connected_.load()) {
            try {
                QhyCfw3FilterWheelDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during QHYCFW3 destruction: " + std::string(e.what()));
            }
        }
        // A move worker can outlive connected_ (a failed move after a link
        // loss): join it regardless so no thread touches a destroyed member.
        cancel_and_join_move();
    }

    // ── AlpacaDriver ─────────────────────────────────────────────────────────

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "QHY CFW3"; }
    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }
    std::string get_unique_id() const override { return "QHY_CFW3_" + std::to_string(device_number_); }
    std::string get_description() const override { return "QHY CFW3 filter wheel driver (USB)"; }
    std::string get_driver_info() const override { return "AlpacaCore QHY CFW3 Filter Wheel Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 3; }

    // VRS firmware date from the connect handshake; web UI only, never
    // DriverInfo. Dedicated narrow mutex (AGENTS.md): the management poll must
    // not queue behind a multi-second connect holding the driver mutex.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) return std::nullopt;
        return firmware_;
    }

    // Connected follows the serial link (issue #445 shape): once a transaction
    // has seen the port die the wrapper latches link_lost_ and releases the fd.
    // Both reads are lock-free; this stays a FAST-timing getter.
    bool get_connected() const override { return connected_.load() && protocol_.link_alive(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override {
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogTag, "QHYCFW3 disconnect error: " + std::string(e.what()));
        }
    }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // A sync connect here is slow (the wheel's post-reset boot is ~17 s),
        // so whole transitions are serialized against each other (issue
        // #528): the base gates only see an ASYNC connect, and a sync
        // disconnect landing inside a sync connect would otherwise see "not
        // connected" twice and return as a no-op while the connect stores
        // true. Guards set_connected() against set_connected() only; never
        // taken in a getter.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        const bool live = connected_.load() && protocol_.link_alive();
        // Disconnect gate on the raw flag: a lost link with an async connect in
        // flight must still run the teardown rather than be recorded as
        // pending and skipped. Connect gate on the link-aware value, so a
        // stale connection is treated as down and reconnects (issue #527).
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(live)) return;
        if (connected ? live : !connected_.load()) return;

        if (connected) {
            if (connected_.load()) {
                // Stale connection over a lost link: drop it before reconnecting.
                teardown_locked();
            }
            // An auto-detected wheel resolves its port here, not in the factory (#659).
            Cfw3DeviceInfo info;
            util::connect_resolved(
                config_, connection_resolved_, connection_resolver_,
                [this, &info](const Cfw3ConnectionConfig& cfg) {
                    try {
                        info = protocol_.connect(cfg);
                    } catch (const AlpacaException& e) {
                        // Only a vanished node is stale. The out-of-step refusal of a wheel
                        // that is still homing, or any other failure on a present port, must
                        // NOT trigger the probe: it DTR-resets every CP210x device on the box.
                        if (util::device_node_missing(cfg.serial_port))
                            throw util::StaleEndpoint(e.what(), e.error_code());
                        throw;
                    }
                },
                kLogTag);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                slot_count_ = info.slot_count;
                slot_count_valid_ = true;
                normalize_slot_data_locked();
                position_cache_ = info.position;
                pending_target_.reset();
            }
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_ = info.firmware;
            }
            connected_.store(true);
            ALPACA_LOG_INFO(kLogTag, "QHYCFW3 connected: " + std::to_string(info.slot_count) + " slots, at slot " +
                                         std::to_string(info.position + 1));
        } else {
            teardown_locked();
            ALPACA_LOG_INFO(kLogTag, "QHYCFW3 disconnected");
        }
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

    // ── FilterWheelDriver ────────────────────────────────────────────────────

    int get_position() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        // -1 while a goto is in flight: the wheel is silent until it arrives,
        // and the worker publishes the arrival slot into the cache.
        if (pending_target_.has_value()) return -1;
        if (position_cache_.has_value()) return position_cache_.value();
        // Cache empty only after a failed move (the worker could not confirm
        // where the wheel stopped): ask once and cache the answer. Lock order
        // mutex_ -> wrapper mutex, same as the worker's publish path.
        const int pos = const_cast<QhyCfw3FilterWheelDriver*>(this)->protocol_.get_position();
        position_cache_ = pos;
        return pos;
    }

    void set_position(int position) override {
        // Range validation precedes the connection check (ASCOM precedence):
        // a negative slot, or one above the protocol's 16-position ceiling, is
        // InvalidValue even while disconnected. The slot-count bound needs the
        // connect handshake and is checked below.
        if (position < 0 || !cfw3_slot_to_command(position).has_value()) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ensure_connected();
        // Lock order: move_thread_mutex_ -> mutex_ (see the member comment).
        // The handle lock is taken here, before the state lock, so this
        // check+join+spawn is atomic against a concurrent set_position() AND
        // against teardown's cancel+join, and never waits on the handle lock
        // while holding mutex_ (the worker's publish path needs mutex_).
        std::lock_guard<std::mutex> handle_lock(move_thread_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        // Re-assert under the locks: teardown_locked() stores connected_ false
        // BEFORE it takes the handle lock to cancel and join, so a goto that
        // passed ensure_connected() a moment ago must not spawn a worker for a
        // wheel that is being disconnected (it would report NotConnected from
        // the worker, or worse, run a real move after a reconnect).
        ensure_connected();
        if (!slot_count_valid_ || slot_count_ <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (position >= slot_count_) {
            throw AlpacaException("Filter position out of range (wheel has " + std::to_string(slot_count_) + " slots)",
                                  AlpacaError::InvalidValue);
        }
        if (pending_target_.has_value()) {
            // The wire carries one goto at a time and the wheel answers only
            // on arrival; a second goto mid-move would be read as a stray byte.
            throw AlpacaException("Filter wheel is moving", AlpacaError::InvalidOperation);
        }
        if (position_cache_.has_value() && position_cache_.value() == position) {
            // Already there. The firmware would answer at once (201409+), but
            // pre-201409 firmware stays silent on a same-slot goto and the
            // wait would run to its timeout; skipping the wire is right for both.
            return;
        }
        // The previous worker has finished (pending_target_ is empty and it
        // clears that flag as its last action under mutex_, which it has
        // therefore already released), so this join only waits for the
        // thread to exit and cannot deadlock on mutex_.
        if (move_thread_.joinable()) move_thread_.join();
        position_cache_.reset();
        pending_target_ = position;
        move_cancel_.store(false);
        move_thread_ = std::thread([this, position] { run_move(position); });
    }

    std::vector<int> get_focus_offsets() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return focus_offsets_;
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_count_valid_ && slot_count_ > 0) {
            validate_slot_count_locked(static_cast<int>(offsets.size()), "focusOffsets");
        }
        focus_offsets_ = offsets;
    }

    std::vector<std::string> get_names() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return filter_names_;
    }

    void set_names(const std::vector<std::string>& names) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> staged = names;
        expand_shorthand_locked(staged);
        if (slot_count_valid_ && slot_count_ > 0) {
            validate_slot_count_locked(static_cast<int>(staged.size()), "names");
        }
        filter_names_ = std::move(staged);
        apply_default_names_locked();
    }

private:
    // Worker body: block on the arrival reply, then publish. Everything that
    // can throw is caught here -- an escaping exception on a std::thread is
    // std::terminate for the whole server.
    void run_move(int target) {
        try {
            const int arrived = protocol_.goto_slot(target, move_cancel_);
            std::lock_guard<std::mutex> lock(mutex_);
            position_cache_ = arrived;
            pending_target_.reset();
        } catch (const std::exception& e) {
            if (!move_cancel_.load()) {
                ALPACA_LOG_ERROR(kLogTag,
                                 "QHYCFW3 move to slot " + std::to_string(target + 1) + " failed: " + e.what());
            }
            std::lock_guard<std::mutex> lock(mutex_);
            // Where the wheel stopped is unknown; leave the cache empty so the
            // next Position read asks the wheel (NOW) instead of guessing.
            pending_target_.reset();
        }
    }

    // Caller must NOT hold mutex_: the worker takes it to publish its result
    // on the way out, so a join under mutex_ would deadlock.
    void cancel_and_join_move() {
        std::lock_guard<std::mutex> handle_lock(move_thread_mutex_);
        // Stored UNDER the handle lock (PR #536 review): set_position() clears
        // this flag and spawns its worker under the same lock, so a store made
        // before taking it could be wiped by a set_position() that slipped in
        // between, and the join below would then sit out the whole move.
        move_cancel_.store(true);
        if (move_thread_.joinable()) move_thread_.join();
    }

    // Disconnect body shared by the explicit disconnect and the stale-link
    // reconnect. Caller holds transition_mutex_ and NOT mutex_ (the worker's
    // publish path takes mutex_). Driver state first, port close second, so a
    // throwing close cannot leave the driver reporting connected on a closed
    // port.
    void teardown_locked() {
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_.clear();
        }
        connected_.store(false);
        // Cancel wakes the worker out of its read slice; the wheel finishes
        // the move on its own and the next connect's NOW reports where it is.
        cancel_and_join_move();
        protocol_.disconnect();
        std::lock_guard<std::mutex> lock(mutex_);
        slot_count_valid_ = false;
        position_cache_.reset();
        pending_target_.reset();
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
        if (!protocol_.link_alive()) {
            throw AlpacaException("Filter wheel link lost; set Connected false then true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    // Slot-data helpers: same shape as the integrated QHY CFW and ZWO EFW
    // drivers, so every filter wheel names and stores filters the same way.
    void normalize_slot_data_locked() {
        if (!slot_count_valid_ || slot_count_ <= 0) return;
        const std::size_t slots = static_cast<std::size_t>(slot_count_);
        expand_shorthand_locked(filter_names_);
        if (filter_names_.empty()) {
            filter_names_.assign(slots, std::string());
        } else if (filter_names_.size() != slots) {
            ALPACA_LOG_WARN(kLogTag, "Configured filterNames count (" + std::to_string(filter_names_.size()) +
                                         ") does not match the CFW3 slot count (" + std::to_string(slots) +
                                         "); resizing to match the wheel");
            filter_names_.resize(slots);
        }
        apply_default_names_locked();
        if (focus_offsets_.empty()) {
            focus_offsets_.assign(slots, 0);
        } else if (focus_offsets_.size() != slots) {
            ALPACA_LOG_WARN(kLogTag, "Configured focusOffsets count (" + std::to_string(focus_offsets_.size()) +
                                         ") does not match the CFW3 slot count (" + std::to_string(slots) +
                                         "); resizing to match the wheel");
            focus_offsets_.resize(slots);
        }
    }

    // "LRGB" -> L,R,G,B only when the lone token has no lowercase letters, so
    // an ordinary name that happens to match the slot count stays whole. Same
    // rule as parseFilterNamesInput in the web UI; keep the two in step.
    void expand_shorthand_locked(std::vector<std::string>& names) const {
        if (!slot_count_valid_ || slot_count_ <= 0 || names.size() != 1) return;
        const std::size_t slots = static_cast<std::size_t>(slot_count_);
        const std::string& candidate = names[0];
        const bool has_lowercase = candidate.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos;
        if (candidate.size() == slots && !has_lowercase && candidate.find_first_of(",; \t") == std::string::npos) {
            std::vector<std::string> expanded;
            expanded.reserve(slots);
            for (char ch : candidate) expanded.emplace_back(1, ch);
            names = std::move(expanded);
        }
    }

    void apply_default_names_locked() {
        for (std::size_t i = 0; i < filter_names_.size(); ++i) {
            if (filter_names_[i].empty()) filter_names_[i] = "Filter " + std::to_string(i + 1);
        }
    }

    void validate_slot_count_locked(int provided_size, std::string_view field_name) const {
        if (!slot_count_valid_ || slot_count_ <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (provided_size != slot_count_) {
            throw AlpacaException(
                "Invalid " + std::string(field_name) + " length: expected " + std::to_string(slot_count_),
                AlpacaError::InvalidValue);
        }
    }

    int device_number_;
    Cfw3ConnectionConfig config_;
    // Set by the by-index factory; empty for an explicit port. connection_resolved_
    // is true once a connect has run the resolver, so a later connect retries
    // that port before scanning again (#659).
    util::ConnectionResolver<Cfw3ConnectionConfig> connection_resolver_;
    bool connection_resolved_ = false;
    std::atomic<bool> connected_;
    Cfw3ProtocolWrapper protocol_;

    // Serializes whole connect/disconnect transitions; see set_connected().
    std::mutex transition_mutex_;
    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    // Driver state: slot data and the position model.
    mutable std::mutex mutex_;
    int slot_count_ = 0;
    bool slot_count_valid_ = false;
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;
    // Settled slot; empty while unknown (after a failed move). Declared
    // mutable so the const get_position() can refill it from a live read.
    mutable std::optional<int> position_cache_;
    // Commanded slot while a goto is in flight; cleared by the worker.
    std::optional<int> pending_target_;

    // Joinable arrival-wait worker (never detached) and its cancel flag. The
    // std::thread handle itself is shared state: set_position() assigns it,
    // teardown and the destructor join it, so it has its own mutex. Lock
    // order everywhere: transition_mutex_ -> move_thread_mutex_ -> mutex_ ->
    // wrapper mutex; the worker takes only the wrapper mutex (inside
    // goto_slot) and then mutex_ (to publish), never move_thread_mutex_, so
    // joining under move_thread_mutex_ cannot wait on itself.
    std::mutex move_thread_mutex_;
    std::thread move_thread_;
    std::atomic<bool> move_cancel_{false};
};

std::unique_ptr<FilterWheelDriver> create_qhy_cfw3_filterwheel(int device_number, const std::string& serial_port,
                                                               const Cfw3Settings& settings) {
    Cfw3ConnectionConfig config;
    config.serial_port = serial_port;
    config.boot_timeout_ms = settings.boot_timeout_ms;
    config.reply_timeout_ms = settings.reply_timeout_ms;
    config.move_timeout_ms = settings.move_timeout_ms;
    return std::make_unique<QhyCfw3FilterWheelDriver>(device_number, std::move(config));
}

std::unique_ptr<FilterWheelDriver> create_qhy_cfw3_filterwheel_deferred(
    int device_number, util::ConnectionResolver<Cfw3ConnectionConfig> resolver) {
    if (!resolver) {
        throw AlpacaException("QHYCFW3 filter wheel: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<QhyCfw3FilterWheelDriver>(device_number, Cfw3ConnectionConfig{}, std::move(resolver));
}

Cfw3ConnectionConfig resolve_qhy_cfw3_filterwheel_by_index(int wheel_index, const Cfw3Settings& settings) {
    auto ports = enumerate_cfw3_ports(settings.boot_timeout_ms);
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("QHYCFW3 filter wheel"),
                              AlpacaError::NotConnected);
    }
    if (wheel_index < 0 || wheel_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Filter wheel index " + std::to_string(wheel_index) + " out of range (detected " +
                                  std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }
    const auto& port = ports[static_cast<std::size_t>(wheel_index)];
    ALPACA_LOG_INFO(kLogTag, "Auto-detected QHYCFW3 at " + port.port_path);
    Cfw3ConnectionConfig config;
    config.serial_port = port.port_path;
    config.boot_timeout_ms = settings.boot_timeout_ms;
    config.reply_timeout_ms = settings.reply_timeout_ms;
    config.move_timeout_ms = settings.move_timeout_ms;
    return config;
}

std::unique_ptr<FilterWheelDriver> create_qhy_cfw3_filterwheel_by_index(int device_number, int wheel_index,
                                                                        const Cfw3Settings& settings) {
    // The serial scan runs at connect time (#659), not here. That matters
    // twice for this wheel: the probe DTR-resets every CP210x device on the
    // box, and at server start-up the wheel is often still booting.
    return create_qhy_cfw3_filterwheel_deferred(device_number, [wheel_index, settings] {
        return resolve_qhy_cfw3_filterwheel_by_index(wheel_index, settings);
    });
}

}  // namespace alpacacore::vendor::qhy
