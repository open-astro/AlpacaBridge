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
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>
#include <alpacacore/vendor/playerone/playerone_pw_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::playerone {

class PlayerOnePWFilterWheelDriver : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    PlayerOnePWFilterWheelDriver(int device_number, int wheel_index, PlayerOnePWSDK& sdk)
        : AsyncConnectable("PlayerOne"),
          sdk_(sdk),
          device_number_(device_number),
          wheel_index_(wheel_index),
          handle_(-1),
          wheel_info_(),
          wheel_info_valid_(false),
          filter_names_(),
          focus_offsets_(),
          connected_(false),
          status_cache_("Player One PW", kStatusTtl, 3, "PlayerOne") {}

    ~PlayerOnePWFilterWheelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                PlayerOnePWFilterWheelDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Error during PW destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && !wheel_info_.name.empty()) {
            return wheel_info_.name;
        }
        return "Player One Phoenix Wheel";
    }

    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_info_valid_ && !wheel_info_.serial_number.empty()) {
            return "PlayerOne_PW_SN_" + wheel_info_.serial_number;
        }
        return "PlayerOne_PW_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "Player One Phoenix Filter Wheel Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore Player One Phoenix Filter Wheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

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
                PlayerOnePWInfo info;
                const int handle = resolve_handle(info);
                sdk.open_wheel(handle);
                try {
                    seed_slot_data(handle, info);
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        handle_ = -1;
                    }
                    sdk.close_wheel(handle);
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
        int close_handle = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_handle = handle_;
            handle_ = -1;
            wheel_info_ = {};
            wheel_info_valid_ = false;
            connected_.store(false);
        }
        status_cache_.reset();
        if (close_handle >= 0) {
            // Waits for an exchange in flight: the handle is never closed under one.
            std::lock_guard<std::mutex> io(io_mutex_);
            sdk.close_wheel(close_handle);
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

    int get_position() const override {
        ensure_connected();
        const int handle = handle_value();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: cache mutex, I/O lock,
        // mutex_), so it must not take mutex_ and no caller may hold mutex_ or the I/O lock around the cache.
        const Status status = status_cache_.get([this, handle] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            if (!connected_.load()) {
                // A disconnect that won the I/O lock has closed the wheel. Returning (not throwing) keeps it
                // out of the link-health count; the disconnect's reset() drops this frame.
                fresh.closed = true;
                return fresh;
            }
            try {
                fresh.position = sdk_.get_position(handle);
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
        int handle = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Check the disconnect sentinel first so a concurrent disconnect yields
            // NotConnected, not a generic DriverException (matches get_position).
            if (handle_ < 0) {
                throw AlpacaException("Filter wheel handle not set", AlpacaError::NotConnected);
            }
            if (!wheel_info_valid_ || wheel_info_.position_count <= 0) {
                throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
            }
            if (position >= wheel_info_.position_count) {
                throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
            }
            handle = handle_;
        }
        try {
            std::lock_guard<std::mutex> io(io_mutex_);
            // Race (invariant B4 i): mutex_ was released after validation, so a disconnect can win the I/O
            // lock first and close the wheel; moving a closed handle would be a use-after-close. Re-check here.
            ensure_connected();
            sdk_.goto_position(handle, position);
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
        if (wheel_info_valid_ && wheel_info_.position_count > 0) {
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
        if (wheel_info_valid_ && wheel_info_.position_count > 0) {
            validate_slot_count_locked(static_cast<int>(names.size()), "names");
        }
        filter_names_ = names;
        apply_default_names_locked();
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
    }

    // Caller holds io_mutex_ (the enumeration is a device call). Publishes nothing: seed_slot_data
    // stores the info at the end of a successful connect (decision 0016 rule 3).
    int resolve_handle(PlayerOnePWInfo& out_info) {
        auto wheels = sdk_.enumerate_wheels();
        if (wheels.empty()) {
            ALPACA_LOG_WARN("PlayerOne", "No Player One filter wheels detected by SDK");
            throw AlpacaException("No Player One filter wheels detected", AlpacaError::NotConnected);
        }
        if (wheel_index_ < 0 || wheel_index_ >= static_cast<int>(wheels.size())) {
            ALPACA_LOG_WARN("PlayerOne", "Filter wheel index out of range: " + std::to_string(wheel_index_) +
                                             " (count=" + std::to_string(wheels.size()) + ")");
            throw AlpacaException("Filter wheel index not found", AlpacaError::InvalidValue);
        }
        out_info = wheels[static_cast<std::size_t>(wheel_index_)];
        return out_info.handle;
    }

    int handle_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ < 0) {
            throw AlpacaException("Filter wheel handle not set", AlpacaError::NotConnected);
        }
        return handle_;
    }

    // The Phoenix Wheel stores per-slot filter aliases and focus offsets in the
    // wheel itself. Use them as defaults at connect; names/offsets supplied via
    // config (set_names/set_focus_offsets) take precedence. Caller holds io_mutex_: the SDK reads run
    // off mutex_, and the result (with handle_) publishes under it.
    void seed_slot_data(int handle, const PlayerOnePWInfo& info) {
        std::size_t slots = 0;
        bool want_names = false;
        bool want_offsets = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (info.position_count <= 0) {
                throw AlpacaException("Filter wheel reported no filter positions", AlpacaError::DriverException);
            }
            slots = static_cast<std::size_t>(info.position_count);
            want_names = filter_names_.empty();
            want_offsets = focus_offsets_.empty();
        }
        auto& sdk = sdk_;

        std::vector<std::string> aliases;
        if (want_names) {
            aliases.assign(slots, std::string());
            for (std::size_t i = 0; i < slots; ++i) {
                try {
                    aliases[i] = sdk.get_filter_alias(handle, static_cast<int>(i));
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("PlayerOne", "PW filter alias unavailable for slot " + std::to_string(i) + ": " +
                                                     std::string(e.what()));
                }
            }
        }
        std::vector<int> offsets;
        if (want_offsets) {
            offsets.assign(slots, 0);
            for (std::size_t i = 0; i < slots; ++i) {
                try {
                    offsets[i] = sdk.get_focus_offset(handle, static_cast<int>(i));
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("PlayerOne", "PW focus offset unavailable for slot " + std::to_string(i) + ": " +
                                                     std::string(e.what()));
                }
            }
        }

        // Identity and limits publish here with handle_, at the end of connect: a connect that throws
        // earlier leaves the pre-connect values.
        std::lock_guard<std::mutex> lock(mutex_);
        handle_ = handle;
        wheel_info_ = info;
        wheel_info_valid_ = true;
        if (filter_names_.empty()) {
            filter_names_ = aliases.empty() ? std::vector<std::string>(slots) : std::move(aliases);
        } else if (filter_names_.size() != slots) {
            ALPACA_LOG_WARN("PlayerOne", "Configured filterNames count (" + std::to_string(filter_names_.size()) +
                                             ") does not match wheel position count (" + std::to_string(slots) +
                                             "); resizing to match the wheel");
            filter_names_.resize(slots);
        }
        apply_default_names_locked();

        if (focus_offsets_.empty()) {
            focus_offsets_ = offsets.empty() ? std::vector<int>(slots, 0) : std::move(offsets);
        } else if (focus_offsets_.size() != slots) {
            ALPACA_LOG_WARN("PlayerOne", "Configured focusOffsets count (" + std::to_string(focus_offsets_.size()) +
                                             ") does not match wheel position count (" + std::to_string(slots) +
                                             "); resizing to match the wheel");
            focus_offsets_.resize(slots);
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
        if (provided_size != wheel_info_.position_count) {
            throw AlpacaException("Invalid " + std::string(field_name) + " length: expected " +
                                      std::to_string(wheel_info_.position_count),
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

    PlayerOnePWSDK& sdk_;
    int device_number_;
    int wheel_index_;
    int handle_;
    PlayerOnePWInfo wheel_info_;
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

std::unique_ptr<FilterWheelDriver> create_playerone_filterwheel(int device_number, int wheel_index) {
    return create_playerone_filterwheel(device_number, wheel_index, PlayerOnePWSDKWrapper::instance());
}

std::unique_ptr<FilterWheelDriver> create_playerone_filterwheel(int device_number, int wheel_index,
                                                                PlayerOnePWSDK& sdk) {
    return std::make_unique<PlayerOnePWFilterWheelDriver>(device_number, wheel_index, sdk);
}

}  // namespace alpacacore::vendor::playerone
