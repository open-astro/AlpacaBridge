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
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace alpacacore::vendor::astroasis {

class AstroasisFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    AstroasisFocuserDriver(int device_number, std::string hid_path,
                           util::ConnectionResolver<std::string> connection_resolver = {},
                           std::unique_ptr<AstroasisHidTransport> transport = nullptr)
        : AsyncConnectable("Astroasis"),
          device_number_(device_number),
          hid_path_(std::move(hid_path)),
          connection_resolver_(std::move(connection_resolver)),
          connected_(false),
          protocol_(std::move(transport)),
          status_cache_("Astroasis focuser", kStatusTtl, 3, "Astroasis") {}

    ~AstroasisFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Astroasis", "Error during focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "Astroasis Oasis Focuser"; }

    DeviceType get_device_type() const override { return DeviceType::Focuser; }

    std::string get_unique_id() const override { return "ASTROASIS_FOCUSER_" + std::to_string(device_number_); }

    std::string get_description() const override { return "Astroasis Oasis Focuser Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore Astroasis Focuser Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }

    // Deliberately a plain atomic read, not under mutex_ -- do NOT pull it in
    // during a sweep of the "connected_ only under mutex_" invariant. Taking
    // the lock here would be safe (async_connectable.h explicitly allows
    // get_connected() to take the driver mutex, and several telescope drivers
    // do; the task tail reads it BEFORE pending_mutex_ so that stays deadlock-
    // free), but it would buy nothing -- connected_ is one atomic -- and would
    // cost: a Connected poll landing during an in-flight connect would block
    // for the whole ~1.1 s HID handshake instead of answering immediately.
    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously — closing the HID handle is trivial and
        // ASCOM clients expect Connected to be false immediately after. The
        // close does take hid_global_mutex(), so it can queue behind a
        // concurrent by-index enumeration's bus scan (bounded; see accepted
        // cost (1) on that mutex in astroasis_protocol_wrapper.cpp) — but
        // connected_ is stored false before it, so the flip never waits on
        // THAT lock. It can still wait on this driver's mutex_ behind a connect
        // handshake, which can itself be queued on hid_global_mutex() behind a
        // concurrent enumeration's bus scan. Getters no longer take mutex_; a
        // status refill holds the cache mutex and Impl::mutex_ instead.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Astroasis", "Focuser disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Driver state mutex (AsyncConnectable obligations 4/5): keeps the
        // connected_ read+act atomic against a racing async connect task's
        // set_connected(true) (this driver's disconnect() calls set_connected
        // synchronously, outside the task machinery) — without it, two
        // set_connected(true) calls can both observe connected_==false and
        // both reach protocol_.connect(), leaking the first HID handle.
        std::lock_guard<std::mutex> lock(mutex_);
        // See AsyncConnectable's class comment for why these gates run before
        // the idempotency check.
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
            // An auto-detected focuser resolves its HID path here, not in the factory (#659).
            util::connect_resolved(
                hid_path_, connection_resolved_, connection_resolver_,
                [this](const std::string& path) {
                    try {
                        protocol_.connect(path);
                    } catch (const AlpacaException& e) {
                        // Only a vanished hidraw node is stale; a handshake miss on a present one is not.
                        if (util::device_node_missing(path)) throw util::StaleEndpoint(e.what(), e.error_code());
                        throw;
                    }
                },
                "Astroasis");
            // MaxStep is a device setting that does not change while connected:
            // read it once here so the getters and move() never go to the device
            // for it. A failure here must not leave the handle open.
            try {
                max_step_.store(protocol_.get_max_step());
            } catch (...) {
                protocol_.disconnect();
                throw;
            }
            status_cache_.reset();
            connected_.store(true);
            ALPACA_LOG_INFO("Astroasis", "Focuser connected");
        } else {
            // Clear driver state BEFORE the SDK close (AGENTS.md concurrency
            // checklist): a throwing close must not leave the driver
            // half-connected.
            connected_.store(false);
            max_step_.store(0);
            status_cache_.reset();
            protocol_.disconnect();
            ALPACA_LOG_INFO("Astroasis", "Focuser disconnected");
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

    // --- Focuser interface ---

    bool get_absolute() const override { return true; }

    bool get_is_moving() const override { return read_status().moving; }

    int get_max_step() const override {
        ensure_connected();
        return max_step_.load();
    }

    int get_max_increment() const override {
        ensure_connected();
        return max_step_.load();
    }

    int get_position() const override { return read_status().position; }

    double get_step_size() const override {
        // No mutex_: touches nothing it guards -- ensure_connected() is
        // already a lock-free atomic read, and every path here throws
        // unconditionally, so taking the lock would only add a stall behind
        // an in-flight connect's full HID handshake for no protection.
        ensure_connected();
        // TODO: the vendor protocol does not expose step size in microns
        // (mechanical, varies by focuser model).
        throw AlpacaException("Step size not available for this focuser", AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        // open-astro#309: the connection check comes first. AGENTS.md's ASCOM
        // contract precedence rule is that only parameter/range validation
        // precedes it -- every other property throws NotConnected when
        // disconnected, with no early return that skips it. ensure_connected()
        // is a lock-free atomic read, so this costs a getter nothing.
        ensure_connected();
        return false;
    }

    bool get_temp_comp() const override {
        ensure_connected();
        return false;
    }

    void set_temp_comp(bool) override {
        // No mutex_: same reasoning as get_step_size() above.
        ensure_connected();
        throw AlpacaException("Temperature compensation not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_temperature() const override {
        const auto status = read_status();
        if (status.temperature_external_valid) {
            return status.temperature_external;
        }
        return status.temperature_internal;
    }

    void halt() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        try {
            protocol_.stop_move();
        } catch (...) {
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();  // a frame up to one TTL old would still say "moving"
    }

    void move(int position) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        const int max_step = max_step_.load();
        // ConformU requires graceful clamping, not exceptions.
        if (position < 0) {
            position = 0;
        } else if (position > max_step) {
            position = max_step;
        }
        try {
            protocol_.move_to(position);
        } catch (...) {
            status_cache_.invalidate();
            throw;
        }
        status_cache_.invalidate();  // the next IsMoving/Position read must see the move
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
    }

    static constexpr std::chrono::milliseconds kStatusTtl{100};

    // One 0x32 transaction fills position, moving and temperature for every
    // getter within the TTL (open-astro#294). The refill takes the wrapper's
    // own mutex, never mutex_, so a getter is not queued behind a connect.
    AstroasisProtocolWrapper::Status read_status() const {
        ensure_connected();
        return status_cache_.get([this] { return const_cast<AstroasisFocuserDriver*>(this)->protocol_.get_status(); });
    }

    int device_number_;
    std::string hid_path_;
    // Set by the by-index factory; empty for an explicit path. connection_resolved_
    // is true once a connect has run the resolver, so a later connect retries
    // that path before scanning again (#659).
    util::ConnectionResolver<std::string> connection_resolver_;
    bool connection_resolved_ = false;
    std::atomic<bool> connected_;
    AstroasisProtocolWrapper protocol_;
    std::atomic<int> max_step_{0};  // read at connect, cleared at disconnect
    mutable util::TtlStatusCache<AstroasisProtocolWrapper::Status> status_cache_;
    mutable std::mutex mutex_;
};

std::unique_ptr<FocuserDriver> create_astroasis_focuser(int device_number, const std::string& hid_path) {
    return std::make_unique<AstroasisFocuserDriver>(device_number, hid_path);
}

std::unique_ptr<FocuserDriver> create_astroasis_focuser(int device_number, const std::string& hid_path,
                                                        std::unique_ptr<AstroasisHidTransport> transport) {
    return std::make_unique<AstroasisFocuserDriver>(device_number, hid_path, util::ConnectionResolver<std::string>{},
                                                    std::move(transport));
}

std::unique_ptr<FocuserDriver> create_astroasis_focuser_deferred(int device_number,
                                                                 util::ConnectionResolver<std::string> resolver) {
    if (!resolver) {
        throw AlpacaException("Astroasis focuser: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<AstroasisFocuserDriver>(device_number, std::string{}, std::move(resolver));
}

std::string resolve_astroasis_focuser_by_index(int focuser_index) {
    auto ports = enumerate_astroasis_focusers();
    if (ports.empty()) {
        throw AlpacaException("No Astroasis Oasis Focuser detected on the USB bus", AlpacaError::NotConnected);
    }
    if (focuser_index < 0 || focuser_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Focuser index " + std::to_string(focuser_index) + " out of range (detected " +
                                  std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }

    const auto& port = ports[static_cast<std::size_t>(focuser_index)];
    ALPACA_LOG_INFO("Astroasis", "Auto-detected focuser at " + port.hid_path);
    return port.hid_path;
}

std::unique_ptr<FocuserDriver> create_astroasis_focuser_by_index(int device_number, int focuser_index) {
    // The USB HID scan runs at connect time (#659), not here.
    return create_astroasis_focuser_deferred(
        device_number, [focuser_index] { return resolve_astroasis_focuser_by_index(focuser_index); });
}

}  // namespace alpacacore::vendor::astroasis
