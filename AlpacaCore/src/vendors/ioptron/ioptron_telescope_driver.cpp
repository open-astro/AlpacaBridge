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
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/device_location_store.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/gps_status.h>
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/lock_gate.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/telescope_command_gate.h>
#include <alpacacore/util/units.h>
#include <alpacacore/util/utc_offset.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <thread>
#include <type_traits>

namespace alpacacore::vendor::ioptron {

// iOptron's limits for the telescope command gate (#2258). PulseGuide takes up to 99999 ms (the protocol's 5-digit
// duration field); MoveAxis moves axes 0 and 1, and its discrete arrow speeds stay the driver's check after the gate.
// The guide-rate static check is finite and greater than 0 (denorm_min is the smallest double above 0); the mount's
// range is validate_guide_rate_value()'s check, which also runs before the connection check (#2257).
constexpr util::TelescopeCommandLimits kCommandLimits = [] {
    util::TelescopeCommandLimits limits;
    limits.pulse_guide_max_ms = 99999;
    limits.move_axis_axes = 2;
    limits.guide_rate_ra_min = std::numeric_limits<double>::denorm_min();
    limits.guide_rate_dec_min = std::numeric_limits<double>::denorm_min();
    limits.texts.pulse_direction = "PulseGuide direction must be 0-3";
    limits.texts.pulse_duration = "PulseGuide duration must be 0-99999 ms";
    limits.texts.move_axis_axis = "Axis must be 0 (Primary) or 1 (Secondary)";
    limits.texts.move_axis_rate_not_finite = "Axis rate out of range";
    limits.texts.guide_rate_not_finite = "GuideRate must be a finite number greater than 0";
    limits.texts.guide_rate_range = "GuideRate must be a finite number greater than 0";
    limits.tracking_rate_max = 4;  // DriveRates 0-3 and 4, the custom rate
    limits.texts.tracking_rate = "Invalid tracking rate";
    return limits;
}();

/**
 * @brief iOptron mount telescope driver implementation.
 *
 * Implements the TelescopeDriver interface for iOptron mounts using
 * the RS-232 command protocol over serial or TCP connection.
 */
class iOptronTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
    // Decision 0016 (#1829): getters never wait on device I/O. Two driver locks, in this order:
    //   transition_mutex_ -> io_mutex_ (the operation lock) -> mutex_ (the state lock, a leaf)
    // io_mutex_ keeps one command's exchanges and state writes together: every member that reaches the mount
    // (a command, a getter whose TTL cache has lapsed, the connect and the close) holds it for the whole call.
    // Some sequences must not be split: :SRA#/:Sd# set the target the :MS1# after them uses, and a
    // Park's :MP1# is followed by its park-position read. mutex_ is never held ACROSS an exchange: every call
    // through Wire drops it for the exchange and takes it back after. The protocol wrapper's own lock
    // serialises the wire itself; the slew and clock-sync bodies take the two locks like a command does.
    //
    // A member that needs no device access (Name, the targets, SiteElevation, UTCDate, the guide-rate cache,
    // the capability and limit getters) takes mutex_ alone (StateLock::Mode::State) and never waits behind
    // the wire. Every write to driver state happens under mutex_, so such a member never reads a torn value.
    // Most writers hold io_mutex_ as well, but some hold mutex_ alone (the clock-sync body publishing the
    // site and UTC offset, the slew-landing and failed-dispatch writes to slew_in_progress_), so a command
    // that read a value before an exchange and uses it after relies only on state that Mode::Io members write.
    //
    // The gate itself (which locks this thread holds, StateLock's two modes, dropping mutex_ around an
    // exchange) is util::LockGate (lock_gate.h, #1729). A full lock table refuses the StateLock with
    // DriverException before any lock is taken (decision 0016 Rule 6).

public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    // The driver's state lock, with the operation lock above it in Mode::Io (util::StateLock). Mode::State is
    // for the members that take no operation lock and never reach the wire.
    using StateLock = util::StateLock;

private:
    // Every exchange with the mount goes through this gate (the process-wide protocol wrapper behind it). The
    // caller holds the operation lock; it gives up the state lock for the call, if it holds it, and takes it
    // back after (also when the call throws). A caller without the operation lock is a defect: it throws
    // DriverException instead of reaching the wire.
    class Wire {
    public:
        explicit Wire(const iOptronTelescopeDriver& driver) : driver_(driver) {}

        template <typename F>
        decltype(auto) call(F&& f) const {
            auto& wrapper = iOptronProtocolWrapper::instance();
            const util::LockGate& gate = driver_.lock_gate_;
            if (!gate.holds_io()) {
                throw AlpacaException("iOptron: a mount exchange without the operation lock",
                                      AlpacaError::DriverException);
            }
            return gate.exchange([&]() -> decltype(auto) { return f(wrapper); });
        }

        // Text under the wrapper's small mutexes, no I/O and never held across an exchange: called directly.
        std::string last_connect_error() const { return iOptronProtocolWrapper::instance().last_connect_error(); }
        std::string link_fault() const { return iOptronProtocolWrapper::instance().link_fault(); }

        bool connect(const ConnectionInfo& info) const {
            return call([&](auto& p) { return p.connect(info); });
        }
        void disconnect() const {
            call([](auto& p) { p.disconnect(); });
        }
        // Takes the wrapper's exchange mutex, so it is an exchange for the gate (decision 0009 rule 2b).
        void lose_link(const std::string& reason) const {
            call([&](auto& p) { p.lose_link(reason); });
        }
        MountInfo get_mount_info() const {
            return call([](auto& p) { return p.get_mount_info(); });
        }
        Position get_position() const {
            return call([](auto& p) { return p.get_position(); });
        }
        AltAz get_alt_az() const {
            return call([](auto& p) { return p.get_alt_az(); });
        }
        MountStatus get_status() const {
            return call([](auto& p) { return p.get_status(); });
        }
        SiteInfo get_site_info() const {
            return call([](auto& p) { return p.get_site_info(); });
        }
        SiteInfo get_site_info(const std::string& gut_reply) const {
            return call([&](auto& p) { return p.get_site_info(gut_reply); });
        }
        SiteInfo get_site_location() const {
            return call([](auto& p) { return p.get_site_location(); });
        }
        AltAz get_park_position() const {
            return call([](auto& p) { return p.get_park_position(); });
        }
        std::chrono::system_clock::time_point get_utc_time() const {
            return call([](auto& p) { return p.get_utc_time(); });
        }
        std::string read_utc_reply() const {
            return call([](auto& p) { return p.read_utc_reply(); });
        }
        void set_target_ra(double ra_hours) const {
            call([&](auto& p) { p.set_target_ra(ra_hours); });
        }
        void set_target_dec(double dec_degrees) const {
            call([&](auto& p) { p.set_target_dec(dec_degrees); });
        }
        GotoReply slew_to_ra_dec() const {
            return call([](auto& p) { return p.slew_to_ra_dec(); });
        }
        void stop_slewing() const {
            call([](auto& p) { p.stop_slewing(); });
        }
        void start_tracking() const {
            call([](auto& p) { p.start_tracking(); });
        }
        void stop_tracking() const {
            call([](auto& p) { p.stop_tracking(); });
        }
        bool park(bool zero_distance_workaround) const {
            return call([&](auto& p) { return p.park(zero_distance_workaround); });
        }
        void unpark() const {
            call([](auto& p) { p.unpark(); });
        }
        void set_park_position(double alt_degrees, double az_degrees) const {
            call([&](auto& p) { p.set_park_position(alt_degrees, az_degrees); });
        }
        void go_to_home() const {
            call([](auto& p) { p.go_to_home(); });
        }
        void find_home() const {
            call([](auto& p) { p.find_home(); });
        }
        void pulse_guide(int direction, int duration_ms) const {
            call([&](auto& p) { p.pulse_guide(direction, duration_ms); });
        }
        void set_longitude(double longitude_degrees) const {
            call([&](auto& p) { p.set_longitude(longitude_degrees); });
        }
        void set_latitude(double latitude_degrees) const {
            call([&](auto& p) { p.set_latitude(latitude_degrees); });
        }
        void set_hemisphere(bool is_northern) const {
            call([&](auto& p) { p.set_hemisphere(is_northern); });
        }
        void set_utc_time(std::chrono::system_clock::time_point utc_time) const {
            call([&](auto& p) { p.set_utc_time(utc_time); });
        }
        void set_timezone_offset(int offset_minutes) const {
            call([&](auto& p) { p.set_timezone_offset(offset_minutes); });
        }
        void set_dst_observed(bool observed) const {
            call([&](auto& p) { p.set_dst_observed(observed); });
        }
        void set_tracking_rate(int rate) const {
            call([&](auto& p) { p.set_tracking_rate(rate); });
        }
        void set_custom_tracking_rate(double rate_multiplier) const {
            call([&](auto& p) { p.set_custom_tracking_rate(rate_multiplier); });
        }
        std::pair<double, double> get_guide_rates() const {
            return call([](auto& p) { return p.get_guide_rates(); });
        }
        void set_guide_rates(double ra_rate, double dec_rate) const {
            call([&](auto& p) { p.set_guide_rates(ra_rate, dec_rate); });
        }
        std::string send_command(const std::string& command) const {
            return call([&](auto& p) { return p.send_command(command); });
        }
        void send_command_blind(const std::string& command) const {
            call([&](auto& p) { p.send_command_blind(command); });
        }

    private:
        const iOptronTelescopeDriver& driver_;
    };

