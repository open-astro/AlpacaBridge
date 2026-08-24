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
#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_iefw_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
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
    IefwFilterWheelDriver(int device_number, std::optional<std::string> serial_port, std::optional<int> wheel_index,
                          const std::string& model)
        : AsyncConnectable("iOptron"),
          device_number_(device_number),
          serial_port_(std::move(serial_port)),
          wheel_index_(wheel_index),
          connected_(false),
          protocol_(),
          model_name_(model == "iefw18" ? "iEFW-18" : "iEFW-15") {}

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

    std::string get_unique_id() const override { return "IOPTRON_IEFW_" + std::to_string(device_number_); }

    std::string get_description() const override { return "iOptron iEFW Filter Wheel Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore iOptron iEFW Filter Wheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware captured at connect; surfaced in the web UI only (never
    // DriverInfo). Non-empty exactly while connected.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Base gates BEFORE the idempotency check (see AsyncConnectable).
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
            IefwConnectionConfig config;
            config.serial_port = resolve_serial_port_locked();
            IefwDeviceInfo info = protocol_.connect(config);
            try {
                slot_count_ = iefw_slot_count(info.model);
                if (iefw_model_name(info.model) != model_name_) {
                    ALPACA_LOG_WARN("iOptron", "Configured model " + model_name_ + " but the wheel reports " +
                                                   iefw_model_name(info.model) + " (model code " +
                                                   std::to_string(info.model) + "); using its " +
                                                   std::to_string(slot_count_) + " slots");
                }
                firmware_ = read_firmware_string(info);
                // Offsets stored on the wheel (set via iOptron's own software)
                // seed FocusOffsets when the config supplies none; config
                // values override and nothing is written back (Player One
                // convention).
                const bool offsets_configured = !focus_offsets_.empty();
                normalize_slot_data_locked();
                if (!offsets_configured) {
                    read_stored_offsets_locked();
                }
            } catch (...) {
                protocol_.disconnect();
                throw;
            }
            moving_ = false;
            connected_.store(true);
            ALPACA_LOG_INFO("iOptron",
                            model_name_ + " filter wheel connected (" + std::to_string(slot_count_) + " slots)");
            return;
        }

        try {
            protocol_.disconnect();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "iEFW disconnect error: " + std::string(e.what()));
        }
        firmware_.clear();
        moving_ = false;
        connected_.store(false);
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
        std::lock_guard<std::mutex> lock(mutex_);
        // :WP# answers -1 while the wheel is moving. Also hold -1 from the
        // moment a move is commanded until the wheel reports the target, so
        // a poll that lands before the motor starts does not show the old
        // slot as "arrived" (IFilterWheel: Position is -1 while moving).
        const std::int32_t wire = const_cast<IefwFilterWheelDriver*>(this)->protocol_.get_position();
        if (wire < 0) {
            return -1;
        }
        if (moving_) {
            if (wire != target_) {
                return -1;
            }
            moving_ = false;
        }
        return static_cast<int>(wire);
    }

    void set_position(int position) override {
        // Range validation precedes the connection check (ASCOM precedence):
        // a negative position is unconditionally invalid. The upper bound
        // depends on the slot count (known after connect), checked below.
        if (position < 0) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_count_ <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (position >= slot_count_) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        protocol_.move_to(position);
        target_ = position;
        moving_ = true;
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
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
    }

    // Serial mode uses the configured port; auto mode scans at connect so a
    // wheel plugged in after startup is still found.
    std::string resolve_serial_port_locked() {
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

    void read_stored_offsets_locked() {
        for (std::int32_t slot = 0; slot < slot_count_; ++slot) {
            try {
                focus_offsets_[static_cast<std::size_t>(slot)] = protocol_.get_stored_offset(slot);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("iOptron",
                                "iEFW stored offset read failed for slot " + std::to_string(slot) + ": " + e.what());
                return;
            }
        }
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

    int device_number_;
    std::optional<std::string> serial_port_;
    std::optional<int> wheel_index_;
    std::atomic<bool> connected_;
    IefwProtocolWrapper protocol_;

    // Everything below is guarded by mutex_.
    mutable std::mutex mutex_;
    std::int32_t slot_count_ = 0;   // from the handshake model code at connect
    const std::string model_name_;  // "iEFW-15" / "iEFW-18" from config; fixed after construction
    std::string firmware_;          // web-UI only; non-empty exactly while connected
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;
    // Position reports -1 from a commanded move until the wheel reports the
    // target. moving_ is mutable so the arrival latch can run in const get_position().
    mutable bool moving_ = false;
    std::int32_t target_ = 0;
};

std::unique_ptr<FilterWheelDriver> create_iefw_filterwheel(int device_number, const std::string& serial_port,
                                                           const std::string& model) {
    return std::make_unique<IefwFilterWheelDriver>(device_number, serial_port, std::nullopt, model);
}

std::unique_ptr<FilterWheelDriver> create_iefw_filterwheel_by_index(int device_number, int wheel_index,
                                                                    const std::string& model) {
    return std::make_unique<IefwFilterWheelDriver>(device_number, std::nullopt, wheel_index, model);
}

}  // namespace alpacacore::vendor::ioptron
