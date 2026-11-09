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
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_rotator_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>

#include "ioptron_ifr_session.h"

namespace alpacacore::vendor::ioptron {

namespace {

// IsMoving stays true from the move's ack until a frame shows the motor running or at its target.
constexpr auto kMoveHold = std::chrono::milliseconds(1000);
constexpr auto kStatusTtl = std::chrono::milliseconds(100);
// The CAA's resolution from the iFR product page; moves land within one step of the target on the rig.
constexpr double kStepSizeDegrees = 0.004;
constexpr const char* kCacheLabel = "iOptron iFR rotator";  // the status cache's label, its fault text prefix
// A frame within this of the target counts as arrived: a little over one step, the landing error measured.
constexpr double kArrivalToleranceDegrees = 0.02;

double wrap(double degrees) { return util::wrap_degrees(degrees); }

// The signed distance between two angles, in (-180, 180].
double angle_between(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

// The firmware mirrors the whole frame when reversed (rig, 2026-11-03): Position = 360 - (physical + U) and
// the offset field reads -U, where U is the offset in the normal frame. MechanicalPosition is the physical
// angle in both directions, as the ZWO CAA reports it.
double mechanical_of(const IfrRotatorStatus& s) {
    return s.reversed ? wrap(360.0 - s.position_deg + s.offset_deg) : wrap(s.position_deg - s.offset_deg);
}

// The Position a physical angle reads at in the current frame.
double position_for_mechanical(const IfrRotatorStatus& s, double mechanical) {
    if (!s.reversed) return wrap(mechanical + s.offset_deg);
    const double u = -s.offset_deg;
    return wrap(360.0 - (mechanical + u));
}

}  // namespace

class IfrRotatorDriver : public RotatorDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    IfrRotatorDriver(int device_number, IfrConnectionConfig config,
                     util::ConnectionResolver<IfrConnectionConfig> resolver, const std::string& model,
                     util::TaskClock& clock)
        : AsyncConnectable("iOptron"),
          clock_(clock),
          device_number_(device_number),
          model_(ifr_model_for(model)),
          session_(std::move(config), std::move(resolver)),
          status_cache_(kCacheLabel, kStatusTtl, 3, "iOptron", clock) {}