    Wire wire() const { return Wire(*this); }

public:
    /**
     * @brief Construct iOptron telescope driver.
     *
     * @param device_number Alpaca device number
     * @param connection_info Connection information (serial or network)
     */
    iOptronTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                           std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                           std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
                           util::ConnectionResolver<ConnectionInfo> connection_resolver = {},
                           std::function<void()> before_slew_worker_lock = {},
                           std::function<void()> before_slew_reap = {}, std::function<void()> before_handoff_lock = {},
                           util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("iOptron"),
          clock_(clock),
          device_number_(device_number),
          connection_info_(connection_info),
          connection_resolver_(std::move(connection_resolver)),
          connected_(false),
          mount_info_(),
          target_ra_hours_(0.0),
          target_dec_degrees_(0.0),
          aperture_diameter_m_(0.0),
          focal_length_m_(0.0),
          site_latitude_cached_(0.0),
          site_longitude_cached_(0.0),
          site_info_valid_(false),
          hemisphere_north_(true),
          site_elevation_m_(site_elevation_m),
          timezone_offset_minutes_(0),
          timezone_offset_valid_(false),
          dst_observed_(false),
          last_site_info_fetch_(clock_.now()),
          cached_ra_hours_(0.0),
          cached_dec_degrees_(0.0),
          cached_side_of_pier_(-1),
          position_cache_valid_(false),
          last_position_update_(clock_.now()),
          cached_alt_degrees_(0.0),
          cached_az_degrees_(0.0),
          altaz_cache_valid_(false),
          last_altaz_update_(clock_.now()),
          cached_guide_rate_(),
          guide_rate_valid_(false),
          cached_status_(),
          status_cache_valid_(false),
          last_status_update_(clock_.now()),
          utc_query_supported_(true),
          fast_cache_until_(std::chrono::steady_clock::time_point{}),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(true)),
          before_slew_worker_lock_(std::move(before_slew_worker_lock)),
          before_slew_reap_(std::move(before_slew_reap)),
          before_handoff_lock_(std::move(before_handoff_lock)) {
        // Initialize mount info (will be populated on connect)
    }

    ~iOptronTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // Cancel and join the async slew dispatch (slew slot) before any member it touches is
        // destroyed. Must run WITHOUT mutex_ held (the thread takes mutex_).
        reap_slew_dispatch();
        if (connected_) {
            // Destructors are implicitly noexcept; a throw from set_connected()
            // would call std::terminate(). Swallow any error during teardown.
            // Qualify the call so it binds statically (we want this class's
            // implementation, not virtual dispatch from a destructor).
            try {
                iOptronTelescopeDriver::set_connected(false);
            } catch (...) {
                // Best-effort disconnect on destruction; log and continue so
                // nothing escapes the noexcept destructor.
                ALPACA_LOG_WARN("iOptron", "Error disconnecting mount during destruction");
            }
        }
        // Unconditional, and last: both starts happen AFTER set_connected()
        // releases mutex_, so a connect can lose the race to a concurrent
        // disconnect and run its start_clock_sync() after that
        // disconnect's stop_clock_sync() has already returned. The
        // driver is then left with connected_ == false and a running clock-sync body
        // that the `if (connected_)` branch above never reaps, which would outlive
        // the members it touches. Both reapers are idempotent (each cancels and
        // joins its slot, a no-op when it is idle), so running them again here
        // costs nothing on the normal path. The zero search watchdog (#2432) is
        // started after find_home() releases its locks, so the same applies.
        stop_clock_sync();
        stop_zero_search_watchdog();
        reap_slew_dispatch();
    }
    
    // AlpacaDriver interface
    
    int get_device_number() const override {
        return device_number_;
    }
    
    std::string get_name() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);  // no wire I/O: never waits behind a connect
        if (!mount_info_.model_name.empty()) {
            return "iOptron " + mount_info_.model_name;
        }
        return "iOptron Telescope";
    }
    
    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }

    // Takes the state lock itself: call it with mutex_ released (the connect scan does, inside Wire::call), never
    // from a *_locked helper, since mutex_ is not recursive.
    std::string fallback_unique_id() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (mount_info_.model_code.empty()) {
            return "iOptron_" + std::to_string(device_number_);
        }
        return "iOptron_" + mount_info_.model_code + "_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "iOptron CEM120,70,40,26, GEM, HEM, HAE, HAZ series and SkyHunter Mount Driver";
    }
    
    std::string get_driver_info() const override {
        return "AlpacaCore iOptron Driver v1.0";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }

    // Reads published_connected_, raised only at the end of a connect and dropped first on a
    // disconnect, so it takes no lock set_connected() holds and stays false while Connecting
    // is true (#910). The blocking-driver list lives in async_connectable.h. A lost link
    // (decision 0009: the wrapper's lock-free latch) reads false at once.
    bool get_connected() const override { return published_connected_.load() && !link_lost(); }

    // Reads only the published copy under its own narrow mutex, never the coarse
    // mutex_ that set_connected() holds across the connect. After a lost link, the
    // wrapper's loss reason, kept until the next Connect or Disconnect (decision 0009
    // rule 5); the wrapper's text mutex is a leaf no exchange holds.
    std::string get_link_fault() const override {
        std::string lost = iOptronProtocolWrapper::instance().link_fault();
        if (!lost.empty()) {
            return lost;
        }
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        return link_fault_text_;
    }

    // Base always spawns (the old spawn-skip `if (connect == get_connected())`
    // is gone — it masked the no-op-connect flag-consumption bug, PR #115
    // round 4). A connect() on an already-connected mount costs one short
    // no-op task; a disconnect() racing that window is recorded and honored
    // by the task tail rather than spawning immediately.
    //
    // LATENCY NOTE: this driver's teardown joins the clock-sync body, which
    // can sit in multi-second serial protocol retries. When that teardown runs
    // as the base's DEFERRED disconnect (disconnect-races-connect path only),
    // connection_mutex_ is held throughout — concurrent connect()/disconnect()
    // and the destructor serialize behind it for the duration. Deliberate
    // (base class "COST" note); this driver is the worst case of it.
    ALPACA_ASYNC_CONNECT_FORWARDERS()

    // The auto-detect factories need the driver's unique id inside the resolver,
    // and the id is assigned only after the driver exists. Call before the first connect.
    void set_location_index(int index) { location_index_ = index; }

    void set_connection_resolver(util::ConnectionResolver<ConnectionInfo> resolver) {
        connection_resolver_ = std::move(resolver);
    }

    static std::string location_of(const ConnectionInfo& info) {
        return info.type == ConnectionType::Network ? util::network_location_for(info.host, info.tcp_port)
                                                    : util::serial_location_for(info.port_path);
    }

    void set_connected(bool connected) override {
        // Decision 0016 (#1829): the whole call runs under transition_mutex_, which no getter takes, and holds
        // io_mutex_ from its first state write to its end. Every exchange below goes through Wire, so mutex_ is
        // free while the handshake, the post-connect reads and the close are on the wire: a getter meanwhile
        // reads NotConnected (check_connected()), its cache or its identity instead of waiting for them.
        // Issue #2419: a host clock change the server's timer saw while this call held transition_mutex_
        // (on_host_clock_changed() stores it as pending before it tries the lock) starts its re-sync on the way
        // out. Declared before the transition lock, so it drains after that lock is released: a change stored
        // after a check made under the lock would otherwise wait for the next set_connected().
        struct DrainPendingResync {
            iOptronTelescopeDriver& self;
            ~DrainPendingResync() { self.drain_pending_resync("Host clock changed during a connect or disconnect"); }
        } drain_pending_resync_on_exit{*this};
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Test seam: the last thing before transition_mutex_ is released, with no other driver lock held.
        struct ExitHook {
            const std::function<void()>& hook;
            ~ExitHook() {
                if (hook) hook();
            }
        } exit_hook{set_connected_exit_hook_};
        if (set_connected_entry_hook_) set_connected_entry_hook_();  // test seam: transition_mutex_ held
        const Wire protocol = wire();
        // Issue #2396 (AB-9): a GOTO or a MoveAxis that was running when the link dropped is stopped as the first
        // command of the next Connect that opens the port, never forgotten. The record is taken here, before the reap
        // below ends the slew body, on a Connect or a Disconnect over the lost link, and it is kept in
        // relink_stop_pending_ until the :Q# is sent: a Connect that fails (at the open, or on the :Q# itself) leaves
        // it for the next one, although the session reset or the teardown has cleared the motion flags. A Disconnect
        // over a live link records nothing (the client left the motion running on a link it could still command).
        if (connected_ && link_lost()) {
            StateLock state(lock_gate_, StateLock::Mode::State);
            if (slew_in_progress_ || axis_move_active_primary_ || axis_move_active_secondary_) {
                relink_stop_pending_ = true;
            }
        }
        if (!connected || (connected_ && link_lost())) {
            // A disconnect, or a connect over a lost link (decision 0009 rule 4), ends the session's bodies
            // first, with no driver lock held: the clock-sync body and the slew dispatch take both locks, so a
            // join under either would deadlock, and a body mid-exchange must finish that exchange and then see
            // its stop before this call takes the operation lock, so it sends nothing more (the clock-sync
            // slot cases). transition_mutex_ keeps every other connect and disconnect out meanwhile. The zero
            // search watchdog takes both locks the same way (#2432).
            stop_clock_sync();
            stop_zero_search_watchdog();
            {
                std::lock_guard<std::mutex> handoff_lock(slew_handoff_mutex_);
                reap_slew_dispatch();
            }
        }
        StateLock lock(lock_gate_);

        ALPACA_LOG_INFO("iOptron", "set_connected called with: " + std::string(connected ? "true" : "false"));

        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_)) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_)) {
            return;
        }
        // The connect check reads the link-aware value, so Connect over a lost link
        // reconnects (decision 0009 rule 4); the disconnect check reads the raw flag, so a
        // lost link still runs the teardown (#1289).
        if (connected ? connected_ && !link_lost() : !connected_) {
            ALPACA_LOG_INFO("iOptron", "Already in requested state, returning");
            return;
        }

        if (connected) {
            ALPACA_LOG_INFO("iOptron", "Attempting to connect...");
            if (connected_) {
                // Connect over a lost link (decision 0009 rule 4): end the dead session first, as
                // Disconnect does (its bodies were joined above), then connect afresh below, which
                // resets the rest of the session state.
                published_connected_ = false;
                connected_ = false;
                protocol.disconnect();  // the port is already released; clears the kept loss reason
            }

            // Auto-detected devices resolve their port or host here, not in the
            // factory: a persisted device is constructed at server start-up,
            // when a Wi-Fi mount has often not joined the network yet (#659).
            // The scan and the open run as one exchange: the state lock is released for them and taken
            // back before the outcome or its exception leaves. The connection members the scan writes are
            // read only under transition_mutex_, which this call holds; io_mutex_ stays held. The nested
            // exchanges inside rely on LockGate::exchange going straight through once the state lock is
            // dropped, and fallback_unique_id() takes mutex_ again inside the scan.
            protocol.call([&](auto&) {
                util::connect_resolved(
                    connection_info_, connection_resolved_, connection_resolver_,
                    [this, &protocol](const ConnectionInfo& info) {
                        ALPACA_LOG_INFO("iOptron", "Calling protocol.connect()...");
                        if (!protocol.connect(info)) {
                            ALPACA_LOG_ERROR("iOptron", "protocol.connect() returned false");
                            // The wrapper's reason (a port another connected device
                            // holds, a busy or missing port) reaches the client (#1202).
                            const std::string reason = protocol.last_connect_error();
                            const std::string message =
                                "Failed to connect to iOptron mount" + (reason.empty() ? std::string() : ": " + reason);
                            // A refused TCP connect, a vanished serial node, or a port another
                            // connected device now holds (#1202) means the
                            // resolved endpoint is gone: re-scan. A node that is present
                            // but would not open is not stale (busy, permissions).
                            if (info.type == ConnectionType::Network || util::serial_endpoint_stale(info.port_path)) {
                                throw util::StaleEndpoint(message);
                            }
                            throw AlpacaException(message);
                        }
                        // An auto-detected mount is remembered once it answered, so the next connect
                        // tries this place first. A configured port or host has no resolver and is
                        // never stored.
                        if (connection_resolver_) {
                            util::DeviceLocationStore::save(util::location_key(get_unique_id(), location_index_),
                                                            location_of(info));
                        }
                    },
                    "iOptron");
            });
            ALPACA_LOG_INFO("iOptron", "protocol.connect() returned true");
            if (relink_stop_pending_) {
                try {
                    protocol.stop_slewing();
                    relink_stop_pending_ = false;
                    ALPACA_LOG_WARN("iOptron", "Reconnected over a lost link with motion running; sent :Q#");
                } catch (const std::exception& e) {
                    protocol.disconnect();
                    throw AlpacaException(
                        std::string("Failed to connect to iOptron mount: the stop for the motion running when the "
                                    "link dropped could not be sent: ") +
                        e.what());
                }
            }
            site_info_valid_ = false;
            position_cache_valid_ = false;
            altaz_cache_valid_ = false;
            status_cache_valid_ = false;
            guide_rate_valid_ = false;
            utc_offset_.clear();
            clock_sync_connect_pass_done_ = false;
            client_disagreement_warned_ = false;
            device_faulted_ = false;
            device_fault_count_ = 0;
            last_device_error_.clear();
            publish_link_fault_locked();
            dec_guide_calibration_attempted_ = false;
            dec_guide_calibrated_ = false;
            dec_guide_inverted_ = true;
            dec_guide_calibration_side_ = -1;
            dec_guide_calibration_in_progress_.store(false);
            axis_move_active_primary_ = false;
            axis_move_active_secondary_ = false;
            tracking_state_before_move_.reset();
            park_override_until_ = std::chrono::steady_clock::time_point{};
            park_finalize_pending_ = false;
            tracking_rate_override_until_ = std::chrono::steady_clock::time_point{};
            clear_hold_windows_locked();
            slew_in_progress_ = false;
            note_unlanded_zero_search_locked();
            driver_motion_ = DriverMotion::None;
            utc_query_supported_ = true;
            pulse_guiding_hold_ra_hours_ = 0.0;
            last_ra_read_valid_ = false;
            last_ra_read_hours_ = 0.0;
            pulse_guiding_ra_baseline_hours_ = 0.0;
            pulse_guiding_ra_expected_delta_hours_ = 0.0;
            pulse_guiding_dec_baseline_degrees_ = 0.0;
            pulse_guiding_dec_expected_delta_degrees_ = 0.0;
            pulse_guiding_hold_dec_degrees_ = 0.0;
            last_dec_read_valid_ = false;
            last_dec_read_degrees_ = 0.0;
            target_ra_set_ = false;
            target_dec_set_ = false;
            target_ra_hours_ = 0.0;
            target_dec_degrees_ = 0.0;
            sync_offset_ra_hours_ = 0.0;
            sync_offset_dec_degrees_ = 0.0;
            invalidate_slew_error_locked();
            // The mount clock is sampled once, before Connected is published (the getter makes no exchange). The
            // one :GUT# reply also gives the site's timezone and DST to the prefetch below, so the connect sends
            // :GUT# once (#1784). The two fields are judged apart: an unusable UTC still leaves usable site fields.
            std::optional<std::string> gut_reply;
            try {
                gut_reply = protocol.read_utc_reply();
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("iOptron", "Mount UTC unavailable on connect: " + std::string(e.what()));
            }
            util::UtcOffset mount_time;
            if (gut_reply) {
                try {
                    mount_time.set(iOptronProtocolWrapper::parse_utc_time(*gut_reply), clock_.now());
                } catch (const std::exception& e) {
                    ALPACA_LOG_DEBUG("iOptron", "Mount UTC unavailable on connect: " + std::string(e.what()));
                }
            }
            utc_offset_ = mount_time;
            // The caches are primed before connected_ is published: a getter that arrives during these reads
            // answers NotConnected at once (check_connected_before_lock()) instead of waiting behind them.
            prefetch_mount_state_locked(gut_reply ? &*gut_reply : nullptr);
            read_guide_rate_locked();
            try {
                mount_info_ = protocol.get_mount_info();
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("iOptron", "Mount info unavailable on connect: " + std::string(e.what()));
            }
            // The reads above log and carry on when they fail; a cable pulled during them lost
            // the link (the wrapper released the port), so this Connect fails rather than
            // publishing a mount that is gone. The reason goes with the failure and is not kept.
            if (link_lost()) {
                const std::string reason = protocol.link_fault();
                protocol.disconnect();
                throw AlpacaException("Failed to connect to iOptron mount: " + reason, AlpacaError::NotConnected);
            }
            connected_ = true;
            fast_cache_until_ = clock_.now() + kFastCacheGrace;
            std::string mount_desc = mount_info_.model_name.empty()
                                         ? "unknown model"
                                         : mount_info_.model_name + " (" + mount_info_.model_code + ")";
            ALPACA_LOG_INFO(
                "iOptron", "Connected to " + mount_desc + " over " +
                               std::string(connection_info_.type == ConnectionType::Serial ? "Serial/USB" : "Network"));
            published_connected_ = true;  // the connect is complete (#910)
            lock.unlock();
            // The connect's body reads the host clock's trust itself, so a change seen during the connect needs
            // no re-sync of its own (#2419).
            resync_pending_.store(false);
            // After the locks are released: the body takes both.
            start_clock_sync();
            return;
        }

        ALPACA_LOG_INFO("iOptron", "Disconnecting...");
        // The bodies were joined above. The state is cleared under mutex_, then the close runs under io_mutex_
        // with mutex_ released for it (decision 0016 item 4), so a member that gets a lock meanwhile reads
        // NotConnected and no exchange sees a closed port.
        published_connected_ = false;
        connected_ = false;
        site_info_valid_ = false;
        position_cache_valid_ = false;
        altaz_cache_valid_ = false;
        status_cache_valid_ = false;
        guide_rate_valid_ = false;
        utc_offset_.clear();
        device_faulted_ = false;
        device_fault_count_ = 0;
        last_device_error_.clear();
        publish_link_fault_locked();
        dec_guide_calibration_attempted_ = false;
        dec_guide_calibrated_ = false;
        dec_guide_inverted_ = true;
        dec_guide_calibration_side_ = -1;
        dec_guide_calibration_in_progress_.store(false);
        utc_query_supported_ = true;
        sync_offset_ra_hours_ = 0.0;
        sync_offset_dec_degrees_ = 0.0;
        fast_cache_until_ = std::chrono::steady_clock::time_point{};
        axis_move_active_primary_ = false;
        axis_move_active_secondary_ = false;
        tracking_state_before_move_.reset();
        park_override_until_ = std::chrono::steady_clock::time_point{};
        park_finalize_pending_ = false;
        tracking_rate_override_until_ = std::chrono::steady_clock::time_point{};
        clear_hold_windows_locked();
        slew_in_progress_ = false;
        note_unlanded_zero_search_locked();
        driver_motion_ = DriverMotion::None;
        pulse_guiding_hold_ra_hours_ = 0.0;
        last_ra_read_valid_ = false;
        last_ra_read_hours_ = 0.0;
        pulse_guiding_ra_baseline_hours_ = 0.0;
        pulse_guiding_ra_expected_delta_hours_ = 0.0;
        pulse_guiding_dec_baseline_degrees_ = 0.0;
        pulse_guiding_dec_expected_delta_degrees_ = 0.0;
        pulse_guiding_hold_dec_degrees_ = 0.0;
        last_dec_read_valid_ = false;
        last_dec_read_degrees_ = 0.0;
        target_ra_set_ = false;
        target_dec_set_ = false;
        target_ra_hours_ = 0.0;
        target_dec_degrees_ = 0.0;
        invalidate_slew_error_locked();
        protocol.disconnect();
        ALPACA_LOG_INFO("iOptron", "Disconnected from mount");
    }

    // "gps_status": whether the mount's GPS receiver holds a fix, with the receiver's own latitude and
    // longitude (issue #1277). Every iOptron mount answers :GLS#, so it is listed whenever connected.
    static constexpr std::string_view kGpsStatusAction = "gps_status";

    std::vector<std::string> get_supported_actions() const override {
        // connected_ and the wrapper's loss latch are atomic: no mutex_, so a command_* exchange holding it cannot
        // stall this getter.
        if (!connected_.load() || link_lost()) return {};
        return {std::string(kGpsStatusAction)};
    }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;  // gps_status takes none
        if (!util::is_gps_status_action(action_name)) {
            throw AlpacaException("Action not supported: " + std::string(action_name),
                                  AlpacaError::ActionNotImplemented);
        }
        StateLock lock(lock_gate_);
        check_connected();
        // :GLS# digit 17 is 2 only for valid GPS data; the site in the same reply is then the receiver's
        // reading. Without a fix the reply holds the stored site, which must not be reported as GPS.
        const SiteInfo site = wire().get_site_location();
        const bool fix = (site.gps_status == 2);
        if (!fix) return util::format_gps_status(false, std::nullopt, std::nullopt);
        return util::format_gps_status(true, site.latitude_degrees, site.longitude_degrees);
    }

    bool can_action(std::string_view action_name) const override {
        return connected_.load() && !link_lost() && util::is_gps_status_action(action_name);
    }
    
    std::string command_blind(std::string_view command, bool raw) override {
        StateLock lock(lock_gate_);
        check_connected();

        const Wire protocol = wire();
        if (raw) {
            protocol.send_command_blind(std::string(command));
        } else {
            // Parse and execute standard Alpaca commands
            // For now, pass through to protocol wrapper
            protocol.send_command_blind(std::string(command));
        }
        return "";
    }
    
    bool command_bool(std::string_view command, bool raw) override {
        StateLock lock(lock_gate_);
        check_connected();

        const Wire protocol = wire();
        if (raw) {
            std::string response = protocol.send_command(std::string(command));
            return (response == "1");
        } else {
            // Parse standard Alpaca commands
            protocol.send_command_blind(std::string(command));
            return true;
        }
    }
    
    std::string command_string(std::string_view command, bool raw) override {
        StateLock lock(lock_gate_);
        check_connected();

        const Wire protocol = wire();
        if (raw) {
            return protocol.send_command(std::string(command));
        } else {
            return protocol.send_command(std::string(command));
        }
    }
    
    // TelescopeDriver interface
    
    AlignmentMode get_alignment_mode() const override {
        // iOptron mounts are equatorial (German Polar)
        return AlignmentMode::GermanPolar;
    }
    
    double get_altitude() const override {
        return serve_cached_or_refill([&] { return altaz_cache_fresh_locked(); },
                                      [&] {
                                          refresh_altaz_cache_locked();
                                          return cached_alt_degrees_;
                                      });
    }

    // The optics and SlewSettleTime are the client's values, not the mount's: no lock, no fault refusal, but
    // NotConnected while disconnected, connecting or lost like every other member (ASCOM contract precedence). The
    // optics setters are not ASCOM members: the catalog and updatedevice (#2057) inject the configured values with
    // them, connected or not.
    double get_aperture_diameter() const override {
        check_connected_lock_free();
        return aperture_diameter_m_;
    }

    void set_aperture_diameter(double meters) override { aperture_diameter_m_ = meters; }

    double get_aperture_area() const override {
        check_connected_lock_free();
        // Derived from the atomic diameter so a live updatedevice cannot leave a stale area (#2057).
        const double d = aperture_diameter_m_;
        return d > 0.0 ? std::numbers::pi * (d / 2.0) * (d / 2.0) : 0.0;
    }
    
    bool get_at_home() const override {
        return serve_cached_or_refill(
            [&] { return status_cache_fresh_locked() && !zero_search_start_check_due_locked(clock_.now()); },
            [&] {
                if (!lock_gate_.holds_io() && zero_search_start_check_due_locked(clock_.now())) {
                    throw RefillNeedsOperationLock{};
                }
                refresh_status_cache_locked();
                if (lock_gate_.holds_io()) {
                    check_zero_search_started_locked(clock_.now());
                }
                // Issue #2420: the status 7 a zero search starts from is the false zero
                // it is there to correct. AtHome only once the search has landed.
                return driver_motion_ != DriverMotion::HomeSearch && cached_status_.is_at_home &&
                       !home_search_unconfirmed_;
            });
    }
    
    bool get_at_park() const override {
        return serve_cached_or_refill(
            [&] { return park_override_until_ > clock_.now() || status_cache_fresh_locked(); },
            [&] {
                auto now = clock_.now();
                if (park_override_until_ > now) {
                    return false;
                }
                // The Unpark override closed between the freshness check and the read of `now` above (the
                // Slewing landing's sibling): a status from inside the window is not served as the answer.
                if (!lock_gate_.holds_io() && !status_cache_fresh_locked()) {
                    throw RefillNeedsOperationLock{};
                }
                refresh_status_cache_locked();
                return cached_status_.is_parked;
            });
    }
    
    double get_azimuth() const override {
        return serve_cached_or_refill([&] { return altaz_cache_fresh_locked(); },
                                      [&] {
                                          refresh_altaz_cache_locked();
                                          return cached_az_degrees_;
                                      });
    }
    
    bool get_can_find_home() const override {
        return true;
    }
    
    bool get_can_park() const override {
        return true;  // All iOptron mounts support parking
    }
    
    bool get_can_pulse_guide() const override {
        return true;  // All iOptron mounts support pulse guiding
    }
    
    bool get_is_pulse_guiding() const override {
        if (!connected_) {
            throw AlpacaException("Not connected to mount", AlpacaError::NotConnected);
        }
        // Lock-free like the rest of this getter: the loss latch and the fault flag are atomics and the fault
        // text has its own leaf mutex (decision 0009 rules 1 and 3). It does not run the staleness loss (rule 2b):
        // lose_link() takes the wrapper mutex, which every serial exchange holds, so the next check_connected() call
        // finds a stale fault instead.
        throw_if_link_lost();
        if (device_faulted_.load()) {
            throw AlpacaException("Mount communications compromised: " + published_fault_text(),
                                  AlpacaError::DriverException);
        }
        return pulse_window_open();
    }

    // True from PulseGuide until its duration plus kPulseGuideCompletionDelay (500 ms) has passed (decision 0022).
    bool pulse_window_open() const { return pulse_guide_hold_.load(std::memory_order_acquire).active(clock_.now()); }

    bool get_can_set_declination_rate() const override {
        return false;  // iOptron doesn't support setting Dec rate directly
    }
    
    bool get_can_set_guide_rates() const override {
        return true;  // iOptron supports guide rates
    }
    
    bool get_can_set_park() const override {
        return true;  // iOptron supports setting park position
    }
    
    bool get_can_set_pier_side() const override {
        return false;  // iOptron doesn't allow setting pier side directly
    }
    
    bool get_can_set_right_ascension_rate() const override {
        return false;  // iOptron doesn't support setting RA rate directly
    }
    
    bool get_can_set_tracking() const override {
        return true;  // iOptron supports start/stop tracking
    }

    bool get_can_slew_alt_az() const override {
        return true;
    }

    bool get_can_slew_alt_az_async() const override {
        return true;
    }

    bool get_can_sync_alt_az() const override {
        return false;
    }
    
    bool get_can_slew() const override {
        return true;  // All iOptron mounts support slewing
    }
    
    bool get_can_slew_async() const override {
        return true;  // iOptron supports async slewing
    }
    
    bool get_can_sync() const override {
        return true;  // iOptron supports sync
    }
    
    bool get_can_unpark() const override {
        return true;  // All iOptron mounts support unpark
    }
    
    double get_declination() const override {
        return serve_cached_or_refill(
            [&] { return pulse_position_hold_[kDecAxis].active(clock_.now()) || position_cache_fresh_locked(); },
            [&] {
                double dec_value = 0.0;
                if (pulse_position_hold_[kDecAxis].active(clock_.now())) {
                    dec_value = pulse_guiding_hold_dec_degrees_;
                } else {
                    refresh_position_cache_locked();
                    dec_value = cached_dec_degrees_;
                }
                // The first read after a Dec pulse serves the predicted position; any later read is live.
                if (pulse_position_correction_[kDecAxis].active(clock_.now())) {
                    dec_value = pulse_guiding_dec_baseline_degrees_ + pulse_guiding_dec_expected_delta_degrees_;
                }
                pulse_position_correction_[kDecAxis].clear();
                last_dec_read_degrees_ = dec_value;
                last_dec_read_valid_ = true;
                return dec_value + sync_offset_dec_degrees_;
            });
    }
    
    double get_declination_rate() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        // iOptron doesn't support getting Dec rate directly
        // Return 0 (not available)
        return 0.0;
    }
    
    void set_declination_rate(double rate) override {
        (void)rate;  // Unused - not supported
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw AlpacaException(
            "Declination rate is not implemented by this mount",
            AlpacaError::PropertyNotImplemented
        );
    }
    
    bool get_tracking() const override {
        return serve_cached_or_refill(
            [&] {
                return (tracking_override_until_ > clock_.now() && status_cache_valid_) || status_cache_fresh_locked();
            },
            [&] { return get_tracking_locked(); });
    }
    
    void set_tracking(bool tracking) override {
        StateLock lock(lock_gate_);
        check_connected();
        const Wire protocol = wire();
        if (tracking) {
            ensure_not_parked_locked("Tracking");
        }
        if (tracking) {
            protocol.start_tracking();
        } else {
            protocol.stop_tracking();
        }
        cached_status_.is_tracking = tracking;
        status_cache_valid_ = true;
        last_status_update_ = clock_.now();
        tracking_override_until_ = last_status_update_ + std::chrono::seconds(2);
        if (axis_move_active_primary_ || axis_move_active_secondary_) {
            tracking_state_before_move_ = tracking;
        } else {
            tracking_state_before_move_.reset();
        }
    }
    
    double get_focal_length() const override {
        check_connected_lock_free();
        return focal_length_m_;
    }
    
    void set_focal_length(double meters) override {
        focal_length_m_ = meters;
    }

    // Issue #2434: the mount's own rate, read with :AG once on the connect path (read_guide_rate_locked())
    // and never here (decision 0016), or the rate the last write sent. A failed read leaves it unknown, and this
    // raises rather than invent a rate.
    GuideRate get_guide_rate() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        if (!guide_rate_valid_) {
            throw AlpacaException("GuideRate is unknown: the mount's guide rate (:AG#) could not be read at connect",
                                  AlpacaError::DriverException);
        }
        return cached_guide_rate_;
    }

    // #2257: the static checks (finite, > 0, inside the mount's range) run before the connection check, and out
    // of range is refused, never clamped. The range is 0.01-0.90 (RA) and 0.10-0.99 (Dec) of the sidereal rate;
    // kEps absorbs rounding at a bound, which set_guide_rate_locked() then clamps inside it.
    static void validate_guide_rate_value(double value, bool ra_axis) {
        util::check_guide_rate_arguments(
            kCommandLimits, ra_axis ? util::GuideRateAxis::RightAscension : util::GuideRateAxis::Declination, value);
        constexpr double kEps = 1e-9;
        const double fraction = value / kSiderealRateDegPerSec;
        const double low = ra_axis ? 0.01 : 0.10;
        const double high = ra_axis ? 0.90 : 0.99;
        if (fraction < low - kEps || fraction > high + kEps) {
            throw AlpacaException("GuideRate must be 0.01-0.90 (RA) and 0.10-0.99 (Dec) of the sidereal rate",
                                  AlpacaError::InvalidValue);
        }
    }

    void set_guide_rate(const GuideRate& rate) override {
        validate_guide_rate_value(rate.ra, true);
        validate_guide_rate_value(rate.dec, false);
        StateLock lock(lock_gate_);
        util::check_guide_rate_state(kCommandLimits, GateChecks{*this, "GuideRate"});
        set_guide_rate_locked(rate);
    }

    // The per-axis setters keep the other axis's rate. They are commands, so when that rate is unknown (the :AG#
    // read at connect failed) they read :AG# again here, not in the getter (decision 0016); a read that fails again
    // raises and nothing is written. The argument check precedes the connection check (#2257).
    void set_guide_rate_right_ascension(double value) override { set_one_guide_rate_axis(true, value); }
    void set_guide_rate_declination(double value) override { set_one_guide_rate_axis(false, value); }

    void set_one_guide_rate_axis(bool ra_axis, double value) {
        validate_guide_rate_value(value, ra_axis);
        StateLock lock(lock_gate_);
        util::check_guide_rate_state(kCommandLimits,
                                     GateChecks{*this, ra_axis ? "GuideRateRightAscension" : "GuideRateDeclination"});
        if (!guide_rate_valid_) read_guide_rate_locked();
        // A serial hangup during the re-read lost the link: NotConnected, not "unknown". Here and not in
        // read_guide_rate_locked(), whose connect-path caller closes the port on a lost link before it throws.
        if (!guide_rate_valid_) throw_if_link_lost();
        if (!guide_rate_valid_) {
            throw AlpacaException("GuideRate is unknown: the mount's guide rate (:AG#) could not be read",
                                  AlpacaError::DriverException);
        }
        GuideRate rate = cached_guide_rate_;
        (ra_axis ? rate.ra : rate.dec) = value;
        set_guide_rate_locked(rate);
    }

    void set_guide_rate_locked(const GuideRate& rate) {
        // validate_guide_rate_value() refused out of range (#2257); the clamp below only absorbs rounding inside
        // its kEps of a bound, because the protocol wrapper refuses a fraction past its bounds.
        const Wire protocol = wire();
        double ra_fraction = rate.ra / kSiderealRateDegPerSec;
        double dec_fraction = rate.dec / kSiderealRateDegPerSec;
        ra_fraction = std::clamp(ra_fraction, 0.01, 0.90);
        dec_fraction = std::clamp(dec_fraction, 0.10, 0.99);
        protocol.set_guide_rates(ra_fraction, dec_fraction);
        // Issue #2434: :RG# carries hundredths of the sidereal rate, so the cache keeps the rate that was sent, not
        // the request.
        cached_guide_rate_.ra = std::round(ra_fraction * 100.0) / 100.0 * kSiderealRateDegPerSec;
        cached_guide_rate_.dec = std::round(dec_fraction * 100.0) / 100.0 * kSiderealRateDegPerSec;
        guide_rate_valid_ = true;
    }

    double get_right_ascension() const override {
        return serve_cached_or_refill(
            [&] { return pulse_position_hold_[kRaAxis].active(clock_.now()) || position_cache_fresh_locked(); },
            [&] {
                double ra_value = 0.0;
                if (pulse_position_hold_[kRaAxis].active(clock_.now())) {
                    ra_value = pulse_guiding_hold_ra_hours_;
                } else {
                    refresh_position_cache_locked();
                    ra_value = cached_ra_hours_;
                }
                // The first read after an RA pulse serves the predicted position; any later read is live.
                if (pulse_position_correction_[kRaAxis].active(clock_.now())) {
                    ra_value =
                        util::wrap_hours(pulse_guiding_ra_baseline_hours_ + pulse_guiding_ra_expected_delta_hours_);
                }
                pulse_position_correction_[kRaAxis].clear();
                last_ra_read_hours_ = ra_value;
                last_ra_read_valid_ = true;
                return util::wrap_hours(ra_value + sync_offset_ra_hours_);
            });
    }

    double get_right_ascension_rate() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        // ASCOM RightAscensionRate is an OFFSET from sidereal tracking, in
        // seconds of RA per sidereal second — NOT the absolute tracking rate in
        // arcsec/sec. The driver never applies an RA-rate offset, so this is
        // always 0.0 (matches the SynScan/Celestron/Bisque drivers).
        return 0.0;
    }
    
    void set_right_ascension_rate(double rate) override {
        (void)rate;  // Unused - not supported
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw AlpacaException(
            "Right ascension rate is not implemented by this mount",
            AlpacaError::PropertyNotImplemented
        );
    }
    
    int get_side_of_pier() const override {
        return serve_cached_or_refill(
            [&] {
                return position_cache_valid_ && !slew_in_progress_ && !cached_status_.is_slewing &&
                       (clock_.now() - last_position_update_) < kSideOfPierPositionTtl && site_info_valid_;
            },
            [&] {
                // Pier side only changes across a slew, flip, park or home, and every
                // motion initiator invalidates the position cache, so a position a
                // few seconds old is authoritative here. One :GEP# round trip costs
                // ~100 ms of mount latency (HAE16 EQ ConformU 4.5.0, 2026-08-25:
                // 0.113 s against the 0.1 s FAST target when the 1 s cache had just
                // lapsed), so this read tolerates a longer staleness than RA/Dec.
                // slew_in_progress_ (set by every GOTO initiator, cleared once
                // Slewing has observed completion) is the authoritative "a move may
                // be changing pier side" signal here: cached_status_.is_slewing is
                // only refreshed when someone polls Slewing/DeviceState, so a client
                // that polls SideOfPier alone across a flip would otherwise ride the
                // 5 s window on pre-flip data (PR #228 review). While a slew is in
                // flight every read goes live. The flag is sticky until the next
                // Slewing/DeviceState poll observes completion, so a client that
                // polls SideOfPier alone keeps live reads after the move as well:
                // never stale, just uncached until it asks whether the slew ended.
                const auto now = clock_.now();
                if (!position_cache_valid_ || slew_in_progress_ || cached_status_.is_slewing ||
                    (now - last_position_update_) >= kSideOfPierPositionTtl) {
                    refresh_position_cache_locked();
                }
                if (!site_info_valid_) {
                    ensure_site_info_cached_locked();
                    // ensure_site_info_cached_locked() sets site_info_valid_ on success;
                    // this re-check detects a failed cache populate, not a dead condition.
                    // cppcheck-suppress identicalInnerCondition
                    if (!site_info_valid_) {
                        return normalize_side_of_pier_value(cached_side_of_pier_);
                    }
                }

                const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
                const double synced_ra = util::wrap_hours(cached_ra_hours_ + sync_offset_ra_hours_);
                const double hour_angle = util::shortest_ra_delta_hours(lst, synced_ra);
                return util::pier_side_for_hour_angle(hour_angle);
            });
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        util::validate_ra_dec(ra, dec, "DestinationSideOfPier");
        return serve_cached_or_refill([&] { return site_info_valid_; },
                                      [&] {
                                          if (!site_info_valid_) {
                                              ensure_site_info_cached_locked();
                                              // ensure_site_info_cached_locked() sets site_info_valid_ on success;
                                              // this re-check detects a failed cache populate, not a dead condition.
                                              // cppcheck-suppress identicalInnerCondition
                                              if (!site_info_valid_) {
                                                  return normalize_side_of_pier_value(cached_side_of_pier_);
                                              }
                                          }

                                          const double lst = util::local_sidereal_hours(current_utc_time_locked(),
                                                                                        site_longitude_cached_);
                                          const double hour_angle = util::shortest_ra_delta_hours(lst, ra);

                                          return util::pier_side_for_hour_angle(hour_angle);
                                      });
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::Topocentric;
    }

    // The driver applies no refraction correction, so it neither knows nor computes the flag (ITelescopeV4).
    bool get_does_refraction() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    void set_does_refraction(bool) override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    int get_slew_settle_time() const override {
        check_connected_lock_free();
        return slew_settle_time_seconds_;
    }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException(
                "Slew settle time must be >= 0 seconds",
                AlpacaError::InvalidValue
            );
        }
        check_connected_lock_free();  // an ASCOM member: InvalidValue first, then NotConnected
        slew_settle_time_seconds_ = seconds;
    }
    
    void set_side_of_pier(int side) override {
        (void)side;  // Unused - not supported
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw AlpacaException(
            "Setting pier side not supported by iOptron mount",
            AlpacaError::PropertyNotImplemented
        );
    }
    
    double get_sidereal_time() const override {
        return serve_cached_or_refill([&] { return site_info_fresh_locked(); },
                                      [&] {
                                          ensure_site_info_cached_locked();
                                          auto utc_now = current_utc_time_locked();
                                          return util::local_sidereal_hours(utc_now, site_longitude_cached_);
                                      });
    }
    
    double get_site_elevation() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (!site_elevation_m_) {
            throw AlpacaException("SiteElevation has not been set", AlpacaError::InvalidOperation);
        }
        return *site_elevation_m_;
    }
    
    void set_site_elevation(double elevation) override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException(
                "Site elevation must be between -300 and 10000 meters",
                AlpacaError::InvalidValue
            );
        }
        site_elevation_m_ = elevation;
        ++site_elevation_write_generation_;  // the background clock sync must not publish over this write
    }
    
    double get_site_latitude() const override {
        return serve_cached_or_refill([&] { return site_info_fresh_locked(); },
                                      [&] {
                                          ensure_site_info_cached_locked();
                                          return site_latitude_cached_;
                                      });
    }

    void set_site_latitude(double latitude) override {
        util::validate_latitude(latitude);
        StateLock lock(lock_gate_);
        check_connected();
        const Wire protocol = wire();
        protocol.set_latitude(latitude);
        protocol.set_hemisphere(latitude >= 0.0);
        site_latitude_cached_ = latitude;
        hemisphere_north_ = (latitude >= 0.0);
        ++site_latitude_write_generation_;  // the background clock sync must not publish over this write
        site_info_valid_ = true;
        last_site_info_fetch_ = clock_.now();
    }

    double get_site_longitude() const override {
        return serve_cached_or_refill([&] { return site_info_fresh_locked(); },
                                      [&] {
                                          ensure_site_info_cached_locked();
                                          return site_longitude_cached_;
                                      });
    }

    void set_site_longitude(double longitude) override {
        util::validate_longitude(longitude);
        StateLock lock(lock_gate_);
        check_connected();
        const Wire protocol = wire();
        protocol.set_longitude(longitude);
        site_longitude_cached_ = longitude;
        ++site_longitude_write_generation_;  // the background clock sync must not publish over this write
        site_info_valid_ = true;
        last_site_info_fetch_ = clock_.now();
    }

    bool get_can_move_axis(int axis) const override {
        if (axis < 0 || axis > 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return (axis == 0 || axis == 1);
    }

    void move_axis(int axis, double rate) override {
        util::check_move_axis_arguments(kCommandLimits, axis, rate);
        double abs_rate = std::abs(rate);
        if (!is_axis_rate_supported(abs_rate)) {
            throw AlpacaException("Axis rate out of range", AlpacaError::InvalidValue);
        }

        StateLock lock(lock_gate_);
        util::check_move_axis_state(kCommandLimits, abs_rate == 0.0, GateChecks{*this, "MoveAxis"});

        const bool was_any_axis_active = axis_move_active_primary_ || axis_move_active_secondary_;
        const Wire protocol = wire();
        if (abs_rate > 0.0) {
            const int arrow_speed = arrow_speed_level_for_rate(abs_rate);
            std::ostringstream cmd;
            cmd << ":SR" << arrow_speed << "#";
            protocol.send_command_blind(cmd.str());
        }
        // Issue #2424: a positive rate turns the tube east on axis 0 (the reported RightAscension rises) and north
        // on axis 1 (the reported Declination rises), the convention in ascom-contract.instructions.md.
        if (axis == 0) {
            // iOptron calls :mw# "RA+" and :me# "RA-" (RS-232 v3.10). RA+ raises RA despite the letter: the :ZS (RA+)
            // and :ZQ (RA-) GOTO trim closes the HAE16/HAE29C landing error that way, and INDI found :mw# raises RA on
            // a CEM120. Confirmed for MoveAxis on the HAE29C on both pier sides (#2424, 2026-11-20).
            if (rate > 0.0) {
                protocol.send_command_blind(":mw#");  // RA+: RA rises, tube turns east
            } else if (rate < 0.0) {
                protocol.send_command_blind(":me#");  // RA-: RA falls, tube turns west
            } else {
                protocol.send_command_blind(":qR#"); // stop RA
            }
        } else if (axis == 1) {
            if (rate > 0.0 || rate < 0.0) {
                // :ms# is iOptron's "Dec+" and :mn# "Dec-", a mechanical sense. The Dec pulse-guide polarity picks
                // the command (shared with pulse_guide(), which this does not change): dec_guide_inverted_ (true; its
                // calibration is never called) and the :GEP# pier digit, so north is :mn# on side 0 and :ms# on
                // side 1. Confirmed on the HAE29C on both pier sides (#2424, 2026-11-20).
                bool north = (rate > 0.0);
                bool invert_dec = dec_guide_inverted_;
                refresh_position_cache_locked();
                if (should_flip_dec_for_pier_locked()) {
                    invert_dec = !invert_dec;
                }
                if (invert_dec) {
                    north = !north;
                }
                protocol.send_command_blind(north ? ":ms#" : ":mn#");
            } else {
                protocol.send_command_blind(":qD#"); // stop Dec
            }
        }

        status_cache_valid_ = false;
        if (rate == 0.0) {
            if (axis == 0) {
                axis_move_active_primary_ = false;
            } else {
                axis_move_active_secondary_ = false;
            }
            if (!axis_move_active_primary_ && !axis_move_active_secondary_ &&
                tracking_state_before_move_.has_value()) {
                const bool desired_tracking = tracking_state_before_move_.value();
                if (desired_tracking) {
                    protocol.start_tracking();
                } else {
                    protocol.stop_tracking();
                }
                cached_status_.is_tracking = desired_tracking;
                status_cache_valid_ = true;
                last_status_update_ = clock_.now();
                tracking_override_until_ = last_status_update_ + std::chrono::seconds(2);
                tracking_state_before_move_.reset();
            }
        } else {
            if (axis == 0) {
                axis_move_active_primary_ = true;
            } else {
                axis_move_active_secondary_ = true;
            }
            if (!was_any_axis_active && !tracking_state_before_move_.has_value()) {
                tracking_state_before_move_ = cached_status_.is_tracking;
            }
        }
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that jogs an axis after a failed GOTO must not be told the OLD
        // goto failed.
        // A GOTO a status refresh already saw stopped is finished (issue #2063), not driver motion.
        const DriverMotion motion = (slew_in_progress_ && !goto_seen_stopped_) ? DriverMotion::Goto : driver_motion_;
        supersede_slew_dispatch_locked();
        clear_slew_hold_locked();  // MoveAxis starts no GOTO: Slewing follows the axis flags and the mount
        // open-astro#843: :qR#/:qD# stop arrow motion only ("other slewing and tracking are not affected",
        // RS-232 command language v3.10), so a GOTO, Park or FindHome the driver started keeps moving after
        // MoveAxis. Keep it as driver motion (a jog too: the protocol does not say it ends one) so a PulseGuide
        // does not mask it; the next fresh no-motion status ends it, the override being cleared above.
        driver_motion_ = motion;
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 0 || axis == 1) {
            const auto& rates = axis_rate_steps_deg_per_sec();
            return {rates.front(), rates.back()};
        }
        return {0.0, 0.0};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis < 0 || axis > 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        if (axis != 0 && axis != 1) {
            // Tertiary axis not supported; empty range set per ASCOM/ConformU (avoids min=max=0 issue).
            return {};
        }
        const auto& rates = axis_rate_steps_deg_per_sec();
        std::vector<std::pair<double, double>> ranges;
        ranges.reserve(rates.size());
        for (double rate : rates) {
            ranges.push_back({rate, rate});
        }
        return ranges;
    }

    bool get_slewing() const override {
        return serve_cached_or_refill(
            [&] {
                return !last_slew_error_.empty() || axis_move_active_primary_ || axis_move_active_secondary_ ||
                       slew_hold_.active(clock_.now()) ||
                       (status_cache_fresh_locked() && !zero_search_start_check_due_locked(clock_.now()) &&
                        !(slew_in_progress_ && goto_refine_active() && slew_refine_count_ < kMaxSlewRefines));
            },
            [&] {
                // open-astro#575: surface a stored async-slew failure as an error
                // instead of a silent false, until AbortSlew or a new slew initiator
                // clears it (see last_slew_error_).
                if (!last_slew_error_.empty()) {
                    throw AlpacaException(last_slew_error_);
                }
                if (axis_move_active_primary_ || axis_move_active_secondary_) {
                    return true;
                }
                auto now = clock_.now();
                if (slew_hold_.active(now)) {
                    return true;
                }
                // The hold closed between the freshness check and the read of `now` above, and the status the
                // state lock could serve as it stands predates the hold. The mount, not the window, says when
                // the slew ended (decision 0022): ask it, under the operation lock.
                if (!lock_gate_.holds_io() &&
                    (!status_cache_fresh_locked() || zero_search_start_check_due_locked(now))) {
                    throw RefillNeedsOperationLock{};
                }
                refresh_status_cache_locked();
                if (lock_gate_.holds_io()) {
                    check_zero_search_started_locked(now);
                }
                // Issue #2420: a zero sensor search moves until a fresh status past its hold reads 7 (the refresh
                // above ends it then) or its bound stops it with a stored error, whatever status it reports meanwhile.
                if (!last_slew_error_.empty()) {
                    throw AlpacaException(last_slew_error_);
                }
                if (driver_motion_ == DriverMotion::HomeSearch) {
                    return true;
                }
                if (cached_status_.is_slewing) {
                    // open-astro#843: the firmware reports system status 2 (slewing) while it runs a pulse.
                    // ASCOM keeps Slewing false during PulseGuide (IsPulseGuiding carries it). The mask covers
                    // the pulse plus kPulseGuideCompletionDelay, and only with no driver-started GOTO still
                    // dispatching and no Park/FindHome still moving (driver_motion_).
                    return !(pulse_window_open() && (!slew_in_progress_ || goto_seen_stopped_) &&
                             driver_motion_ == DriverMotion::None);
                }
                // HAE29C/HAE16 firmware quirk (hardware-verified 2026-07-14 / 2026-08-25): GOTO stops
                // compensating sidereal motion during its final ~1 s approach, so
                // slews settle a consistent ~11-16 arcsec east of the RA target
                // (ConformU tolerance is +/-10"). Re-issuing the GOTO does NOT fix it:
                // the firmware deadbands slews shorter than ~15". Instead, close the
                // residual with a duration-computed pulse-guide trim (guide rate is
                // known, so err/rate gives the exact pulse length), re-measuring and
                // iterating up to kMaxSlewRefines. RA only: Dec settles within ~0.2"
                // on this firmware and Dec pulse polarity flips with pier side.
                // Slewing stays true while a trim pulse runs.
                // Issue #2063: the client's pulse takes over from the trim of a GOTO the mount already finished.
                if (pulse_window_open() && goto_seen_stopped_) {
                    return false;
                }
                if (slew_in_progress_ && goto_refine_active() && slew_refine_count_ < kMaxSlewRefines) {
                    // The trim reads the position itself. Under the state lock alone (the slew hold closed
                    // between the freshness check and the read of `now` above) the refused exchange would be
                    // taken for a failed trim and the GOTO reported landed with its RA error: run again under
                    // the operation lock instead.
                    if (!lock_gate_.holds_io()) {
                        throw RefillNeedsOperationLock{};
                    }
                    ++slew_refine_count_;
                    try {
                        const Wire protocol = wire();
                        Position pos = protocol.get_position();
                        double ra_err_hours = pos.ra_hours - slew_target_ra_hours_;
                        if (ra_err_hours > 12.0) {
                            ra_err_hours -= 24.0;
                        } else if (ra_err_hours < -12.0) {
                            ra_err_hours += 24.0;
                        }
                        const double ra_err_arcsec = ra_err_hours * 15.0 * 3600.0;
                        constexpr double kTrimMinArcsec = 5.0;    // below this: on target
                        constexpr double kTrimMaxArcsec = 120.0;  // above this: not a settle error
                        if (std::fabs(ra_err_arcsec) > kTrimMinArcsec && std::fabs(ra_err_arcsec) < kTrimMaxArcsec) {
                            double guide_fraction = kDefaultGuideRateFraction;
                            try {
                                guide_fraction = protocol.get_guide_rates().first;
                            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the default is kept
                                // Unreadable guide rate — fall back to the default fraction.
                            }
                            if (guide_fraction < 0.1 || guide_fraction > 1.0) {
                                guide_fraction = kDefaultGuideRateFraction;
                            }
                            const double trim_rate_arcsec_per_s = guide_fraction * 15.041;
                            int duration_ms =
                                static_cast<int>(std::fabs(ra_err_arcsec) / trim_rate_arcsec_per_s * 1000.0);
                            duration_ms = std::clamp(duration_ms, 100, 8000);
                            // Reported RA too large -> mount is east of target -> pulse
                            // west (RA-, direction 3); too small -> east (RA+, 2).
                            const int direction = ra_err_arcsec > 0 ? 3 : 2;
                            ALPACA_LOG_INFO("iOptron", "GOTO settled " + std::to_string(ra_err_arcsec) +
                                                           " arcsec from RA target; pulse-guide trim " +
                                                           std::to_string(slew_refine_count_) + "/" +
                                                           std::to_string(kMaxSlewRefines) + " (" +
                                                           std::to_string(duration_ms) +
                                                           " ms, final-approach firmware quirk)");
                            send_with_slew_hold_locked(std::chrono::milliseconds(duration_ms + 500),
                                                       [&] { protocol.pulse_guide(direction, duration_ms); });
                            goto_seen_stopped_ = false;  // the trim pulse is part of the GOTO: Slewing stays true
                            status_cache_valid_ = false;
                            position_cache_valid_ = false;
                            return true;
                        }
                    } catch (const std::exception&) {
                        // Best-effort trim; fall through and report slew complete, unless the link was lost: then
                        // NotConnected, never a "not slewing" from a mount that is gone.
                        throw_if_link_lost();
                    }
                }
                slew_in_progress_ = false;
                return false;
            });
    }
    
    double get_target_declination() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        if (!target_dec_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }
    
    void set_target_declination(double dec) override {
        util::validate_dec(dec, "TargetDeclination");
        StateLock lock(lock_gate_);
        check_connected();

        target_dec_degrees_ = dec;
        target_dec_set_ = true;
        const Wire protocol = wire();
        protocol.set_target_dec(dec);
    }
    
    double get_target_right_ascension() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        if (!target_ra_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }
    
    void set_target_right_ascension(double ra) override {
        util::validate_ra(ra, "TargetRightAscension");
        StateLock lock(lock_gate_);
        check_connected();

        target_ra_hours_ = ra;
        target_ra_set_ = true;
        const Wire protocol = wire();
        protocol.set_target_ra(ra);
    }
    
    int get_tracking_rate() const override {
        return serve_cached_or_refill(
            [&] {
                return (tracking_rate_override_until_ > clock_.now() && status_cache_valid_) ||
                       status_cache_fresh_locked();
            },
            [&] {
                auto now = clock_.now();
                if (tracking_rate_override_until_ > now && status_cache_valid_) {
                    return static_cast<int>(cached_status_.tracking_rate);
                }
                refresh_status_cache_locked();
                // Alpaca TrackingRate uses DriveRates enum values (0-4).
                return static_cast<int>(cached_status_.tracking_rate);
            });
    }

    void set_tracking_rate(int rate) override {
        util::check_tracking_rate_arguments(kCommandLimits, rate);
        StateLock lock(lock_gate_);
        util::check_tracking_rate_state(kCommandLimits, GateChecks{*this, "TrackingRate"});

        const Wire protocol = wire();

        protocol.set_tracking_rate(rate);
        if (rate == 4) {
            protocol.set_custom_tracking_rate(custom_tracking_rate_);
        }
        cached_status_.tracking_rate = rate;
        status_cache_valid_ = true;
        last_status_update_ = clock_.now();
        tracking_rate_override_until_ = last_status_update_ + std::chrono::seconds(2);
    }

    std::vector<int> get_tracking_rates() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        
        // Return supported tracking rates as DriveRates enum values:
        // 0 = driveSidereal, 1 = driveLunar, 2 = driveSolar, 3 = driveKing, 4 = driveCustom
        return {0, 1, 2, 3, 4};
    }
    
    std::chrono::system_clock::time_point get_utc_date() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();

        if (const auto utc = utc_offset_.current(clock_.now())) return *utc;
        throw AlpacaException("UTCDate has not been established; set the mount time first",
                              AlpacaError::InvalidOperation);
    }
    
    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        StateLock lock(lock_gate_);
        check_connected();

        const Wire protocol = wire();
        const auto client_minus_host = utc - host_utc_now();  // before the write (#409)
        protocol.set_utc_time(utc);
        utc_offset_.set(utc, clock_.now());
        ++utc_write_generation_;  // the background clock sync must not publish over this write
        // The mount now runs on the client's clock and so does the cached
        // pointing time; on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("iOptron", client_minus_host, client_disagreement_warned_);
    }
    
    // Telescope methods
    
    void find_home() override {
        bool zero_search = false;
        {
            StateLock lock(lock_gate_);
            check_connected();

            if (!get_can_find_home()) {
                throw AlpacaException("Find home not supported on this mount model");
            }

            ensure_not_parked_locked("FindHome");
            zero_search = zero_search_active();
            // A new FindHome starts over; a zero search's own status 2 would clear the flag anyway, and a :MH# model
            // returns below at a status 7 it can trust.
            home_search_unconfirmed_ = false;
            // Force-refresh: a stale cached is_at_home (e.g. set before a park/
            // unpark cycle, which never touches it) short-circuits the whole home
            // operation and the client then sees AtHome == false. A zero search ignores is_at_home, so it reads the
            // status only to end a search still running (the overdue stop): right after ConformU's AbortSlew and
            // Tracking writes the HAE29C took 0.8 s (USB) and 1.36 s (Wi-Fi) to answer :GLS#, which put FindHome
            // past its 1 s initiator target.
            if (!zero_search || driver_motion_ == DriverMotion::HomeSearch) {
                refresh_status_cache_locked(true);
            }
            // Issue #2420: a mount with a zero sensor search wakes from a power loss at status 7 wherever it points,
            // so status 7 proves nothing there and the search always runs (about 20 s from a true zero too).
            if (!zero_search && cached_status_.is_at_home) {
                return;
            }

            const Wire protocol = wire();
            // ASCOM FindHome locates home with sensors: :MSH# on the verified zero-search models, otherwise :MH#
            // (go to zero by the position model).
            send_with_slew_hold_locked(std::chrono::seconds(5), [&] {
                if (zero_search) {
                    protocol.find_home();
                } else {
                    protocol.go_to_home();
                }
            });
            supersede_slew_dispatch_locked();
            driver_motion_ = zero_search ? DriverMotion::HomeSearch : DriverMotion::Home;
            if (zero_search) {
                home_search_deadline_ = clock_.now() + kZeroSearchTimeout;  // only a search has a bound (#2432)
                home_search_seen_moving_ = false;
                home_search_resent_ = false;
                home_search_start_until_ = clock_.now() + kZeroSearchStartGrace;
                home_search_next_poll_ = clock_.now() + kZeroSearchPollInterval;
            }
            status_cache_valid_ = false;
            position_cache_valid_ = false;
        }
        // Issue #2432: the bound runs on its own slot, so the :Q# goes out at kZeroSearchTimeout whether or not a
        // client reads the status. Started with no driver lock held (decision 0006 rule 4).
        if (zero_search) {
            start_zero_search_watchdog();
        }
    }

    void park() override {
        StateLock lock(lock_gate_);
        check_connected();
        
        refresh_status_cache_locked();
        if (cached_status_.is_parked) {
            return;
        }

        const Wire protocol = wire();
        try {
            send_with_slew_hold_locked(std::chrono::seconds(5), [&] {
                if (!protocol.park(hae29c_quirks_active())) {
                    throw AlpacaException("Failed to park mount");
                }
            });
            clear_device_fault_locked();
        } catch (const std::exception& e) {
            throw_if_link_lost();
            record_device_fault_locked("Park", e.what());
            throw AlpacaException(std::string("Failed to park mount: ") + e.what(),
                                  AlpacaError::DriverException);
        }
        supersede_slew_dispatch_locked();
        driver_motion_ = DriverMotion::Park;
        park_override_until_ = std::chrono::steady_clock::time_point{};
        cached_status_.is_parked = false;
        cached_status_.is_slewing = true;
        cached_status_.is_at_home = false;
        status_cache_valid_ = true;
        last_status_update_ = clock_.now();
        position_cache_valid_ = false;
        // HAE29C firmware quirk (hardware-verified 2026-07-14): :MP1# park
        // slews complete physically but the mount keeps reporting
        // system-status 2 ("slewing") forever instead of 6 ("parked");
        // tracking-off (:ST0#) finalizes the transition. Arm the finalizer —
        // refresh_status_cache_locked() fires it once the mount is observed
        // stationary at the park target while still claiming to slew.
        park_finalize_pending_ = hae29c_quirks_active();
        park_target_valid_ = false;
        if (park_finalize_pending_) {
            try {
                park_target_ = protocol.get_park_position();
                park_target_valid_ = true;
            } catch (const std::exception&) {
                park_target_valid_ = false;
                throw_if_link_lost();  // :MP1# went out, but a pulled cable is reported in this call
            }
        }
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> handoff_lock(slew_handoff_mutex_);
        // open-astro#768: fence the async slew dispatch first, so no :MS1#
        // can reach the mount after the stop below. Runs without mutex_ held
        // (the dispatch body takes it), like every other reap site. Taking
        // the handoff lock can wait for a concurrent initiator's in-flight
        // dispatch transaction before the stop can be sent.
        reap_slew_dispatch();
        StateLock lock(lock_gate_);
        // open-astro#728: a latched fault must not stop a client stopping the
        // mount. Skip the latch check and the cached slewing flag, neither of
        // which can be trusted on a faulted link, and send the stop; a sent stop
        // clears the latch. InvalidWhileParked is not raised while latched.
        // Issue #2395 (AB-3): the stop goes out before the parked check, which
        // may read the status; InvalidWhileParked comes only from a read that
        // succeeded, so a failed read never keeps the stop off the wire.
        const bool faulted = connected_ && device_faulted_;
        if (!faulted) {
            check_connected();
        }
        // open-astro#575: AbortSlew is a valid clearing command for a stored
        // slew failure -- the client acted on the error, so the next Slewing
        // read must answer normally again. Clear unconditionally (a
        // soft-failed goto never set is_slewing).
        invalidate_slew_error_locked();
        // open-astro#768: the stop is sent even when the cached status says the
        // mount is not slewing; that cache can lag a GOTO the mount accepted.
        const Wire protocol = wire();
        try {
            protocol.stop_slewing();
            clear_device_fault_locked();
        } catch (const std::exception& e) {
            throw_if_link_lost();
            record_device_fault_locked("AbortSlew", e.what());
            throw AlpacaException(std::string("AbortSlew failed: ") + e.what(),
                                  AlpacaError::DriverException);
        }
        const bool parked = !faulted && parked_per_successful_read_locked();
        // Issue #2000, ITelescopeV4 AbortSlew: an equatorial slew ends with Tracking on, and an aborted MoveAxis
        // returns Tracking to its state before the motion. The firmware reports tracking off for a whole GOTO and
        // :Q# leaves it as it is, so a stopped GOTO would otherwise stay untracked. An Alt/Az slew needs Tracking
        // off (#1377) and Park and FindHome end without it, so those are left as the stop leaves them.
        std::optional<bool> restore_tracking;
        if (slew_in_progress_ && equatorial_slew_) {
            restore_tracking = true;
        } else if ((axis_move_active_primary_ || axis_move_active_secondary_) &&
                   tracking_state_before_move_.has_value()) {
            restore_tracking = tracking_state_before_move_;
        }
        slew_in_progress_ = false;
        equatorial_slew_ = false;
        note_unlanded_zero_search_locked();
        driver_motion_ = DriverMotion::None;
        axis_move_active_primary_ = false;
        axis_move_active_secondary_ = false;
        tracking_state_before_move_.reset();
        status_cache_valid_ = false;
        clear_slew_hold_locked();
        if (parked) {
            throw AlpacaException("AbortSlew is not allowed while parked", AlpacaError::InvalidWhileParked);
        }
        if (restore_tracking.has_value()) {
            try {
                if (*restore_tracking) {
                    protocol.start_tracking();
                } else {
                    protocol.stop_tracking();
                }
            } catch (const std::exception& e) {
                throw_if_link_lost();
                record_device_fault_locked("AbortSlew", e.what());
                throw AlpacaException(
                    std::string("AbortSlew stopped the mount but could not restore Tracking: ") + e.what(),
                    AlpacaError::DriverException);
            }
            // The mount is stopped: the cache that serves the read-back window must not keep the GOTO's status.
            cached_status_.is_tracking = *restore_tracking;
            cached_status_.is_slewing = false;
            cached_status_.system_status = *restore_tracking ? 1 : 0;
            status_cache_valid_ = true;
            last_status_update_ = clock_.now();
            tracking_override_until_ = last_status_update_ + std::chrono::seconds(2);
        }
    }
    
    void pulse_guide(int direction, int duration) override {
        // Argument validation precedes the connection and park checks (ASCOM).
        util::check_pulse_guide_arguments(kCommandLimits, direction, duration);
        StateLock lock(lock_gate_);
        util::check_pulse_guide_state(kCommandLimits, GateChecks{*this, "PulseGuide", /*fast_parked=*/true});
        // Issue #2063: a GOTO nobody polled is still flagged here, and so is a Park or FindHome (#2504). Ask the
        // mount before the pulse starts, while its status is still the motion's: the pulse itself makes the
        // firmware report slewing.
        const auto own_motion_running = [&] {
            return (slew_in_progress_ && !goto_seen_stopped_) || driver_motion_ != DriverMotion::None;
        };
        if (own_motion_running() && !slew_hold_.active(clock_.now())) {
            refresh_status_cache_locked(/*force=*/true);
            ensure_not_parked_fast_locked("PulseGuide");  // a Park that landed unpolled: the gate read the old cache
        }
        // Issue #2504: a pulse would change the GOTO, Park or FindHome the driver started.
        if (own_motion_running()) {
            throw AlpacaException("PulseGuide is not allowed while a slew, park or home is running",
                                  AlpacaError::InvalidOperation);
        }
        const bool is_dec = (direction == 0 || direction == 1);
        const bool is_ra = (direction == 2 || direction == 3);
        // Issue #2503: the pulse and a jog on its axis would drive one motor at once. The jog keeps running, and a
        // pulse on the other axis is still accepted.
        if (is_dec ? axis_move_active_secondary_ : axis_move_active_primary_) {
            throw AlpacaException("PulseGuide is not allowed on an axis MoveAxis is moving",
                                  AlpacaError::InvalidOperation);
        }

        const Wire protocol = wire();
        int calibrated_direction = direction;
        if (is_dec) {
            bool invert_dec = dec_guide_inverted_;
            if (should_flip_dec_for_pier_locked()) {
                invert_dec = !invert_dec;
            }
            if (invert_dec) {
                calibrated_direction = (direction == 0) ? 1 : 0;
            }
        }
        // Decision 0022 rule 2: every window below is armed before the pulse command goes out.
        const auto now = clock_.now();
        const auto pulse = std::chrono::milliseconds(duration);
        if (duration > 0 && is_dec) {
            if (last_ra_read_valid_) {
                pulse_guiding_hold_ra_hours_ = last_ra_read_hours_;
            } else {
                pulse_guiding_hold_ra_hours_ = cached_ra_hours_;
            }
            pulse_position_hold_[kRaAxis].arm(now, pulse + kPulseGuideCompletionDelay + kPulseGuideHoldGrace);
            pulse_position_hold_[kDecAxis].clear();
            GuideRate guide_rate;
            if (guide_rate_valid_) {
                guide_rate = cached_guide_rate_;
            } else {
                guide_rate.ra = kDefaultGuideRateFraction * kSiderealRateDegPerSec;
                guide_rate.dec = kDefaultGuideRateFraction * kSiderealRateDegPerSec;
            }
            double expected_delta_degrees = guide_rate.dec * (duration / 1000.0);
            if (direction == 1) {
                expected_delta_degrees = -expected_delta_degrees;
            }
            pulse_guiding_dec_expected_delta_degrees_ = expected_delta_degrees;
            if (last_dec_read_valid_) {
                pulse_guiding_dec_baseline_degrees_ = last_dec_read_degrees_;
            } else {
                pulse_guiding_dec_baseline_degrees_ = cached_dec_degrees_;
            }
            pulse_position_correction_[kDecAxis].arm(now,
                                                     pulse + kPulseGuideCompletionDelay + kPulseGuideCorrectionGrace);
        } else if (duration > 0 && is_ra) {
            if (last_dec_read_valid_) {
                pulse_guiding_hold_dec_degrees_ = last_dec_read_degrees_;
            } else {
                pulse_guiding_hold_dec_degrees_ = cached_dec_degrees_;
            }
            pulse_position_hold_[kDecAxis].arm(now, pulse + kPulseGuideCompletionDelay + kPulseGuideHoldGrace);
            pulse_position_hold_[kRaAxis].clear();
            GuideRate guide_rate;
            if (guide_rate_valid_) {
                guide_rate = cached_guide_rate_;
            } else {
                guide_rate.ra = kDefaultGuideRateFraction * kSiderealRateDegPerSec;
                guide_rate.dec = kDefaultGuideRateFraction * kSiderealRateDegPerSec;
            }
            double expected_delta_hours = (guide_rate.ra * (duration / 1000.0)) / 15.0;
            if (direction == 3) {
                expected_delta_hours = -expected_delta_hours;
            }
            pulse_guiding_ra_expected_delta_hours_ = expected_delta_hours;
            if (last_ra_read_valid_) {
                pulse_guiding_ra_baseline_hours_ = last_ra_read_hours_;
            } else {
                pulse_guiding_ra_baseline_hours_ = cached_ra_hours_;
            }
            pulse_position_correction_[kRaAxis].arm(now,
                                                    pulse + kPulseGuideCompletionDelay + kPulseGuideCorrectionGrace);
        } else {
            pulse_position_hold_[kRaAxis].clear();
            pulse_position_hold_[kDecAxis].clear();
        }
        // IsPulseGuiding reads this window lock-free, so it is true while the command is on the wire. A zero
        // duration sends a pulse that ends at once: the window closes.
        util::HoldWindow guiding;
        if (duration > 0) {
            guiding.arm(now, pulse + kPulseGuideCompletionDelay);
        }
        pulse_guide_hold_.store(guiding, std::memory_order_release);
        try {
            protocol.pulse_guide(calibrated_direction, duration);
        } catch (...) {
            // A failed dispatch (decision 0022 rule 2): no pulse runs, so no window stays open.
            clear_pulse_windows_locked();
            throw;
        }
    }
    
    void set_park() override {
        StateLock lock(lock_gate_);
        check_connected();
        
        // Get current position and set as park position
        const Wire protocol = wire();
        AltAz altaz = protocol.get_alt_az();
        protocol.set_park_position(altaz.altitude_degrees, altaz.azimuth_degrees);
        status_cache_valid_ = false;
        position_cache_valid_ = false;
    }
    
    void slew_to_coordinates(double ra, double dec) override {
        util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinates");
        std::uint64_t generation = 0;
        {
            StateLock lock(lock_gate_);
            double phys_ra = util::wrap_hours(ra - sync_offset_ra_hours_);
            double phys_dec = dec - sync_offset_dec_degrees_;
            util::check_equatorial_slew_state(kCommandLimits, GateChecks{*this, "SlewToCoordinates"});
            prepare_slew_state_locked(ra, dec, phys_ra, phys_dec, "SlewToCoordinates");
            equatorial_slew_ = true;
            dispatch_slew_command_locked(phys_ra, phys_dec, nullptr, "SlewToCoordinates");
            generation = slew_operation_generation_;
        }
        wait_for_slew_completion("SlewToCoordinates", generation);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinatesAsync");
        if (before_handoff_lock_) before_handoff_lock_();
        std::lock_guard<std::mutex> handoff_lock(slew_handoff_mutex_);
        double phys_ra = 0.0;
        double phys_dec = 0.0;
        std::uint64_t generation = 0;
        std::string previous_slew_error;
        {
            StateLock lock(lock_gate_);
            phys_ra = util::wrap_hours(ra - sync_offset_ra_hours_);
            phys_dec = dec - sync_offset_dec_degrees_;
            previous_slew_error = last_slew_error_;
            util::check_equatorial_slew_state(kCommandLimits, GateChecks{*this, "SlewToCoordinatesAsync"});
            prepare_slew_state_locked(ra, dec, phys_ra, phys_dec, "SlewToCoordinatesAsync");
            equatorial_slew_ = true;
            generation = slew_operation_generation_;
        }
        start_slew_body(generation, previous_slew_error, "Failed to start async slew",
                        [this, phys_ra, phys_dec, generation](util::OperationContext& ctx) {
                            run_slew_body(ctx, generation, "SlewToCoordinatesAsync", [&] {
                                dispatch_slew_command_locked(phys_ra, phys_dec, &ctx, "SlewToCoordinatesAsync");
                            });
                        });
    }
    
    void slew_to_target() override {
        double ra = 0.0;
        double dec = 0.0;
        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            check_connected();
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates(ra, dec);
    }
    
    void slew_to_target_async() override {
        double ra = 0.0;
        double dec = 0.0;
        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            check_connected();
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates_async(ra, dec);
    }
    
    void sync_to_coordinates(double ra, double dec) override {
        util::validate_ra_dec(ra, dec, "SyncToCoordinates");
        StateLock lock(lock_gate_);
        check_connected();
        ensure_not_parked_locked("SyncToCoordinates");

        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;

        refresh_position_cache_locked(true);
        sync_offset_ra_hours_ = ra - cached_ra_hours_;
        sync_offset_dec_degrees_ = dec - cached_dec_degrees_;
        if (sync_offset_ra_hours_ > 12.0) sync_offset_ra_hours_ -= 24.0;
        if (sync_offset_ra_hours_ < -12.0) sync_offset_ra_hours_ += 24.0;
    }
    
    void sync_to_target() override {
        StateLock lock(lock_gate_);
        check_connected();
        if (!target_ra_set_ || !target_dec_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        ensure_not_parked_locked("SyncToTarget");

        refresh_position_cache_locked(true);
        sync_offset_ra_hours_ = target_ra_hours_ - cached_ra_hours_;
        sync_offset_dec_degrees_ = target_dec_degrees_ - cached_dec_degrees_;
        if (sync_offset_ra_hours_ > 12.0) sync_offset_ra_hours_ -= 24.0;
        if (sync_offset_ra_hours_ < -12.0) sync_offset_ra_hours_ += 24.0;
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        validate_alt_az(altitude, azimuth, "SlewToAltAzAsync");
        if (before_handoff_lock_) before_handoff_lock_();
        std::lock_guard<std::mutex> handoff_lock(slew_handoff_mutex_);
        std::uint64_t generation = 0;
        std::string previous_slew_error;
        {
            StateLock lock(lock_gate_);
            check_connected();
            if (!site_info_valid_) {
                ensure_site_info_cached_locked();
                // ensure_site_info_cached_locked() sets site_info_valid_ on success;
                // this re-check detects a failed cache populate, not a dead condition.
                // cppcheck-suppress identicalInnerCondition
                if (!site_info_valid_) {
                    throw AlpacaException("Site information unavailable for Alt/Az slew",
                                          AlpacaError::ValueNotSet);
                }
            }
            ensure_not_parked_locked("SlewToAltAzAsync");
            refuse_alt_az_slew_while_tracking_locked("SlewToAltAzAsync");
            previous_slew_error = last_slew_error_;
            supersede_slew_dispatch_locked();
            generation = slew_operation_generation_;
            slew_in_progress_ = true;
            goto_seen_stopped_ = false;
            // Same reason as prepare_slew_state_locked(): Slewing must read
            // true from the moment this initiator returns, not from when the
            // dispatch body gets the mutex.
            slew_hold_.arm(clock_.now(), std::chrono::seconds(5));
        }

        start_slew_body(generation, previous_slew_error, "Failed to start async Alt/Az slew",
                        [this, altitude, azimuth, generation](util::OperationContext& ctx) {
                            run_slew_body(ctx, generation, "SlewToAltAzAsync", [&] {
                                auto converted = alt_az_to_ra_dec(altitude, azimuth, site_latitude_cached_,
                                                                  site_longitude_cached_, current_utc_time_locked());
                                double ra_hours = converted.first;
                                double dec_degrees = converted.second;
                                // The initiator already claimed and published this generation.
                                prepare_slew_state_locked(ra_hours, dec_degrees, ra_hours, dec_degrees,
                                                          "SlewToAltAzAsync", false);
                                dispatch_slew_command_locked(ra_hours, dec_degrees, &ctx, "SlewToAltAzAsync");
                            });
                        });
    }

    void slew_to_alt_az(double altitude, double azimuth) override {
        double ra_hours = 0.0;
        double dec_degrees = 0.0;
        std::uint64_t generation = 0;
        validate_alt_az(altitude, azimuth, "SlewToAltAz");
        {
            StateLock lock(lock_gate_);
            check_connected();
            ensure_site_info_cached_locked();
            if (!site_info_valid_) {
                throw AlpacaException("Site information unavailable for Alt/Az slew",
                                      AlpacaError::ValueNotSet);
            }
            ensure_not_parked_locked("SlewToAltAz");
            refuse_alt_az_slew_while_tracking_locked("SlewToAltAz");
            auto converted = alt_az_to_ra_dec(
                altitude, azimuth, site_latitude_cached_, site_longitude_cached_, current_utc_time_locked());
            ra_hours = converted.first;
            dec_degrees = converted.second;
            start_slew_to_coordinates_locked(ra_hours, dec_degrees);
            generation = slew_operation_generation_;
        }
        wait_for_slew_completion("SlewToAltAz", generation);
    }
    
    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw AlpacaException(
            "Alt/Az sync not supported by iOptron driver",
            AlpacaError::MethodNotImplemented
        );
    }
    
    void unpark() override {
        StateLock lock(lock_gate_);
        check_connected();

        const Wire protocol = wire();
        try {
            send_with_slew_hold_locked(std::chrono::seconds(2), [&] { protocol.unpark(); });
            clear_device_fault_locked();
        } catch (const std::exception& e) {
            throw_if_link_lost();
            record_device_fault_locked("Unpark", e.what());
            throw AlpacaException(std::string("Failed to unpark mount: ") + e.what(),
                                  AlpacaError::DriverException);
        }
        supersede_slew_dispatch_locked();
        cached_status_.is_parked = false;
        cached_status_.is_slewing = true;
        cached_status_.is_at_home = false;
        status_cache_valid_ = true;
        last_status_update_ = clock_.now();
        park_override_until_ = last_status_update_ + kUnparkGrace;
        park_finalize_pending_ = false;
    }

