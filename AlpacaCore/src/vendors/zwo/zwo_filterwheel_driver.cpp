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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_efw_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::zwo {

class ZWOEFWFilterWheelDriver : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ZWOEFWFilterWheelDriver(int device_number, std::optional<int> wheel_id, std::optional<int> wheel_index,
                            ZWOEFWSDK& sdk)
        : AsyncConnectable("ZWO"),
          sdk_(sdk),
          device_number_(device_number),
          wheel_id_(wheel_id),
          wheel_index_(wheel_index),
          serial_number_(),
          wheel_info_(),
          wheel_info_valid_(false),
          filter_names_(),
          focus_offsets_(),
          connected_(false),
          status_cache_("ZWO EFW", kStatusTtl, 3, "ZWO") {}

    ~ZWOEFWFilterWheelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during EFW destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && !wheel_info_.name.empty()) {
            return wheel_info_.name;
        }
        return "ZWO EFW";
    }

    DeviceType get_device_type() const override {
        return DeviceType::FilterWheel;
    }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "ZWO_EFW_SN_" + serial_number_;
        }
        if (wheel_id_.has_value()) {
            return "ZWO_EFW_ID_" + std::to_string(wheel_id_.value());
        }
        return "ZWO_EFW_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO EFW Filter Wheel Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO EFW Filter Wheel Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only (never in
    // DriverInfo). EFWGetSDKVersion() returns "1, 7, 0, 0"; render as "1.7.0.0".
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = sdk_.get_sdk_version();
        // get_sdk_version() returns the literal "unknown" when EFWGetSDKVersion()
        // yields nullptr — suppress the row rather than show "unknown".
        if (version.empty() || version == "unknown") {
            return std::nullopt;
        }
        return util::normalize_dotted_version(version);
    }

    int get_interface_version() const override {
        return 3;
    }

    bool get_connected() const override {
        return connected_.load();
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // transition_mutex_ spans the whole call and no getter takes it: the connect handshake
        // below runs under it and the I/O lock, never under mutex_ (decision 0016).
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

        auto& sdk = sdk_;
        if (connected) {
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const int resolved_id = resolve_wheel_id();
                sdk.open_wheel(resolved_id);
                try {
                    ZWOEFWInfo info;
                    if (!sdk.get_wheel_info_by_id(resolved_id, info)) {
                        throw AlpacaException("Failed to read filter wheel info", AlpacaError::DriverException);
                    }
                    std::string serial;
                    try {
                        serial = sdk.get_serial_number(resolved_id);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("ZWO", "EFW serial number unavailable: " + std::string(e.what()));
                    }
                    // Identity and limits publish here, at the end of connect (decision 0016 rule 3):
                    // a connect that throws before this hold leaves the pre-connect values.
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (wheel_index_.has_value()) {
                        wheel_id_ = resolved_id;
                    }
                    wheel_info_ = info;
                    wheel_info_valid_ = true;
                    normalize_slot_data_locked();
                    serial_number_ = std::move(serial);
                } catch (...) {
                    sdk.close_wheel(resolved_id);
                    throw;
                }
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            status_cache_.reset();
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws (e.g.
        // device unplugged) the error still surfaces, but the driver must not
        // stay half-connected.
        std::optional<int> close_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_id = wheel_id_;
            if (wheel_index_.has_value()) {
                wheel_id_.reset();
                wheel_info_ = {};
                wheel_info_valid_ = false;
                serial_number_.clear();
            }
            connected_.store(false);
        }
        status_cache_.reset();
        if (close_id.has_value()) {
            // Waits for an exchange in flight: the handle is never closed under one.
            std::lock_guard<std::mutex> io(io_mutex_);
            sdk.close_wheel(close_id.value());
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

    int get_position() const override {
        ensure_connected();
        const int id = wheel_id_value();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: cache mutex, I/O lock,
        // mutex_), so it must not take mutex_ and no caller may hold mutex_ or the I/O lock around the cache.
        const Status status = status_cache_.get([this, id] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            if (!connected_.load()) {
                // A disconnect that won the I/O lock has closed the wheel. Returning (not throwing) keeps it
                // out of the link-health count; the disconnect's reset() drops this frame.
                fresh.closed = true;
                return fresh;
            }
            try {
                fresh.position = sdk_.get_position(id);
            } catch (const AlpacaException&) {
                throw;
            } catch (const std::exception& e) {
                // Convert a vendor failure to AlpacaException (AGENTS.md); the cache counts it either way.
                throw AlpacaException(e.what(), AlpacaError::DriverException);
            }
            return fresh;
        });
        if (status.closed) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
        if (status.position == kMovingPosition) {
            // A moving frame is never held: the next poll sees the move end at once.
            status_cache_.invalidate();
        }
        return status.position;
    }

    void set_position(int position) override {
        // Range validation precedes the connection check (ASCOM precedence, per
        // driver-lifecycle.instructions.md): a negative position is unconditionally invalid, so it is
        // InvalidValue even while disconnected. The upper bound depends on the
        // slot count (unknown until connect), so it is checked below under the lock.
        if (position < 0) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ensure_connected();
        int id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Check the disconnect sentinel first so a concurrent disconnect yields
            // NotConnected, not a generic DriverException (matches get_position).
            if (!wheel_id_.has_value()) {
                throw AlpacaException("Filter wheel ID not set", AlpacaError::NotConnected);
            }
            if (!wheel_info_valid_ || wheel_info_.slot_count <= 0) {
                throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
            }
            if (position >= wheel_info_.slot_count) {
                throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
            }
            id = wheel_id_.value();
        }
        try {
            std::lock_guard<std::mutex> io(io_mutex_);
            // Race (invariant B4 i): mutex_ was released after validation, so a disconnect can win the I/O
            // lock first and close the wheel; moving a closed id would be a use-after-close. Re-check here.
            ensure_connected();
            sdk_.set_position(id, position);
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
        if (wheel_info_valid_ && wheel_info_.slot_count > 0) {
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
        // Expand a single-token shorthand ("LRGB" -> L,R,G,B) BEFORE validating
        // the count so it works when connected too, not only on the pre-connect
        // config path (matches normalize_slot_data_locked). Stage on a local copy
        // and only commit on success, so a validation throw leaves the existing
        // filter_names_ untouched. Per the "fix shared patterns everywhere" rule.
        std::vector<std::string> staged = names;
        expand_shorthand_locked(staged);
        if (wheel_info_valid_ && wheel_info_.slot_count > 0) {
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

    // Caller holds io_mutex_ (the enumeration is a device call). Publishes nothing: set_connected
    // stores the id and info at the end of a successful connect (decision 0016 rule 3).
    int resolve_wheel_id() {
        if (wheel_index_.has_value()) {
            auto wheels = sdk_.enumerate_wheels();
            if (wheels.empty()) {
                ALPACA_LOG_WARN("ZWO", "No ZWO EFW wheels detected by SDK");
                throw AlpacaException("No ZWO EFW wheels detected", AlpacaError::NotConnected);
            }
            int index = wheel_index_.value();
            for (const auto& info : wheels) {
                if (info.slot_index == index) {
                    return info.wheel_id;
                }
            }
            ALPACA_LOG_WARN("ZWO", "Filter wheel SDK slot not found: " + std::to_string(index));
            throw AlpacaException("Filter wheel index not found", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_id_.has_value()) {
            return wheel_id_.value();
        }

        throw AlpacaException("Filter wheel ID not specified", AlpacaError::InvalidValue);
    }

    int wheel_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wheel_id_.has_value()) {
            throw AlpacaException("Filter wheel ID not set", AlpacaError::NotConnected);
        }
        return wheel_id_.value();
    }

    void normalize_slot_data_locked() {
        if (!wheel_info_valid_ || wheel_info_.slot_count <= 0) {
            return;
        }
        const std::size_t slots = static_cast<std::size_t>(wheel_info_.slot_count);
        expand_shorthand_locked(filter_names_);
        if (filter_names_.empty()) {
            filter_names_.assign(slots, std::string());
        } else if (filter_names_.size() != slots) {
            ALPACA_LOG_WARN("ZWO", "Configured filterNames count (" + std::to_string(filter_names_.size()) +
                                       ") does not match wheel slot count (" + std::to_string(slots) +
                                       "); resizing to match the wheel");
            filter_names_.resize(slots);
        }
        apply_default_names_locked();
        if (focus_offsets_.empty()) {
            focus_offsets_.assign(slots, 0);
        } else if (focus_offsets_.size() != slots) {
            ALPACA_LOG_WARN("ZWO", "Configured focusOffsets count (" + std::to_string(focus_offsets_.size()) +
                                       ") does not match wheel slot count (" + std::to_string(slots) +
                                       "); resizing to match the wheel");
            focus_offsets_.resize(slots);
        }
    }

    // Expand a single delimiter-less token into per-slot single-character names
    // ("LRGB" -> L,R,G,B) only when it looks like a shorthand code (no lowercase
    // letters), so ordinary names like "Clear" or "Ha_NB" that happen to match
    // the slot count are left intact. No-op until the slot count is known.
    void expand_shorthand_locked(std::vector<std::string>& names) const {
        if (!wheel_info_valid_ || wheel_info_.slot_count <= 0 || names.size() != 1) {
            return;
        }
        const std::size_t slots = static_cast<std::size_t>(wheel_info_.slot_count);
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
        if (!wheel_info_valid_ || wheel_info_.slot_count <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (provided_size != wheel_info_.slot_count) {
            throw AlpacaException("Invalid " + std::string(field_name) +
                                  " length: expected " + std::to_string(wheel_info_.slot_count),
                                  AlpacaError::InvalidValue);
        }
    }

    // One refill reads one device value. closed: a disconnect won the I/O lock (not a frame, not a link failure).
    struct Status {
        bool closed{};
        int position{};
    };

    static constexpr std::chrono::milliseconds kStatusTtl{100};
    static constexpr int kMovingPosition = -1;  // the SDK's value while the wheel moves

    ZWOEFWSDK& sdk_;
    int device_number_;
    std::optional<int> wheel_id_;
    std::optional<int> wheel_index_;
    std::string serial_number_;
    ZWOEFWInfo wheel_info_;
    bool wheel_info_valid_;
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;
    std::atomic<bool> connected_;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // short state reads and writes, never held across an SDK call), then the SDK wrapper's own mutex.
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the
    // device transactions (connect, close, move, the Position refill). Never take the cache mutex
    // (reset/invalidate) while holding io_mutex_ or mutex_.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel(int device_number, int wheel_id, ZWOEFWSDK& sdk) {
    return std::make_unique<ZWOEFWFilterWheelDriver>(device_number, wheel_id, std::nullopt, sdk);
}

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel_by_index(int device_number, int wheel_index,
                                                                       ZWOEFWSDK& sdk) {
    return std::make_unique<ZWOEFWFilterWheelDriver>(device_number, std::nullopt, wheel_index, sdk);
}

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel(int device_number, int wheel_id) {
    return create_zwo_efw_filterwheel(device_number, wheel_id, ZWOEFWSDKWrapper::instance());
}

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel_by_index(int device_number, int wheel_index) {
    return create_zwo_efw_filterwheel_by_index(device_number, wheel_index, ZWOEFWSDKWrapper::instance());
}

} // namespace alpacacore::vendor::zwo
