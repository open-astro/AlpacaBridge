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
#include <alpacacore/util/poll_settle.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogTag = "ToupTek";

}  // namespace

class ToupTekFilterWheelDriver : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ToupTekFilterWheelDriver(int device_number, std::optional<int> wheel_index, std::optional<std::string> wheel_id,
                             ToupTekSDK& sdk, util::TaskClock& clock)
        : AsyncConnectable(kLogTag),
          sdk_(sdk),
          clock_(clock),
          device_number_(device_number),
          wheel_index_(wheel_index),
          wheel_id_(std::move(wheel_id)),
          status_cache_("ToupTek AFW", kStatusTtl, 3, kLogTag, clock) {}

    ~ToupTekFilterWheelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                ToupTekFilterWheelDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, std::string("Error during AFW destruction: ") + e.what());
            }
        }
    }

    // AlpacaDriver --------------------------------------------------------

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.name.empty()) {
            return info_.name;
        }
        return "ToupTek AFW";
    }

    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.id.empty()) {
            return "TOUPTEK_AFW_" + info_.id;
        }
        if (wheel_id_.has_value() && !wheel_id_->empty()) {
            return "TOUPTEK_AFW_" + *wheel_id_;
        }
        return "TOUPTEK_AFW_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "ToupTek AFW Filter Wheel Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore ToupTek AFW Filter Wheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only (never in
    // DriverInfo). Toupcam_Version() returns e.g. "57.27567.20260128".
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = sdk_.get_sdk_version();
        if (version.empty() || version == "unknown") {
            return std::nullopt;
        }
        return util::normalize_dotted_version(version);
    }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        if (!connected && !connected_.load()) {
            // Disconnect requested mid-homing (a sync connect holds transition_mutex_ and the wheel
            // open for the poll, a window the base's conn_task_ gate cannot see). This gate runs BEFORE
            // transition_mutex_ so the disconnect returns at once instead of waiting out the homing.
            // connected_ is still false, so the "connected == connected_" early-return below would
            // silently drop it: the wheel would come up Connected despite an explicit disconnect.
            // Record the intent; the in-flight connect consumes it after homing and stays disconnected.
            std::lock_guard<std::mutex> lock(mutex_);
            if (connecting_homing_) {
                record_pending_disconnect();
                return;
            }
        }
        // transition_mutex_ spans the rest of the call and no getter takes it: the connect handshake
        // and the homing poll run under it and the I/O lock, never under mutex_ (decision 0016).
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Base gate for the async startup window (connection thread spawned but
        // not yet holding the transition lock, connecting_homing_ still false).
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = sdk_;

        if (connected) {
            if (connecting_homing_) {
                // Redundant since transition_mutex_ serialises set_connected (a second connect now waits
                // and then sees connected_); kept as a cheap guard against a double ref-counted open
                // (decision: keep, the disconnect gate above needs the flag).
                return;
            }
            if (consume_pending_disconnect(connected_.load())) {
                // A disconnect arrived AFTER this connect was requested but before
                // this thread took the transition lock (the spawner publishes the in-flight
                // state first). The disconnect is newer, so it supersedes this connect:
                // consume the flag and stay disconnected instead of opening the wheel.
                return;
            }
            set_connecting_homing(true);
            // Everything after the flag is set — INCLUDING enumeration and the SDK
            // open, which throw on a missing/unplugged wheel — must run inside the
            // try, so the catch always clears connecting_homing_. If it stayed set
            // after a failed connect, a stale flag would send every later disconnect
            // down the pending-disconnect path.
            ToupFilterWheelInfo info;
            HToupcam handle = nullptr;
            int slots = 0;
            bool superseded = false;
            try {
                {
                    std::lock_guard<std::mutex> io(io_mutex_);
                    info = resolve_wheel(sdk);
                    handle = sdk.open_filter_wheel_by_id(info.id);
                    slots = sdk.get_filter_wheel_slot_count(handle);
                    if (slots <= 0) {
                        throw AlpacaException("Filter wheel reported invalid slot count", AlpacaError::DriverException);
                    }
                    // Re-apply the wheel's slot configuration, then home it, so the
                    // firmware establishes its slot reference before any move. This
                    // mirrors the toupbase reference driver's connect sequence and
                    // is what stops the wheel from hunting/ticking without landing
                    // (notably after a firmware update).
                    sdk.set_filter_wheel_slot_count(handle, slots);
                    sdk.reset_filter_wheel(handle);
                    // reset_filter_wheel returns immediately but the firmware takes
                    // ~1.5 s to home. Wait for it to settle before reporting connected:
                    // a SetPosition arriving mid-home aborts the cycle and leaves the
                    // slot reference unknown (moves then land on wrong slots). The SDK
                    // reports -1 while moving; poll until it settles (or time out).
                    // The poll holds the I/O lock but not mutex_, so a concurrent
                    // GetName/GetUniqueId (NINA calls these eagerly at enumeration)
                    // doesn't block for up to 6 s; handle_/info_ are still unpublished,
                    // so other readers see "not connected" and fast-fail. It reads the
                    // SDK directly, never through status_cache_: a good homing read
                    // through the cache would clear the failure count.
                    const bool settled = wait_for_home(sdk, handle);
                    {
                        // The consume and the flag clear are one mutex_ hold: a disconnect recorded
                        // between them (the gate in the top of set_connected reads connecting_homing_
                        // under mutex_) would be reported successful and then lost, leaving the wheel
                        // Connected and the flag set for the next connect. I/O lock then mutex_ is the
                        // decision 0016 order. A set_connected(false) that landed during the homing
                        // poll is honoured: nothing is published and the handle is closed below.
                        std::lock_guard<std::mutex> lock(mutex_);
                        superseded = consume_pending_disconnect(connected_.load());
                        connecting_homing_ = false;
                        if (!superseded && settled) {
                            handle_ = handle;
                            info_ = info;
                            slot_count_ = slots;
                            normalize_slot_data_locked();
                        }
                    }
                    if (superseded) {
                        // A requested disconnect, not a failure: close the just-opened handle and
                        // return cleanly (handle_/info_ were never published).
                        sdk.close_filter_wheel(handle);
                        return;
                    }
                    if (!settled) {
                        // Neither a moving->settled nor a stable-slot signal in 6 s:
                        // the wheel is obstructed or the firmware is stuck. Fail the
                        // connect rather than report Connected on a wheel whose slot
                        // reference is unknown (every move would then land wrong).
                        throw AlpacaException("Filter wheel did not settle after homing", AlpacaError::DriverException);
                    }
                }
            } catch (...) {
                if (handle && !superseded) {  // null if resolve/open itself threw — nothing to close
                    std::lock_guard<std::mutex> io(io_mutex_);
                    sdk.close_filter_wheel(handle);
                }
                {
                    // Restore a clean disconnected state — handle_/info_/slot_count_
                    // may have been published before the throw.
                    std::lock_guard<std::mutex> lock(mutex_);
                    handle_ = nullptr;
                    info_ = {};
                    slot_count_ = 0;
                    connecting_homing_ = false;
                }
                // Deliberately consumed even if a disconnect was recorded during
                // homing: the connect failed, so the device already ends in the
                // disconnected state that request wanted — and a stale flag must
                // not abort the user's NEXT (retry) connect at its entry check.
                consume_pending_disconnect(connected_.load());
                throw;
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            status_cache_.reset();
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws (e.g.
        // device unplugged) the error still surfaces, but the driver must not
        // stay half-connected. The slot count is reset so a post-disconnect
        // set_names/set_focus_offsets isn't validated against a stale count (e.g. after
        // swapping a 5-slot wheel for a 7-slot before reconnecting). A fresh instance starts at 0 too.
        HToupcam close_handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_handle = handle_;
            handle_ = nullptr;
            slot_count_ = 0;
            info_ = {};
            connected_.store(false);
        }
        status_cache_.reset();
        if (close_handle) {
            // Waits for an exchange in flight: the handle is never closed under one.
            std::lock_guard<std::mutex> io(io_mutex_);
            sdk.close_filter_wheel(close_handle);
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

    // FilterWheelDriver ---------------------------------------------------

    int get_position() const override {
        ensure_connected();
        const HToupcam handle = handle_value();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: cache mutex, I/O lock,
        // mutex_), so it must not take mutex_ and no caller may hold mutex_ or the I/O lock around the cache.
        const Status status = status_cache_.get([this, handle] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            if (!connected_.load()) {
                // A disconnect that won the I/O lock has closed the handle. Returning (not throwing) keeps it
                // out of the link-health count; the disconnect's reset() drops this frame.
                fresh.closed = true;
                return fresh;
            }
            fresh.position = sdk_.get_filter_wheel_position(handle);
            return fresh;
        });
        if (status.closed) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
        // The SDK returns -1 while the wheel is in motion, which is exactly the
        // ASCOM "moving" sentinel — pass it through unchanged, and never hold it:
        // the next poll sees the move end at once.
        if (status.position == kMovingPosition) {
            status_cache_.invalidate();
        }
        return status.position;
    }

    void set_position(int position) override {
        // Range validation precedes the connection check (ASCOM precedence, per
        // driver-lifecycle.instructions.md): a negative position is unconditionally invalid, so it is
        // InvalidValue even while disconnected. The upper bound depends on
        // slot_count_ (0 until connect), so it is checked below under the lock.
        if (position < 0) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ensure_connected();
        HToupcam handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Check the disconnect sentinel first so a concurrent disconnect yields
            // NotConnected, not a generic DriverException (matches get_position).
            if (!handle_) {
                throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
            }
            if (slot_count_ <= 0) {
                throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
            }
            if (position >= slot_count_) {
                throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
            }
            handle = handle_;
        }
        try {
            std::lock_guard<std::mutex> io(io_mutex_);
            // Race (invariant B4 i): mutex_ was released after validation, so a disconnect can win the I/O
            // lock first and close the wheel; moving a closed handle would be a use-after-close. Re-check here.
            ensure_connected();
            // Single absolute move; the wheel was homed at connect so the firmware
            // can traverse to any slot. get_position() reports -1 until it arrives.
            sdk_.set_filter_wheel_position(handle, position);
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
        if (slot_count_ > 0) {
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
        // the count, so the shorthand works when connected too — not only on the
        // pre-connect config path (where slot_count_ is still 0). Stage on a local
        // copy and only commit on success, so a validation throw leaves the
        // existing filter_names_ untouched (not a half-applied array).
        std::vector<std::string> staged = names;
        expand_shorthand_locked(staged);
        if (slot_count_ > 0) {
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

    HToupcam handle_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!handle_) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
        return handle_;
    }

    void set_connecting_homing(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        connecting_homing_ = value;
    }

    // Caller holds io_mutex_ (the enumeration is a device call).
    ToupFilterWheelInfo resolve_wheel(ToupTekSDK& sdk) {
        auto wheels = sdk.enumerate_filter_wheels();
        if (wheels.empty()) {
            ALPACA_LOG_WARN(kLogTag, "No ToupTek AFW filter wheels detected by SDK");
            throw AlpacaException("No ToupTek AFW filter wheels detected", AlpacaError::NotConnected);
        }
        if (wheel_id_.has_value() && !wheel_id_->empty()) {
            for (const auto& w : wheels) {
                if (w.id == *wheel_id_) {
                    return w;
                }
            }
            throw AlpacaException("ToupTek AFW id not found: " + *wheel_id_, AlpacaError::NotConnected);
        }
        if (wheel_index_.has_value()) {
            int index = wheel_index_.value();
            if (index < 0 || index >= static_cast<int>(wheels.size())) {
                throw AlpacaException("Filter wheel index out of range", AlpacaError::InvalidValue);
            }
            return wheels[static_cast<std::size_t>(index)];
        }
        throw AlpacaException("Filter wheel identifier not specified", AlpacaError::InvalidValue);
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
            ALPACA_LOG_WARN(kLogTag, "Configured filterNames count (" + std::to_string(filter_names_.size()) +
                                         ") does not match wheel slot count (" + std::to_string(slots) +
                                         "); resizing to match the wheel");
            filter_names_.resize(slots);
        }
        apply_default_names_locked();
        if (focus_offsets_.empty()) {
            focus_offsets_.assign(slots, 0);
        } else if (focus_offsets_.size() != slots) {
            ALPACA_LOG_WARN(kLogTag, "Configured focusOffsets count (" + std::to_string(focus_offsets_.size()) +
                                         ") does not match wheel slot count (" + std::to_string(slots) +
                                         "); resizing to match the wheel");
            focus_offsets_.resize(slots);
        }
    }

    // Block until the wheel firmware finishes its home cycle (SDK reports -1
    // while moving, a non-negative slot once settled), or a timeout elapses.
    // Sleeps first so the move has actually begun before the first read.
    // Returns true once the wheel has provably homed, false on timeout (the
    // caller fails the connect). Requires kStableReads CONSECUTIVE non-negative
    // reads before declaring the wheel settled — even after the -1 "moving"
    // state has cleared — so a brief valid-slot bounce during deceleration is
    // not mistaken for a completed home (any -1 read resets the counter).
    // Hardware that homes in under one poll interval (never reporting -1)
    // settles via the same consecutive-read requirement rather than waiting out
    // the full timeout. The unconditional stability requirement costs at most
    // kStableReads poll intervals over an on-first-slot return.
    bool wait_for_home(ToupTekSDK& sdk, HToupcam handle) {
        constexpr int kPollMs = 100;
        constexpr int kMaxPolls = 60;    // 60 polls x 100 ms = 6 s cap
        constexpr int kStableReads = 3;  // consecutive real-slot reads = settled
        // The settle DECISION lives in the pure util::ConsecutiveSettle state
        // machine (deceleration-bounce reset, poll budget, final-poll settle),
        // unit-tested with scripted sequences in test_poll_settle.cpp — this
        // loop only supplies the cadence and the reading.
        util::ConsecutiveSettle settle(kStableReads, kMaxPolls);
        for (;;) {
            clock_.sleep_for(std::chrono::milliseconds(kPollMs));
            int pos = sdk.get_filter_wheel_position(handle);
            switch (settle.feed(pos >= 0)) {
                case util::ConsecutiveSettle::State::Settled:
                    return true;
                case util::ConsecutiveSettle::State::TimedOut:
                    return false;
                case util::ConsecutiveSettle::State::Pending:
                    break;
            }
        }
    }

    // Expand a single delimiter-less token into per-slot single-character names
    // ("LRGB" -> L,R,G,B) only when it looks like a shorthand code (no lowercase
    // letters), so ordinary names like "Clear" or "Ha_NB" that happen to match
    // the slot count are left intact. No-op until the slot count is known.
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
        if (slot_count_ <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (provided_size != slot_count_) {
            throw AlpacaException(
                "Invalid " + std::string(field_name) + " length: expected " + std::to_string(slot_count_),
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

    // Injected SDK seam (issue #104); see touptek_sdk_wrapper.h.
    ToupTekSDK& sdk_;
    util::TaskClock& clock_;
    int device_number_;
    std::optional<int> wheel_index_;
    std::optional<std::string> wheel_id_;

    HToupcam handle_{nullptr};
    ToupFilterWheelInfo info_{};
    int slot_count_{0};
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;

    std::atomic<bool> connected_{false};
    // True while a set_connected(true) has opened the wheel and is homing. Guarded by mutex_. Read by
    // the sync disconnect gate BEFORE transition_mutex_, which is why this driver keeps its own
    // record_pending_disconnect() call: the base's conn_task_ gate cannot see this sync-connect homing
    // window, and the disconnect must return at once instead of waiting out the homing.
    bool connecting_homing_ = false;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // short state reads and writes, never held across an SDK call), then the SDK wrapper's own mutex.
    // transition_mutex_ spans set_connected() only and no getter takes it (the disconnect-during-homing
    // gate takes mutex_ alone); io_mutex_ serialises the device transactions (connect and homing, close,
    // move, the Position refill). Never take the cache mutex (reset/invalidate) while holding io_mutex_
    // or mutex_.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_index(int device_number, int wheel_index) {
    return std::make_unique<ToupTekFilterWheelDriver>(device_number, wheel_index, std::nullopt,
                                                      ToupTekSDKWrapper::instance(), util::default_task_clock());
}

std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_id(int device_number, const std::string& wheel_id) {
    return std::make_unique<ToupTekFilterWheelDriver>(device_number, std::nullopt, wheel_id,
                                                      ToupTekSDKWrapper::instance(), util::default_task_clock());
}

std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_index(int device_number, int wheel_index,
                                                                       ToupTekSDK& sdk, util::TaskClock& clock) {
    return std::make_unique<ToupTekFilterWheelDriver>(device_number, wheel_index, std::nullopt, sdk, clock);
}

std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_id(int device_number, const std::string& wheel_id,
                                                                    ToupTekSDK& sdk, util::TaskClock& clock) {
    return std::make_unique<ToupTekFilterWheelDriver>(device_number, std::nullopt, wheel_id, sdk, clock);
}

}  // namespace alpacacore::vendor::touptek
