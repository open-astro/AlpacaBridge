// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogTag = "ToupTek";

}  // namespace

class ToupTekFilterWheelDriver : public FilterWheelDriver {
public:
    ToupTekFilterWheelDriver(int device_number, std::optional<int> wheel_index, std::optional<std::string> wheel_id)
        : device_number_(device_number), wheel_index_(wheel_index), wheel_id_(std::move(wheel_id)) {}

    ~ToupTekFilterWheelDriver() override {
        {
            // Block any new connection task from spawning a thread that would
            // outlive this object (destructor race -> std::terminate on an
            // unjoined connection_thread_).
            std::lock_guard<std::mutex> lock(connection_mutex_);
            shutting_down_ = true;
        }
        stop_connection_thread();
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

    std::string get_unique_id() const override {
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
        auto version = ToupTekSDKWrapper::instance().get_sdk_version();
        if (version.empty() || version == "unknown") {
            return std::nullopt;
        }
        return util::normalize_dotted_version(version);
    }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!connected && (connecting_homing_ || (connecting_.load() && inflight_is_connect_.load()))) {
            // Disconnect requested while a connect is in flight — either mid-homing
            // (connecting_homing_, mutex_ released for the poll) or in the async
            // startup window (connection thread spawned but not yet holding mutex_,
            // so connecting_homing_ is still false). In both, connected_ is still
            // false and the "connected == connected_" early-return below would
            // silently drop this: the wheel would come up Connected despite an
            // explicit disconnect. Record the intent; the in-flight connect consumes
            // it at entry (startup window) or after homing and stays disconnected.
            pending_disconnect_ = true;
            return;
        }
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = ToupTekSDKWrapper::instance();

        if (connected) {
            if (connecting_homing_) {
                // Another set_connected(true) is already mid-connect and has
                // released mutex_ during the homing poll. Opening again would double
                // the ref-counted SDK open and leak the handle when only one close
                // fires. The in-flight connect will publish handle_/connected_; this
                // call becomes a no-op. (connecting_ in start_connection_task only
                // guards the async path — set_connected is a public sync entry too.)
                return;
            }
            if (pending_disconnect_) {
                // A disconnect arrived AFTER this connect was requested but before
                // this thread acquired mutex_ (the spawner sets connecting_ first;
                // acquiring mutex_ can additionally wait behind property getters).
                // The disconnect is newer, so it supersedes this connect: consume
                // the flag and stay disconnected instead of opening the wheel.
                pending_disconnect_ = false;
                return;
            }
            connecting_homing_ = true;
            // Everything after the flag is set — INCLUDING enumeration and the SDK
            // open, which throw on a missing/unplugged wheel — must run inside the
            // try, so the catch always clears connecting_homing_. If it stayed set
            // after a failed connect, the guard above would silently no-op every
            // retry: a permanent reconnect lockout until the driver is destroyed.
            ToupFilterWheelInfo info;
            HToupcam handle = nullptr;
            try {
                info = resolve_wheel_locked(sdk);
                handle = sdk.open_filter_wheel_by_id(info.id);
                int slots = sdk.get_filter_wheel_slot_count(handle);
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
                // Release mutex_ during the poll so a concurrent GetName/GetUniqueId
                // (NINA calls these eagerly at enumeration) doesn't block for up to
                // 6 s. set_connected is serialised by connection_mutex_, so no other
                // set_connected races this window, and handle_/info_ are still
                // unpublished so other readers see "not connected" and fast-fail.
                lock.unlock();
                const bool settled = wait_for_home(sdk, handle);
                lock.lock();
                if (pending_disconnect_) {
                    // A set_connected(false) landed during the mutex-released homing
                    // poll. Honor it: close the just-opened handle and stay
                    // disconnected rather than publishing handle_/connected_. This is
                    // a requested disconnect, not a failure — return cleanly (no throw).
                    sdk.close_filter_wheel(handle);
                    handle_ = nullptr;
                    info_ = {};
                    slot_count_ = 0;
                    connecting_homing_ = false;
                    pending_disconnect_ = false;
                    return;
                }
                if (!settled) {
                    // Neither a moving->settled nor a stable-slot signal in 6 s:
                    // the wheel is obstructed or the firmware is stuck. Fail the
                    // connect rather than report Connected on a wheel whose slot
                    // reference is unknown (every move would then land wrong).
                    throw AlpacaException("Filter wheel did not settle after homing", AlpacaError::DriverException);
                }
                handle_ = handle;
                info_ = info;
                slot_count_ = slots;
                normalize_slot_data_locked();
            } catch (...) {
                if (handle) {  // null if resolve/open itself threw — nothing to close
                    sdk.close_filter_wheel(handle);
                }
                // Restore a clean disconnected state — handle_/info_/slot_count_
                // may have been published before the throw. Re-take the lock if a
                // throw from the mutex-released wait_for_home window landed here.
                if (!lock.owns_lock()) {
                    lock.lock();
                }
                handle_ = nullptr;
                info_ = {};
                slot_count_ = 0;
                connecting_homing_ = false;   // cleared under mutex_ on failure
                pending_disconnect_ = false;  // consumed with this attempt
                throw;
            }
            connecting_homing_ = false;  // cleared under mutex_ on success
            connected_.store(true);
            return;
        }