    ~IfrRotatorDriver() override {
        shutdown_connection();
        try {
            std::lock_guard<std::mutex> transition(transition_mutex_);
            teardown_locked();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Error during iFR rotator destruction: " + std::string(e.what()));
        }
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override {
        return unlisted_model_.load() ? "iOptron Rotator" : std::string("iOptron ") + model_.label + " Rotator";
    }
    DeviceType get_device_type() const override { return DeviceType::Rotator; }
    std::string fallback_unique_id() const override { return "IOPTRON_IFR_ROTATOR_" + std::to_string(device_number_); }
    std::string get_description() const override {
        return std::string("iOptron ") + model_.label + " focuser-rotator, rotator (CAA)";
    }
    std::string get_driver_info() const override { return "AlpacaCore iOptron iFR Rotator Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }

    std::optional<std::string> get_device_firmware() const override {
        if (!connected_.load()) return std::nullopt;
        const auto link = session_.current();
        if (!link || !link->link_alive() || link->info().rotator_firmware.empty()) return std::nullopt;
        return link->info().rotator_firmware;
    }

    bool get_connected() const override { return connected_.load() && session_.alive(); }
    // Decision 0009 rules 1 and 5: the session's loss reason, kept after a loss, else the :RI# fault the status
    // cache latched (Connected stays true). Both are leaf-mutex reads; neither waits on the wire.
    std::string get_link_fault() const override {
        std::string lost = session_.link_fault();
        if (!lost.empty()) return lost;
        return status_cache_.fault();
    }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "iFR rotator disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(connected_.load())) return;
        if (connected ? connected_.load() && session_.alive() : !connected_.load()) return;

        if (!connected) {
            teardown_locked();
            ALPACA_LOG_INFO("iOptron", "iFR rotator disconnected");
            return;
        }
        teardown_locked();
        std::shared_ptr<IfrLink> link = session_.open();
        unlisted_model_.store(!is_ifr_model(link->info().model));
        try {
            const IfrRotatorStatus status = link->rotator_status();
            if (status.size_tenths != model_.size_tenths) {
                ALPACA_LOG_WARN("iOptron", std::string("iFR configured as the ") + model_.label +
                                               " but the unit reports size " + std::to_string(status.size_tenths) +
                                               " (tenths of an inch); check the Model setting");
            }
            ALPACA_LOG_INFO("iOptron", "iFR rotator connected on " + link->port() + " at " +
                                           std::to_string(status.position_deg) + " deg" +
                                           (status.reversed ? " (reversed)" : ""));
        } catch (...) {
            link.reset();
            session_.close();
            throw;
        }
        status_cache_.reset();
        session_.publish(std::move(link));
        connected_.store(true);
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

    // --- Rotator ---

    bool get_can_reverse() const override { return true; }

    bool get_reverse() const override { return read_status().reversed; }

    void set_reverse(bool reverse) override {
        const auto link = connected_link();
        ensure_link_healthy(*link);
        IfrAck ack = IfrAck::Silent;
        try {
            ack = link->rotator_set_reverse(reverse);
        } catch (...) {
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();  // the reported angle mirrors with the direction
        if (ack == IfrAck::Refused) {
            throw AlpacaException("The iFR rotator refused the Reverse change", AlpacaError::DriverException);
        }
    }

    bool get_is_moving() const override {
        const IfrRotatorStatus status = read_status();
        const auto now = clock_.now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (move_hold_.active(now)) {
            if (status.moving ||
                std::fabs(angle_between(status.position_deg, target_position_)) <= kArrivalToleranceDegrees) {
                move_hold_.clear();
                return status.moving;
            }
            return true;
        }
        return status.moving;
    }

    double get_mechanical_position() const override { return mechanical_of(read_status()); }

    double get_position() const override { return wrap(read_status().position_deg); }

    double get_step_size() const override {
        ensure_connected();
        return kStepSizeDegrees;
    }

    double get_target_position() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return has_target_position_ ? target_position_ : 0.0;
    }

    void set_target_position(double position) override {
        validate_angle(position);  // InvalidValue before NotConnected
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = wrap(position);
        has_target_position_ = true;
    }

    void halt() override {
        const auto link = connected_link();
        lose_link_if_fault_stale(*link);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            move_hold_.clear();
        }
        // Sent even while a fault stands: a stop is never withheld. A latched fault still fails the call below.
        try {
            if (link->rotator_halt() == IfrAck::Refused) {
                ALPACA_LOG_WARN("iOptron", "iFR rotator refused :RQ# (Halt)");
            }
        } catch (...) {
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();
        throw_if_faulted();
    }

    void move(double position) override {
        // IRotator.Move: relative to the current Position.
        validate_angle(position);
        const auto link = connected_link();
        status_cache_.invalidate();  // relative to the live angle, not a frame up to one TTL old
        const double target = wrap(read_status().position_deg + position);
        start_move(link, target);
    }

    void move_absolute(double position) override {
        validate_angle(position);
        start_move(connected_link(), wrap(position));
    }

    void move_mechanical(double position) override {
        validate_angle(position);
        const auto link = connected_link();
        status_cache_.invalidate();  // the offset and direction are taken from a live frame
        const IfrRotatorStatus status = read_status();
        start_move(link, position_for_mechanical(status, wrap(position)));
    }

    void sync(double position) override {
        validate_angle(position);
        const auto link = connected_link();
        ensure_link_healthy(*link);
        const double target = wrap(position);
        status_cache_.invalidate();
        const bool reversed = read_status().reversed;
        // The firmware keeps the offset itself, across power loss (rig, 2026-11-03), so nothing is stored here
        // (decision 0008's file is for rotators that cannot). A :RY sent while reversed stores an offset that
        // does not match the mirrored frame, so a reversed Sync is made in the normal frame at the mirrored angle.
        try {
            IfrAck ack = IfrAck::Silent;
            if (!reversed) {
                ack = link->rotator_sync(target);
            } else {
                if (link->rotator_set_reverse(false) == IfrAck::Refused) {
                    throw AlpacaException("The iFR rotator refused the direction change Sync needs",
                                          AlpacaError::DriverException);
                }
                try {
                    ack = link->rotator_sync(wrap(360.0 - target));
                } catch (...) {
                    restore_reverse(*link);
                    throw;
                }
                restore_reverse(*link);
            }
            if (ack == IfrAck::Refused) {
                throw AlpacaException("The iFR rotator refused the Sync", AlpacaError::DriverException);
            }
        } catch (...) {
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();
        std::lock_guard<std::mutex> lock(mutex_);
        target_position_ = target;
        has_target_position_ = true;
    }

private:
    static void validate_angle(double position) {
        if (!std::isfinite(position)) {
            throw AlpacaException("Rotator position is not finite", AlpacaError::InvalidValue);
        }
    }

    void ensure_connected() const {
        if (!connected_.load()) throw AlpacaException("Rotator not connected", AlpacaError::NotConnected);
        if (!session_.alive()) {
            throw AlpacaException("iFR link lost; Connect again once the unit is back", AlpacaError::NotConnected);
        }
    }

    std::shared_ptr<IfrLink> connected_link() const {
        ensure_connected();
        auto link = session_.current();
        if (!link) throw AlpacaException("Rotator not connected", AlpacaError::NotConnected);
        return link;
    }

    IfrRotatorStatus read_status() const {
        const auto link = connected_link();
        try {
            return status_cache_.get([&link] { return link->rotator_status(); });
        } catch (const AlpacaException&) {
            // A loss inside the refill is NotConnected however many reads failed before it (decision 0009 rule 3),
            // and so is a fault that has now stood past the staleness bound (rule 2).
            lose_link_if_fault_stale(*link);
            throw;
        }
    }

    // Decision 0009 rule 2: an :RI# fault that has stood past util::kLinkStalenessBound on the task clock loses the
    // link, found by the next failed read or write (there is no poll). The loss is the session's, so the focuser
    // of the same unit reads Connected false too. Throws NotConnected once the link is lost. Takes no driver lock;
    // the session's I/O lock, then the cache's leaf fault mutex.
    void lose_link_if_fault_stale(IfrLink& link) const {
        // The text first: a stale fault was latched at least the bound ago, so it is already set here.
        const std::string fault = status_cache_.fault();
        if (link.lose_link_if([this] { return status_cache_.fault_stale(); },
                              "iFR rotator status fault stood for " +
                                  std::to_string(util::kLinkStalenessBound.count()) +
                                  " s with no good reply: " + fault)) {
            throw AlpacaException("iFR link lost; Connect again once the unit is back", AlpacaError::NotConnected);
        }
    }

    // Decision 0009 rule 1: a latched fault fails a write as it fails a read. Only a good :RI# clears it.
    void throw_if_faulted() const {
        if (const std::string fault = status_cache_.fault(); !fault.empty()) {
            throw AlpacaException(std::string(kCacheLabel) + " communications compromised: " + fault,
                                  AlpacaError::DriverException);
        }
    }

    void ensure_link_healthy(IfrLink& link) const {
        lose_link_if_fault_stale(link);
        throw_if_faulted();
    }

    void restore_reverse(IfrLink& link) {
        if (link.rotator_set_reverse(true) != IfrAck::Accepted) {
            ALPACA_LOG_ERROR("iOptron", "iFR rotator: Reverse could not be restored after Sync; check it");
        }
    }

    void start_move(const std::shared_ptr<IfrLink>& link, double target) {
        ensure_link_healthy(*link);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_position_ = target;
            has_target_position_ = true;
            move_hold_.arm(clock_.now(), kMoveHold);
        }
        IfrAck ack = IfrAck::Silent;
        try {
            ack = link->rotator_move(target);
        } catch (...) {
            clear_hold();
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();
        if (ack == IfrAck::Refused) {
            clear_hold();
            throw AlpacaException("The iFR rotator refused the move to " + std::to_string(target) + " deg",
                                  AlpacaError::DriverException);
        }
    }

    void clear_hold() {
        std::lock_guard<std::mutex> lock(mutex_);
        move_hold_.clear();
    }

    void teardown_locked() {
        connected_.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            move_hold_.clear();
            has_target_position_ = false;
            target_position_ = 0.0;
        }
        status_cache_.reset();
        session_.close();
    }

    util::TaskClock& clock_;
    const int device_number_;
    std::atomic<bool> unlisted_model_{false};
    const IfrModel model_;
    detail::IfrSessionHolder session_;
    std::atomic<bool> connected_{false};
    std::mutex transition_mutex_;  // set_connected() against set_connected() only (#528)
    // Leaf state lock: target and hold. Never held across an exchange.
    mutable std::mutex mutex_;
    mutable util::HoldWindow move_hold_;
    double target_position_ = 0.0;
    bool has_target_position_ = false;
    mutable util::TtlStatusCache<IfrRotatorStatus> status_cache_;
};

std::unique_ptr<RotatorDriver> create_ifr_rotator(int device_number, const std::string& serial_port,
                                                  const std::string& model, util::TaskClock& clock) {
    IfrConnectionConfig config;
    config.serial_port = serial_port;
    return std::make_unique<IfrRotatorDriver>(device_number, std::move(config),
                                              util::ConnectionResolver<IfrConnectionConfig>{}, model, clock);
}

std::unique_ptr<RotatorDriver> create_ifr_rotator_deferred(int device_number,
                                                           util::ConnectionResolver<IfrConnectionConfig> resolver,
                                                           const std::string& model, util::TaskClock& clock) {
    if (!resolver) {
        throw AlpacaException("iOptron iFR rotator: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<IfrRotatorDriver>(device_number, IfrConnectionConfig{}, std::move(resolver), model, clock);
}

std::unique_ptr<RotatorDriver> create_ifr_rotator_by_index(int device_number, int index, const std::string& model,
                                                           util::TaskClock& clock) {
    return create_ifr_rotator_deferred(device_number, [index] { return resolve_ifr_by_index(index); }, model, clock);
}

}  // namespace alpacacore::vendor::ioptron
