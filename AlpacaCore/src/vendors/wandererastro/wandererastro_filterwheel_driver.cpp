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
    WandererFilterWheelDriver(int device_number, FilterWheelConnectionConfig config)
        : AsyncConnectable("WandererAstro"),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          protocol_() {
        // The whole Wanderer lineup is 8-slot, so slot-sized state can be built
        // at construction rather than at connect (unlike SDK-enumerated wheels).
        filter_names_.assign(kFilterWheelSlotCount, std::string());
        apply_default_names_locked();
        focus_offsets_.assign(kFilterWheelSlotCount, 0);
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

    std::string get_unique_id() const override { return "WANDERERASTRO_FILTERWHEEL_" + std::to_string(device_number_); }

    std::string get_description() const override { return "WandererAstro SFW Filter Wheel Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore WandererAstro FilterWheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware date (YYYY-MM-DD), cached in the protocol wrapper from the first
    // streamed status frame and cleared there on disconnect. Surfaced in the
    // web UI only, never in DriverInfo.
    std::optional<std::string> get_device_firmware() const override { return protocol_.get_firmware_date(); }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

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
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent and would be silently dropped
        // without the record; a connect must honor a newer pending disconnect.
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
            connected_.store(true);
            ALPACA_LOG_INFO("WandererAstro", "Filter wheel connected");
        } else {
            protocol_.disconnect();  // clears the wrapper's cached firmware date
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                model_.clear();
                moving_ = false;
                target_slot_ = 0;
            }
            connected_.store(false);
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
        return focus_offsets_;
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        validate_slot_count_locked(static_cast<int>(offsets.size()), "focusOffsets");
        focus_offsets_ = offsets;
    }

    std::vector<std::string> get_names() const override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return filter_names_;
    }

    void set_names(const std::vector<std::string>& names) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // Expand a single-token shorthand ("LRGBSHOC" -> L,R,G,B,S,H,O,C)
        // before validating the count, then stage on a local copy so a
        // validation throw leaves the existing names untouched (matches the
        // ZWO/PlayerOne filter wheel drivers).
        std::vector<std::string> staged = names;
        expand_shorthand_locked(staged);
        validate_slot_count_locked(static_cast<int>(staged.size()), "names");
        filter_names_ = std::move(staged);
        apply_default_names_locked();
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
    }

    // Expand a single delimiter-less token into per-slot single-character names
    // ("LRGBSHOC" -> L,R,G,B,S,H,O,C) only when it looks like a shorthand code
    // (no lowercase letters), so ordinary names like "Clear" are left intact.
    void expand_shorthand_locked(std::vector<std::string>& names) const {
        if (names.size() != 1) {
            return;
        }
        const std::string& candidate = names[0];
        const bool has_lowercase = candidate.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos;
        if (candidate.size() == static_cast<std::size_t>(kFilterWheelSlotCount) && !has_lowercase &&
            candidate.find_first_of(",; \t") == std::string::npos) {
            std::vector<std::string> expanded;
            expanded.reserve(candidate.size());
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
        if (provided_size != kFilterWheelSlotCount) {
            throw AlpacaException(
                "Invalid " + std::string(field_name) + " length: expected " + std::to_string(kFilterWheelSlotCount),
                AlpacaError::InvalidValue);
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
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;

    std::mutex transition_mutex_;  // serializes set_connected() connect/disconnect transitions
};

std::unique_ptr<FilterWheelDriver> create_wandererastro_filterwheel(int device_number, const std::string& serial_port,
                                                                    int baud_rate) {
    FilterWheelConnectionConfig config;
    config.serial_port = serial_port;
    config.baud_rate = baud_rate;
    return std::make_unique<WandererFilterWheelDriver>(device_number, std::move(config));
}

std::unique_ptr<FilterWheelDriver> create_wandererastro_filterwheel_by_index(int device_number, int filterwheel_index) {
    // Defer the (blocking) port scan to connect time, which runs on the
    // driver's background connection thread — never at registration.
    FilterWheelConnectionConfig config;
    config.auto_detect_index = filterwheel_index;
    return std::make_unique<WandererFilterWheelDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::wandererastro
