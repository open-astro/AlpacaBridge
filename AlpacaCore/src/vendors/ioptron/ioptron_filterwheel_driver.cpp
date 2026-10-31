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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_iefw_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::vendor::ioptron {

// Shape and semantics follow the ZWO EFW driver (the project's canonical
// filter wheel): names/offsets live in config, are sized to the slot count
// learned at connect, and single-token shorthand ("LRGB") expands per slot.
class IefwFilterWheelDriver : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    IefwFilterWheelDriver(int device_number, std::optional<std::string> serial_port, std::optional<int> wheel_index,
                          const std::string& model, util::TaskClock& clock)
        : AsyncConnectable("iOptron"),
          device_number_(device_number),
          serial_port_(std::move(serial_port)),
          wheel_index_(wheel_index),
          connected_(false),
          protocol_(),
          model_name_(model == "iefw18" ? "iEFW-18" : "iEFW-15"),
          status_cache_(kCacheLabel, kStatusTtl, 3, "iOptron", clock) {}

    ~IefwFilterWheelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("iOptron", "Error during iEFW destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    // Name follows the user-selected model (config "model": iefw15 / iefw18),
    // fixed at construction. The slot count comes from the wheel's
    // :DeviceInfo# model code at connect (99 = 5 slots, 98 = 8 slots,
    // confirmed on an iEFW-15); a mismatch with the selected model is logged.
    std::string get_name() const override { return "iOptron " + model_name_; }

    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }

    std::string fallback_unique_id() const override { return "IOPTRON_IEFW_" + std::to_string(device_number_); }

    std::string get_description() const override { return "iOptron iEFW Filter Wheel Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore iOptron iEFW Filter Wheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware captured at connect; surfaced in the web UI only (never
    // DriverInfo). Set at connect and kept until the next Connect or
    // Disconnect; a lost link hides it (the wrapper's lock-free latch), so the
    // web UI never shows firmware beside Connected false.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (firmware_.empty() || !protocol_.link_alive()) {
            return std::nullopt;
        }
        return firmware_;
    }

    int get_interface_version() const override { return 3; }

    // Connected follows the serial link (decision 0009 rule 2): once an exchange
    // has seen the port gone the wrapper latches the loss and releases the port.
    // Both reads are lock-free.
    bool get_connected() const override { return connected_.load() && protocol_.link_alive(); }

    // Decision 0009 rules 1 and 5: the loss reason, kept after the link is lost, else the Position fault latched by
    // the status cache (Connected stays true). Neither read waits on the wire: the wrapper's text mutex and the
    // cache's fault mutex are leaves no exchange holds.
    std::string get_link_fault() const override {
        std::string lost = protocol_.link_fault();
        if (!lost.empty()) {
            return lost;
        }
        return status_cache_.fault();
    }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // transition_mutex_ spans the whole call and no getter takes it: the connect handshake runs
        // under it and the I/O lock, never under mutex_ (decision 0016).
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Base gates BEFORE the idempotency check (see AsyncConnectable).
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        // The connect check reads the link-aware value, so Connect over a lost
        // link reconnects (decision 0009 rule 4); the disconnect check reads
        // the raw flag, so a lost link still runs the teardown (#1289).
        if (connected ? connected_.load() && protocol_.link_alive() : !connected_.load()) {
            return;
        }

        if (connected) {
            // Start clean: after a lost link the raw flag and the firmware
            // from before the loss are still set, and Connected must not read
            // true while the reconnect is still reading the wheel.
            teardown_locked();
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                IefwConnectionConfig config;
                config.serial_port = resolve_serial_port();
                IefwDeviceInfo info = protocol_.connect(config);
                try {
                    publish_connect_state(info);
                } catch (...) {
                    protocol_.disconnect();
                    throw;
                }
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            // Also drops a closed frame that a refill racing the last disconnect cached after that
            // disconnect's reset(): it counts as a good reply and would be served for up to the TTL.
            status_cache_.reset();
            connected_.store(true);
            std::int32_t slots = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                slots = slot_count_;
            }
            ALPACA_LOG_INFO("iOptron", model_name_ + " filter wheel connected (" + std::to_string(slots) + " slots)");
            return;
        }

        teardown_locked();
        ALPACA_LOG_INFO("iOptron", "iEFW filter wheel disconnected");
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

    int get_position() const override {
        ensure_connected();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: cache mutex, I/O lock,
        // mutex_), so it must not take mutex_ and no caller may hold mutex_ or the I/O lock around the cache.
        Status status;
        try {
            status = status_cache_.get([this] {
                std::lock_guard<std::mutex> io(io_mutex_);
                Status fresh;
                if (!connected_.load()) {
                    // A disconnect that won the I/O lock has closed the port. Returning (not throwing) keeps it
                    // out of the link-health count; the disconnect's reset() drops this frame.
                    fresh.closed = true;
                    return fresh;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    fresh.move_seq = move_seq_;
                }
                fresh.wire = protocol_.get_position();
                return fresh;
            });
        } catch (const std::exception&) {
            // A refill that lost the link counts as a failed read for the cache, which turns the third one in a row
            // into its "communications compromised" DriverException; a lost link is NotConnected (decision 0009
            // rule 3), whatever the count. So is a fault that has now stood past the staleness bound (rule 2).
            ensure_connected();
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                lose_link_if_fault_stale_locked();
            }
            throw;
        }
        if (status.closed) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
        // :WP# answers -1 while the wheel is moving. Also hold -1 from the
        // moment a move is commanded until the wheel reports the target, so
        // a poll that lands before the motor starts does not show the old
        // slot as "arrived" (IFilterWheel: Position is -1 while moving).
        int result = static_cast<int>(status.wire);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status.wire < 0) {
                result = -1;
            } else if (moving_) {
                // A frame read before the latest move was commanded cannot show that move's arrival.
                if (status.move_seq != move_seq_ || status.wire != target_) {
                    result = -1;
                } else {
                    moving_ = false;
                }
            }
        }
        if (result < 0) {
            // A moving frame is not held: the next poll goes to the wheel, so the move end is seen at once.
            status_cache_.invalidate();
        }
        return result;
    }

    void set_position(int position) override {
        // Range validation precedes the connection check (ASCOM precedence):
        // a negative position is unconditionally invalid. The upper bound
        // depends on the slot count (known after connect), checked below.
        if (position < 0) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ensure_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot_count_ <= 0) {
                throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
            }
            if (position >= slot_count_) {
                throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
            }
        }
        try {
            // The I/O lock spans the latch set, :WM and the restore, so two concurrent moves cannot
            // interleave (A sets its target, B sets and sends :WM, A sends :WM: the wheel ends on A's
            // slot with B's target latched).
            std::lock_guard<std::mutex> io(io_mutex_);
            // A disconnect that won the I/O lock has closed the port; mutex_ held across the move gave
            // this guarantee before the conversion.
            ensure_connected();
            // Decision 0009 rule 1: a latched Position fault refuses the move, as it refuses the read; a wheel that
            // does not answer :WP# would otherwise take :WM with no ack and report success. Past the staleness
            // bound the refused move loses the link (rule 2). Only a good Position read clears the fault.
            lose_link_if_fault_stale_locked();
            if (const std::string fault = status_cache_.fault(); !fault.empty()) {
                throw AlpacaException(std::string(kCacheLabel) + " communications compromised: " + fault,
                                      AlpacaError::DriverException);
            }
            std::int32_t previous_target = 0;
            bool previous_moving = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                previous_target = target_;
                previous_moving = moving_;
                target_ = position;
                moving_ = true;
                ++move_seq_;
            }
            try {
                protocol_.move_to(position);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                target_ = previous_target;
                moving_ = previous_moving;
                throw;
            }
        } catch (...) {
            // A refused or half-done move leaves the cached frame untrusted.
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();
    }

    std::vector<int> get_focus_offsets() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return focus_offsets_;
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Config-time values are accepted at any length and resized at
        // connect; once connected the length must match the slot count.
        if (connected_.load()) {
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
        // Stage on a copy so a validation throw leaves filter_names_ intact.
        std::vector<std::string> staged = names;
        expand_shorthand_locked(staged);
        if (connected_.load()) {
            validate_slot_count_locked(static_cast<int>(staged.size()), "names");
        }
        filter_names_ = std::move(staged);
        apply_default_names_locked();
    }