private:
    static constexpr std::chrono::seconds kSiteInfoCacheTtl{60};
    static constexpr std::chrono::milliseconds kStatusCacheTtl{1000};
    static constexpr std::chrono::milliseconds kPositionCacheTtl{1000};
    static constexpr std::chrono::seconds kSideOfPierPositionTtl{5};
    static constexpr std::chrono::milliseconds kSlewPollInterval{250};
    static constexpr std::chrono::seconds kSlewTimeout{300};
    static constexpr std::chrono::seconds kFastCacheGrace{30};
    static constexpr std::chrono::seconds kUnparkGrace{120};
    // How long past its duration a pulse is still reported (IsPulseGuiding, the #843 Slewing mask and the position
    // holds). Measured on the HAE29C (issue #2001, 2026-11-03): the firmware's guiding status (:GLS# system status 3)
    // ends 70 to 380 ms after the duration (300 ms pulses at 440-680 ms, 1 s at 1100-1280 ms, 3 s at 3350 ms). The
    // 1 s delay before it cost a guider about 0.6 s per correction.
    static constexpr std::chrono::milliseconds kPulseGuideCompletionDelay{500};
    static constexpr std::chrono::milliseconds kPulseGuideHoldGrace{2000};
    static constexpr std::chrono::milliseconds kPulseGuideCorrectionGrace{5000};
    static constexpr double kSlewCompletionToleranceArcsec = 60.0;
    // Issue #2396: a synchronous GOTO whose position settles farther than this from the target failed.
    static constexpr double kSlewFarMissArcsec = 600.0;
    static constexpr double kSiderealSeconds = 86164.0905;
    static constexpr double kSiderealRateDegPerSec = 360.0 / kSiderealSeconds;
    static constexpr double kDefaultGuideRateFraction = 0.5;

    // The HAE29C park firmware-quirk workarounds (wedged-park finalizer,
    // zero-distance park finalizer) are gated on the exact model code so
    // other iOptron mounts (HAE43, HEM27, ...) keep the original,
    // hardware-validated behavior.
    bool hae29c_quirks_active() const { return mount_info_.model_code == "0036"; }

    // Issue #2420: FindHome runs the :MSH# zero sensor search. Hardware-verified on the HAE29C EQ (0036, RC51,
    // 2026-11-19); other models (HAE29C EC/AA 0037-0039 and the CEM models the v3.10 spec lists included) keep
    // :MH# until a rig log verifies the search on them.
    bool zero_search_active() const { return mount_info_.model_code == "0036"; }

    // GOTO final-approach RA refinement. Hardware-verified on HAE29C EQ
    // (0036, 2026-07-14) and HAE16 EQ (0012, ConformU 2026-08-25: same
    // 11-16 arcsec east RA settle, Dec within 0.2 arcsec, park clean).
    bool goto_refine_active() const { return mount_info_.model_code == "0036" || mount_info_.model_code == "0012"; }

    // Whether a call may try the mount again while a fault is latched (decision 0009 rule 1). Read: the getters that
    // already read position, Alt/Az or status from the mount. Every other call, writes included, only refuses.
    enum class LatchProbe : std::uint8_t { Refuse, Read };

    // At most one probe per this interval while latched, on the task clock: a probe that times out holds io_mutex_
    // (Wire drops mutex_ for the read) for the read timeout (5 s by default), so a client polling at 1 Hz is not
    // blocked on every call behind the operation lock, and a mount that
    // answers again is found within 5 s of polling, five probes before util::kLinkStalenessBound loses the link.
    static constexpr std::chrono::seconds kFaultProbeInterval{5};

    // NotConnected while disconnected and once the link is lost (decision 0009 rule 3), checked before any cache;
    // DriverException while a fault is latched (rule 1), which past util::kLinkStalenessBound loses the link (rule 2).
    // While latched, a LatchProbe::Read call once per kFaultProbeInterval sends one :GLS# status read instead of
    // refusing: a good reply clears the latch (note_device_read_ok_locked()) and the call goes on; a failed one keeps
    // it and throws DriverException; a hangup over serial is a lost link (NotConnected). Between probes it refuses
    // with nothing sent. The probe and the staleness loss are exchanges (the loss takes the wrapper's exchange
    // mutex), so only a caller that holds the operation lock runs them; a Mode::State member refuses with the
    // latched text and leaves the loss to the next Mode::Io call, as IsPulseGuiding does (decision 0016).
    void check_connected(LatchProbe probe = LatchProbe::Refuse) const {
        if (!connected_) {
            throw AlpacaException("Not connected to mount", AlpacaError::NotConnected);
        }
        throw_if_link_lost();
        if (device_faulted_) {
            if (lock_gate_.holds_io()) {
                lose_link_if_fault_stale();
                if (probe == LatchProbe::Read && clock_.now() >= next_fault_probe_) {
                    next_fault_probe_ = clock_.now() + kFaultProbeInterval;
                    refresh_status_cache_locked(true);
                    return;
                }
            }
            throw AlpacaException("Mount communications compromised: " + last_device_error_,
                                  AlpacaError::DriverException);
        }
    }

    // A getter that may reach the mount checks the lock-free flag before it takes the operation lock, so a
    // read that lands while a connect holds that lock answers NotConnected at once instead of waiting for the
    // handshake (decision 0016); check_connected() under the lock then closes the disconnect race.
    void check_connected_before_lock() const {
        if (!connected_) {
            throw AlpacaException("Not connected to mount", AlpacaError::NotConnected);
        }
    }

    // NotConnected while disconnected or connecting and once the link is lost (decision 0009 rule 3), with no lock
    // and no fault refusal: for the members that serve the client's own values (the optics, SlewSettleTime).
    void check_connected_lock_free() const {
        check_connected_before_lock();
        throw_if_link_lost();
    }

    // Signalled by a refill helper that, under the state lock alone, finds the cache it would serve was never
    // primed or was invalidated: serve_cached_or_refill() runs the body again under the operation lock. Not an
    // std::exception on purpose, so no catch on the way back mistakes it for a failed exchange.
    struct RefillNeedsOperationLock {};

    // Decision 0016 rule 1 (#1829): a getter served from a cache takes the state lock alone and answers when its
    // cache is fresh (the common case: every cache is primed at connect and refilled on a TTL), so it never waits
    // behind a command, the clock-sync body or another getter's refill on the wire. Only when the cache has
    // lapsed, or a fault is latched (the probe is an exchange), does it take the operation lock and run `body`
    // under both locks, where the refill reaches the mount. `fresh` and `body` run under mutex_; `fresh` true
    // must mean `body` reaches no wire: the refill helpers return early on the same predicates. The check and
    // the refill read the clock separately, so a TTL, hold window or override can lapse between the two (a 1 Hz
    // poller lands on the TTL edge). A refill reached that way serves a primed cache as it stands, and when the
    // cache it would serve was never primed or was invalidated it signals RefillNeedsOperationLock instead of
    // reaching the wire: the body is then run again under both locks (serve_as_is_locked()). A body signals
    // before it writes any state, because the second run repeats everything before the signal: Declination and
    // RightAscension write their last_*_read_ after the refill, Slewing counts a refine after its signal.
    template <typename Fresh, typename Body>
    std::invoke_result_t<Body&> serve_cached_or_refill(Fresh&& fresh, Body&& body) const {
        check_connected_before_lock();
        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            if (!connected_) {
                throw AlpacaException("Not connected to mount", AlpacaError::NotConnected);
            }
            throw_if_link_lost();
            if (!device_faulted_ && fresh()) {
                try {
                    return body();
                } catch (const RefillNeedsOperationLock&) {  // NOLINT(bugprone-empty-catch): the body reruns below
                    // Lapsed between `fresh` and the body's own check, on a read that needs the exchange: the
                    // body runs again below, under the operation lock.
                }
            }
        }
        StateLock lock(lock_gate_);
        check_connected(LatchProbe::Read);
        return body();
    }

    // Whether a cache can be served without a refill: primed and within the post-connect grace or its TTL. The
    // refill helpers below return early on the same predicate.
    //
    // A refill under the state lock alone comes from serve_cached_or_refill(), whose freshness check passed a
    // moment earlier on its own clock read; the TTL, hold window or override lapsed in between. A primed cache
    // is served as it stands (true) and the next read refills it under the operation lock. A cache that was
    // never primed or was invalidated (a slew, Park, FindHome or SetPark drops it, and a hold window can close on
    // it) needs the exchange, so the refill signals the template to run the body again under both locks. Under
    // the operation lock the refill goes ahead (false).
    bool serve_as_is_locked(bool cache_valid) const {
        if (lock_gate_.holds_io()) {
            return false;
        }
        if (cache_valid) {
            return true;
        }
        throw RefillNeedsOperationLock{};
    }
    bool position_cache_fresh_locked() const {
        const auto now = clock_.now();
        return position_cache_valid_ && (now < fast_cache_until_ || (now - last_position_update_) < kPositionCacheTtl);
    }
    bool altaz_cache_fresh_locked() const {
        const auto now = clock_.now();
        return altaz_cache_valid_ && (now < fast_cache_until_ || (now - last_altaz_update_) < kPositionCacheTtl);
    }
    bool status_cache_fresh_locked() const {
        const auto now = clock_.now();
        return status_cache_valid_ && (now < fast_cache_until_ || (now - last_status_update_) < kStatusCacheTtl);
    }
    bool site_info_fresh_locked() const {
        const auto now = clock_.now();
        return site_info_valid_ && (now < fast_cache_until_ || (now - last_site_info_fetch_) < kSiteInfoCacheTtl);
    }

    // Decision 0009 rule 2a: the wrapper saw the serial port gone (or lose_link() ran) and released it. Lock-free.
    static bool link_lost() { return !iOptronProtocolWrapper::instance().link_alive(); }

    // A lost link is NotConnected with the loss reason: never a DriverException that counts toward the fault latch,
    // and never a value read before the loss. Called first in every catch that turns a failed exchange into a
    // DriverException or into a value or "slew done" served instead (the wedged-park finalizer included, whose
    // caller serves the status it just read). The catches that only skip optional work (the GOTO trim's guide-rate
    // fallback, the calibrations, the clock-sync retries, the connect's reads, which the connect follows with its own
    // loss check) and the slew body, which records its failed dispatch, leave a loss to the next call through
    // check_connected(); Connected reads false at once either way.
    static void throw_if_link_lost() {
        const auto& link = iOptronProtocolWrapper::instance();  // link_alive() and link_fault() do no I/O
        if (link.link_alive()) {
            return;
        }
        const std::string reason = link.link_fault();
        throw AlpacaException(
            reason.empty() ? std::string("Mount link lost; Connect again once the mount is back") : reason,
            AlpacaError::NotConnected);
    }

    // Decision 0009 rule 2b: a latched fault that has stood past util::kLinkStalenessBound on the task clock is a lost
    // link, found by the next call through check_connected() (no poll). The wrapper releases the port and keeps the
    // reason (rules 3 and 5), then this throws NotConnected. The wrapper call is an exchange for the gate (it
    // takes the wrapper's exchange mutex), so it runs under io_mutex_ with mutex_ released; link_fault_mutex_ is
    // a leaf released before it.
    void lose_link_if_fault_stale() const {
        const auto since = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(device_fault_since_ticks_.load()));
        if (!device_faulted_ || clock_.now() - since < util::kLinkStalenessBound) {
            return;
        }
        wire().lose_link("Mount fault stood for " + std::to_string(util::kLinkStalenessBound.count()) +
                         " s with no good reply: " + published_fault_text());
        throw_if_link_lost();
    }

    std::string published_fault_text() const {
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        return link_fault_text_;
    }

    // Issue #2395 (AB-3): true only when a status that was read says parked (the cache, or a fresh read). A failed
    // read is not "parked": AbortSlew, which runs this after its stop, then returns normally. A lost link throws.
    bool parked_per_successful_read_locked() const {
        if (park_override_until_ > clock_.now()) {
            return false;
        }
        if (status_cache_valid_ && !cached_status_.is_parked) {
            return false;
        }
        try {
            refresh_status_cache_locked();
        } catch (const std::exception&) {
            throw_if_link_lost();
            return false;
        }
        return cached_status_.is_parked;
    }

    void ensure_not_parked_locked(const char* action) const {
        auto now = clock_.now();
        if (park_override_until_ > now) {
            park_override_until_ = now + kUnparkGrace;
            return;
        }
        if (status_cache_valid_ && !cached_status_.is_parked) {
            return;
        }
        refresh_status_cache_locked();
        if (cached_status_.is_parked) {
            throw AlpacaException(std::string(action) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    void ensure_not_parked_fast_locked(const char* action) const {
        auto now = clock_.now();
        if (park_override_until_ > now) {
            park_override_until_ = now + kUnparkGrace;
            return;
        }
        if (status_cache_valid_) {
            if (cached_status_.is_parked) {
                throw AlpacaException(std::string(action) + " is not allowed while parked",
                                      AlpacaError::InvalidWhileParked);
            }
            return;
        }
        if (cached_status_.is_parked) {
            throw AlpacaException(std::string(action) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    // A single transient timeout must not brick the whole session: only latch
    // device_faulted_ (which makes every call through check_connected() throw)
    // after several CONSECUTIVE failures. A successful read resets the count
    // via note_device_read_ok_locked().
    static constexpr int kDeviceFaultThreshold = 3;

    void record_device_fault_locked(const char* context, const std::string& detail) const {
        ++device_fault_count_;
        last_device_error_ = std::string(context) + ": " + detail;
        if (device_fault_count_ >= kDeviceFaultThreshold) {
            if (!device_faulted_) {
                // The staleness bound runs from the latch (decision 0009 rule 2b), on the task clock; the first
                // probe one interval after it, since the read that latched has just failed.
                device_fault_since_ticks_.store(clock_.now().time_since_epoch().count());
                next_fault_probe_ = clock_.now() + kFaultProbeInterval;
            }
            device_faulted_ = true;
            ALPACA_LOG_ERROR("iOptron", "Device fault (latched after " + std::to_string(device_fault_count_) +
                                            " consecutive failures) - " + last_device_error_);
        } else {
            ALPACA_LOG_WARN("iOptron", "Device fault (transient, " + std::to_string(device_fault_count_) + "/" +
                                           std::to_string(kDeviceFaultThreshold) + ") - " + last_device_error_);
        }
        publish_link_fault_locked();
    }

    void clear_device_fault_locked() const {
        device_fault_count_ = 0;
        device_faulted_ = false;
        last_device_error_.clear();
        publish_link_fault_locked();
    }

    // Copies the latched fault text to the narrow-mutex copy get_link_fault() reads.
    // Call under mutex_ after every change to device_faulted_ or last_device_error_.
    void publish_link_fault_locked() const {
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        link_fault_text_ = device_faulted_ ? last_device_error_ : std::string{};
    }

    // open-astro#728: a successful read ends the run of failures, so only
    // consecutive failures reach the threshold. A good reply also clears a
    // latch that has tripped (decision 0009 rule 1): while latched it comes
    // from a check_connected() probe. AbortSlew's sent stop and a reconnect
    // clear it too (Park and Unpark also clear it on success, but
    // check_connected() refuses them while it is set).
    void note_device_read_ok_locked() const {
        if (device_faulted_) {
            ALPACA_LOG_INFO("iOptron", "Device fault cleared: the mount answered again");
            clear_device_fault_locked();
            return;
        }
        device_fault_count_ = 0;
    }

    // `gut_reply`: the connect's :GUT# reply, for the site's timezone and DST; null when that read failed, and
    // the site read then sends its own :GUT# as before (#1784).
    void prefetch_mount_state_locked(const std::string* gut_reply) {
        const Wire protocol = wire();
        auto now = clock_.now();
        try {
            Position pos = protocol.get_position();
            cached_dec_degrees_ = pos.dec_degrees;
            cached_ra_hours_ = pos.ra_hours;
            cached_side_of_pier_ = pos.side_of_pier;
            position_cache_valid_ = true;
            last_position_update_ = now;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Failed to prefetch position cache: " + std::string(e.what()));
        }
        try {
            AltAz altaz = protocol.get_alt_az();
            cached_alt_degrees_ = altaz.altitude_degrees;
            cached_az_degrees_ = altaz.azimuth_degrees;
            altaz_cache_valid_ = true;
            last_altaz_update_ = now;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Failed to prefetch Alt/Az cache: " + std::string(e.what()));
        }
        try {
            cached_status_ = protocol.get_status();
            status_cache_valid_ = true;
            last_status_update_ = now;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Failed to prefetch status cache: " + std::string(e.what()));
        }
        try {
            SiteInfo site = gut_reply ? protocol.get_site_info(*gut_reply) : protocol.get_site_info();
            site_longitude_cached_ = site.longitude_degrees;
            site_latitude_cached_ = site.latitude_degrees;
            hemisphere_north_ = site.is_northern_hemisphere;
            timezone_offset_minutes_ = site.timezone_offset_minutes;
            timezone_offset_valid_ = true;
            dst_observed_ = site.dst_observed;
            site_info_valid_ = true;
            last_site_info_fetch_ = now;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Failed to prefetch site info cache: " + std::string(e.what()));
        }
    }

    bool get_tracking_locked() const {
        auto now = clock_.now();
        if (tracking_override_until_ > now && status_cache_valid_) {
            return cached_status_.is_tracking;
        }
        refresh_status_cache_locked();
        return cached_status_.is_tracking;
    }

    // ITelescopeV4: SlewToAltAz and SlewToAltAzAsync throw InvalidOperation while Tracking is true (#1377), since a
    // tracking mount drifts off the requested Alt/Az. Reads the same value as get_tracking(), from the status cache
    // under mutex_: the same TTL-gated status read the equatorial slews' guard makes (it may send one `:GLS#`
    // when the cache has expired; the parked check does not refresh a valid, unparked cache), or the tracking
    // write's read-back window. Runs after the argument, connected and parked checks and before anything is sent or
    // superseded.
    void refuse_alt_az_slew_while_tracking_locked(const char* label) const {
        if (get_tracking_locked()) {
            throw AlpacaException(std::string(label) + " is not allowed while Tracking is true",
                                  AlpacaError::InvalidOperation);
        }
    }

    // The command gate's state checks in iOptron's words (#2258): util/telescope_command_gate.h decides the order,
    // these answer each step with the driver's own texts and reads. check_connected() also refuses a latched fault,
    // so the gate's fault step has nothing left to check. PulseGuide reads the parked flag from the cache
    // (`fast_parked`); the other families may refresh it from the mount.
    struct GateChecks {
        const iOptronTelescopeDriver& driver;
        const char* who = nullptr;
        bool fast_parked = false;
        void check_connected() const { driver.check_connected(); }
        void check_not_parked(bool /*stop*/) const {
            fast_parked ? driver.ensure_not_parked_fast_locked(who) : driver.ensure_not_parked_locked(who);
        }
        void check_no_fault() const {}
        // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation
        // (open-astro#1274). Runs after the parked check and before the new slew
        // supersedes a dispatch in flight, so a refusal leaves that motion alone.
        // Not part of prepare_slew_state_locked(): the Alt/Az slews share it.
        void check_tracking() const {
            if (driver.tracking_off_for_slew_locked()) {
                throw AlpacaException(std::string(who) + " requires Tracking to be true",
                                      AlpacaError::InvalidOperation);
            }
        }
    };

    // Not get_tracking_locked(): :GLS reports slewing (2) instead of tracking,
    // so the getter reads a GOTO as off, and refusing there would refuse a
    // retarget of a GOTO in flight (guiding (3) and a meridian flip (4) read
    // as on since #2186). Off is a commanded Tracking=false still in its
    // read-back window, or a stopped mount (0, or 7 at home); parked (6) is
    // refused before this runs.
    bool tracking_off_for_slew_locked() const {
        if (tracking_override_until_ > clock_.now() && status_cache_valid_) {
            return !cached_status_.is_tracking;
        }
        refresh_status_cache_locked();
        return cached_status_.system_status == 0 || cached_status_.system_status == 7;
    }

    void prepare_slew_state_locked(double ascom_ra, double ascom_dec, double phys_ra, double phys_dec,
                                   const char* label, bool supersede = true) {
        check_connected();
        ensure_not_parked_locked(label);
        if (supersede) supersede_slew_dispatch_locked();

        target_ra_hours_ = ascom_ra;
        target_dec_degrees_ = ascom_dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
        slew_in_progress_ = true;
        goto_seen_stopped_ = false;
        slew_target_ra_hours_ = phys_ra;
        slew_target_dec_degrees_ = phys_dec;
        slew_refine_count_ = 0;
        // Arm the post-dispatch Slewing override HERE, not only after the
        // mount accepts the GOTO: the async initiators hand the wire commands
        // to a thread, and a Slewing poll that wins the mutex before that
        // thread has sent :MS1 would otherwise see a not-slewing mount, run
        // the settle refinement against the pre-slew position and report the
        // slew complete before it started (caught by the loopback fake-mount
        // test). Every dispatch-failure path clears it again.
        slew_hold_.arm(clock_.now(), std::chrono::seconds(5));
    }

    void invalidate_slew_error_locked() {
        ++slew_operation_generation_;
        last_slew_error_.clear();
    }

    void supersede_slew_dispatch_locked() {
        // Clear an unsent GOTO's state before a stale worker observes its
        // generation mismatch, preventing a ghost slew/trim after newer motion.
        // The slew window is left to the caller: every initiator but MoveAxis arms its own, and MoveAxis clears it.
        invalidate_slew_error_locked();
        slew_in_progress_ = false;
        equatorial_slew_ = false;
        note_unlanded_zero_search_locked();
        driver_motion_ = DriverMotion::None;
    }

    void clear_slew_hold_locked() const { slew_hold_.clear(); }

    // Decision 0022 rule 2: arms the slew window for `hold` before `send` puts the command on the wire. A send that
    // throws takes its window back (the one before it is restored), so a command that failed leaves Slewing as it was.
    template <typename Send>
    void send_with_slew_hold_locked(std::chrono::milliseconds hold, Send&& send) const {
        const util::HoldWindow before = slew_hold_;
        slew_hold_.arm(clock_.now(), hold);
        try {
            send();
        } catch (...) {
            slew_hold_ = before;
            throw;
        }
    }

    // Decision 0022 rule 2: a connect and a disconnect start with no window open.
    void clear_hold_windows_locked() const {
        clear_slew_hold_locked();
        clear_pulse_windows_locked();
    }

    // Every pulse window: IsPulseGuiding's, and the position holds and predictions of PulseGuide.
    void clear_pulse_windows_locked() const {
        pulse_guide_hold_.store(util::HoldWindow{}, std::memory_order_release);
        for (auto& window : pulse_position_hold_) window.clear();
        for (auto& window : pulse_position_correction_) window.clear();
    }

    // `async_ctx` is the slew slot's context when the GOTO runs on its body: a rejection is then recorded in
    // last_slew_error_ and logged instead of thrown. Null: the synchronous initiators, which throw.
    // Issue #2395: a :MS1# the mount refuses is an error, never re-sent as the counterweight-up :MS2#: the firmware
    // is the only layer that knows the mount's limits. Issue #2396: a :MS1# with no reply or an unknown one is
    // accepted only when a status read right after it shows the mount slewing, and the read before it did not;
    // otherwise the mount is sent a stop (a GOTO that did arrive is stopped) and the slew fails.
    void dispatch_slew_command_locked(double ra, double dec, util::OperationContext* async_ctx, const char* label) {
        const Wire protocol = wire();
        GotoReply reply = GotoReply::Refused;
        bool moving_before = true;
        try {
            moving_before = mount_moving_before_goto_locked();
            protocol.set_target_ra(ra);
            protocol.set_target_dec(dec);
            // Decision 0022 rule 2: re-armed before the :MS1# goes out, so the window runs from the GOTO itself
            // and not from an initiator whose body waited for mutex_.
            slew_hold_.arm(clock_.now(), std::chrono::seconds(5));
            goto_seen_stopped_ = false;  // a stop seen while the body waited predates this GOTO
            reply = protocol.slew_to_ra_dec();
        } catch (...) {
            // A dispatch that failed (the synchronous initiators throw it on): no GOTO went out.
            slew_in_progress_ = false;
            clear_slew_hold_locked();
            throw;
        }
        std::string failure;
        if (reply == GotoReply::Refused) {
            failure = std::string(label) + " failed: mount rejected the target";
        } else if (reply != GotoReply::Accepted) {
            bool stop_sent = false;
            bool confirmed = false;
            try {
                confirmed = unanswered_goto_confirmed_locked(moving_before, stop_sent);
            } catch (...) {
                slew_in_progress_ = false;  // a link lost during the confirmation
                clear_slew_hold_locked();
                throw;
            }
            if (!confirmed) {
                failure = std::string(label) + " failed: the mount gave " +
                          (reply == GotoReply::NoReply ? "no reply" : "an unknown reply") +
                          " to the GOTO (:MS1#) and its status does not confirm the slew; " +
                          (stop_sent ? "the mount was sent a stop" : "the stop could not be sent");
            }
        }
        if (!failure.empty()) {
            slew_in_progress_ = false;
            clear_slew_hold_locked();
            if (async_ctx != nullptr) {
                // open-astro#575: a reap by a newer initiator is not a failure -- that
                // initiator already owns clearing/replacing last_slew_error_.
                // AbortSlew cancels and joins the slot (open-astro#768), so none can
                // record a failure after the abort cleared the error.
                if (async_ctx->stop_reason() == util::StopReason::None) {
                    last_slew_error_ = failure;
                }
                // Decision 0007 rule 9: a refusal answers the client's own target, so it is INFO; a missing or
                // unknown reply comes from the link and stays WARN.
                if (reply == GotoReply::Refused) {
                    ALPACA_LOG_INFO("iOptron", failure);
                } else {
                    ALPACA_LOG_WARN("iOptron", failure);
                }
                return;
            }
            if (reply == GotoReply::Refused) {
                throw AlpacaException("Slew rejected by mount - target may violate altitude limits");
            }
            throw AlpacaException(failure);
        }
        status_cache_valid_ = false;
        position_cache_valid_ = false;
    }

    // Issue #2396: the status read before the :MS1#. Motion already running that reads 2 (a GOTO, a Park, or a pulse
    // on firmware that reports one as slewing, open-astro#843) or 4 (a meridian flip) would make the read after an
    // unanswered :MS1# look like confirmation, so it is recorded here. A pulse that reads 3 (the HAE29C, #2186)
    // confirms nothing either way. A failed read counts as moving: it confirms nothing, so an unanswered GOTO after
    // it is stopped. A lost link throws NotConnected.
    bool mount_moving_before_goto_locked() const {
        try {
            const MountStatus status = wire().get_status();
            return status.system_status == 2 || status.system_status == 4;
        } catch (const std::exception& e) {
            throw_if_link_lost();
            ALPACA_LOG_WARN("iOptron", std::string("Status read before a GOTO failed, so a GOTO without a clear :MS1# "
                                                   "reply will not be confirmed and the mount will be sent a stop: ") +
                                           e.what());
            return true;
        }
    }

    // Issue #2396: a :MS1# that got no reply, or an unknown one, may still have reached the mount. One status read
    // decides: slewing (2) or a meridian flip (4) is a GOTO the mount runs, unless the mount was already moving
    // before the :MS1# (@p moving_before), when the 2 or 4 may be that older motion. Anything else, a failed read
    // included, is not acceptance, and the mount is sent :Q#, so a GOTO that arrives or starts late is stopped,
    // and so is the older motion. A lost link throws NotConnected.
    bool unanswered_goto_confirmed_locked(bool moving_before, bool& stop_sent) {
        const Wire protocol = wire();
        try {
            const MountStatus status = protocol.get_status();
            if (moving_before && (status.system_status == 2 || status.system_status == 4)) {
                ALPACA_LOG_WARN("iOptron", "GOTO with no clear :MS1# reply not confirmed: the mount reports status " +
                                               std::to_string(status.system_status) +
                                               ", but it was already moving before the GOTO");
            } else if (status.system_status == 2 || status.system_status == 4) {
                ALPACA_LOG_WARN("iOptron", "GOTO with no clear :MS1# reply confirmed: the mount reports status " +
                                               std::to_string(status.system_status));
                return true;
            }
        } catch (const std::exception& e) {
            throw_if_link_lost();
            ALPACA_LOG_WARN("iOptron", std::string("Status read after an unanswered :MS1# failed: ") + e.what());
        }
        try {
            protocol.stop_slewing();
            stop_sent = true;
        } catch (const std::exception& e) {
            throw_if_link_lost();
            ALPACA_LOG_WARN("iOptron", std::string("Stop after an unanswered :MS1# failed: ") + e.what());
        }
        status_cache_valid_ = false;
        return false;
    }

    void start_slew_to_coordinates_locked(double ra, double dec) {
        prepare_slew_state_locked(ra, dec, ra, dec, "SlewToCoordinates");
        dispatch_slew_command_locked(ra, dec, nullptr, "SlewToCoordinates");
    }

    // Issue #2396: a synchronous GOTO that fails after its dispatch (the 300 s bound, a far miss, failed position
    // reads) ends the slew state, hold included, and sends :Q# (best effort), as an unconfirmed GOTO does, so a
    // mount still moving is stopped. A stop that cannot be sent is logged and named in the error. @p generation is
    // slew_operation_generation_ after this GOTO's dispatch: when a newer motion (another client's GOTO, a MoveAxis,
    // Park, AbortSlew or a reconnect) has bumped it, the mount is that motion's, and neither the stop nor the state
    // clear runs.
    [[noreturn]] void fail_synchronous_slew(std::string failure, std::uint64_t generation) {
        StateLock lock(lock_gate_);
        if (generation != slew_operation_generation_) {
            throw AlpacaException(failure + "; a newer motion owns the mount, so no stop was sent");
        }
        slew_in_progress_ = false;
        clear_slew_hold_locked();
        try {
            wire().stop_slewing();
            failure += "; the mount was sent a stop";
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Stop after a failed synchronous slew failed: " + std::string(e.what()));
            failure += "; the stop could not be sent, so the mount may still be moving";
        }
        status_cache_valid_ = false;
        position_cache_valid_ = false;
        throw AlpacaException(failure);
    }

    void wait_for_slew_completion(const char* label, std::uint64_t generation) {
        const auto deadline = clock_.now() + kSlewTimeout;
        while (get_slewing()) {
            if (clock_.now() > deadline) {
                fail_synchronous_slew(std::string(label) + " timed out waiting for slew to complete", generation);
            }
            clock_.sleep_for(kSlewPollInterval);
        }

        // The mount may clear its slewing status well before the physical
        // slew finishes (observed on WiFi: GLS→0 while mount still moving
        // tens of degrees).  Wait for position readings to stabilize — two
        // consecutive reads within tolerance of each other — AND target
        // reached.  Uses the slew deadline, not a fixed iteration cap.
        bool has_target = false;
        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            has_target = target_ra_set_ && target_dec_set_;
        }
        if (has_target) {
            static constexpr double kStableThresholdArcsec = 30.0;
            static constexpr int kRequiredStableReads = 3;
            double prev_ra_hours = std::numeric_limits<double>::quiet_NaN();
            double prev_dec_degrees = std::numeric_limits<double>::quiet_NaN();
            int stable_count = 0;
            int failed_reads = 0;
            bool landed = false;

            while (clock_.now() < deadline) {
                std::optional<bool> target_reached;
                double cur_ra = 0, cur_dec = 0;
                double target_ra = 0, target_dec = 0;
                {
                    StateLock lock(lock_gate_);
                    target_reached = slew_target_reached_locked();
                    cur_ra = cached_ra_hours_;
                    cur_dec = cached_dec_degrees_;
                    target_ra = slew_target_ra_hours_;
                    target_dec = slew_target_dec_degrees_;
                }

                if (target_reached.value_or(false)) {
                    landed = true;
                    break;
                }

                // Issue #2396: a failed position read is no sample, so it can neither count as stable nor land the
                // slew on the position from before it.
                if (!target_reached.has_value()) {
                    if (++failed_reads >= kDeviceFaultThreshold) {
                        fail_synchronous_slew(
                            std::string(label) + " failed: the mount position could not be read to confirm the landing",
                            generation);
                    }
                    prev_ra_hours = std::numeric_limits<double>::quiet_NaN();
                    stable_count = 0;
                    clock_.sleep_for(kSlewPollInterval);
                    continue;
                }
                failed_reads = 0;

                if (!std::isnan(prev_ra_hours)) {
                    double ra_delta = std::abs(util::shortest_ra_delta_hours(cur_ra, prev_ra_hours)) * 15.0 * 3600.0;
                    double dec_delta = std::abs(cur_dec - prev_dec_degrees) * 3600.0;
                    if (ra_delta < kStableThresholdArcsec && dec_delta < kStableThresholdArcsec) {
                        ++stable_count;
                    } else {
                        stable_count = 0;
                    }
                }
                prev_ra_hours = cur_ra;
                prev_dec_degrees = cur_dec;

                if (stable_count >= kRequiredStableReads) {
                    // Issue #2396 landing gate: a mount that stopped far from the target did not land. The RA
                    // error is taken on the sky (times cos Dec), so a GOTO near the pole, where RA is loosely
                    // defined, is not failed for it.
                    const double ra_sky_arcsec = std::abs(util::shortest_ra_delta_hours(cur_ra, target_ra)) * 15.0 *
                                                 3600.0 * std::cos(target_dec * std::numbers::pi / 180.0);
                    const double dec_arcsec = std::abs(cur_dec - target_dec) * 3600.0;
                    if (ra_sky_arcsec > kSlewFarMissArcsec || dec_arcsec > kSlewFarMissArcsec) {
                        fail_synchronous_slew(std::string(label) + " failed: the mount stopped " +
                                                  std::to_string(static_cast<long>(std::lround(ra_sky_arcsec))) +
                                                  " arcsec (RA) and " +
                                                  std::to_string(static_cast<long>(std::lround(dec_arcsec))) +
                                                  " arcsec (Dec) from the target",
                                              generation);
                    }
                    ALPACA_LOG_INFO("iOptron", std::string(label) +
                                    ": position stabilized but target not reached");
                    landed = true;
                    break;
                }

                clock_.sleep_for(kSlewPollInterval);
            }
            if (!landed) {
                fail_synchronous_slew(std::string(label) + " timed out waiting for the mount to land on the target",
                                      generation);
            }
        }

        if (slew_settle_time_seconds_ > 0) {
            clock_.sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
        }
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (generation == slew_operation_generation_) {
            slew_in_progress_ = false;  // a newer motion owns slew_in_progress_ otherwise
        }
    }

    void refresh_position_cache_locked(bool force = false) const {
        auto now = clock_.now();
        if (!force && (position_cache_fresh_locked() || serve_as_is_locked(position_cache_valid_))) {
            return;
        }
        const Wire protocol = wire();
        try {
            Position pos = protocol.get_position();
            cached_dec_degrees_ = pos.dec_degrees;
            cached_ra_hours_ = pos.ra_hours;
            cached_side_of_pier_ = pos.side_of_pier;
            position_cache_valid_ = true;
            last_position_update_ = now;
            note_device_read_ok_locked();
        } catch (const std::exception& e) {
            throw_if_link_lost();
            record_device_fault_locked("Position", e.what());
            throw AlpacaException(std::string("Failed to refresh mount position: ") + e.what(),
                                  AlpacaError::DriverException);
        }
    }

    void refresh_altaz_cache_locked(bool force = false) const {
        auto now = clock_.now();
        if (!force && (altaz_cache_fresh_locked() || serve_as_is_locked(altaz_cache_valid_))) {
            return;
        }
        const Wire protocol = wire();
        try {
            AltAz altaz = protocol.get_alt_az();
            cached_alt_degrees_ = altaz.altitude_degrees;
            cached_az_degrees_ = altaz.azimuth_degrees;
            altaz_cache_valid_ = true;
            last_altaz_update_ = now;
            note_device_read_ok_locked();
        } catch (const std::exception& e) {
            throw_if_link_lost();
            record_device_fault_locked("AltAz", e.what());
            throw AlpacaException(std::string("Failed to refresh mount Alt/Az: ") + e.what(),
                                  AlpacaError::DriverException);
        }
    }
    
    void refresh_status_cache_locked(bool force = false) const {
        auto now = clock_.now();
        if (!force && (status_cache_fresh_locked() || serve_as_is_locked(status_cache_valid_))) {
            return;
        }
        const Wire protocol = wire();
        try {
            cached_status_ = protocol.get_status();
            status_cache_valid_ = true;
            last_status_update_ = now;
            note_device_read_ok_locked();
        } catch (const std::exception& e) {
            throw_if_link_lost();
            record_device_fault_locked("Status", e.what());
            throw AlpacaException(std::string("Failed to refresh mount status: ") + e.what(),
                                  AlpacaError::DriverException);
        }
        finalize_wedged_park_locked();
        stop_overdue_zero_search_locked(now);
        note_zero_search_motion_locked();
        if (!cached_status_.is_at_home) {
            home_search_unconfirmed_ = false;  // the mount left the 7 a failed search stopped at
        }
        // A fresh status ends a driver-started Park or FindHome whichever getter fetched it (AtHome, AtPark,
        // Tracking or Slewing): at home after a FindHome at once, any other no-motion status only past the
        // initiator's override (the firmware can still report its old status just after the command). A Park
        // needs no early end: a parked mount refuses PulseGuide and Unpark ends the window.
        // Runs after the finalizer, which may have re-read the status.
        // A zero search (issue #2420) ends only on status 7 past its hold, and only once a status 2 showed it
        // moving: the false zero it starts from reads 7 too, so a 7 the mount never left is not a landing (a
        // :MSH# ignored at a false zero would otherwise read as home). A running search reads 2 until it lands, so
        // another no-motion status does not end it either.
        const bool reached_home = driver_motion_ == DriverMotion::Home && cached_status_.is_at_home;
        const bool search_running = driver_motion_ == DriverMotion::HomeSearch &&
                                    (!cached_status_.is_at_home || slew_hold_.active(now) || !home_search_seen_moving_);
        if (!search_running && (reached_home || (!cached_status_.is_slewing && !slew_hold_.active(now)))) {
            driver_motion_ = DriverMotion::None;
        }
        // Issue #2063: a status that shows the mount stopped past the dispatch window ends the GOTO for the
        // PulseGuide mask and MoveAxis even if no client reads Slewing. slew_in_progress_ stays for get_slewing(),
        // which still runs the settle trim and clears it.
        if (!cached_status_.is_slewing && !slew_hold_.active(now)) {
            goto_seen_stopped_ = true;
        }
    }

    // Issue #2420: bound a zero sensor search. Past kZeroSearchTimeout with no status 7 the mount is stopped (:Q#,
    // best-effort like the park finalizer) and the failure is kept for Slewing until AbortSlew or a new initiator
    // (the failed-async-body rule). Runs before the landing clause, so a search that lands this read is not stopped.
    // Issue #2432: the watchdog slot (run_zero_search_watchdog()) reads the status at the bound, so this runs then
    // whether or not a client polls; a client's read past the bound that gets here first stops it the same way.
    void stop_overdue_zero_search_locked(std::chrono::steady_clock::time_point now) const {
        if (driver_motion_ != DriverMotion::HomeSearch || (cached_status_.is_at_home && home_search_seen_moving_) ||
            now < home_search_deadline_) {
            return;
        }
        stop_zero_search_locked("FindHome: the zero sensor search did not reach the zero position within " +
                                std::to_string(kZeroSearchTimeout.count()) + " s");
    }

    // Every fresh status during a zero search: a status 2 means the search started (it reads 2 until it lands on 7).
    void note_zero_search_motion_locked() const {
        if (driver_motion_ == DriverMotion::HomeSearch && cached_status_.is_slewing) {
            home_search_seen_moving_ = true;
        }
    }

    // Whether check_zero_search_started_locked() would act on the cached status now. The watching getters (Slewing,
    // AtHome) treat their cache as stale while it is true, so they reach the check under the operation lock even when
    // another getter (Tracking, AtPark, TrackingRate) or an initiator refreshed the shared status just before.
    bool zero_search_start_check_due_locked(std::chrono::steady_clock::time_point now) const {
        return driver_motion_ == DriverMotion::HomeSearch && !home_search_seen_moving_ && status_cache_valid_ &&
               !cached_status_.is_slewing && now >= home_search_start_until_;
    }

    // A zero search the mount never started. A running search reads status 2 until it lands on 7 (HAE29C traces over
    // USB and Wi-Fi, 2026-11-20). In one Wi-Fi ConformU run the mount ignored :MSH# and read 0 for the whole
    // kZeroSearchTimeout bound. So past its start grace, a status that is neither motion nor the zero, with no motion
    // seen since :MSH#, means the search did not start: send :MSH# once more with a fresh grace, and if that one does
    // not start either, stop the search with a stored error instead of waiting out the bound. A status 7 with no
    // motion seen counts the same: the HAE29C wakes from a power loss at a false zero that reads 7, and a :MSH# it
    // ignores there must not read as home. Every search traced, from the true zero too, read 2 for at least a
    // second, and the watchdog reads the status every kZeroSearchPollInterval until it sees one, so a short search
    // is not missed for want of a client poll. Called by the getters that watch the search (Slewing, AtHome) and
    // by the watchdog, under the operation lock: an initiator's own status read must not put a :MSH# on the wire
    // just before its command supersedes the search. The grace is its own deadline, not the slew hold, which
    // MoveAxis clears while the search keeps running.
    void check_zero_search_started_locked(std::chrono::steady_clock::time_point now) const {
        if (driver_motion_ != DriverMotion::HomeSearch) {
            return;
        }
        note_zero_search_motion_locked();
        if (home_search_seen_moving_ || cached_status_.is_slewing || now < home_search_start_until_) {
            return;
        }
        if (home_search_resent_) {
            stop_zero_search_locked("FindHome: the zero sensor search did not start (the mount stayed at status " +
                                    std::to_string(cached_status_.system_status) + " after :MSH# was sent twice)");
            return;
        }
        home_search_resent_ = true;
        ALPACA_LOG_WARN("iOptron", "FindHome: the zero sensor search did not start (status " +
                                       std::to_string(cached_status_.system_status) + "); sending :MSH# again");
        try {
            send_with_slew_hold_locked(std::chrono::seconds(5), [&] { wire().find_home(); });
        } catch (const std::exception& e) {
            throw_if_link_lost();  // a pulled cable is NotConnected
            stop_zero_search_locked(
                std::string("FindHome: the zero sensor search did not start, and sending :MSH# again failed: ") +
                e.what());
            return;
        }
        home_search_start_until_ = clock_.now() + kZeroSearchStartGrace;  // from the send, as find_home()
        status_cache_valid_ = false;  // the status just read is from before the second :MSH#
    }

    // A zero search that ends before it showed motion (AbortSlew, a superseding command, a connect or disconnect):
    // the 7 the mount may still read is the false zero it started from, so AtHome must not take it for home.
    void note_unlanded_zero_search_locked() const {
        if (driver_motion_ == DriverMotion::HomeSearch && !home_search_seen_moving_) {
            home_search_unconfirmed_ = true;
        }
    }

    // Ends the zero search the driver started: sends :Q# and keeps `failure` for Slewing (issue #2420, #2432).
    void stop_zero_search_locked(const std::string& failure) const {
        driver_motion_ = DriverMotion::None;
        // The search did not land: the 7 the mount may still read is the false zero it started from, not home.
        home_search_unconfirmed_ = true;
        clear_slew_hold_locked();
        // Stored before the stop goes out: the exchange gives up mutex_, and a reader in that gap must not take
        // the stale status for a finished search.
        last_slew_error_ = failure + "; the mount was stopped";
        try {
            wire().stop_slewing();
        } catch (const std::exception& e) {
            throw_if_link_lost();  // a pulled cable is NotConnected
            last_slew_error_ = failure + ", and stopping the mount failed: " + e.what();
        }
        ALPACA_LOG_WARN("iOptron", last_slew_error_);
        status_cache_valid_ = false;  // the status just read is from before the stop
    }

    // Issue #2432: the zero search's bound on its own slot. Call with no driver lock held (decision 0006 rule 4). The
    // body follows whichever search is current when it wakes, so two FindHome calls that start it in either order
    // leave the newer search bounded. A start the slot refuses fails closed: the search is stopped and FindHome
    // throws, so no zero search runs without its bound.
    void start_zero_search_watchdog() {
        try {
            zero_search_watchdog_.start([this](util::OperationContext& ctx) {
                try {
                    run_zero_search_watchdog(ctx);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("iOptron", std::string("Zero search watchdog failed: ") + e.what());
                    throw;
                }
            });
        } catch (const std::exception& e) {
            std::string failure = std::string("FindHome: the zero search watchdog could not start: ") + e.what();
            StateLock lock(lock_gate_);
            if (connected_ && driver_motion_ == DriverMotion::HomeSearch) {
                stop_zero_search_locked(failure);
                failure = last_slew_error_;
            }
            throw AlpacaException(failure, AlpacaError::DriverException);
        }
    }

    // Until the search shows a status 2, reads the status every kZeroSearchPollInterval and runs the start check
    // (check_zero_search_started_locked(): one more :MSH#, then a stop). After that it waits on the task clock for
    // the current search's bound, then reads the status under the operation lock: a status 7 after motion lands the
    // search there (refresh_status_cache_locked()), anything else stops it with the stored failure. A status read that
    // fails at the bound stops it too (fail closed); a lost link ends the body, since the loss already ends the
    // session. Ends at once when no search runs, the driver is disconnected or the slot stops it.
    void run_zero_search_watchdog(util::OperationContext& ctx) {
        while (true) {
            std::chrono::steady_clock::duration remaining{};
            {
                StateLock lock(lock_gate_);
                if (!connected_ || ctx.stop_reason() != util::StopReason::None ||
                    driver_motion_ != DriverMotion::HomeSearch) {
                    return;
                }
                const auto now = clock_.now();
                // Until the search shows motion, read its status every kZeroSearchPollInterval and run the start
                // check, so a short search is seen and an ignored :MSH# is caught with no client polling. A failed
                // read here is left to the next poll or the bound; a lost link ends the body.
                const bool polling = zero_search_poll_enabled_.load() && !home_search_seen_moving_;
                if (now < home_search_deadline_ && polling && now >= home_search_next_poll_) {
                    home_search_next_poll_ = now + kZeroSearchPollInterval;
                    try {
                        refresh_status_cache_locked(true);
                        // Judged at the time the read started: the status is from then, not from after the
                        // exchange (which gives up mutex_), so a slow reply cannot push it past a grace it read in.
                        check_zero_search_started_locked(now);
                    } catch (const std::exception& e) {
                        if (link_lost()) return;
                        ALPACA_LOG_DEBUG("iOptron", std::string("Zero search poll read failed (the next poll or the "
                                                                "bound retries): ") +
                                                        e.what());
                    }
                    if (driver_motion_ != DriverMotion::HomeSearch) return;
                }
                if (now >= home_search_deadline_) {
                    try {
                        refresh_status_cache_locked(true);
                    } catch (const std::exception& e) {
                        if (link_lost()) return;
                        if (driver_motion_ == DriverMotion::HomeSearch) {
                            const std::string read_failure = e.what();
                            stop_zero_search_locked("FindHome: no zero sensor search status could be read at its " +
                                                    std::to_string(kZeroSearchTimeout.count()) + " s bound (" +
                                                    read_failure + ")");
                        }
                    }
                    return;
                }
                remaining = home_search_deadline_ - now;
                if (zero_search_poll_enabled_.load() && !home_search_seen_moving_) {
                    remaining = std::min(remaining, home_search_next_poll_ - now);
                }
            }
            if (!ctx.wait_for(remaining)) return;
        }
    }

    // Cancel and join the zero search watchdog. Without mutex_ held; under transition_mutex_ (set_connected()) or in
    // the destructor, which serializes its reapers. The body never takes transition_mutex_.
    void stop_zero_search_watchdog() { zero_search_watchdog_.cancel_all_and_join(); }

    // See park(): once an :MP1# park has physically arrived, some firmware
    // (HAE29C) stays wedged in "slewing" until tracking is explicitly turned
    // off. Runs after every fresh status fetch; best-effort — a failure here
    // just retries on the next refresh. Known limit: while the finalizer keeps failing the mount stays
    // "slewing", so driver_motion_ stays Park until Unpark or AbortSlew (conservative: Slewing stays true).
    void finalize_wedged_park_locked() const {
        if (!park_finalize_pending_) {
            return;
        }
        if (cached_status_.is_parked) {
            park_finalize_pending_ = false;
            return;
        }
        if (!cached_status_.is_slewing || !park_target_valid_) {
            return;
        }
        const Wire protocol = wire();
        try {
            AltAz current = protocol.get_alt_az();
            constexpr double kEpsDeg = 0.01;  // 36 arcsec; GAC/GPC agree to 0.01"
            // Azimuth delta normalized to [-180, 180]: a mount at 359.995 with
            // a park azimuth of 0 is on target (issue #135).
            if (std::fabs(current.altitude_degrees - park_target_.altitude_degrees) >= kEpsDeg ||
                std::fabs(std::remainder(current.azimuth_degrees - park_target_.azimuth_degrees, 360.0)) >= kEpsDeg) {
                return;  // still genuinely slewing toward the park position
            }
            ALPACA_LOG_INFO("iOptron",
                            "Park slew arrived but mount still reports slewing; sending tracking-off "
                            "to finalize (HAE firmware quirk)");
            protocol.stop_tracking();
            cached_status_ = protocol.get_status();
            last_status_update_ = clock_.now();
            if (cached_status_.is_parked) {
                park_finalize_pending_ = false;
            }
        } catch (const std::exception&) {
            // A pulled cable is NotConnected, never the status read just before it served as AtPark or Slewing.
            throw_if_link_lost();
            // Otherwise best-effort: leave the flag armed and retry on the next refresh.
        }
    }

    void ensure_site_info_cached_locked(bool force_refresh = false) const {
        auto now = clock_.now();
        if (!force_refresh && (site_info_fresh_locked() || serve_as_is_locked(site_info_valid_))) {
            return;
        }
        const Wire protocol = wire();
        try {
            SiteInfo site = protocol.get_site_info();
            if ((std::abs(site.latitude_degrees) > 90.0) ||
                (std::abs(site.longitude_degrees) > 180.0)) {
                if (site_info_valid_) {
                    ALPACA_LOG_WARN("iOptron", "Invalid site info reported by mount; using cached values");
                    last_site_info_fetch_ = now;
                    return;
                }
                throw AlpacaException("Invalid site information reported by mount",
                                      AlpacaError::DriverException);
            }
            site_latitude_cached_ = site.latitude_degrees;
            site_longitude_cached_ = site.longitude_degrees;
            hemisphere_north_ = site.is_northern_hemisphere;
            timezone_offset_minutes_ = site.timezone_offset_minutes;
            timezone_offset_valid_ = true;
            dst_observed_ = site.dst_observed;
            site_info_valid_ = true;
            last_site_info_fetch_ = now;
            note_device_read_ok_locked();
        } catch (const std::exception& e) {
            throw_if_link_lost();
            if (site_info_valid_) {
                ALPACA_LOG_WARN("iOptron", std::string("Failed to read site info; using cached values: ") + e.what());
                last_site_info_fetch_ = now;
                return;
            }
            record_device_fault_locked("SiteInfo", e.what());
            throw AlpacaException(std::string("Failed to read site info: ") + e.what(),
                                  AlpacaError::DriverException);
        }
    }
    
    struct LocalTimeInfo {
        int offset_minutes = 0;
        bool dst_active = false;
    };

    // `ctx`: the clock-sync body's retries stop when it is stopped; the synchronous
    // connect-path callers pass none and keep the plain delay.
    bool retry_mount_command(const char* label, const std::function<void()>& fn,
                             util::OperationContext* ctx = nullptr) {
        constexpr int kMaxAttempts = 3;
        const auto delay = std::chrono::milliseconds(750);
        std::string last_error;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            if (ctx && ctx->stop_reason() != util::StopReason::None) {
                return false;  // stopped between commands: send nothing more
            }
            try {
                fn();
                return true;
            } catch (const std::exception& e) {
                last_error = e.what();
                if (attempt < kMaxAttempts) {
                    if (ctx ? !ctx->wait_for(delay) : (clock_.sleep_for(delay), false)) {
                        break;  // the clock-sync body was stopped: stop retrying
                    }
                }
            }
        }
        ALPACA_LOG_WARN("iOptron", std::string("Failed to sync ") + label + ": " + last_error);
        return false;
    }

    void calibrate_dec_guide_locked(int duration_ms) {
        refresh_status_cache_locked(true);
        if (cached_status_.is_slewing || cached_status_.is_parked) {
            ALPACA_LOG_WARN("iOptron", "Skipping Dec guide calibration while slewing or parked");
            return;
        }

        const Wire protocol = wire();
        try {
            const auto start = protocol.get_position();
            dec_guide_calibration_side_ = start.side_of_pier;
            if (std::abs(start.dec_degrees) > 85.0) {
                ALPACA_LOG_WARN("iOptron", "Skipping Dec guide calibration near the pole (Dec=" +
                                              std::to_string(start.dec_degrees) + ")");
                return;
            }
            const int sample_duration = std::clamp(duration_ms, 500, 2000);
            protocol.pulse_guide(0, sample_duration);
            clock_.sleep_for(std::chrono::milliseconds(sample_duration + 700));
            Position end = protocol.get_position();
            for (int i = 0; i < 2; ++i) {
                clock_.sleep_for(std::chrono::milliseconds(200));
                end = protocol.get_position();
            }

            const double delta = end.dec_degrees - start.dec_degrees;
            const double min_delta = 0.0005;  // ~1.8 arcsec
            if (std::abs(delta) < min_delta) {
                ALPACA_LOG_WARN("iOptron", "Dec guide calibration inconclusive; using default mapping (delta=" +
                                              std::to_string(delta) + " deg)");
                return;
            }

            dec_guide_inverted_ = (delta < 0.0);
            dec_guide_calibrated_ = true;
            ALPACA_LOG_INFO("iOptron", std::string("Dec guide calibration: ") +
                                           (dec_guide_inverted_ ? "inverted" : "normal"));
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", std::string("Dec guide calibration failed: ") + e.what());
        }
    }

    // Issue #2434: one :AG# read on the connect path, and again from a per-axis setter while the rate is unknown. A
    // failed read leaves the rate unknown (GuideRate raises) and does not fail the connect; a hangup over serial
    // loses the link, which the connect's link check reports.
    void read_guide_rate_locked() {
        const Wire protocol = wire();
        try {
            const auto [ra_fraction, dec_fraction] = protocol.get_guide_rates();
            cached_guide_rate_.ra = ra_fraction * kSiderealRateDegPerSec;
            cached_guide_rate_.dec = dec_fraction * kSiderealRateDegPerSec;
            guide_rate_valid_ = true;
        } catch (const std::exception& e) {
            guide_rate_valid_ = false;
            ALPACA_LOG_WARN("iOptron", std::string("Guide rate unavailable on connect: ") + e.what());
        }
    }

    // Empty when the position read failed.
    std::optional<bool> slew_target_reached_locked() const {
        try {
            refresh_position_cache_locked(true);
        } catch (const std::exception&) {
            throw_if_link_lost();  // a synchronous slew ends with NotConnected, not "position stabilized"
            return std::nullopt;
        }
        const double ra_delta_hours = std::abs(util::shortest_ra_delta_hours(slew_target_ra_hours_, cached_ra_hours_));
        const double dec_delta_degrees = std::abs(slew_target_dec_degrees_ - cached_dec_degrees_);
        const double ra_arcsec = ra_delta_hours * 15.0 * 3600.0;
        const double dec_arcsec = dec_delta_degrees * 3600.0;
        return ra_arcsec <= kSlewCompletionToleranceArcsec &&
               dec_arcsec <= kSlewCompletionToleranceArcsec;
    }

    static void validate_alt_az(double altitude_degrees, double azimuth_degrees, const char* label) {
        if (!std::isfinite(altitude_degrees) || altitude_degrees < -90.0 || altitude_degrees > 90.0) {
            throw AlpacaException(std::string(label) + " altitude must be in [-90, 90] degrees",
                                  AlpacaError::InvalidValue);
        }
        if (!std::isfinite(azimuth_degrees) || azimuth_degrees < 0.0 || azimuth_degrees >= 360.0) {
            throw AlpacaException(std::string(label) + " azimuth must be in [0, 360) degrees",
                                  AlpacaError::InvalidValue);
        }
    }

    void sync_site_settings_with_mount_locked() {
        if (pending_site_latitude_.has_value()) {
            double latitude = pending_site_latitude_.value();
            if (retry_mount_command("site latitude", [&]() { wire().set_latitude(latitude); })) {
                site_latitude_cached_ = latitude;
                hemisphere_north_ = (latitude >= 0.0);
                site_info_valid_ = true;
                last_site_info_fetch_ = clock_.now();
            }
        }

        if (pending_site_longitude_.has_value()) {
            double longitude = pending_site_longitude_.value();
            if (retry_mount_command("site longitude", [&]() { wire().set_longitude(longitude); })) {
                site_longitude_cached_ = longitude;
                site_info_valid_ = true;
                last_site_info_fetch_ = clock_.now();
            }
        }

        if (pending_site_latitude_.has_value()) {
            bool is_north = pending_site_latitude_.value() >= 0.0;
            retry_mount_command("hemisphere", [&]() { wire().set_hemisphere(is_north); });
        }

        if (pending_site_elevation_.has_value()) {
            site_elevation_m_ = pending_site_elevation_.value();
        }
    }

    // The host's timezone and DST at host time `now` (host_utc_now() in the clock-sync body, #2419).
    static LocalTimeInfo compute_local_timezone_info(std::time_t now) {
        LocalTimeInfo info;
        std::tm local_tm {};
        std::tm utc_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &now);
        gmtime_s(&utc_tm, &now);
#else
        // Reentrant: the clock-sync body runs on its own thread (std::localtime / std::gmtime share one buffer).
        localtime_r(&now, &local_tm);
        gmtime_r(&now, &utc_tm);
#endif
        std::time_t local_time = std::mktime(&local_tm);
        std::time_t utc_interpreted_as_local = std::mktime(&utc_tm);
        double offset_seconds = std::difftime(local_time, utc_interpreted_as_local);
        int offset_minutes = static_cast<int>(std::llround(offset_seconds / 60.0));
        if (offset_minutes < -720) {
            offset_minutes = -720;
        } else if (offset_minutes > 780) {
            offset_minutes = 780;
        }
        info.offset_minutes = offset_minutes;
        info.dst_active = (local_tm.tm_isdst > 0);
        return info;
    }

    // Starts the clock-sync body on the clock-sync slot (decision 0006). start() cancels nothing
    // itself: it supersedes the previous body, whose waits wake at once, and never waits for it.
    // Call without mutex_ held (the body takes it). Clock sync is not an ASCOM async operation and
    // ITelescopeV4 has no completion property for it, so a body that throws is kept by the slot
    // (clock_sync_failure()) and logged, until the next start().
    // `resync_why` is empty for the connect's own pass; otherwise it says what the host clock did after the
    // connect (#2419), and the body first checks the mount's clock against the host's. `decided_generation` is
    // utc_write_generation_ when a re-sync was decided (the connect's own pass ignores it).
    void start_clock_sync(std::string resync_why = {}, uint64_t decided_generation = 0) {
        clock_sync_.start([this, why = std::move(resync_why), decided_generation](util::OperationContext& ctx) {
            try {
                run_clock_sync(ctx, why, decided_generation);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("iOptron", std::string("Clock sync failed: ") + e.what());
                throw;
            }
        });
    }

    // Issue #2419: the server's timer saw the host clock step or become trusted. Re-syncs the mount clock on the
    // clock-sync slot. The timer thread calls this with no lock held. It does no I/O and joins nothing, and it does not
    // wait for transition_mutex_: it stores the change as pending first, and a connect or a disconnect holding the lock
    // drains it after letting go (drain_pending_resync()), so a change that arrives as set_connected() returns is not
    // lost. AsyncOperation::start() itself can wait up to kStaleReapTimeout (2 s) when three replaced bodies are still
    // running, for example on a blocked link (#2432).
    void on_host_clock_changed(const util::HostClockEvent& event) noexcept override {
        // sync_time_on_connect_ is fixed at construction; published_connected_ is lock-free.
        if (!sync_time_on_connect_ || !published_connected_.load()) return;
        try {
            resync_pending_.store(true);
            drain_pending_resync(event.kind == util::HostClockEvent::Kind::Stepped
                                     ? "Host clock stepped by " + std::to_string(event.step.count()) + " ms"
                                     : std::string("Host clock became trusted"));
        } catch (...) {  // NOLINT(bugprone-empty-catch): a timer-thread hook must not throw (the string building)
        }
    }

    // Starts the re-sync a host clock change left pending, unless another thread holds transition_mutex_: every
    // holder (set_connected()) calls this after letting go, so a change stored while the lock was held is never lost.
    // A change seen while disconnected is dropped. Call with no driver lock held.
    void drain_pending_resync(const std::string& why) noexcept {
        try {
            while (resync_pending_.load()) {
                std::unique_lock<std::mutex> transition(transition_mutex_, std::try_to_lock);
                if (!transition.owns_lock()) {
                    if (host_clock_busy_hook_) host_clock_busy_hook_();  // test seam: no driver lock held
                    return;                                              // its holder drains as it lets go
                }
                if (!resync_pending_.exchange(false)) return;
                start_resync_transition_locked(why);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): a timer-thread hook must not throw
        }
    }

    // Under transition_mutex_ and no other driver lock: starts a re-sync when the mount is still connected. A
    // refused start() (a slot still draining, a refused thread, earlier bodies stuck on a blocked link) is logged;
    // the next host clock change tries again. The re-sync is decided here: a client UTCDate write after this point is
    // newer, so the re-sync's :SUT does not go out over it.
    void start_resync_transition_locked(const std::string& why) noexcept {
        try {
            if (!published_connected_.load() || link_lost()) return;
            start_clock_sync(why, utc_write_generation_.load());
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", std::string("Could not start the mount clock re-sync: ") + e.what());
        } catch (...) {
            ALPACA_LOG_WARN("iOptron", "Could not start the mount clock re-sync");
        }
    }

    // Host UTC now: system_clock, or the test seam (set before connect).
    std::chrono::system_clock::time_point host_utc_now() const {
        return host_utc_source_ ? host_utc_source_() : std::chrono::system_clock::now();
    }

    static std::string format_utc(std::chrono::system_clock::time_point utc) {
        const std::time_t t = std::chrono::system_clock::to_time_t(utc);
        std::tm tm{};
#if defined(_WIN32)
        if (gmtime_s(&tm, &t) != 0) return "unknown";
#else
        if (gmtime_r(&t, &tm) == nullptr) return "unknown";
#endif
        char text[32];
        if (std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) return "unknown";
        return text;
    }

    // Under mutex_: a GOTO, Park, FindHome or MoveAxis the driver started is still under way. A GOTO counts until
    // a status read shows it stopped (goto_seen_stopped_, #2063), so a client that never polls Slewing does not hold
    // a re-sync back for ever.
    bool driver_motion_active_locked() const {
        return driver_motion_ != DriverMotion::None || (slew_in_progress_ && !goto_seen_stopped_) ||
               axis_move_active_primary_ || axis_move_active_secondary_;
    }

    // Issue #2419: a re-sync never changes the mount clock under motion the driver started, since the firmware aims
    // a GOTO by it. Waits through `ctx` (a disconnect or a newer start ends the wait) until the motion ends, looking
    // again every util::kHostClockResyncMotionPoll on the task clock. False when stopped or disconnected, or when the
    // mount still moves at `give_up_at` (one deadline per re-sync, util::kHostClockResyncMotionBound after it
    // started): that sets `gave_up`, and the re-sync leaves the mount clock for the next host clock change.
    bool wait_for_driver_motion_to_end(util::OperationContext& ctx, util::TaskClock::clock::time_point give_up_at,
                                       bool& gave_up) {
        bool logged = false;
        while (true) {
            {
                StateLock lock(lock_gate_, StateLock::Mode::State);
                if (!connected_ || ctx.stop_reason() != util::StopReason::None) return false;
                if (!driver_motion_active_locked()) return true;
            }
            if (clock_.now() >= give_up_at) {
                ALPACA_LOG_WARN("iOptron", "The mount was still moving after " +
                                               std::to_string(util::kHostClockResyncMotionBound.count()) +
                                               " min; the mount clock re-sync gives up until the next host clock "
                                               "change");
                gave_up = true;
                return false;
            }
            if (!logged) {
                ALPACA_LOG_INFO("iOptron", "The mount is moving; the mount clock re-sync waits for the motion to end");
                logged = true;
            }
            if (!ctx.wait_for(util::kHostClockResyncMotionPoll)) return false;
        }
    }

    void run_clock_sync(util::OperationContext& ctx, const std::string& resync_why, uint64_t decided_generation) {
        if (!ctx.wait_for(std::chrono::milliseconds(250))) {
            return;
        }
        if (clock_sync_test_hook_) {
            clock_sync_test_hook_();
        }
        std::optional<double> lat, lon;
        std::optional<int> elev;
        bool do_clock_sync = false;
        bool connect_pass_done = false;
        std::optional<std::chrono::system_clock::time_point> mount_utc;
        // Client write generations seen with our own write (start snapshot, then the successful send).
        uint64_t lat_generation = 0, lon_generation = 0, elev_generation = 0;
        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                return;
            }
            lat = pending_site_latitude_;
            lon = pending_site_longitude_;
            elev = pending_site_elevation_;
            do_clock_sync = sync_time_on_connect_;
            lat_generation = site_latitude_write_generation_;
            lon_generation = site_longitude_write_generation_;
            elev_generation = site_elevation_write_generation_;
            mount_utc = utc_offset_.current(clock_.now());
            connect_pass_done = clock_sync_connect_pass_done_;
        }
        // A re-sync that superseded the connect's own pass before that pass finished (a host clock change in the
        // first second after Connect), or that follows a connect pass that threw before it published, runs as the
        // connect pass: it must still send the site, and may not stop at a mount clock that already agrees (#2419).
        // It is still a host clock re-sync (`host_clock_resync`), so the motion wait and the write check below
        // apply to it too (#2432).
        const bool host_clock_resync = !resync_why.empty();
        const bool resync = host_clock_resync && connect_pass_done;
        // Issue #2419: the host clock goes into the mount only once it is trusted (NTP-synchronised, or set or
        // confirmed by a client, or a step seen since start). A Pi with no RTC boots on the time of its last
        // shutdown; :SUT from it put RC51's mount 6 h 48 min out. The site commands below do not depend on it.
        if (do_clock_sync && !util::HostClockTrust::trusted()) {
            do_clock_sync = false;
            if (resync) return;
            ALPACA_LOG_WARN("iOptron",
                            "Host clock is not trusted (not NTP-synchronised, not set or confirmed by a client); "
                            "leaving the mount clock (" +
                                (mount_utc ? format_utc(*mount_utc) : std::string("unknown")) +
                                ") alone until the host clock is set or a client writes UTCDate");
        }
        if (resync && !do_clock_sync) return;

        // Each command takes the operation lock and the state lock for itself, like a client's command, and
        // releases both before the retry wait: a getter or a command never waits behind a retry. A body that
        // was stopped, or that a disconnect's join missed (it started after it), sends nothing (#2419).
        const Wire protocol = wire();
        auto exchange = [this, &ctx](auto&& send) {
            StateLock lock(lock_gate_);
            if (!connected_ || ctx.stop_reason() != util::StopReason::None) return;
            send();
        };
        // Issue #2432: a re-sync's site and clock writes. The motion wait below is only a check, and a GOTO, Park,
        // FindHome or MoveAxis can start right after it, so each write checks again inside its own exchange: under
        // the operation lock, which every motion command takes, no motion can start between that check and the
        // write. A write that finds motion waits for it to end and checks again; a stop or a disconnect during that
        // wait sends nothing (fail closed). Any host clock re-sync checks, one that runs as the connect pass
        // included; the connect's own pass writes as before.
        // A re-sync waits out motion for at most kHostClockResyncMotionBound in all (#2419); once it gave up, no
        // later write goes out and the body ends without publishing.
        const auto motion_give_up_at = clock_.now() + util::kHostClockResyncMotionBound;
        bool motion_gave_up = false;
        auto gated_write = [&](auto&& send) {
            while (!motion_gave_up) {
                {
                    StateLock lock(lock_gate_);
                    if (!connected_ || ctx.stop_reason() != util::StopReason::None) return;
                    if (!host_clock_resync || !driver_motion_active_locked()) {
                        send();
                        return;
                    }
                }
                if (!wait_for_driver_motion_to_end(ctx, motion_give_up_at, motion_gave_up)) return;
            }
        };
        // The mount's own clock, fresh from :GUT#; nullopt when it cannot say.
        auto read_mount_utc = [&]() -> std::optional<std::chrono::system_clock::time_point> {
            std::optional<std::chrono::system_clock::time_point> utc;
            try {
                exchange([&] { utc = protocol.get_utc_time(); });
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("iOptron", "Mount UTC unavailable for the clock re-sync: " + std::string(e.what()));
            }
            return utc;
        };

        std::optional<std::chrono::system_clock::time_point> old_mount_utc;
        if (host_clock_resync) {
            if (!wait_for_driver_motion_to_end(ctx, motion_give_up_at, motion_gave_up)) return;
            if (clock_sync_resync_hook_) clock_sync_resync_hook_();  // test seam: no driver lock held
        }
        if (resync) {
            old_mount_utc = read_mount_utc();
            if (old_mount_utc) {
                const auto diff =
                    std::chrono::duration_cast<std::chrono::milliseconds>(host_utc_now() - *old_mount_utc);
                if (diff < util::HostClock::kClientDisagreementWarn &&
                    diff > -util::HostClock::kClientDisagreementWarn) {
                    ALPACA_LOG_INFO("iOptron", resync_why + "; the mount clock (" + format_utc(*old_mount_utc) +
                                                   ") already agrees, no re-sync");
                    return;
                }
            }
        }

        if (lat.has_value()) {
            double latitude = lat.value();
            retry_mount_command(
                "site latitude",
                [&]() {
                    gated_write([&] {
                        protocol.set_latitude(latitude);
                        lat_generation = site_latitude_write_generation_;
                    });
                },
                &ctx);
            retry_mount_command(
                "hemisphere", [&]() { gated_write([&] { protocol.set_hemisphere(latitude >= 0.0); }); }, &ctx);
        }
        if (lon.has_value()) {
            double longitude = lon.value();
            retry_mount_command(
                "site longitude",
                [&]() {
                    gated_write([&] {
                        protocol.set_longitude(longitude);
                        lon_generation = site_longitude_write_generation_;
                    });
                },
                &ctx);
        }

        util::UtcOffset synced_time;
        uint64_t synced_generation = 0;
        bool client_utc_kept = false;
        LocalTimeInfo tz_info;
        if (do_clock_sync) {
            tz_info = compute_local_timezone_info(std::chrono::system_clock::to_time_t(host_utc_now()));
            retry_mount_command(
                "timezone offset",
                [&]() { gated_write([&] { protocol.set_timezone_offset(tz_info.offset_minutes); }); }, &ctx);
            retry_mount_command(
                "DST flag", [&]() { gated_write([&] { protocol.set_dst_observed(tz_info.dst_active); }); }, &ctx);
            retry_mount_command(
                "UTC clock",
                [&]() {
                    if (clock_sync_before_utc_exchange_hook_) clock_sync_before_utc_exchange_hook_();  // no lock held
                    gated_write([&] {
                        // A client UTCDate write after the re-sync was decided (while it waited out a GOTO, say) is
                        // newer and keeps the mount clock. Checked here, under the operation lock that write takes
                        // too, so it cannot land between the check and the :SUT. The connect's own pass compares at
                        // publication only (#1780).
                        if (host_clock_resync && utc_write_generation_.load() != decided_generation) {
                            if (!client_utc_kept) {
                                ALPACA_LOG_INFO(
                                    "iOptron",
                                    resync_why + "; a client wrote UTCDate since, so the mount keeps that time");
                            }
                            client_utc_kept = true;
                            return;
                        }
                        if (clock_sync_utc_hook_) clock_sync_utc_hook_(false);
                        // Fresh host time for each send attempt: a retry must not send the first attempt's time.
                        const auto now_utc = host_utc_now();
                        const auto sent_at = clock_.now();
                        protocol.set_utc_time(now_utc);
                        synced_time.set(now_utc, sent_at);
                        synced_generation = utc_write_generation_;
                    });
                },
                &ctx);
        }
        if (motion_gave_up) return;
        if (resync && synced_time.valid()) {
            const auto new_mount_utc = read_mount_utc();
            ALPACA_LOG_INFO("iOptron", resync_why + "; mount UTC " +
                                           (old_mount_utc ? format_utc(*old_mount_utc) : std::string("unknown")) +
                                           " -> " +
                                           (new_mount_utc ? format_utc(*new_mount_utc) : std::string("unknown")));
        }
        if (clock_sync_utc_hook_) clock_sync_utc_hook_(true);

        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                return;
            }
            // A client site write after our own send is later on the wire, so it keeps its value.
            if (lat.has_value() && lat_generation == site_latitude_write_generation_) {
                site_latitude_cached_ = lat.value();
                hemisphere_north_ = (lat.value() >= 0.0);
                site_info_valid_ = true;
                last_site_info_fetch_ = clock_.now();
            }
            if (lon.has_value() && lon_generation == site_longitude_write_generation_) {
                site_longitude_cached_ = lon.value();
                site_info_valid_ = true;
                last_site_info_fetch_ = clock_.now();
            }
            if (elev.has_value() && elev_generation == site_elevation_write_generation_) {
                site_elevation_m_ = elev.value();
            }
            if (do_clock_sync) {
                timezone_offset_minutes_ = tz_info.offset_minutes;
                timezone_offset_valid_ = true;
                dst_observed_ = tz_info.dst_active;
                // A client UTCDate write after our :SUT is later on the wire, so it keeps the anchor.
                if (synced_time.valid() && synced_generation == utc_write_generation_) utc_offset_ = synced_time;
            }
            clock_sync_connect_pass_done_ = true;
        }
    }

    // Cancel and join the clock-sync body. Without mutex_ held. clock_sync_reap_mutex_ serializes
    // reapers across the whole join (docs/failures/0011): a second caller would otherwise see an empty
    // slot while the first still owns the running body, and disconnect would tear the protocol down
    // under it. The body never takes it.
    void stop_clock_sync() {
        std::lock_guard<std::mutex> reap(clock_sync_reap_mutex_);
        clock_sync_.cancel_all_and_join();
    }

    // Cancel and join the async slew dispatch on the slew slot (decision 0006).
    // Must be called WITHOUT mutex_ held -- the body takes mutex_, so joining
    // under the lock would deadlock -- and every caller holds
    // slew_handoff_mutex_ (or is the destructor), because the slot requires
    // its reapers to be serialized across the whole join. The body sends one
    // command (no long waits), so the join is quick.
    // open-astro#768: abort and disconnect fence the dispatch through this.
    void reap_slew_dispatch() { slew_.cancel_all_and_join(); }

    // Starts `body` on the slew slot after the initiator published its state
    // and bumped slew_operation_generation_. Call without mutex_ held, under
    // slew_handoff_mutex_. A refused start (no thread) undoes the published
    // state and restores the error the initiator cleared.
    void start_slew_body(std::uint64_t generation, std::string& previous_slew_error, const char* failure_prefix,
                         std::function<void(util::OperationContext&)> body) {
        try {
            if (before_slew_reap_) before_slew_reap_();
            // The previous dispatch is cancelled and joined first, so a newer
            // initiator never leaves an older body running (open-astro#768).
            reap_slew_dispatch();
            slew_.start(std::move(body));
        } catch (const std::exception& e) {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            if (generation == slew_operation_generation_) {
                slew_in_progress_ = false;
                clear_slew_hold_locked();
                last_slew_error_ = std::move(previous_slew_error);
            }
            throw AlpacaException(std::string(failure_prefix) + ": " + e.what(), AlpacaError::DriverException);
        }
    }

    // The body of an async slew: takes mutex_, stands down when a newer
    // initiator, MoveAxis, Park, disconnect or AbortSlew got there first, and
    // records a real failure in last_slew_error_ (never one caused by its own
    // cancellation). `dispatch` runs under mutex_.
    template <typename Dispatch>
    void run_slew_body(util::OperationContext& ctx, std::uint64_t generation, const char* label, Dispatch&& dispatch) {
        if (before_slew_worker_lock_) before_slew_worker_lock_();
        StateLock lock(lock_gate_);
        // A superseding initiator already cleared/replaced the GOTO state
        // before bumping this generation; never publish or clear state owned
        // by that newer operation.
        if (generation != slew_operation_generation_) return;
        if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
            slew_in_progress_ = false;
            clear_slew_hold_locked();
            return;
        }
        try {
            dispatch();
        } catch (const std::exception& e) {
            slew_in_progress_ = false;
            clear_slew_hold_locked();
            if (ctx.stop_reason() == util::StopReason::None) {
                last_slew_error_ = std::string(label) + " failed: " + e.what();
            }
            ALPACA_LOG_WARN("iOptron", std::string(label) + " dispatch failed: " + e.what());
        } catch (...) {
            slew_in_progress_ = false;
            clear_slew_hold_locked();
            if (ctx.stop_reason() == util::StopReason::None) {
                last_slew_error_ = std::string(label) + " failed with an unknown error";
            }
            ALPACA_LOG_WARN("iOptron", std::string(label) + " dispatch failed with unknown exception");
        }
    }