        if (handle_) {
            sdk.close_filter_wheel(handle_);
            handle_ = nullptr;
        }
        // Reset the slot count so a post-disconnect set_names/set_focus_offsets
        // isn't validated against a stale count (e.g. after swapping a 5-slot wheel
        // for a 7-slot before reconnecting). A fresh instance starts at 0 too.
        slot_count_ = 0;
        info_ = {};
        connected_.store(false);
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
        // Hold mutex_ across the SDK call: set_connected(false) takes mutex_
        // before it closes handle_, so serialising here prevents a
        // use-after-close if a disconnect races in (matches the thermal switch
        // driver's get_switch_value pattern).
        std::lock_guard<std::mutex> lock(mutex_);
        if (!handle_) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
        // The SDK returns -1 while the wheel is in motion, which is exactly the
        // ASCOM "moving" sentinel — pass it through unchanged.
        return ToupTekSDKWrapper::instance().get_filter_wheel_position(handle_);
    }

    void set_position(int position) override {
        // Range validation precedes the connection check (ASCOM precedence, per
        // AGENTS.md): a negative position is unconditionally invalid, so it is
        // InvalidValue even while disconnected. The upper bound depends on
        // slot_count_ (0 until connect), so it is checked below under the lock.
        if (position < 0) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ensure_connected();
        // Hold mutex_ across validation and the SDK move for the same
        // use-after-close reason as get_position.
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
        // Single absolute move; the wheel was homed at connect so the firmware
        // can traverse to any slot. get_position() reports -1 until it arrives.
        ToupTekSDKWrapper::instance().set_filter_wheel_position(handle_, position);
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

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (shutting_down_) {
            return;  // Destruction in progress; never spawn a new thread.
        }
        if (connecting_.load()) {
            // A connect/disconnect is already in flight. An async disconnect (the
            // router dispatches both the legacy Connected=false PUT and the Alpaca v2
            // disconnect endpoint through here) racing an in-flight CONNECT must NOT
            // be dropped, or the wheel comes up Connected despite the explicit
            // request. Record the intent whenever the in-flight task is a connect —
            // covering the startup window (thread spawned, mutex_ not yet held, so
            // connecting_homing_ still false; widened in practice by property getters
            // contending on mutex_) as well as the homing poll. The connect consumes
            // the flag at entry, after homing, or in the post-connect re-check below.
            // A disconnect racing an in-flight DISCONNECT needs nothing (idempotent).
            // Lock order connection_mutex_ -> mutex_ — set_connected takes mutex_
            // alone and never connection_mutex_, so this never inverts.
            if (!connect && inflight_is_connect_.load()) {
                std::lock_guard<std::mutex> state_lock(mutex_);
                pending_disconnect_ = true;
            }
            return;
        }
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
        inflight_is_connect_.store(connect);
        connecting_.store(true);
        connection_thread_ = std::thread([this, connect]() {
            try {
                set_connected(connect);
                if (connect) {
                    // Consume a disconnect that landed after the post-homing check
                    // (or after an entry-consumed abort) but before this task
                    // finished — without this, a disconnect arriving in that tail
                    // would be recorded and never honored. The remaining exposure
                    // is the few instructions between this check and
                    // connecting_.store(false); a leaked flag there self-heals (the
                    // next connect consumes it at entry and the client retries).
                    bool need_disconnect = false;
                    {
                        std::lock_guard<std::mutex> state_lock(mutex_);
                        if (pending_disconnect_) {
                            pending_disconnect_ = false;
                            need_disconnect = connected_.load();
                        }
                    }
                    if (need_disconnect) {
                        set_connected(false);
                    }
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR(kLogTag, std::string("AFW connection failed: ") + e.what());
            }
            connecting_.store(false);
        });
    }

    void stop_connection_thread() {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
    }

    ToupFilterWheelInfo resolve_wheel_locked(ToupTekSDKWrapper& sdk) {
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
    static bool wait_for_home(ToupTekSDKWrapper& sdk, HToupcam handle) {
        constexpr int kPollMs = 100;
        constexpr int kMaxWaitMs = 6000;  // budget; effective wait is up to one
                                          // kPollMs longer (sleep runs before the
                                          // guard rejects waited == kMaxWaitMs)
        constexpr int kStableReads = 3;   // consecutive real-slot reads = settled
        int stable = 0;
        for (int waited = 0; waited < kMaxWaitMs; waited += kPollMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            int pos = sdk.get_filter_wheel_position(handle);
            if (pos < 0) {
                stable = 0;  // firmware reports -1 while the home move runs
            } else if (++stable >= kStableReads) {
                return true;  // settled: kStableReads consecutive real-slot reads
            }
        }
        return false;  // never settled within the timeout
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

    int device_number_;
    std::optional<int> wheel_index_;
    std::optional<std::string> wheel_id_;

    HToupcam handle_{nullptr};
    ToupFilterWheelInfo info_{};
    int slot_count_{0};
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> connecting_{false};
    // Direction of the task connecting_ refers to (true = connect). Written in
    // start_connection_task under connection_mutex_ before connecting_ goes true;
    // atomic so the sync set_connected(false) gate (which holds mutex_, never
    // connection_mutex_ — lock order forbids it) can read it. Only meaningful
    // while connecting_ is true; always checked conjunctively with it.
    std::atomic<bool> inflight_is_connect_{false};
    // True while a set_connected(true) has opened the wheel and is homing with
    // mutex_ released. Guarded by mutex_. Blocks a racing SYNC set_connected(true)
    // (the public ASCOM Connected setter, which bypasses start_connection_task's
    // connecting_ guard) from opening the same wheel a second time — a double
    // ref-counted open would leak the handle when only one close fires.
    bool connecting_homing_ = false;
    // Set when a disconnect arrives while a connect is in flight — during the
    // mutex-released homing poll (connecting_homing_) OR in the async startup
    // window before the connect thread holds mutex_ (connecting_ +
    // inflight_is_connect_). In both, connected_ is still false so the plain
    // "connected == connected_" early-return would silently drop the disconnect
    // and the wheel would come up Connected despite an explicit request. The
    // in-flight connect consumes it at entry (superseded — stays disconnected),
    // after homing (closes the freshly-opened handle), or in the connection
    // task's post-connect re-check. Guarded by mutex_.
    bool pending_disconnect_ = false;
    bool shutting_down_ = false;  // guarded by connection_mutex_
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_index(int device_number, int wheel_index) {
    return std::make_unique<ToupTekFilterWheelDriver>(device_number, wheel_index, std::nullopt);
}

std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_id(int device_number, const std::string& wheel_id) {
    return std::make_unique<ToupTekFilterWheelDriver>(device_number, std::nullopt, wheel_id);
}

}  // namespace alpacacore::vendor::touptek
