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
#include <alpacacore/util/filterwheel_slots.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::vendor::wandererastro {

class WandererFilterWheelDriver : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    WandererFilterWheelDriver(int device_number, FilterWheelConnectionConfig config,
                              util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("WandererAstro"),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          protocol_(clock) {
        // The whole Wanderer lineup is 8-slot, so the count is published at
        // construction and never cleared: names and offsets are length-checked
        // even while disconnected (unlike SDK-enumerated wheels, issue #1991).
        slots_.publish_slot_count(kFilterWheelSlotCount, "WandererAstro");
    }

    ~WandererFilterWheelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                // Tear down directly rather than via the virtual set_connected(),
                // which must not be dispatched during destruction.
                protocol_.disconnect();
                connected_.store(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("WandererAstro", "Error during filter wheel destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (model_ == "WSFW368") {
            return "WandererAstro SFW36S";
        }
        if (model_ == "WSFW508") {
            return "WandererAstro SFW50";
        }
        return "WandererAstro Filter Wheel";
    }

    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }

    std::string fallback_unique_id() const override {
        return "WANDERERASTRO_FILTERWHEEL_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "WandererAstro SFW Filter Wheel Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore WandererAstro FilterWheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware date (YYYY-MM-DD), cached in the protocol wrapper from the first
    // streamed status frame and cleared there on disconnect. Surfaced in the
    // web UI only, never in DriverInfo.
    std::optional<std::string> get_device_firmware() const override { return protocol_.get_firmware_date(); }

    int get_interface_version() const override { return 3; }

    // Connected follows the serial link (decision 0009, issue #1988): once the
    // wrapper has lost the link (a hangup, or a fault standing past
    // util::kLinkStalenessBound) it has released the port. Both reads are
    // lock-free.
    bool get_connected() const override { return connected_.load() && protocol_.link_alive(); }

    // Decision 0009 point 5: the fault text while faulted, the loss reason
    // after a loss until the next Connect or Disconnect.
    std::string get_link_fault() const override {
        const auto fault = protocol_.link_fault();
        return fault ? *fault : std::string{};
    }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously — closing the port + stopping the reader is
        // trivial and ASCOM clients expect Connected to be false immediately.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("WandererAstro", "Filter wheel disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Serialize the whole connect/disconnect transition (see the cover
        // driver: without this, concurrent disconnects would join the wrapper's
        // reader thread twice — undefined behavior).
        std::lock_guard<std::mutex> transition(transition_mutex_);
        const bool live = get_connected();
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent and would be silently dropped
        // without the record; a connect must honor a newer pending disconnect.
        // The disconnect gate reads the raw flag, so a lost link still runs
        // the teardown; the connect gate reads the link-aware value, so
        // Connect over a lost link reconnects (decision 0009 point 4).
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        // Over a lost link the pending disconnect is consumed here without a
        // teardown, so the raw connected_ may stay true. That is harmless: the
        // port is already released, Connected reads false, and the next
        // Disconnect or Connect runs teardown().
        if (connected && consume_pending_disconnect(live)) {
            return;
        }
        if (connected ? live : !connected_.load()) {
            return;
        }
        if (connected) {
            if (connected_.load()) {
                teardown();  // a lost link: drop it before reconnecting, never reopen it as it was
            }
            // Resolve an auto-detect request here (on the background connection
            // thread), not at registration. A local copy keeps config_ as the
            // durable intent so a later reconnect re-scans.
            FilterWheelConnectionConfig effective = config_;
            if (effective.serial_port.empty() && effective.auto_detect_index >= 0) {
                auto ports = enumerate_wanderer_filterwheel_ports();
                if (ports.empty()) {
                    throw AlpacaException(util::serial_auto_detect_failed_message("Wanderer filter wheel"),
                                          AlpacaError::NotConnected);
                }
                if (effective.auto_detect_index >= static_cast<int>(ports.size())) {
                    throw AlpacaException("Filter wheel index " + std::to_string(effective.auto_detect_index) +
                                              " out of range (detected " + std::to_string(ports.size()) + ")",
                                          AlpacaError::NotConnected);
                }
                const auto& port = ports[static_cast<std::size_t>(effective.auto_detect_index)];
                ALPACA_LOG_INFO("WandererAstro", "Auto-detected " + port.model + " at " + port.port_path);
                effective.serial_port = port.port_path;
            }
            // A saved serial entry with no port (#508 item 1) is registered so it stays listed;
            // refuse here, in words an operator can act on, instead of opening "".
            if (effective.serial_port.empty()) {
                throw AlpacaException(
                    "WandererAstro filter wheel: portPath is required when connectionType is 'serial' (or use 'auto').",
                    AlpacaError::NotConnected);
            }
            const std::string model = protocol_.connect(effective);
            try {
                // Home the wheel once per connect, matching the vendor's INDI
                // reference. Fire-and-forget: the wheel spins to re-detect its
                // zero and the streamed position tracks it; no move target is
                // recorded, so Position simply reports the live slot.
                protocol_.calibrate();
                std::lock_guard<std::mutex> lock(state_mutex_);
                model_ = model;
                moving_ = false;
                target_slot_ = 0;
            } catch (...) {
                // The calibrate command never reached the wheel — undo the
                // protocol connect so the open port and reader thread aren't
                // leaked while the driver still reports disconnected.
                protocol_.disconnect();
                throw;
            }
            if (!protocol_.link_alive()) {
                // The port died before the session was published: fail this
                // connect. disconnect() also clears the loss reason, so a
                // connect that never completed leaves get_link_fault() empty.
                protocol_.disconnect();
                throw AlpacaException("Wanderer filter wheel link lost during connect", AlpacaError::NotConnected);
            }
            connected_.store(true);
            ALPACA_LOG_INFO("WandererAstro", "Filter wheel connected");
        } else {
            teardown();
            ALPACA_LOG_INFO("WandererAstro", "Filter wheel disconnected");
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

    // --- FilterWheel interface ---

    int get_position() const override {
        ensure_connected();
        ensure_link_up();  // -1 would masquerade as "moving" (issue #237)
        const FilterWheelStatus s = protocol_.get_status();
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!s.valid) {
            // Connected but no frame yet (shouldn't happen — connect() waits for
            // one) — report "moving/unknown" per the IFilterWheel contract
            // rather than inventing a slot.
            return -1;
        }
        if (moving_) {
            if (s.position != target_slot_) {
                return -1;  // ASCOM: Position is -1 while the wheel is moving
            }
            // Arrived: latch idle so a later out-of-band position change (e.g.
            // a manual recalibration) is reported as the live slot, not as a
            // never-ending move.
            moving_ = false;
        }
        return s.position - 1;  // wire is 1-based, Alpaca is 0-based
    }

    void set_position(int position) override {
        // The slot count is fixed for the whole lineup, so the full range is
        // validated before the connection check (ASCOM precedence: an
        // out-of-range position is unconditionally invalid).
        if (position < 0 || position >= kFilterWheelSlotCount) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ensure_connected();
        ensure_link_up();
        const int target = position + 1;  // Alpaca 0-based -> wire 1-based
        const FilterWheelStatus s = protocol_.get_status();
        // Hold state_mutex_ across the write so the move target and the command
        // can't be separated by a concurrent set_position (lock order is always
        // state_mutex_ -> protocol mutex; the protocol never calls back).
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (s.valid && !moving_ && s.position == target) {
            return;  // already there — the wheel ignores same-slot moves anyway
        }
        const bool prev_moving = moving_;
        const int prev_target = target_slot_;
        moving_ = true;
        target_slot_ = target;
        try {
            protocol_.select_filter(target);
        } catch (...) {
            // The command never reached the wheel — restore the prior state so
            // Position doesn't report a move that never started.
            moving_ = prev_moving;
            target_slot_ = prev_target;
            throw;
        }
    }

    std::vector<int> get_focus_offsets() const override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return slots_.offsets();
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        slots_.set_offsets(offsets);
    }

    std::vector<std::string> get_names() const override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return slots_.names();
    }

    void set_names(const std::vector<std::string>& names) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        slots_.set_names(names);
    }