public:
    void set_slew_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        slew_.set_spawn_for_testing(std::move(spawn));
    }
    void set_zero_search_watchdog_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        zero_search_watchdog_.set_spawn_for_testing(std::move(spawn));
    }
    void set_zero_search_poll_for_testing(bool enabled) { zero_search_poll_enabled_.store(enabled); }
    void set_clock_sync_hook_for_testing(std::function<void()> hook) { clock_sync_test_hook_ = std::move(hook); }
    void set_clock_sync_utc_hook_for_testing(std::function<void(bool)> hook) { clock_sync_utc_hook_ = std::move(hook); }
    void start_clock_sync_for_testing() { start_clock_sync(); }
    void set_host_utc_source_for_testing(std::function<std::chrono::system_clock::time_point()> source) {
        host_utc_source_ = std::move(source);
    }
    void set_clock_sync_resync_hook_for_testing(std::function<void()> hook) {
        clock_sync_resync_hook_ = std::move(hook);
    }
    void set_connected_entry_hook_for_testing(std::function<void()> hook) {
        set_connected_entry_hook_ = std::move(hook);
    }
    void set_host_clock_busy_hook_for_testing(std::function<void()> hook) { host_clock_busy_hook_ = std::move(hook); }
    void set_clock_sync_before_utc_exchange_hook_for_testing(std::function<void()> hook) {
        clock_sync_before_utc_exchange_hook_ = std::move(hook);
    }
    void set_connected_exit_hook_for_testing(std::function<void()> hook) { set_connected_exit_hook_ = std::move(hook); }
    std::optional<std::string> clock_sync_failure_for_testing() const { return clock_sync_.last_failure(); }