private:
    // NotConnected while disconnected and once the wrapper has seen the port
    // gone (decision 0009 rule 3); lock-free, and checked before the status
    // cache, so a frame from before the loss is never served.
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
        if (!protocol_.link_alive()) {
            throw AlpacaException("Filter wheel link lost; Connect again once the wheel is back",
                                  AlpacaError::NotConnected);
        }
    }

    // Decision 0009 rule 2: a Position fault that has stood past util::kLinkStalenessBound on the task clock is a
    // lost link, found by the next failed read or refused move (there is no poll). The wrapper releases the port and
    // keeps the reason (rules 3 and 5); then this throws NotConnected. Caller holds io_mutex_ (so no exchange is in
    // flight) and no other driver lock; this reads the cache's leaf fault mutex, then takes the wrapper's mutex.
    // Checked under io_mutex_: a Connect since the failed read reset the cache in its teardown before it reopened
    // the port, so a fresh session is never lost on the old fault.
    void lose_link_if_fault_stale_locked() const {
        // The text first: a stale fault was latched at least the bound ago, so it is already set here (a refill that
        // finished its exchange before this took io_mutex_ can clear it or latch a fresh one, never a stale one).
        const std::string fault = status_cache_.fault();
        if (!status_cache_.fault_stale()) {
            return;
        }
        protocol_.lose_link("iEFW Position fault stood for " + std::to_string(util::kLinkStalenessBound.count()) +
                            " s with no good reply: " + fault);
        ensure_connected();
    }

    // Disconnect body shared by the explicit disconnect and the connect. Caller holds transition_mutex_ and no other
    // driver lock. Driver state first, port close second (driver-lifecycle.instructions.md, issue #387): if the close
    // throws the driver must not stay half-connected. The wrapper's disconnect() also clears the kept loss reason;
    // after a lost link the port is already released and the close is a no-op.
    void teardown_locked() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            firmware_.clear();
            connected_.store(false);
        }
        status_cache_.reset();
        try {
            // Waits for an exchange in flight: the port is never closed under one.
            std::lock_guard<std::mutex> io(io_mutex_);
            protocol_.disconnect();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "iEFW disconnect error: " + std::string(e.what()));
        }
    }

    // Serial mode uses the configured port; auto mode scans at connect so a
    // wheel plugged in after startup is still found.
    std::string resolve_serial_port() {
        if (serial_port_.has_value() && !serial_port_->empty()) {
            return *serial_port_;
        }
        auto ports = enumerate_iefw_ports();
        if (ports.empty()) {
            throw AlpacaException(util::serial_auto_detect_failed_message("iOptron iEFW filter wheel"),
                                  AlpacaError::NotConnected);
        }
        const int index = wheel_index_.value_or(0);
        if (index < 0 || index >= static_cast<int>(ports.size())) {
            throw AlpacaException("Filter wheel index " + std::to_string(index) + " out of range (detected " +
                                      std::to_string(ports.size()) + ")",
                                  AlpacaError::InvalidValue);
        }
        const auto& port = ports[static_cast<std::size_t>(index)];
        ALPACA_LOG_INFO("iOptron", "Auto-detected " + iefw_model_name(port.info.model) + " at " + port.port_path);
        return port.port_path;
    }

    // :FW1# gives a 12-character firmware string; fall back to the numeric
    // build from the handshake if the wheel does not answer it.
    std::string read_firmware_string(const IefwDeviceInfo& info) {
        try {
            std::string fw = protocol_.get_firmware();
            if (!fw.empty()) {
                return fw;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "iEFW firmware query failed: " + std::string(e.what()));
        }
        return info.firmware > 0 ? std::to_string(info.firmware) : std::string();
    }

    // Caller holds io_mutex_ (the reads are wheel exchanges); the state publishes under mutex_.
    void publish_connect_state(const IefwDeviceInfo& info) {
        const std::int32_t slots = iefw_slot_count(info.model);
        if (iefw_model_name(info.model) != model_name_) {
            ALPACA_LOG_WARN("iOptron", "Configured model " + model_name_ + " but the wheel reports " +
                                           iefw_model_name(info.model) + " (model code " + std::to_string(info.model) +
                                           "); using its " + std::to_string(slots) + " slots");
        }
        std::string firmware = read_firmware_string(info);
        // Offsets stored on the wheel (set via iOptron's own software)
        // seed FocusOffsets when the config supplies none; config
        // values override and nothing is written back (Player One
        // convention).
        bool offsets_configured = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            offsets_configured = !focus_offsets_.empty();
        }
        std::vector<int> stored;
        if (!offsets_configured) {
            stored = read_stored_offsets(slots);
        }
        // The two reads above log and carry on when they fail; a cable pulled during them lost the link (the
        // wrapper released the port), so this Connect fails rather than publishing a wheel that is gone.
        if (!protocol_.link_alive()) {
            throw AlpacaException(protocol_.link_fault(), AlpacaError::NotConnected);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        slot_count_ = slots;
        firmware_ = std::move(firmware);
        // A FocusOffsets write that landed during the :WF reads is kept (the check above ran unlocked).
        const bool seed = !offsets_configured && focus_offsets_.empty();
        normalize_slot_data_locked();
        if (seed) {
            focus_offsets_ = std::move(stored);
        }
        // The move latch from before a loss or a Disconnect ends here, under the I/O lock: a set_position that
        // won the lock before the teardown's connected_ flag was seen can still set it after the teardown, but
        // none can run once the handshake holds the lock. Position is not readable while disconnected.
        moving_ = false;
    }

    // A failed read stops the loop; the remaining slots stay 0.
    std::vector<int> read_stored_offsets(std::int32_t slots) {
        std::vector<int> offsets(static_cast<std::size_t>(std::max<std::int32_t>(slots, 0)), 0);
        for (std::int32_t slot = 0; slot < slots; ++slot) {
            try {
                offsets[static_cast<std::size_t>(slot)] = protocol_.get_stored_offset(slot);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("iOptron",
                                "iEFW stored offset read failed for slot " + std::to_string(slot) + ": " + e.what());
                break;
            }
        }
        return offsets;
    }

    void normalize_slot_data_locked() {
        if (slot_count_ <= 0) {
            return;
        }
        const std::size_t slots = static_cast<std::size_t>(slot_count_);
        expand_shorthand_locked(filter_names_);
        if (filter_names_.empty()) {
            filter_names_.assign(slots, std::string());
        } else if (filter_names_.size() != slots) {
            ALPACA_LOG_WARN("iOptron", "Configured filterNames count (" + std::to_string(filter_names_.size()) +
                                           ") does not match wheel slot count (" + std::to_string(slots) +
                                           "); resizing to match the wheel");
            filter_names_.resize(slots);
        }
        apply_default_names_locked();
        if (focus_offsets_.empty()) {
            focus_offsets_.assign(slots, 0);
        } else if (focus_offsets_.size() != slots) {
            ALPACA_LOG_WARN("iOptron", "Configured focusOffsets count (" + std::to_string(focus_offsets_.size()) +
                                           ") does not match wheel slot count (" + std::to_string(slots) +
                                           "); resizing to match the wheel");
            focus_offsets_.resize(slots);
        }
    }

    // "LRGB" -> L,R,G,B when the lone token has no lowercase letters and its
    // length equals the slot count (same rule as the JS parseFilterNamesInput
    // and the ZWO/ToupTek drivers). No-op until the slot count is known.
    void expand_shorthand_locked(std::vector<std::string>& names) const {
        if (slot_count_ <= 0 || names.size() != 1) {
            return;
        }
        const std::size_t slots = static_cast<std::size_t>(slot_count_);
        const std::string& candidate = names[0];
        const bool has_lowercase = candidate.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos;
        if (candidate.size() == slots && !has_lowercase && candidate.find_first_of(",; \t") == std::string::npos) {
            std::vector<std::string> expanded;
            expanded.reserve(slots);
            for (char ch : candidate) {
                expanded.emplace_back(1, ch);
            }
            names = std::move(expanded);
        }
    }

    void apply_default_names_locked() {
        for (std::size_t i = 0; i < filter_names_.size(); ++i) {
            if (filter_names_[i].empty()) {
                filter_names_[i] = "Filter " + std::to_string(i + 1);
            }
        }
    }

    void validate_slot_count_locked(int provided_size, std::string_view field_name) const {
        if (provided_size != slot_count_) {
            throw AlpacaException(
                "Invalid " + std::string(field_name) + " length: expected " + std::to_string(slot_count_),
                AlpacaError::InvalidValue);
        }
    }

    // One refill is one :WP#. `wire` is the raw reply (-1 while moving); the moving_/target_ latch is
    // applied by the reader after the cache, never stored in the frame.
    struct Status {
        std::int32_t wire = 0;
        bool closed = false;
        std::uint64_t move_seq = 0;  // move_seq_ when the refill took the I/O lock; the latch needs a match
    };
    static constexpr std::chrono::milliseconds kStatusTtl{100};
    static constexpr const char* kCacheLabel = "iOptron iEFW";  // the status cache's label, its fault text prefix

    int device_number_;
    std::optional<std::string> serial_port_;
    std::optional<int> wheel_index_;
    std::atomic<bool> connected_;
    mutable IefwProtocolWrapper protocol_;  // its own mutex serialises one exchange (SDK-wrapper leaf)

    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // never held across a wheel exchange or the connect handshake), then the protocol wrapper's mutex.
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the
    // latch set + :WM + restore of a move against a :WP# refill and the connect/close. Nothing calls the
    // cache (reset/invalidate) while holding io_mutex_ or mutex_; fault() takes only the leaf fault_mutex_.
    // A lost link is latched inside an exchange (under io_mutex_ and the wrapper's mutex) and takes only the
    // wrapper's leaf fault-text mutex, never a cache lock; link_alive() is lock-free (#1723). A stale fault is lost
    // under io_mutex_ by lose_link_if_fault_stale_locked(), which reads the cache's leaf fault mutex (never the
    // cache mutex) and then takes the wrapper's mutex.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;

    // Everything below is guarded by mutex_.
    mutable std::mutex mutex_;
    std::int32_t slot_count_ = 0;   // from the handshake model code at connect
    const std::string model_name_;  // "iEFW-15" / "iEFW-18" from config; fixed after construction
    std::string firmware_;          // web-UI only; set at connect, cleared by the next Connect/Disconnect
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;
    // Position reports -1 from a commanded move until the wheel reports the
    // target. moving_ is mutable so the arrival latch can run in const get_position().
    mutable bool moving_ = false;
    std::int32_t target_ = 0;
    std::uint64_t move_seq_ = 0;  // bumped with each latch set, under io_mutex_ and mutex_

    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<FilterWheelDriver> create_iefw_filterwheel(int device_number, const std::string& serial_port,
                                                           const std::string& model, util::TaskClock& clock) {
    return std::make_unique<IefwFilterWheelDriver>(device_number, serial_port, std::nullopt, model, clock);
}

std::unique_ptr<FilterWheelDriver> create_iefw_filterwheel_by_index(int device_number, int wheel_index,
                                                                    const std::string& model, util::TaskClock& clock) {
    return std::make_unique<IefwFilterWheelDriver>(device_number, std::nullopt, wheel_index, model, clock);
}

}  // namespace alpacacore::vendor::ioptron
