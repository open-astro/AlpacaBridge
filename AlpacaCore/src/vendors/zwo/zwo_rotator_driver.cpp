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
#include <alpacacore/util/device_name.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_epoch.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_caa_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
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

class ZWOCAARotatorDriver : public RotatorDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ZWOCAARotatorDriver(int device_number, std::optional<int> rotator_id, std::optional<int> rotator_index,
                        ZWOCAASDK& sdk, util::TaskClock& clock)
        : AsyncConnectable("ZWO"),
          sdk_(sdk),
          device_number_(device_number),
          rotator_id_(rotator_id),
          rotator_index_(rotator_index),
          serial_number_(),
          rotator_type_(),
          rotator_info_(),
          rotator_info_valid_(false),
          min_degree_(0.0),
          max_degree_(360.0),
          target_position_(0.0),
          has_target_position_(false),
          sync_offset_(0.0),
          // The TTL and the fault stamp read the injected clock (decision 0005), so a test ages both with advance().
          status_cache_("ZWO CAA", kStatusTtl, 3, "ZWO", clock) {}

    ~ZWOCAARotatorDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (link_.is_open()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall): most-derived class
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during CAA destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rotator_info_valid_ && !rotator_info_.name.empty()) {
            return util::branded_name("ZWO", rotator_info_.name);
        }
        return "ZWO CAA";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Rotator;
    }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return unique_id_locked();
    }

    // Decision 0018: the router calls this at the upgrade start only. Before a
    // connect the fallback is the SDK-ID or device-number ID, the key a CAA
    // without a serial saved under; a serial-keyed offset waits for the connect.
    // A client that connected first has read 0 from the stored-ID key, so a
    // connected driver takes the moved offset too. transition_mutex_ keeps a
    // connect or disconnect out of the window; the move and the write run under
    // mutex_, as sync() saves and writes, so a concurrent Sync cannot land
    // between them.
    void adopt_legacy_state() override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        const std::string assigned = assigned_unique_id();
        if (assigned.empty()) {
            return;
        }
        const std::string legacy_key = fallback_unique_id();
        std::lock_guard<std::mutex> lock(mutex_);
        const double offset = util::RotatorSyncOffsetStore::load_or_adopt(assigned, legacy_key);
        if (link_.is_open()) {
            sync_offset_ = offset;
        }
    }

    std::string get_description() const override {
        return "ZWO CAA Rotator Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO CAA Rotator Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only (never in
    // DriverInfo). CAAGetSDKVersion() returns "1, 7, 0, 0"; render as "1.7.0.0".
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = sdk_.get_sdk_version();
        if (version.empty() || version == "unknown") {
            return std::nullopt;
        }
        return util::normalize_dotted_version(version);
    }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override { return link_.is_open(); }

    // Decision 0009 rule 5: the one fault text. While lost, the loss reason, kept until the next Connect or
    // Disconnect; otherwise the status cache's latched fault, empty while healthy.
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
                {
                    std::lock_guard<std::mutex> lock(firmware_mutex_);
                    firmware_.clear();  // a connect that fails reports no firmware
                }
                const auto opened = bind_rotator_locked();
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
            reverse_ = false;
            if (rotator_index_.has_value()) {
                rotator_id_.reset();
                rotator_info_ = {};
                rotator_info_valid_ = false;
                serial_number_.clear();
                rotator_type_.clear();
            }
        }
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_.clear();
        }
        status_cache_.reset();
        // Waits for an exchange in flight: the handle is never closed under one.
        std::lock_guard<std::mutex> io(io_mutex_);
        release_open_locked();
    }

    // CAA firmware read once at connect; web UI only, never DriverInfo. Cheap getter: no SDK call, and only the
    // leaf firmware_mutex_ (decision 0016). Empty (unread, or cleared by Connect or Disconnect) is no firmware; no
    // Connected check, the cache is empty while disconnected.
    std::optional<std::string> get_device_firmware() const override {
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

    bool get_can_reverse() const override {
        return true;
    }

    bool get_reverse() const override {
        ensure_connected();
        // Reverse is a writable device setting held in memory: a dead link must not serve it.
        refuse_if_faulted();
        std::lock_guard<std::mutex> lock(mutex_);
        return reverse_;
    }

    void set_reverse(bool reverse) override {
        ensure_connected();
        refuse_if_faulted();
        with_io([&](int id) {
            sdk_.set_reverse(id, reverse);
            std::lock_guard<std::mutex> lock(mutex_);
            reverse_ = reverse;
        });
        // The reported angle is the reverse-applied one, so the frame is stale too.
        status_cache_.invalidate();
    }

    bool get_is_moving() const override { return read_status().is_moving; }

    double get_mechanical_position() const override {
        const double degree = read_status().degree;
        // CAAGetDegree() returns the logical (reverse-applied) angle. When Reverse is
        // enabled, the hardware inverts the angle: logical = 360 - physical. We must
        // un-apply that inversion to return the true mechanical (physical) position.
        std::lock_guard<std::mutex> lock(mutex_);
        if (reverse_) {
            return normalize_angle(360.0 - degree);
        }
        return normalize_angle(degree);
    }

    double get_position() const override {
        double mechanical = read_status().degree;
        // sync_offset_ is mutex_-guarded (written by sync()/connect); an
        // unlocked read here is torn against a concurrent sync (M12).
        std::lock_guard<std::mutex> lock(mutex_);
        return normalize_angle(mechanical + sync_offset_);
    }

    double get_step_size() const override {
        // The CAA SDK exposes no step size; ASCOM says PropertyNotImplemented
        // rather than a made-up 0. Connection check first (open-astro#309).
        ensure_connected();
        throw AlpacaException("StepSize is not available from the ZWO CAA SDK", AlpacaError::PropertyNotImplemented);
    }

    double get_target_position() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return has_target_position_ ? target_position_ : 0.0;
    }

    void set_target_position(double position) override {
        validate_angle(position);  // InvalidValue wins over NotConnected (driver-lifecycle.instructions.md)
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = normalize_angle(position);
        has_target_position_ = true;
    }

    void halt() override {
        ensure_connected();
        // Waits for a status refill in flight (the I/O lock is not preemptible); the stop follows at once.
        with_io([&](int id) { sdk_.stop(id); });
        status_cache_.invalidate();
    }

    void move(double position) override {
        validate_angle(position);  // InvalidValue wins over NotConnected (driver-lifecycle.instructions.md)
        ensure_connected();
        status_cache_.invalidate();  // relative to the live angle, not a frame up to one TTL old
        // No refuse_if_faulted() here: this live read refuses a latched fault itself (a failed refill throws, a good
        // one clears the latch), so a relative move never reaches the SDK on a faulted link.
        double current = get_position();
        double target = normalize_angle(current + position);
        double mechanical_target = to_mechanical_angle(target);
        with_io([&](int id) { sdk_.move_absolute(id, mechanical_target); });
        status_cache_.invalidate();
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = target;
        has_target_position_ = true;
    }

    void move_absolute(double position) override {
        validate_angle(position);  // InvalidValue wins over NotConnected (driver-lifecycle.instructions.md)
        ensure_connected();
        refuse_if_faulted();
        double target = normalize_angle(position);
        double mechanical_target = to_mechanical_angle(target);
        with_io([&](int id) { sdk_.move_absolute(id, mechanical_target); });
        status_cache_.invalidate();
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = target;
        has_target_position_ = true;
    }

    void move_mechanical(double position) override {
        validate_angle(position);  // InvalidValue wins over NotConnected (driver-lifecycle.instructions.md)
        ensure_connected();
        refuse_if_faulted();
        double mechanical_target = normalize_angle(position);
        with_io([&](int id) { sdk_.move_mechanical(id, mechanical_target); });
        status_cache_.invalidate();
        // CAAGetDegree() returns the logical (reverse-applied) angle. When Reverse is
        // enabled, the logical angle for a given mechanical angle is 360 - mechanical.
        std::lock_guard<std::mutex> lock(mutex_);
        const bool is_reversed = reverse_;
        double logical = is_reversed ? normalize_angle(360.0 - mechanical_target) : mechanical_target;
        target_position_ = normalize_angle(logical + sync_offset_);
        has_target_position_ = true;
    }

    void sync(double position) override {
        validate_angle(position);  // InvalidValue wins over NotConnected (driver-lifecycle.instructions.md)
        ensure_connected();
        status_cache_.invalidate();  // the offset is taken against the live angle
        double mechanical = read_status().degree;
        double target = normalize_angle(position);
        const double new_offset = normalize_angle(target - mechanical);
        // Write sync_offset_ under the same mutex_ that every reader takes —
        // the pre-lock write raced get_position()/to_mechanical_angle (M12).
        std::lock_guard<std::mutex> lock(mutex_);
        // IRotatorV4: the offset must persist. A Sync that cannot store it has
        // failed, so it throws and keeps the previous offset (decision 0008).
        if (!util::RotatorSyncOffsetStore::save(store_key_locked(), new_offset)) {
            throw AlpacaException(
                "ZWO CAA: Sync could not save the offset, so it was not applied. "
                "Check that the service can write config/rotator_sync_offsets.json.",
                AlpacaError::DriverException);
        }
        sync_offset_ = new_offset;
        target_position_ = target;
        has_target_position_ = true;
    }