private:
    std::chrono::system_clock::time_point current_utc_time_locked() const {
        if (const auto utc = utc_offset_.current(clock_.now())) {
            return *utc;
        }
        return std::chrono::system_clock::now();
    }

    static std::pair<double, double> alt_az_to_ra_dec(double altitude_degrees,
                                                      double azimuth_degrees,
                                                      double latitude_degrees,
                                                      double longitude_degrees,
                                                      std::chrono::system_clock::time_point utc_time) {
        const double deg_to_rad = std::numbers::pi / 180.0;
        const double rad_to_deg = 180.0 / std::numbers::pi;
        const double alt_rad = altitude_degrees * deg_to_rad;
        const double az_rad = azimuth_degrees * deg_to_rad;
        const double lat_rad = latitude_degrees * deg_to_rad;

        const double sin_alt = std::sin(alt_rad);
        const double cos_alt = std::cos(alt_rad);
        const double sin_lat = std::sin(lat_rad);
        const double cos_lat = std::cos(lat_rad);

        double sin_dec = sin_alt * sin_lat + cos_alt * cos_lat * std::cos(az_rad);
        sin_dec = std::clamp(sin_dec, -1.0, 1.0);
        const double dec_rad = std::asin(sin_dec);
        const double cos_dec = std::cos(dec_rad);

        double ha_rad = 0.0;
        if (std::abs(cos_lat) > 1e-12 && std::abs(cos_dec) > 1e-12) {
            const double sin_h = -std::sin(az_rad) * cos_alt / cos_dec;
            const double cos_h = (sin_alt - sin_lat * sin_dec) / (cos_lat * cos_dec);
            ha_rad = std::atan2(sin_h, cos_h);
        }

        const double ha_hours = ha_rad * 12.0 / std::numbers::pi;
        const double lst_hours = util::local_sidereal_hours(utc_time, longitude_degrees);
        const double ra_hours = util::wrap_hours(lst_hours - ha_hours);
        const double dec_degrees = dec_rad * rad_to_deg;
        return {ra_hours, dec_degrees};
    }

    static int normalize_side_of_pier_value(int side) {
        return (side == 0 || side == 1 || side == 2) ? side : 2;
    }

    static const std::vector<double>& axis_rate_steps_deg_per_sec() {
        static const std::vector<double> rates = [] {
            const double sidereal_deg_per_sec = 15.0 / 3600.0;
            std::vector<double> values = {
                sidereal_deg_per_sec * 1.0,
                sidereal_deg_per_sec * 2.0,
                sidereal_deg_per_sec * 8.0,
                sidereal_deg_per_sec * 16.0,
                sidereal_deg_per_sec * 64.0,
                sidereal_deg_per_sec * 128.0,
                sidereal_deg_per_sec * 256.0,
                sidereal_deg_per_sec * 512.0,
                6.0
            };
            return values;
        }();
        return rates;
    }

    static bool is_axis_rate_supported(double abs_rate) {
        if (abs_rate == 0.0) {
            return true;
        }
        const auto& rates = axis_rate_steps_deg_per_sec();
        for (double rate : rates) {
            if (std::abs(abs_rate - rate) <= 1e-6) {
                return true;
            }
        }
        return false;
    }

    static int arrow_speed_level_for_rate(double abs_rate) {
        const auto& rates = axis_rate_steps_deg_per_sec();
        double best_delta = std::numeric_limits<double>::max();
        int best_index = 0;
        for (size_t i = 0; i < rates.size(); ++i) {
            const double delta = std::abs(abs_rate - rates[i]);
            if (delta < best_delta) {
                best_delta = delta;
                best_index = static_cast<int>(i);
            }
        }
        return std::clamp(best_index + 1, 1, 9);
    }

    bool should_flip_dec_for_pier_locked() const {
        if (cached_side_of_pier_ == 0 || cached_side_of_pier_ == 1) {
            return (cached_side_of_pier_ == 1);
        }
        return false;
    }

    // Task waits, settle/poll sleeps and deadlines run on this clock (open-astro#1223,
    // decision 0005). Real by default; UTC pointing time stays on system_clock
    // (decision 0001) and serial/TCP I/O timeouts stay real. Declared first:
    // later members are initialised from it.
    util::TaskClock& clock_;
    int device_number_;
    ConnectionInfo connection_info_;
    // Set by the auto-detect factories; empty for an explicit port or host.
    // connection_resolved_ is true once a connect has run the resolver, so a
    // later connect retries that endpoint before scanning again.
    util::ConnectionResolver<ConnectionInfo> connection_resolver_;
    int location_index_ = 0;  // the mount index the resolver searches, part of the location key
    bool connection_resolved_ = false;
    std::atomic<bool> connected_{false};
    // What get_connected() reads: true only once a connect has fully finished (#910).
    std::atomic<bool> published_connected_{false};
    mutable MountInfo mount_info_;
    std::mutex transition_mutex_;
    // The operation lock (decision 0016): held by every member that reaches the mount for the whole member, and
    // by set_connected() from its first state write to its end. Taken before mutex_, never under it.
    mutable std::mutex io_mutex_;
    // The state lock, a leaf: never held across an exchange (Wire::call drops it for the call).
    mutable std::mutex mutex_;
    // Which of the two this thread holds (util::LockGate, #1729); it refers to the two mutexes above.
    const util::LockGate lock_gate_{io_mutex_, mutex_, "iOptron"};
    bool client_disagreement_warned_ = false;  // open-astro#409, re-armed on connect

    // Target coordinates
    double target_ra_hours_;
    double target_dec_degrees_;
    std::atomic<double> aperture_diameter_m_ = 0.0;
    std::atomic<double> focal_length_m_ = 0.0;
    int slew_settle_time_seconds_ = 0;
    double custom_tracking_rate_ = 1.0;
    // open-astro#346 (the shape #304 fixed on the Sky-Watcher driver): ASCOM
    // treats the two target properties as independent, so each must throw
    // ValueNotSet until that property itself has been written. One shared flag
    // let a write to either unlock both, and a client reading the one it did
    // not set got a default 0 instead of an error. The paths that legitimately
    // define both coordinates at once -- the slew and sync coordinate forms,
    // the position-override and arrival reads, and the connect/disconnect
    // resets -- still set or clear both.
    bool target_ra_set_ = false;
    bool target_dec_set_ = false;
    // Decision 0022: the pulse-guide windows, on the task clock. IsPulseGuiding's runs for the pulse plus
    // kPulseGuideCompletionDelay; the getter reads it without mutex_ (written under it), hence the atomic.
    mutable std::atomic<util::HoldWindow> pulse_guide_hold_{};
    static_assert(std::atomic<util::HoldWindow>::is_always_lock_free);
    // Per axis (kRaAxis, kDecAxis), under mutex_: the hold serves the axis a pulse on the other axis does not
    // move (pulse_guiding_hold_ra_hours_ / pulse_guiding_hold_dec_degrees_) and the correction serves the pulsed
    // axis's predicted position to its first read.
    static constexpr std::size_t kRaAxis = 0;
    static constexpr std::size_t kDecAxis = 1;
    mutable std::array<util::HoldWindow, 2> pulse_position_hold_{};
    mutable std::array<util::HoldWindow, 2> pulse_position_correction_{};
    mutable double pulse_guiding_hold_ra_hours_ = 0.0;
    mutable bool last_ra_read_valid_ = false;
    mutable double last_ra_read_hours_ = 0.0;
    mutable double pulse_guiding_ra_baseline_hours_ = 0.0;
    mutable double pulse_guiding_ra_expected_delta_hours_ = 0.0;
    mutable double pulse_guiding_dec_baseline_degrees_ = 0.0;
    mutable double pulse_guiding_dec_expected_delta_degrees_ = 0.0;
    mutable double pulse_guiding_hold_dec_degrees_ = 0.0;
    mutable bool last_dec_read_valid_ = false;
    mutable double last_dec_read_degrees_ = 0.0;
    
    // Cached mount information
    mutable double site_latitude_cached_;
    mutable double site_longitude_cached_;
    mutable bool site_info_valid_;
    mutable bool hemisphere_north_;
    mutable std::optional<double> site_elevation_m_;  // nullopt until set or configured
    mutable int timezone_offset_minutes_;
    mutable bool timezone_offset_valid_;
    mutable bool dst_observed_;
    mutable std::chrono::steady_clock::time_point last_site_info_fetch_;
    
    mutable double sync_offset_ra_hours_ = 0.0;
    mutable double sync_offset_dec_degrees_ = 0.0;

    mutable double cached_ra_hours_ = 0.0;
    mutable double cached_dec_degrees_ = 0.0;
    mutable int cached_side_of_pier_ = -1;
    mutable bool position_cache_valid_;
    mutable std::chrono::steady_clock::time_point last_position_update_;
    mutable double cached_alt_degrees_ = 0.0;
    mutable double cached_az_degrees_ = 0.0;
    mutable bool altaz_cache_valid_;
    mutable std::chrono::steady_clock::time_point last_altaz_update_;
    mutable GuideRate cached_guide_rate_{};
    mutable bool guide_rate_valid_;

    mutable bool dec_guide_calibration_attempted_ = false;
    mutable bool dec_guide_calibrated_ = false;
    mutable bool dec_guide_inverted_ = true;
    mutable int dec_guide_calibration_side_ = -1;
    std::atomic<bool> dec_guide_calibration_in_progress_{false};
    
    mutable MountStatus cached_status_{};
    mutable bool status_cache_valid_;
    mutable std::chrono::steady_clock::time_point last_status_update_;
    mutable bool axis_move_active_primary_ = false;
    mutable bool axis_move_active_secondary_ = false;
    mutable std::optional<bool> tracking_state_before_move_{};
    mutable std::chrono::steady_clock::time_point park_override_until_{};
    // HAE29C wedged-park finalizer state — see park()/finalize_wedged_park_locked().
    mutable bool park_finalize_pending_ = false;
    mutable bool park_target_valid_ = false;
    mutable AltAz park_target_{};

    mutable util::UtcOffset utc_offset_;
    mutable bool slew_in_progress_ = false;
    // Issue #2396 (AB-9): a GOTO or MoveAxis was running when the link was lost and no :Q# has reached the mount
    // since; the next Connect that opens the port sends it first. Under mutex_.
    bool relink_stop_pending_ = false;
    // Under mutex_: a fresh status showed the mount stopped while slew_in_progress_ was still set (nobody read
    // Slewing). Reset by every GOTO initiator, by dispatch_slew_command_locked() and by a settle trim dispatch.
    mutable bool goto_seen_stopped_ = false;
    // Firmware motion from a Park or FindHome the driver started (no slew_in_progress_ for those), or
    // from a GOTO whose dispatch a MoveAxis superseded: keeps get_slewing() true when a PulseGuide
    // arrives mid-motion. Set by park()/find_home() and move_axis(), cleared by
    // refresh_status_cache_locked() once a fresh status reports at home after a FindHome, or no motion
    // past the initiator's override, and by AbortSlew, any new initiator but MoveAxis
    // (supersede_slew_dispatch_locked, which Unpark also calls) and disconnect.
    // HomeSearch (issue #2420) is a FindHome running the :MSH# zero sensor search: MoveAxis keeps it like the others,
    // and it ends only on status 7 past its hold, on its bound (home_search_deadline_) or on AbortSlew, a new
    // initiator or a disconnect.
    enum class DriverMotion : std::uint8_t { None, Park, Home, HomeSearch, Goto };
    // Under mutex_: the GOTO in slew_in_progress_ came from SlewToCoordinates* or SlewToTarget* (which route through
    // them), so AbortSlew turns Tracking back on (issue #2000). False for an Alt/Az slew, cleared by a superseding
    // initiator and by AbortSlew.
    bool equatorial_slew_ = false;
    mutable DriverMotion driver_motion_ = DriverMotion::None;
    // Task-clock instant past which a zero search with no status 7 is stopped (issue #2420). Set by find_home() for a
    // zero search only (#2432) and read only while driver_motion_ is HomeSearch (the status refresh and the zero
    // search watchdog), so the sites that end the motion need not reset it.
    std::chrono::steady_clock::time_point home_search_deadline_{};
    // Set by find_home() for a zero search and read only while driver_motion_ is HomeSearch: a fresh status since
    // :MSH# showed motion (status 2), and :MSH# was already sent a second time (check_zero_search_started_locked()).
    mutable bool home_search_seen_moving_ = false;
    mutable bool home_search_resent_ = false;
    // A zero search that ended without a landing (the driver stopped it because it never started or its bound
    // fired, or AbortSlew, a superseding command, a connect or a disconnect ended it before any status 2): AtHome
    // stays false while the mount reads 7, since that 7 is the false zero, until a new FindHome or a status other
    // than 7. Kept across a reconnect, which does not move the mount. Under mutex_.
    mutable bool home_search_unconfirmed_ = false;
    // Until this task-clock instant a zero search with no status 2 yet is still starting: set with each :MSH#.
    mutable std::chrono::steady_clock::time_point home_search_start_until_{};
    static constexpr std::chrono::seconds kZeroSearchStartGrace{5};
    // The watchdog's status reads until a search shows motion (status 2); the shortest traced search read 2 for
    // about a second before its 7. Read and written only under the operation lock (the watchdog body, find_home()).
    mutable std::chrono::steady_clock::time_point home_search_next_poll_{};
    static constexpr std::chrono::milliseconds kZeroSearchPollInterval{1000};
    // Test seam (set_zero_search_poll_for_testing): off, the watchdog waits only for the bound, as before the polling,
    // so a case about client reads is not raced by the watchdog's reads. On in production.
    std::atomic<bool> zero_search_poll_enabled_{true};
    // About 20 s from Orion on RC51 (HAE29C, 2026-11-19); the bound covers a worst-case start with margin.
    static constexpr std::chrono::seconds kZeroSearchTimeout{120};
    // open-astro#575: an async slew that fails AFTER the initiator returned
    // (mount rejects the GOTO, soft-failed and logged as a no-op) used to
    // leave Slewing read FALSE -- indistinguishable from a landed goto. Set
    // (under mutex_) on a REAL failure (never on the dispatch body's own
    // cancellation), cleared by the next slew initiator and by AbortSlew.
    // Consulted by get_slewing() before the cached status.
    mutable std::string last_slew_error_;
    std::uint64_t slew_operation_generation_ = 0;
    static constexpr int kMaxSlewRefines = 3;
    mutable int slew_refine_count_ = 0;
    mutable double slew_target_ra_hours_ = 0.0;
    mutable double slew_target_dec_degrees_ = 0.0;
    // Written under mutex_; atomic so the lock-free IsPulseGuiding can refuse on a fault (decision 0009 rule 1).
    mutable std::atomic<bool> device_faulted_{false};
    // When the latch tripped, in task-clock ticks (decision 0009 rule 2b); read only while device_faulted_ is set.
    mutable std::atomic<std::chrono::steady_clock::rep> device_fault_since_ticks_{0};
    // The earliest task-clock time check_connected() may probe a latched fault; under mutex_.
    mutable std::chrono::steady_clock::time_point next_fault_probe_{};
    mutable int device_fault_count_ = 0;
    mutable std::string last_device_error_;
    mutable std::mutex link_fault_mutex_;
    mutable std::string link_fault_text_;
    mutable bool utc_query_supported_;
    mutable std::chrono::steady_clock::time_point fast_cache_until_{};
    mutable std::chrono::steady_clock::time_point tracking_override_until_{};
    mutable std::chrono::steady_clock::time_point tracking_rate_override_until_{};
    // Decision 0022: Slewing reads true while it is active, from a GOTO, Park, FindHome, Unpark or GOTO trim
    // dispatch for that initiator's hold. Under mutex_.
    mutable util::HoldWindow slew_hold_;
    // Serializes clock-sync reapers across the join; see stop_clock_sync().
    std::mutex clock_sync_reap_mutex_;
    // Test seam, set before connect: runs on the clock-sync body after its start delay.
    std::function<void()> clock_sync_test_hook_;
    // Test hook: called with false before each :SUT send attempt (operation lock held), true after the sends.
    std::function<void(bool)> clock_sync_utc_hook_;
    // Test seam, set before connect: the host UTC the clock sync writes and set_utc_date() compares (#2419).
    std::function<std::chrono::system_clock::time_point()> host_utc_source_;
    // Test seams (issue #2432), set before the call they hook: run on a re-sync's body once its motion wait found
    // the mount still, and at the start of set_connected() with transition_mutex_ held.
    std::function<void()> clock_sync_resync_hook_;
    std::function<void()> set_connected_entry_hook_;
    // Test seams (issue #2419): run when on_host_clock_changed() finds transition_mutex_ held (no driver lock held),
    // and as set_connected()'s last step before it releases transition_mutex_.
    std::function<void()> host_clock_busy_hook_;
    std::function<void()> set_connected_exit_hook_;
    // Test seam (issue #2419): runs before each attempt of the clock sync's UTC write, before its exchange, with no
    // driver lock held.
    std::function<void()> clock_sync_before_utc_exchange_hook_;
    // Issue #2419: a host clock change not acted on yet: stored before on_host_clock_changed() tries
    // transition_mutex_, drained by whoever holds that lock as it lets go.
    std::atomic<bool> resync_pending_{false};
    // Issue #2419, under mutex_: a clock-sync body of this session ran to its end (cleared by connect). Until then a
    // re-sync that superseded the connect's pass runs that pass in its place.
    bool clock_sync_connect_pass_done_ = false;
    // Bumped by every client UTCDate write (under the operation lock); the clock sync compares it at publication,
    // and a host clock re-sync also at its :SUT against the value it was decided with (read under transition_mutex_
    // only, hence atomic).
    std::atomic<uint64_t> utc_write_generation_{0};
    // Bumped by each client site write (under mutex_), compared like utc_write_generation_; one per field.
    uint64_t site_latitude_write_generation_ = 0;
    uint64_t site_longitude_write_generation_ = 0;
    uint64_t site_elevation_write_generation_ = 0;
    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;
    const std::function<void()> before_slew_worker_lock_;
    const std::function<void()> before_slew_reap_;
    const std::function<void()> before_handoff_lock_;
    std::mutex slew_handoff_mutex_;
    // The async slew dispatch (decision 0006), declared last so the slot is destroyed, and its body
    // joined, before any member the body touches. Private generation: only start() bumps it, while the
    // driver's own slew_operation_generation_ stays the supersede guard (MoveAxis, Park and FindHome bump
    // it without the handoff lock).
    util::OperationGeneration slew_slot_generation_;
    util::AsyncOperation slew_{"iOptron slew", slew_slot_generation_, clock_};
    // The connect-time clock-sync body (decision 0006); its own generation, so a slew start never
    // supersedes it. Declared after everything the body touches.
    util::OperationGeneration clock_sync_generation_;
    util::AsyncOperation clock_sync_{"iOptron clock sync", clock_sync_generation_, clock_};
    // Issue #2432: the zero search's bound (run_zero_search_watchdog()); its own generation, so no other start
    // supersedes it. Declared after everything the body touches.
    util::OperationGeneration zero_search_watchdog_generation_;
    util::AsyncOperation zero_search_watchdog_{"iOptron zero search watchdog", zero_search_watchdog_generation_,
                                               clock_};
};

