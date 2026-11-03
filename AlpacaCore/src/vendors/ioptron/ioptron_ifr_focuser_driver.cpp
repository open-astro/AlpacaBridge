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
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_focuser_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

#include "ioptron_ifr_session.h"

namespace alpacacore::vendor::ioptron {

namespace {

// IsMoving stays true from the move's ack until a frame shows the motor running or at its target, so a poll
// that lands before the motor starts never reads as arrived. On the rig the first :FI# after the ack already
// shows the motor running; the hold covers a slower start.
constexpr auto kMoveHold = std::chrono::milliseconds(1000);
constexpr auto kStatusTtl = std::chrono::milliseconds(100);
// The focuser's minimum step, from the iFR product page (0.4 um); 62,822 steps measured between the 3"'s
// limit stops over its 25 mm travel agree.
constexpr double kStepSizeMicrons = 0.4;
constexpr const char* kCacheLabel = "iOptron iFR focuser";  // the status cache's label, its fault text prefix

}  // namespace

class IfrFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    IfrFocuserDriver(int device_number, IfrConnectionConfig config,
                     util::ConnectionResolver<IfrConnectionConfig> resolver, const std::string& model,
                     util::TaskClock& clock)
        : AsyncConnectable("iOptron"),
          clock_(clock),
          device_number_(device_number),
          model_(ifr_model_for(model)),
          session_(std::move(config), std::move(resolver)),
          status_cache_(kCacheLabel, kStatusTtl, 3, "iOptron", clock) {}

