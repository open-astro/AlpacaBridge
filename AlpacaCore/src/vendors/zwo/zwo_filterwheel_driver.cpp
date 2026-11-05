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
#include <alpacacore/util/link_epoch.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_efw_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::zwo {

namespace {

// CWE-117: the serial and SDK error texts are device text; control characters never reach the log or a client.
std::string printable(std::string text) {
    std::replace_if(
        text.begin(), text.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f; }, '?');
    return text;
}

}  // namespace

class ZWOEFWFilterWheelDriver : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ZWOEFWFilterWheelDriver(int device_number, std::optional<int> wheel_id, std::optional<int> wheel_index,
                            ZWOEFWSDK& sdk, util::TaskClock& clock)
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
          // The TTL and the fault stamp read the injected clock (decision 0005), so a test ages both with advance().
          status_cache_("ZWO EFW", kStatusTtl, 3, "ZWO", clock) {}

    ~ZWOEFWFilterWheelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (link_.is_open()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
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

    bool get_connected() const override { return link_.is_open(); }

    // Decision 0009 rule 5: the one fault text. While lost, the loss reason, kept until the next Connect or
    // Disconnect; otherwise the Position cache's latched fault, empty while healthy.
    std::string get_link_fault() const override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!lost_reason_.empty()) {
                return lost_reason_;
            }
        }
        return status_cache_.fault();
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
        if (!connected && record_disconnect_if_connect_in_flight(link_.is_open())) {
            return;
        }
        if (connected && consume_pending_disconnect(link_.is_open())) {
            return;
        }
        // A Disconnect of a lost link (Connected already reads false) still tears down: it drops the kept loss
        // reason (decision 0009 rule 5) and releases an open the loss left behind.
        if (connected == link_.is_open() && (connected || !has_lost_reason())) {
            return;
        }
        // A new epoch first: a call that captured the old link can no longer drop this one (issue #1668).
        link_.begin_transition();

        if (connected) {
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                release_open_locked();  // an open left from a lost link
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lost_reason_.clear();
                }
                const auto opened = bind_wheel_locked();
                try {
                    sdk_.with_link(open_id_, open_link_, [&] { publish_connect_state(open_id_, opened.serial); });
                } catch (...) {
                    release_open_locked();
                    throw;
                }
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            status_cache_.reset();
            link_.publish_open();
            return;
        }

        // Clear driver state before the SDK close: a close that fails for another reason than a removed unit
        // still surfaces, but the driver must not stay half-connected.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lost_reason_.clear();  // decision 0009 rule 5: a Disconnect drops the kept loss reason
        }
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_.clear();
            if (wheel_index_.has_value()) {
                wheel_id_.reset();
                wheel_info_ = {};
                wheel_info_valid_ = false;
                serial_number_.clear();
            }
        }
        status_cache_.reset();
        // Waits for an exchange in flight: the handle is never closed under one.
        std::lock_guard<std::mutex> io(io_mutex_);
        release_open_locked();
    }

    // EFW firmware read once at connect; web UI only, never DriverInfo. Cheap getter: no SDK call, and only the
    // leaf firmware_mutex_ (decision 0016). Empty or unread is no firmware.
    std::optional<std::string> get_device_firmware() const override {
        if (!link_.is_open()) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
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
        // The session this read belongs to: a staleness loss below drops only this one, never a link a Connect
        // opened since.
        const auto captured = link_.load();
        Status status;
        try {
            status = fetch_status();
        } catch (const std::exception&) {
            // Decision 0009 rule 2(b), issue #1668: a fault that has stood past the staleness bound is a lost link,
            // found here by the next failed read (the driver has no poll of its own).
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                lose_link_if_fault_stale_locked(captured);
            }
            ensure_connected();  // lost, here or by another caller: NotConnected, not the cache's DriverException
            throw;
        }
        if (status.closed) {
            throw AlpacaException(closed_reason(), AlpacaError::NotConnected);
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!wheel_info_valid_ || wheel_info_.slot_count <= 0) {
                // A disconnect that cleared the info since the check above: NotConnected, not a DriverException.
                if (!link_.is_open()) {
                    throw AlpacaException(lost_reason_.empty() ? "Filter wheel not connected" : lost_reason_,
                                          AlpacaError::NotConnected);
                }
                throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
            }
            if (position >= wheel_info_.slot_count) {
                throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
            }
        }
        // Decision 0009 rule 1: a latched Position fault refuses the move with its text.
        if (const std::string fault = status_cache_.fault(); !fault.empty()) {
            throw AlpacaException("ZWO EFW communications compromised: " + fault, AlpacaError::DriverException);
        }
        try {
            std::lock_guard<std::mutex> io(io_mutex_);
            // A disconnect that won the I/O lock has closed the wheel: call_locked() refuses before the SDK.
            call_locked([&] { sdk_.set_position(open_id_, position); });
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
    // One refill reads one device value. closed: a disconnect won the I/O lock, or the link was
    // lost (not a frame, not a link failure).
    struct Status {
        bool closed{};
        int position{};
    };

    static constexpr std::chrono::milliseconds kStatusTtl{100};
    static constexpr int kMovingPosition = -1;  // the SDK's value while the wheel moves

    // Reads the wheel's info under the open's link and publishes the connect-time state: the id an index binding
    // resolved to, the info and slot data, then the serial read at the open (decision 0016 rule 3: a connect that
    // throws before this publish leaves the pre-connect values). Caller holds io_mutex_ and not mutex_.
    void publish_connect_state(int wheel_id, const std::string& serial) {
        // Informational: a failed read never fails the connect, the hook then reports no firmware.
        std::string firmware;
        try {
            firmware = sdk_.get_firmware_version(wheel_id);
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ZWO", "EFW firmware unavailable: " + std::string(e.what()));
        }
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_ = firmware;
        }
        ZWOEFWInfo info;
        if (!sdk_.get_wheel_info_by_id(wheel_id, info)) {
            throw AlpacaException("Failed to read filter wheel info", AlpacaError::DriverException);
        }
        if (serial.empty()) {
            ALPACA_LOG_WARN("ZWO",
                            "EFW serial number unavailable: Connect cannot tell this wheel from another after a "
                            "replug");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_index_.has_value()) {
            wheel_id_ = wheel_id;
        }
        wheel_info_ = info;
        wheel_info_valid_ = true;
        normalize_slot_data_locked();
        serial_number_ = serial;
    }

    // One Position frame from the cache, refilled under the I/O lock when it has aged out. Caller holds no driver
    // lock (lock order: cache mutex, I/O lock, mutex_).
    Status fetch_status() const {
        return status_cache_.get([this] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            try {
                call_locked([&] { fresh.position = sdk_.get_position(open_id_); });
            } catch (const AlpacaException& e) {
                if (!util::is_link_loss(e)) {
                    throw;  // a fault: the cache counts it toward the latch
                }
                // A disconnect that won the I/O lock, or a lost link. Returning (not throwing) keeps it out of the
                // link-health count; the reader throws NotConnected, and the next connect's reset() drops this frame.
                fresh = Status{};
                fresh.closed = true;
                return fresh;
            } catch (const std::exception& e) {
                // Convert a vendor failure to AlpacaException (AGENTS.md); the cache counts it either way.
                throw AlpacaException(e.what(), AlpacaError::DriverException);
            }
            last_position_ = fresh.position;
            return fresh;
        });
    }

    // Decision 0009 rule 2(b), issue #1668 (the EAF shape of #1666): a Position fault that has stood past
    // util::kLinkStalenessBound on the task clock, with no good read since it latched, is a lost link (rule 3):
    // Connected false, members NotConnected, DeviceState empty, the open closed here, the reason kept for
    // get_link_fault() until the next Connect or Disconnect, one WARN. Caller holds io_mutex_ (no device call is in
    // flight) and no other driver lock. @p captured is the link word the read loaded before its refill, so a Connect
    // or Disconnect since then makes mark_lost() refuse and the new session is never dropped on the old one's fault.
    void lose_link_if_fault_stale_locked(util::LinkEpoch::Word captured) const {
        const std::string fault = status_cache_.fault();
        if (!status_cache_.fault_stale() || !link_.mark_lost(captured)) {
            return;
        }
        const std::string reason = unit_label_locked() + " did not answer for " +
                                   std::to_string(util::kLinkStalenessBound.count()) + " s (" + printable(fault) +
                                   "); Connect again when it answers";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lost_reason_ = reason;
        }
        ALPACA_LOG_WARN("ZWO", unit_label_locked() + " lost: Position fault stood for " +
                                   std::to_string(util::kLinkStalenessBound.count()) + " s; last position " +
                                   std::to_string(last_position_));
        // The wheel may still be attached (a generic error, not a removal), so close the open now: best effort,
        // the loss stands whatever the close does.
        try {
            release_open_locked();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("ZWO", "EFW close at a lost link failed: " + printable(e.what()));
        }
    }

    void ensure_connected() const {
        if (!link_.is_open()) {
            throw AlpacaException(closed_reason(), AlpacaError::NotConnected);
        }
    }

    bool has_lost_reason() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !lost_reason_.empty();
    }

    std::string closed_reason() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lost_reason_.empty() ? "Filter wheel not connected" : lost_reason_;
    }

    // Caller holds io_mutex_. Runs @p fn under this driver's open; the link word is read first, so a disconnect
    // that won the I/O lock refuses here. Loss evidence from the SDK (EFW_ERROR_REMOVED, CLOSED, INVALID_ID on a
    // per-device call, or a link the shared open table retired) drops the link at once (decision 0009 rule 2a,
    // issue #1668) and surfaces as NotConnected; any other error surfaces as itself.
    void call_locked(const std::function<void()>& fn) const {
        const auto captured = link_.load();
        if (!util::LinkEpoch::is_open(captured)) {
            throw AlpacaException(closed_reason(), AlpacaError::NotConnected);
        }
        try {
            sdk_.with_link(open_id_, open_link_, fn);
        } catch (const std::exception& e) {
            if (!util::is_link_loss(e)) {
                throw;
            }
            const std::string reason = unit_label_locked() + " was removed from USB; Connect again when it is back";
            if (link_.mark_lost(captured)) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lost_reason_ = reason;
                }
                // Nothing is re-applied at the next Connect; the last-known position goes to the log once.
                ALPACA_LOG_WARN("ZWO", unit_label_locked() + " lost (" + printable(e.what()) + "); last position " +
                                           std::to_string(last_position_));
            }
            throw AlpacaException(reason, AlpacaError::NotConnected);
        }
    }

    // Caller holds io_mutex_.
    std::string unit_label_locked() const {
        return "ZWO EFW " + (bound_serial_.empty() ? "id " + std::to_string(open_id_) : printable(bound_serial_));
    }

    // Caller holds io_mutex_. Opens the wheel this driver is bound to and records the open. Once a serial is known
    // (an earlier connect in this process) every Connect scans for it, so a replug that moved the wheel to another
    // SDK id, or swapped two wheels' ids, still opens the same wheel and never another one (issue #1668). Opening a
    // candidate does not move it: three Connect rounds on an EFW left Position at its slot (#1668, 2026-10-31).
    SharedOpenTable::Opened bind_wheel_locked() {
        if (bound_serial_.empty()) {
            const int id = resolve_wheel_id();
            auto opened = sdk_.open_wheel_link(id);
            open_id_ = id;
            open_link_ = opened.link;
            bound_serial_ = opened.serial;
            return opened;
        }
        const auto wheels = sdk_.enumerate_wheels();
        for (const auto& info : wheels) {
            std::optional<SharedOpenTable::Opened> candidate;
            try {
                candidate = sdk_.open_wheel_candidate(info.wheel_id);
            } catch (const std::exception& e) {
                // Another process holds it, or its serial read failed: nothing proves which wheel it is.
                ALPACA_LOG_DEBUG("ZWO",
                                 "EFW scan skips id " + std::to_string(info.wheel_id) + ": " + printable(e.what()));
                continue;
            }
            if (!candidate.has_value()) {
                ALPACA_LOG_DEBUG("ZWO", "EFW scan skips id " + std::to_string(info.wheel_id) +
                                            ": held by a wheel that reports no serial");
                continue;
            }
            if (candidate->serial == bound_serial_) {
                open_id_ = info.wheel_id;
                open_link_ = candidate->link;
                return *candidate;
            }
            try {
                sdk_.close_wheel_link(info.wheel_id, candidate->link);
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG(
                    "ZWO", "EFW scan close of id " + std::to_string(info.wheel_id) + " failed: " + printable(e.what()));
            }
        }
        throw AlpacaException("ZWO EFW " + printable(bound_serial_) + " not attached (" +
                                  std::to_string(wheels.size()) +
                                  " found); plug it in and Connect again, or re-save the device in the web UI or "
                                  "restart the service to bind another wheel",
                              AlpacaError::NotConnected);
    }

    // Caller holds io_mutex_. Drops this driver's open, if any. A NotConnected close (the wheel is gone, for example
    // a Connected toggle across a replug) is logged and swallowed; any other close error surfaces.
    void release_open_locked() const {
        if (open_link_ == 0) {
            return;
        }
        const int id = open_id_;
        const std::uint64_t link = open_link_;
        open_link_ = 0;
        try {
            sdk_.close_wheel_link(id, link);
        } catch (const std::exception& e) {
            if (!util::is_link_loss(e)) {
                throw;
            }
            ALPACA_LOG_WARN("ZWO", "EFW close of a removed wheel: " + printable(e.what()));
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

    ZWOEFWSDK& sdk_;
    int device_number_;
    std::optional<int> wheel_id_;
    std::optional<int> wheel_index_;
    std::string serial_number_;
    ZWOEFWInfo wheel_info_;
    bool wheel_info_valid_;
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;
    // Connected, as epoch << 1 | open bit: begin_transition() and publish_open() in set_connected(), mark_lost() on
    // loss evidence (issue #1668).
    mutable util::LinkEpoch link_;
    // Under io_mutex_: the open this driver holds (open_link_ 0 = none), the serial every Connect binds once one is
    // known (kept across Disconnect, in memory only: decision 0018), and the last position read, for the loss log.
    // Mutable: a staleness loss found by the const Position getter closes the open.
    int open_id_{-1};
    mutable std::uint64_t open_link_{0};
    std::string bound_serial_;
    mutable int last_position_{0};
    // Under mutex_: why a lost link refuses, cleared by the next Connect or Disconnect.
    mutable std::string lost_reason_;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // short state reads and writes, never held across an SDK call), then the SDK's SharedOpenTable mutex.
    // link_.mark_lost() takes no lock and the SDK's with_link() holds none.
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the
    // device transactions (connect, close, move, the Position refill). Never take the cache mutex
    // (reset/invalidate) while holding io_mutex_ or mutex_.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    // Leaf lock for firmware_ alone (the hook never waits on mutex_ or the I/O lock).
    mutable std::mutex firmware_mutex_;
    std::string firmware_;
    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel(int device_number, int wheel_id, ZWOEFWSDK& sdk,
                                                              util::TaskClock& clock) {
    return std::make_unique<ZWOEFWFilterWheelDriver>(device_number, wheel_id, std::nullopt, sdk, clock);
}

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel_by_index(int device_number, int wheel_index,
                                                                       ZWOEFWSDK& sdk, util::TaskClock& clock) {
    return std::make_unique<ZWOEFWFilterWheelDriver>(device_number, std::nullopt, wheel_index, sdk, clock);
}

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel(int device_number, int wheel_id) {
    return create_zwo_efw_filterwheel(device_number, wheel_id, ZWOEFWSDKWrapper::instance());
}

std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel_by_index(int device_number, int wheel_index) {
    return create_zwo_efw_filterwheel_by_index(device_number, wheel_index, ZWOEFWSDKWrapper::instance());
}

} // namespace alpacacore::vendor::zwo