private:
    // One frame of the live readings; a single refill fills all of it (open-astro#294).
    struct Status {
        bool closed{};  // a disconnect won the I/O lock, or the link was lost: not a frame, not a link failure
        bool is_moving{};
        double degree{};  // logical (reverse-applied) angle, as CAAGetDegree() reports it
    };

    static constexpr std::chrono::milliseconds kStatusTtl{100};

    Status read_status() const {
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
        return status;
    }

    // One status frame from the cache, refilled under the I/O lock when it has aged out. Caller holds no driver lock
    // (lock order: cache mutex, I/O lock, mutex_).
    Status fetch_status() const {
        return status_cache_.get([this] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            try {
                call_locked([&] {
                    fresh.is_moving = sdk_.get_motion_status(open_id_).is_moving;
                    fresh.degree = sdk_.get_degree(open_id_);
                });
            } catch (const std::exception& e) {
                if (!util::is_link_loss(e)) {
                    throw;  // a fault: the cache counts it toward the latch
                }
                // A disconnect that won the I/O lock, or a lost link. Returning (not throwing) keeps it out of the
                // link-health count; the reader throws NotConnected, and the next connect's reset() drops this frame.
                fresh = Status{};
                fresh.closed = true;
                return fresh;
            }
            last_degree_ = fresh.degree;
            return fresh;
        });
    }

    // Decision 0009 rule 1: a latched status fault refuses a write with its text (Halt stays allowed).
    void refuse_if_faulted() const {
        if (const std::string fault = status_cache_.fault(); !fault.empty()) {
            throw AlpacaException("ZWO CAA communications compromised: " + fault, AlpacaError::DriverException);
        }
    }

    // Decision 0009 rule 2(b), issue #1668 (the EAF shape of #1666): a status fault that has stood past
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
        ALPACA_LOG_WARN("ZWO", unit_label_locked() + " lost: status fault stood for " +
                                   std::to_string(util::kLinkStalenessBound.count()) + " s; last angle " +
                                   std::to_string(last_degree_));
        // The rotator may still be attached (a generic error, not a removal), so close the open now: best effort,
        // the loss stands whatever the close does.
        try {
            release_open_locked();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("ZWO", "CAA close at a lost link failed: " + printable(e.what()));
        }
    }

    // Caller holds mutex_.
    std::string unique_id_locked() const { return unique_id_for(serial_number_, rotator_id_); }

    // The offset-store key: the stored ID (decision 0018), or the
    // serial-based ID when none was assigned. Takes no lock of its own beyond
    // the base accessor, so it is safe under mutex_.
    std::string store_key_locked() const {
        const std::string assigned = assigned_unique_id();
        return assigned.empty() ? unique_id_locked() : assigned;
    }

    std::string unique_id_for(const std::string& serial, std::optional<int> rotator_id) const {
        if (!serial.empty()) {
            return "ZWO_CAA_SN_" + serial;
        }
        if (rotator_id.has_value()) {
            return "ZWO_CAA_ID_" + std::to_string(rotator_id.value());
        }
        return "ZWO_CAA_" + std::to_string(device_number_);
    }

    // One SDK transaction on the open handle. The I/O lock excludes the close in set_connected(false), and
    // call_locked() checks the link under it: a disconnect that won the lock has closed the handle.
    template <typename Fn>
    void with_io(Fn&& fn) {
        std::lock_guard<std::mutex> io(io_mutex_);
        call_locked([&] { fn(open_id_); });
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
        return lost_reason_.empty() ? "Rotator not connected" : lost_reason_;
    }

    // Caller holds io_mutex_. Runs @p fn under this driver's open; the link word is read first, so a disconnect
    // that won the I/O lock refuses here. Loss evidence from the SDK (CAA_ERROR_REMOVED, CLOSED, INVALID_ID on a
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
                // Nothing is re-applied at the next Connect; the last-known angle goes to the log once.
                ALPACA_LOG_WARN("ZWO", unit_label_locked() + " lost (" + printable(e.what()) + "); last angle " +
                                           std::to_string(last_degree_));
            }
            throw AlpacaException(reason, AlpacaError::NotConnected);
        }
    }

    // Caller holds io_mutex_.
    std::string unit_label_locked() const {
        return "ZWO CAA " + (bound_serial_.empty() ? "id " + std::to_string(open_id_) : printable(bound_serial_));
    }

    // Caller holds io_mutex_. Opens the rotator this driver is bound to and records the open. Once a serial is known
    // (an earlier connect in this process) every Connect scans for it, so a replug that moved the rotator to another
    // SDK id, or swapped two rotators' ids, still opens the same rotator and never another one (issue #1668).
    // TODO(#1668): confirm on a rig that CAAOpen of a candidate does not move it (the EFW does not, 2026-10-31).
    SharedOpenTable::Opened bind_rotator_locked() {
        if (bound_serial_.empty()) {
            const int id = resolve_rotator_id();
            auto opened = sdk_.open_rotator_link(id);
            open_id_ = id;
            open_link_ = opened.link;
            bound_serial_ = opened.serial;
            return opened;
        }
        const auto rotators = sdk_.enumerate_rotators();
        for (const auto& info : rotators) {
            std::optional<SharedOpenTable::Opened> candidate;
            try {
                candidate = sdk_.open_rotator_candidate(info.rotator_id);
            } catch (const std::exception& e) {
                // Another process holds it, or its serial read failed: nothing proves which rotator it is.
                ALPACA_LOG_DEBUG("ZWO",
                                 "CAA scan skips id " + std::to_string(info.rotator_id) + ": " + printable(e.what()));
                continue;
            }
            if (!candidate.has_value()) {
                ALPACA_LOG_DEBUG("ZWO", "CAA scan skips id " + std::to_string(info.rotator_id) +
                                            ": held by a rotator that reports no serial");
                continue;
            }
            if (candidate->serial == bound_serial_) {
                open_id_ = info.rotator_id;  // published as rotator_id_ by publish_connect_state(), at the end
                open_link_ = candidate->link;
                return *candidate;
            }
            try {
                sdk_.close_rotator_link(info.rotator_id, candidate->link);
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("ZWO", "CAA scan close of id " + std::to_string(info.rotator_id) +
                                            " failed: " + printable(e.what()));
            }
        }
        throw AlpacaException("ZWO CAA " + printable(bound_serial_) + " not attached (" +
                                  std::to_string(rotators.size()) +
                                  " found); plug it in and Connect again, or re-save the device in the web UI or "
                                  "restart the service to bind another rotator",
                              AlpacaError::NotConnected);
    }

    // Caller holds io_mutex_. Drops this driver's open, if any. A NotConnected close (the rotator is gone, for
    // example a Connected toggle across a replug) is logged and swallowed; any other close error surfaces.
    void release_open_locked() const {
        if (open_link_ == 0) {
            return;
        }
        const int id = open_id_;
        const std::uint64_t link = open_link_;
        open_link_ = 0;
        try {
            sdk_.close_rotator_link(id, link);
        } catch (const std::exception& e) {
            if (!util::is_link_loss(e)) {
                throw;
            }
            ALPACA_LOG_WARN("ZWO", "CAA close of a removed rotator: " + printable(e.what()));
        }
    }

    // Caller holds io_mutex_ (the enumeration is a device call). Publishes nothing: publish_connect_state() stores the
    // id and info at the end of a successful connect (decision 0016 rule 3), as the EFW driver does.
    int resolve_rotator_id() {
        if (rotator_index_.has_value()) {
            auto rotators = sdk_.enumerate_rotators();
            if (rotators.empty()) {
                ALPACA_LOG_WARN("ZWO", "No ZWO CAA rotators detected by SDK");
                throw AlpacaException("No ZWO CAA rotators detected", AlpacaError::NotConnected);
            }
            int index = rotator_index_.value();
            for (const auto& info : rotators) {
                if (info.slot_index == index) {
                    return info.rotator_id;
                }
            }
            ALPACA_LOG_WARN("ZWO", "CAA rotator SDK slot not found: " + std::to_string(index));
            throw AlpacaException("Rotator index not found", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (rotator_id_.has_value()) {
            return rotator_id_.value();
        }

        throw AlpacaException("Rotator ID not specified", AlpacaError::InvalidValue);
    }

    // Reads the connect-time state from the device, then publishes it: the id an index binding resolved to, the info,
    // serial and type under mutex_ (short), the rotation limit as an atomic; a connect that throws before this publish
    // leaves the pre-connect values (decision 0016 rule 3). Caller holds io_mutex_ and not mutex_.
    void publish_connect_state(int rotator_id, const std::string& serial) {
        // Informational: a failed read never fails the connect, the hook then reports no firmware. Published
        // last, after every throw point, so only a finished connect reports one (decision 0016 rule 3).
        std::string firmware;
        try {
            firmware = sdk_.get_firmware_version(rotator_id);
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ZWO", "CAA firmware unavailable: " + std::string(e.what()));
        }
        ZWOCAARotatorInfo info;
        if (!sdk_.get_rotator_info_by_id(rotator_id, info)) {
            throw AlpacaException("Failed to read rotator info", AlpacaError::DriverException);
        }
        double max_degree = max_degree_.load();
        if (info.max_degree > 0.0) {
            max_degree = info.max_degree;
        }
        try {
            const double reported = sdk_.get_max_degree(rotator_id);
            if (reported > 0.0) {
                max_degree = reported;
            }
        } catch (const std::exception& e) {
            if (util::is_link_loss(e)) {
                throw;  // the rotator is gone: the connect closes what it opened and fails
            }
            if (info.max_degree <= 0.0) {
                max_degree = 360.0;
            }
        }
        // Reverse only changes through set_reverse(): read once, then served from here.
        const bool reverse = sdk_.get_reverse(rotator_id);
        if (serial.empty()) {
            ALPACA_LOG_WARN("ZWO",
                            "CAA serial number unavailable: Connect cannot tell this rotator from another after a "
                            "replug");
        }
        std::string type;
        try {
            type = sdk_.get_rotator_type(rotator_id);
        } catch (const std::exception& e) {
            if (util::is_link_loss(e)) {
                throw;  // the rotator is gone: the connect closes what it opened and fails
            }
            ALPACA_LOG_WARN("ZWO", "CAA type unavailable: " + std::string(e.what()));
        }
        // Persisted Sync offset, keyed by the stored ID (decision 0018). An offset
        // saved under the serial-based ID moves here, since the serial read just
        // above names the body. One saved under an SDK-ID or device-number ID
        // names only a slot, so adopt_legacy_state() moves it at the upgrade
        // start and a connect never does.
        const std::string legacy_key = unique_id_for(serial, rotator_id);
        const std::string assigned = assigned_unique_id();
        const std::string key = assigned.empty() ? legacy_key : assigned;
        const double sync_offset = serial.empty() ? util::RotatorSyncOffsetStore::load(key)
                                                  : util::RotatorSyncOffsetStore::load_or_adopt(key, legacy_key);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (rotator_index_.has_value()) {
                rotator_id_ = rotator_id;  // the id an index binding, or a rebind by serial, resolved to
            }
            rotator_info_ = info;
            rotator_info_valid_ = true;
            min_degree_ = 0.0;
            reverse_ = reverse;
            serial_number_ = serial;
            rotator_type_ = type;
            sync_offset_ = sync_offset;
            target_position_ = 0.0;
            has_target_position_ = false;
        }
        max_degree_.store(max_degree);
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_ = firmware;
        }
    }

    void validate_angle(double position) const {
        if (!std::isfinite(position)) {
            throw AlpacaException("Rotator position is not finite", AlpacaError::InvalidValue);
        }
    }

    double normalize_angle(double angle) const {
        const double max_degree = max_degree_.load();
        double wrap = max_degree > 0.0 ? max_degree : 360.0;
        if (wrap <= 0.0) {
            return angle;
        }
        double normalized = std::fmod(angle, wrap);
        if (normalized < 0.0) {
            normalized += wrap;
        }
        // A tiny negative remainder plus wrap rounds to exactly wrap.
        if (normalized >= wrap) {
            normalized -= wrap;
        }
        return normalized;
    }

    // Callers do NOT hold mutex_ (called before their SDK move + lock).
    double to_mechanical_angle(double logical_angle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return normalize_angle(logical_angle - sync_offset_);
    }

    ZWOCAASDK& sdk_;
    int device_number_;
    std::optional<int> rotator_id_;
    std::optional<int> rotator_index_;
    std::string serial_number_;
    std::string rotator_type_;
    ZWOCAARotatorInfo rotator_info_;
    bool rotator_info_valid_;
    double min_degree_;
    // Published at the end of connect, read lock-free by normalize_angle() from
    // move()/move_absolute()/sync() before they take mutex_; atomic so the two never race.
    std::atomic<double> max_degree_;
    double target_position_;
    bool has_target_position_;
    double sync_offset_;
    bool reverse_{false};
    // Connected, as epoch << 1 | open bit: begin_transition() and publish_open() in set_connected(), mark_lost() on
    // loss evidence (issue #1668).
    mutable util::LinkEpoch link_;
    // Under io_mutex_: the open this driver holds (open_link_ 0 = none), the serial every Connect binds once one is
    // known (kept across Disconnect, in memory only: decision 0018), and the last angle read, for the loss log.
    // Mutable: a staleness loss found by a const getter closes the open.
    int open_id_{-1};
    mutable std::uint64_t open_link_{0};
    std::string bound_serial_;
    mutable double last_degree_{0.0};
    // Under mutex_: why a lost link refuses, cleared by the next Connect or Disconnect.
    mutable std::string lost_reason_;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // short state reads and writes, never held across an SDK call), then the SDK's SharedOpenTable mutex.
    // link_.mark_lost() takes no lock and the SDK's with_link() holds none.
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the
    // device transactions (connect, close, move, halt, set_reverse, the status refill). Never take the
    // cache mutex (reset/invalidate) while holding io_mutex_ or mutex_. fault() takes only the
    // cache's leaf fault_mutex_, never waits for a refill, and is safe from a getter.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    // Leaf lock for firmware_ alone (the hook never waits on mutex_ or the I/O lock).
    mutable std::mutex firmware_mutex_;
    std::string firmware_;
    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator(int device_number, int rotator_id, ZWOCAASDK& sdk,
                                                      util::TaskClock& clock) {
    return std::make_unique<ZWOCAARotatorDriver>(device_number, rotator_id, std::nullopt, sdk, clock);
}

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator_by_index(int device_number, int rotator_index, ZWOCAASDK& sdk,
                                                               util::TaskClock& clock) {
    return std::make_unique<ZWOCAARotatorDriver>(device_number, std::nullopt, rotator_index, sdk, clock);
}

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator(int device_number, int rotator_id) {
    return create_zwo_caa_rotator(device_number, rotator_id, ZWOCAASDKWrapper::instance());
}

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator_by_index(int device_number, int rotator_index) {
    return create_zwo_caa_rotator_by_index(device_number, rotator_index, ZWOCAASDKWrapper::instance());
}

} // namespace alpacacore::vendor::zwo