private:
    // NotConnected while disconnected and once the link is lost (decision
    // 0009 point 3), before the fault check or the Position cache is reached.
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
        if (!protocol_.link_alive()) {
            throw AlpacaException("Filter wheel link lost; Connect again once the wheel is back",
                                  AlpacaError::NotConnected);
        }
    }

    // Driver state first, then the wrapper teardown (joins the reader, clears
    // the cached status, firmware date and loss reason, closes the port if the
    // loss has not already released it). Caller holds transition_mutex_.
    void teardown() {
        connected_.store(false);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            model_.clear();
            moving_ = false;
            target_slot_ = 0;
        }
        protocol_.disconnect();
    }

    // Issue #237: Position is served from the reader thread's cache, so a
    // dead serial link would otherwise report the last slot forever. While
    // the wrapper has the link latched faulted, Position and moves throw
    // DriverException (Connected is still true, so not NotConnected) until
    // frames resume. Names and offsets are driver-side and keep answering.
    void ensure_link_up() const {
        if (const auto fault = protocol_.link_fault()) {
            throw AlpacaException("Filter wheel communications compromised: " + *fault, AlpacaError::DriverException);
        }
    }

    int device_number_;
    FilterWheelConnectionConfig config_;
    std::atomic<bool> connected_;
    WandererFilterWheelProtocolWrapper protocol_;

    mutable std::mutex state_mutex_;
    std::string model_;  // status model token captured at connect ("WSFW368"/"WSFW508")
    // Commanded move target (wire slot, 1-based). Position reports -1 until the
    // streamed position matches, then latches idle. Guarded by state_mutex_;
    // moving_ is mutable so the arrival latch can run in const get_position().
    mutable bool moving_ = false;
    int target_slot_ = 0;
    util::FilterWheelSlots slots_;  // names and offsets; guarded by state_mutex_

    std::mutex transition_mutex_;  // serializes set_connected() connect/disconnect transitions
};

std::unique_ptr<FilterWheelDriver> create_wandererastro_filterwheel(int device_number, const std::string& serial_port,
                                                                    int baud_rate, util::TaskClock& clock) {
    FilterWheelConnectionConfig config;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<WandererFilterWheelDriver>(device_number, std::move(config), clock);
}

std::unique_ptr<FilterWheelDriver> create_wandererastro_filterwheel_by_index(int device_number, int filterwheel_index) {
    // Defer the (blocking) port scan to connect time, which runs on the
    // driver's background connection thread — never at registration.
    FilterWheelConnectionConfig config;
    config.auto_detect_index = filterwheel_index;
    return std::make_unique<WandererFilterWheelDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::wandererastro