// Factory function implementation
std::unique_ptr<TelescopeDriver> create_ioptron_telescope(int device_number, const ConnectionInfo& connection_info,
                                                          util::TaskClock& clock) {
    return std::make_unique<iOptronTelescopeDriver>(device_number, connection_info, std::nullopt, std::nullopt,
                                                    std::nullopt, std::nullopt,
                                                    util::ConnectionResolver<ConnectionInfo>{}, std::function<void()>{},
                                                    std::function<void()>{}, std::function<void()>{}, clock);
}

std::unique_ptr<TelescopeDriver> create_ioptron_telescope_with_slew_worker_hook(
    int device_number, const ConnectionInfo& connection_info, std::function<void()> before_worker_lock,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::function<void()> before_slew_reap, std::function<void()> before_handoff_lock, util::TaskClock& clock) {
    return std::make_unique<iOptronTelescopeDriver>(
        device_number, connection_info, site_latitude_deg, site_longitude_deg, std::nullopt, std::nullopt,
        util::ConnectionResolver<ConnectionInfo>{}, std::move(before_worker_lock), std::move(before_slew_reap),
        std::move(before_handoff_lock), clock);
}

std::unique_ptr<TelescopeDriver> create_ioptron_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, util::TaskClock& clock) {
    return std::make_unique<iOptronTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                    site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                    util::ConnectionResolver<ConnectionInfo>{}, std::function<void()>{},
                                                    std::function<void()>{}, std::function<void()>{}, clock);
}