    ~IfrFocuserDriver() override {
        // Blocks new connection tasks and joins the one in flight: first, before the members it touches go.
        shutdown_connection();
        try {
            std::lock_guard<std::mutex> transition(transition_mutex_);
            teardown_locked();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Error during iFR focuser destruction: " + std::string(e.what()));
        }
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return std::string("iOptron ") + model_.label + " Focuser"; }
    DeviceType get_device_type() const override { return DeviceType::Focuser; }
    std::string fallback_unique_id() const override { return "IOPTRON_IFR_FOCUSER_" + std::to_string(device_number_); }
    std::string get_description() const override {
        return std::string("iOptron ") + model_.label + " focuser-rotator, focuser";
    }
    std::string get_driver_info() const override { return "AlpacaCore iOptron iFR Focuser Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }

    // Web UI only, never DriverInfo. Read from the session's connect-time info; hidden once the link is lost.
    std::optional<std::string> get_device_firmware() const override {
        if (!connected_.load()) return std::nullopt;
        const auto link = session_.current();
        if (!link || !link->link_alive() || link->info().focuser_firmware.empty()) return std::nullopt;
        return link->info().focuser_firmware;
    }

    // Lock-free (decision 0016 rule 3): the flag and the session's loss latch.
    bool get_connected() const override { return connected_.load() && session_.alive(); }
    // Decision 0009 rules 1 and 5: the session's loss reason, kept after a loss, else the :FI# fault the status
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
            ALPACA_LOG_WARN("iOptron", "iFR focuser disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Whole transitions are serialized against each other (#528); getters never take this lock.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(connected_.load())) return;
        // Connect over a lost link reconnects (decision 0009 rule 4); a disconnect after a loss still tears down.
        if (connected ? connected_.load() && session_.alive() : !connected_.load()) return;

        if (!connected) {
            teardown_locked();
            ALPACA_LOG_INFO("iOptron", "iFR focuser disconnected");
            return;
        }
        teardown_locked();
        std::shared_ptr<IfrLink> link = session_.open();
        try {
            // One frame before Connected is published, so every getter has an answer, and the size check.
            const IfrFocuserStatus status = link->focuser_status();
            check_size(*link);
            ALPACA_LOG_INFO("iOptron",
                            "iFR focuser connected on " + link->port() + " at step " + std::to_string(status.position));
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

    // --- Focuser ---

    bool get_absolute() const override { return true; }

    bool get_is_moving() const override {
        const IfrFocuserStatus status = read_status();
        const auto now = clock_.now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (move_hold_.active(now)) {
            // The motor started, or the move is already over: the frame is the truth from here on.
            if (status.moving || status.position == target_) {
                move_hold_.clear();
                return status.moving;
            }
            return true;
        }
        return status.moving;
    }

    int get_max_step() const override {
        ensure_connected();
        return model_.max_step;
    }

    int get_max_increment() const override {
        ensure_connected();
        return model_.max_step;
    }

    int get_position() const override { return read_status().position; }

    double get_step_size() const override {
        ensure_connected();
        return kStepSizeMicrons;
    }

    bool get_temp_comp_available() const override {
        ensure_connected();
        return false;
    }

    bool get_temp_comp() const override {
        ensure_connected();
        return false;
    }

    void set_temp_comp(bool) override {
        ensure_connected();
        throw AlpacaException("The iFR has no temperature compensation", AlpacaError::PropertyNotImplemented);
    }

    double get_temperature() const override { return read_status().temperature_c; }

    void halt() override {
        const auto link = connected_link();
        lose_link_if_fault_stale(*link);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            move_hold_.clear();
        }
        // Sent even while a fault stands: a stop is never withheld. A latched fault still fails the call below.
        try {
            if (link->focuser_halt() == IfrAck::Refused) {
                ALPACA_LOG_WARN("iOptron", "iFR focuser refused :FQ# (Halt)");
            }
        } catch (...) {
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();
        throw_if_faulted();
    }

    void move(int position) override {
        if (position < 0 || position > model_.max_step) {
            throw AlpacaException(
                "Focuser position " + std::to_string(position) + " is outside 0.." + std::to_string(model_.max_step),
                AlpacaError::InvalidValue);
        }
        const auto link = connected_link();
        ensure_link_healthy(*link);
        {
            // Armed before the command goes out (decision 0022 rule 2's shape).
            std::lock_guard<std::mutex> lock(mutex_);
            target_ = position;
            move_hold_.arm(clock_.now(), kMoveHold);
        }
        IfrAck ack = IfrAck::Silent;
        try {
            ack = link->focuser_move(position);
        } catch (...) {
            clear_hold();
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();
        if (ack != IfrAck::Refused) return;
        // The firmware refuses a move to where the focuser already is (rig, 2026-11-03), and a move it cannot
        // make. Only the first is a successful Move.
        clear_hold();
        const IfrFocuserStatus now = link->focuser_status();
        if (now.position == position) return;
        throw AlpacaException("The iFR focuser refused the move to " + std::to_string(position) + " (it is at " +
                                  std::to_string(now.position) +
                                  "): an end stop, or a position its firmware will not reach",
                              AlpacaError::DriverException);
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        if (!session_.alive()) {
            throw AlpacaException("iFR link lost; Connect again once the unit is back", AlpacaError::NotConnected);
        }
    }

    std::shared_ptr<IfrLink> connected_link() const {
        ensure_connected();
        auto link = session_.current();
        if (!link) throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        return link;
    }

    IfrFocuserStatus read_status() const {
        const auto link = connected_link();
        try {
            return status_cache_.get([&link] { return link->focuser_status(); });
        } catch (const AlpacaException&) {
            // A loss inside the refill is NotConnected however many reads failed before it (decision 0009 rule 3),
            // and so is a fault that has now stood past the staleness bound (rule 2).
            lose_link_if_fault_stale(*link);
            throw;
        }
    }

    // Decision 0009 rule 2: a :FI# fault that has stood past util::kLinkStalenessBound on the task clock loses the
    // link, found by the next failed read or write (there is no poll). The loss is the session's, so the rotator
    // of the same unit reads Connected false too. Throws NotConnected once the link is lost. Takes no driver lock;
    // the session's I/O lock, then the cache's leaf fault mutex.
    void lose_link_if_fault_stale(IfrLink& link) const {
        // The text first: a stale fault was latched at least the bound ago, so it is already set here.
        const std::string fault = status_cache_.fault();
        if (link.lose_link_if([this] { return status_cache_.fault_stale(); },
                              "iFR focuser status fault stood for " +
                                  std::to_string(util::kLinkStalenessBound.count()) +
                                  " s with no good reply: " + fault)) {
            throw AlpacaException("iFR link lost; Connect again once the unit is back", AlpacaError::NotConnected);
        }
    }

    // Decision 0009 rule 1: a latched fault fails a write as it fails a read. Only a good :FI# clears it.
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

    void clear_hold() {
        std::lock_guard<std::mutex> lock(mutex_);
        move_hold_.clear();
    }

    void check_size(IfrLink& link) const {
        const IfrRotatorStatus r = link.rotator_status();
        if (r.size_tenths != model_.size_tenths) {
            ALPACA_LOG_WARN("iOptron",
                            std::string("iFR configured as the ") + model_.label + " but the unit reports size " +
                                std::to_string(r.size_tenths / 10.0).substr(0, 3) + "\"; check the Model setting");
        }
    }

    // Caller holds transition_mutex_. Driver state first, then the session (driver-lifecycle: state is cleared
    // before the close, so a throwing close cannot trap the driver half-connected).
    void teardown_locked() {
        connected_.store(false);
        clear_hold();
        status_cache_.reset();
        session_.close();
    }

    util::TaskClock& clock_;
    const int device_number_;
    const IfrModel model_;
    detail::IfrSessionHolder session_;
    std::atomic<bool> connected_{false};
    std::mutex transition_mutex_;  // set_connected() against set_connected() only (#528)
    // Leaf state lock: the move target and hold. Never held across an exchange; taken after a refill returns.
    mutable std::mutex mutex_;
    mutable util::HoldWindow move_hold_;
    int target_ = 0;
    // Its lock is held across the :FI# refill and then the session's I/O lock: cache -> I/O -> nothing else.
    mutable util::TtlStatusCache<IfrFocuserStatus> status_cache_;
};

std::unique_ptr<FocuserDriver> create_ifr_focuser(int device_number, const std::string& serial_port,
                                                  const std::string& model, util::TaskClock& clock) {
    IfrConnectionConfig config;
    config.serial_port = serial_port;
    return std::make_unique<IfrFocuserDriver>(device_number, std::move(config),
                                              util::ConnectionResolver<IfrConnectionConfig>{}, model, clock);
}

std::unique_ptr<FocuserDriver> create_ifr_focuser_deferred(int device_number,
                                                           util::ConnectionResolver<IfrConnectionConfig> resolver,
                                                           const std::string& model, util::TaskClock& clock) {
    if (!resolver) {
        throw AlpacaException("iOptron iFR focuser: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<IfrFocuserDriver>(device_number, IfrConnectionConfig{}, std::move(resolver), model, clock);
}

IfrConnectionConfig resolve_ifr_by_index(int index) {
    const auto ports = enumerate_ifr_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("iOptron iFR"), AlpacaError::NotConnected);
    }
    if (index < 0 || index >= static_cast<int>(ports.size())) {
        throw AlpacaException(
            "iFR index " + std::to_string(index) + " out of range (detected " + std::to_string(ports.size()) + ")",
            AlpacaError::InvalidValue);
    }
    IfrConnectionConfig config;
    config.serial_port = ports[static_cast<std::size_t>(index)].port_path;
    ALPACA_LOG_INFO("iOptron", "Auto-detected iFR at " + config.serial_port);
    return config;
}

std::unique_ptr<FocuserDriver> create_ifr_focuser_by_index(int device_number, int index, const std::string& model,
                                                           util::TaskClock& clock) {
    return create_ifr_focuser_deferred(device_number, [index] { return resolve_ifr_by_index(index); }, model, clock);
}

}  // namespace alpacacore::vendor::ioptron
