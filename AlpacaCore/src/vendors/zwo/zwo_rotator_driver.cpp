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
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_caa_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>

namespace alpacacore::vendor::zwo {

class ZWOCAARotatorDriver : public RotatorDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ZWOCAARotatorDriver(int device_number, std::optional<int> rotator_id, std::optional<int> rotator_index,
                        ZWOCAASDK& sdk)
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
          connected_(false),
          status_cache_("ZWO CAA", kStatusTtl, 3, "ZWO") {}

    ~ZWOCAARotatorDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);
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
            return rotator_info_.name;
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
        if (connected_.load()) {
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
                const int resolved_id = resolve_rotator_id();
                sdk.open_rotator(resolved_id);
                try {
                    publish_connect_state(resolved_id);
                } catch (...) {
                    sdk.close_rotator(resolved_id);
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
            close_id = rotator_id_;
            reverse_ = false;
            if (rotator_index_.has_value()) {
                rotator_id_.reset();
                rotator_info_ = {};
                rotator_info_valid_ = false;
                serial_number_.clear();
                rotator_type_.clear();
            }
            connected_.store(false);
        }
        status_cache_.reset();
        if (close_id.has_value()) {
            // Waits for an exchange in flight: the handle is never closed under one.
            std::lock_guard<std::mutex> io(io_mutex_);
            sdk.close_rotator(close_id.value());
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

    bool get_can_reverse() const override {
        return true;
    }

    bool get_reverse() const override {
        ensure_connected();
        // Reverse is a writable device setting held in memory: a dead link must not serve it.
        if (const std::string fault = status_cache_.fault(); !fault.empty()) {
            throw AlpacaException("ZWO CAA communications compromised: " + fault, AlpacaError::DriverException);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return reverse_;
    }

    void set_reverse(bool reverse) override {
        ensure_connected();
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
        bool closed{};  // a disconnect won the I/O lock: not a frame, not a link failure
        bool is_moving{};
        double degree{};  // logical (reverse-applied) angle, as CAAGetDegree() reports it
    };

    static constexpr std::chrono::milliseconds kStatusTtl{100};

    Status read_status() const {
        ensure_connected();
        const int id = rotator_id_value();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: cache mutex, I/O lock,
        // mutex_), so it must not take mutex_ and no caller may hold mutex_ or the I/O lock around the cache.
        Status status = status_cache_.get([this, id] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            if (!connected_.load()) {
                // A disconnect that won the I/O lock has closed the handle. Returning (not throwing) keeps it
                // out of the link-health count; the disconnect's reset() drops this frame.
                fresh.closed = true;
                return fresh;
            }
            fresh.is_moving = sdk_.get_motion_status(id).is_moving;
            fresh.degree = sdk_.get_degree(id);
            return fresh;
        });
        if (status.closed) {
            throw AlpacaException("Rotator not connected", AlpacaError::NotConnected);
        }
        return status;
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
    // the connected check is repeated under it: a disconnect that won the lock has closed the handle.
    template <typename Fn>
    void with_io(Fn&& fn) {
        std::lock_guard<std::mutex> io(io_mutex_);
        ensure_connected();
        fn(rotator_id_value());
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Rotator not connected", AlpacaError::NotConnected);
        }
    }

    // Caller holds io_mutex_ (the enumeration is a device call); the id and info publish under mutex_.
    int resolve_rotator_id() {
        if (rotator_index_.has_value()) {
            auto rotators = sdk_.enumerate_rotators();
            if (rotators.empty()) {
                ALPACA_LOG_WARN("ZWO", "No ZWO CAA rotators detected by SDK");
                throw AlpacaException("No ZWO CAA rotators detected", AlpacaError::NotConnected);
            }
            int index = rotator_index_.value();
            if (index < 0 || index >= static_cast<int>(rotators.size())) {
                ALPACA_LOG_WARN("ZWO", "CAA rotator index out of range: " + std::to_string(index) +
                                         " (count=" + std::to_string(rotators.size()) + ")");
                throw AlpacaException("Rotator index not found", AlpacaError::InvalidValue);
            }
            const auto& info = rotators[static_cast<std::size_t>(index)];
            std::lock_guard<std::mutex> lock(mutex_);
            rotator_id_ = info.rotator_id;
            rotator_info_ = info;
            rotator_info_valid_ = true;
            return info.rotator_id;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (rotator_id_.has_value()) {
            return rotator_id_.value();
        }

        throw AlpacaException("Rotator ID not specified", AlpacaError::InvalidValue);
    }

    // Reads the connect-time state from the device, then publishes it: the info, serial and type under
    // mutex_ (short), the rotation limit as an atomic. Caller holds io_mutex_ and not mutex_.
    void publish_connect_state(int rotator_id) {
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
        } catch (const std::exception&) {
            if (info.max_degree <= 0.0) {
                max_degree = 360.0;
            }
        }
        // Reverse only changes through set_reverse(): read once, then served from here.
        const bool reverse = sdk_.get_reverse(rotator_id);
        std::string serial;
        try {
            serial = sdk_.get_serial_number(rotator_id);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("ZWO", "CAA serial number unavailable: " + std::string(e.what()));
        }
        std::string type;
        try {
            type = sdk_.get_rotator_type(rotator_id);
        } catch (const std::exception& e) {
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
        return normalized;
    }

    // Callers do NOT hold mutex_ (called before their SDK move + lock).
    double to_mechanical_angle(double logical_angle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return normalize_angle(logical_angle - sync_offset_);
    }

    int rotator_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!rotator_id_.has_value()) {
            throw AlpacaException("Rotator ID not set", AlpacaError::NotConnected);
        }
        return rotator_id_.value();
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
    std::atomic<bool> connected_;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // short state reads and writes, never held across an SDK call), then the SDK wrapper's own mutex.
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the
    // device transactions (connect, close, move, halt, set_reverse, the status refill). Never take the
    // cache mutex (reset/invalidate/fault) while holding io_mutex_ or mutex_.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator(int device_number, int rotator_id, ZWOCAASDK& sdk) {
    return std::make_unique<ZWOCAARotatorDriver>(device_number, rotator_id, std::nullopt, sdk);
}

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator_by_index(int device_number, int rotator_index, ZWOCAASDK& sdk) {
    return std::make_unique<ZWOCAARotatorDriver>(device_number, std::nullopt, rotator_index, sdk);
}

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator(int device_number, int rotator_id) {
    return create_zwo_caa_rotator(device_number, rotator_id, ZWOCAASDKWrapper::instance());
}

std::unique_ptr<RotatorDriver> create_zwo_caa_rotator_by_index(int device_number, int rotator_index) {
    return create_zwo_caa_rotator_by_index(device_number, rotator_index, ZWOCAASDKWrapper::instance());
}

} // namespace alpacacore::vendor::zwo