std::unique_ptr<TelescopeDriver> create_ioptron_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect, util::TaskClock& clock) {
    if (!connection_resolver) {
        throw AlpacaException("iOptron telescope: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<iOptronTelescopeDriver>(device_number, ConnectionInfo{}, site_latitude_deg,
                                                    site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                    std::move(connection_resolver), std::function<void()>{},
                                                    std::function<void()>{}, std::function<void()>{}, clock);
}

ConnectionInfo resolve_ioptron_serial_auto(int mount_index, const std::string& location_key) {
    // The port that worked last time goes first: one :MountInfo# instead of the full scan.
    // :MountInfo# answers a model code only, so the check cannot tell two mounts of one model
    // apart; a port a connected driver holds is never probed (decision 0017).
    if (!location_key.empty()) {
        if (const auto remembered = util::recall_serial_location(
                location_key, [](const std::string& path) { return !probe_ioptron_serial_port(path).empty(); })) {
            ALPACA_LOG_INFO("iOptron", "Mount answered on remembered port " + *remembered);
            ConnectionInfo conn;
            conn.type = ConnectionType::Serial;
            conn.port_path = *remembered;
            conn.baud_rate = 115200;
            return conn;
        }
    }
    auto ports = enumerate_ioptron_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("iOptron mount"));
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) +
                              " out of range (found " + std::to_string(ports.size()) + " mount(s))");
    }

    const auto& port = ports[static_cast<std::size_t>(mount_index)];
    std::string model = model_code_to_name(port.model_code);
    ALPACA_LOG_INFO("iOptron", "Auto-detected mount on " + port.port_path +
                    " (model " + port.model_code +
                    (model.empty() ? "" : " / " + model) + ")");

    ConnectionInfo conn;
    conn.type = ConnectionType::Serial;
    conn.port_path = port.port_path;
    conn.baud_rate = 115200;
    return conn;
}

ConnectionInfo resolve_ioptron_network_auto(int mount_index, const std::string& location_key) {
    // The address that worked last time goes first: one handshake instead of the 2-5 s sweep.
    if (!location_key.empty()) {
        std::string host;
        int tcp_port = 0;
        const auto remembered = util::DeviceLocationStore::load(location_key);
        if (remembered && util::parse_network_location(*remembered, host, tcp_port)) {
            if (!probe_ioptron_network_host(host, tcp_port).empty()) {
                ALPACA_LOG_INFO("iOptron", "Mount answered at remembered address " + *remembered);
                ConnectionInfo conn;
                conn.type = ConnectionType::Network;
                conn.host = host;
                conn.tcp_port = tcp_port;
                return conn;
            }
            ALPACA_LOG_INFO("iOptron", "Remembered address " + *remembered + " did not answer; searching");
        }
    }
    ALPACA_LOG_INFO("iOptron", "Starting network auto-discovery...");
    auto hosts = enumerate_ioptron_network_hosts();
    if (hosts.empty()) {
        throw AlpacaException(
            "No iOptron mount found on the local network. "
            "Tried known default addresses and scanned local subnets on ports 8899 & 4030. "
            "Check the logs for details. Verify the mount WiFi is connected and the mount is powered on.");
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(hosts.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) +
                              " out of range (found " + std::to_string(hosts.size()) + " mount(s) on network)");
    }

    const auto& found = hosts[static_cast<std::size_t>(mount_index)];
    std::string model = model_code_to_name(found.model_code);
    ALPACA_LOG_INFO("iOptron", "Network auto-detected mount at " + found.host +
                    ":" + std::to_string(found.tcp_port) +
                    " (model " + found.model_code +
                    (model.empty() ? "" : " / " + model) + ")");

    ConnectionInfo conn;
    conn.type = ConnectionType::Network;
    conn.host = found.host;
    conn.tcp_port = found.tcp_port;
    return conn;
}

std::unique_ptr<TelescopeDriver> create_ioptron_telescope_auto(int device_number, int mount_index,
                                                               std::optional<double> site_latitude_deg,
                                                               std::optional<double> site_longitude_deg,
                                                               std::optional<double> site_elevation_m,
                                                               std::optional<bool> sync_time_on_connect,
                                                               util::TaskClock& clock) {
    // The serial scan runs at connect time (#659), not here.
    auto driver = std::make_unique<iOptronTelescopeDriver>(
        device_number, ConnectionInfo{}, site_latitude_deg, site_longitude_deg, site_elevation_m, sync_time_on_connect,
        util::ConnectionResolver<ConnectionInfo>{}, std::function<void()>{}, std::function<void()>{},
        std::function<void()>{}, clock);
    const iOptronTelescopeDriver* self = driver.get();
    driver->set_location_index(mount_index);
    driver->set_connection_resolver([self, mount_index] {
        return resolve_ioptron_serial_auto(mount_index, util::location_key(self->get_unique_id(), mount_index));
    });
    return driver;
}

std::unique_ptr<TelescopeDriver> create_ioptron_telescope_auto_network(int device_number, int mount_index,
                                                                       std::optional<double> site_latitude_deg,
                                                                       std::optional<double> site_longitude_deg,
                                                                       std::optional<double> site_elevation_m,
                                                                       std::optional<bool> sync_time_on_connect,
                                                                       util::TaskClock& clock) {
    // The subnet sweep runs at connect time (#659), not here: at server
    // start-up the mount is usually still joining the access point.
    auto driver = std::make_unique<iOptronTelescopeDriver>(
        device_number, ConnectionInfo{}, site_latitude_deg, site_longitude_deg, site_elevation_m, sync_time_on_connect,
        util::ConnectionResolver<ConnectionInfo>{}, std::function<void()>{}, std::function<void()>{},
        std::function<void()>{}, clock);
    const iOptronTelescopeDriver* self = driver.get();
    driver->set_location_index(mount_index);
    driver->set_connection_resolver([self, mount_index] {
        return resolve_ioptron_network_auto(mount_index, util::location_key(self->get_unique_id(), mount_index));
    });
    return driver;
}

void set_slew_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_slew_spawn_for_testing(std::move(spawn));
    }
}

void set_zero_search_watchdog_spawn_for_testing(TelescopeDriver& driver,
                                                std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_zero_search_watchdog_spawn_for_testing(std::move(spawn));
    }
}

void set_zero_search_poll_for_testing(TelescopeDriver& driver, bool enabled) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_zero_search_poll_for_testing(enabled);
    }
}

void set_clock_sync_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_clock_sync_hook_for_testing(std::move(hook));
    }
}

void set_clock_sync_utc_hook_for_testing(TelescopeDriver& driver, std::function<void(bool)> hook) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_clock_sync_utc_hook_for_testing(std::move(hook));
    }
}

void start_clock_sync_for_testing(TelescopeDriver& driver) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->start_clock_sync_for_testing();
    }
}

void set_host_utc_source_for_testing(TelescopeDriver& driver,
                                     std::function<std::chrono::system_clock::time_point()> source) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_host_utc_source_for_testing(std::move(source));
    }
}

void set_clock_sync_resync_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_clock_sync_resync_hook_for_testing(std::move(hook));
    }
}

void set_connected_entry_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_connected_entry_hook_for_testing(std::move(hook));
    }
}

void set_host_clock_busy_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_host_clock_busy_hook_for_testing(std::move(hook));
    }
}

void set_connected_exit_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_connected_exit_hook_for_testing(std::move(hook));
    }
}

void set_clock_sync_before_utc_exchange_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_clock_sync_before_utc_exchange_hook_for_testing(std::move(hook));
    }
}

std::optional<std::string> clock_sync_failure_for_testing(const TelescopeDriver& driver) {
    if (const auto* ioptron = dynamic_cast<const iOptronTelescopeDriver*>(&driver)) {
        return ioptron->clock_sync_failure_for_testing();
    }
    return std::nullopt;
}

} // namespace alpacacore::vendor::ioptron
