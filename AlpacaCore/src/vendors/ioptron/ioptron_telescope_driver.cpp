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
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/lock_gate.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/task_clock.h>
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
    // Some sequences must not be split: :SRA#/:Sd# set the target the :MS1#/:MS2# after them uses, and a
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
        SiteInfo get_site_location() const {
            return call([](auto& p) { return p.get_site_location(); });
        }
        AltAz get_park_position() const {
            return call([](auto& p) { return p.get_park_position(); });
        }
        std::chrono::system_clock::time_point get_utc_time() const {
            return call([](auto& p) { return p.get_utc_time(); });
        }
        void set_target_ra(double ra_hours) const {
            call([&](auto& p) { p.set_target_ra(ra_hours); });
        }
        void set_target_dec(double dec_degrees) const {
            call([&](auto& p) { p.set_target_dec(dec_degrees); });
        }
        bool slew_to_ra_dec() const {
            return call([](auto& p) { return p.slew_to_ra_dec(); });
        }
        bool slew_to_ra_dec_cw_up() const {
            return call([](auto& p) { return p.slew_to_ra_dec_cw_up(); });
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
        // costs nothing on the normal path.
        stop_clock_sync();
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
    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

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
        std::lock_guard<std::mutex> transition(transition_mutex_);
        const Wire protocol = wire();
        if (!connected || (connected_ && link_lost())) {
            // A disconnect, or a connect over a lost link (decision 0009 rule 4), ends the session's bodies
            // first, with no driver lock held: the clock-sync body and the slew dispatch take both locks, so a
            // join under either would deadlock, and a body mid-exchange must finish that exchange and then see
            // its stop before this call takes the operation lock, so it sends nothing more (the clock-sync
            // slot cases). transition_mutex_ keeps every other connect and disconnect out meanwhile.
            stop_clock_sync();
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
            site_info_valid_ = false;
            position_cache_valid_ = false;
            altaz_cache_valid_ = false;
            status_cache_valid_ = false;
            guide_rate_valid_ = false;
            utc_offset_.clear();
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
            guide_rate_calibration_attempted_ = false;
            guide_rate_calibrated_ = false;
            guide_rate_scale_ra_ = 1.0;
            guide_rate_scale_dec_ = 1.0;
            axis_move_active_primary_ = false;
            axis_move_active_secondary_ = false;
            tracking_state_before_move_.reset();
            park_override_until_ = std::chrono::steady_clock::time_point{};
            park_finalize_pending_ = false;
            tracking_rate_override_until_ = std::chrono::steady_clock::time_point{};
            clear_hold_windows_locked();
            slew_in_progress_ = false;
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
            // The mount clock is sampled once, before Connected is published (the getter makes no exchange).
            util::UtcOffset mount_time;
            try {
                const auto utc = protocol.get_utc_time();
                mount_time.set(utc, clock_.now());
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG("iOptron", "Mount UTC unavailable on connect: " + std::string(e.what()));
            }
            utc_offset_ = mount_time;
            // The caches are primed before connected_ is published: a getter that arrives during these reads
            // answers NotConnected at once (check_connected_before_lock()) instead of waiting behind them.
            prefetch_mount_state_locked();
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
        guide_rate_calibration_attempted_ = false;
        guide_rate_calibrated_ = false;
        guide_rate_scale_ra_ = 1.0;
        guide_rate_scale_dec_ = 1.0;
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
    
    double get_aperture_diameter() const override {
        return aperture_diameter_m_;
    }

    void set_aperture_diameter(double meters) override { aperture_diameter_m_ = meters; }

    double get_aperture_area() const override {
        // Derived from the atomic diameter so a live updatedevice cannot leave a stale area (#2057).
        const double d = aperture_diameter_m_;
        return d > 0.0 ? std::numbers::pi * (d / 2.0) * (d / 2.0) : 0.0;
    }
    
    bool get_at_home() const override {
        return serve_cached_or_refill([&] { return status_cache_fresh_locked(); },
                                      [&] {
                                          refresh_status_cache_locked();
                                          return cached_status_.is_at_home;
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
        return focal_length_m_;
    }
    
    void set_focal_length(double meters) override {
        focal_length_m_ = meters;
    }
    
    GuideRate get_guide_rate() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        
        try {
            if (guide_rate_valid_) {
                return cached_guide_rate_;
            }
            cached_guide_rate_.ra = kDefaultGuideRateFraction * kSiderealRateDegPerSec;
            cached_guide_rate_.dec = kDefaultGuideRateFraction * kSiderealRateDegPerSec;
            guide_rate_valid_ = true;
            return cached_guide_rate_;
        } catch (const std::exception& e) {
            record_device_fault_locked("GuideRate", e.what());
            throw AlpacaException(std::string("Failed to read guide rates: ") + e.what(),
                                  AlpacaError::DriverException);
        }
    }
    
    void set_guide_rate(const GuideRate& rate) override {
        if (!std::isfinite(rate.ra) || !std::isfinite(rate.dec)) {
            throw AlpacaException("GuideRate must be a finite number", AlpacaError::InvalidValue);
        }
        StateLock lock(lock_gate_);
        check_connected();
        
        const double scale_ra = guide_rate_calibrated_ ? guide_rate_scale_ra_ : 1.0;
        const double scale_dec = guide_rate_calibrated_ ? guide_rate_scale_dec_ : 1.0;
        const Wire protocol = wire();
        double ra_fraction = rate.ra / (kSiderealRateDegPerSec * scale_ra);
        double dec_fraction = rate.dec / (kSiderealRateDegPerSec * scale_dec);
        ra_fraction = std::clamp(ra_fraction, 0.01, 0.90);
        dec_fraction = std::clamp(dec_fraction, 0.10, 0.99);
        protocol.set_guide_rates(ra_fraction, dec_fraction);
        cached_guide_rate_ = rate;
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
        return slew_settle_time_seconds_;
    }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException(
                "Slew settle time must be >= 0 seconds",
                AlpacaError::InvalidValue
            );
        }
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
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 (Primary) or 1 (Secondary)",
                                  AlpacaError::InvalidValue);
        }

        double abs_rate = std::abs(rate);
        if (!is_axis_rate_supported(abs_rate)) {
            throw AlpacaException("Axis rate out of range", AlpacaError::InvalidValue);
        }

        StateLock lock(lock_gate_);
        check_connected();
        ensure_not_parked_locked("MoveAxis");

        const bool was_any_axis_active = axis_move_active_primary_ || axis_move_active_secondary_;
        const Wire protocol = wire();
        if (abs_rate > 0.0) {
            const int arrow_speed = arrow_speed_level_for_rate(abs_rate);
            std::ostringstream cmd;
            cmd << ":SR" << arrow_speed << "#";
            protocol.send_command_blind(cmd.str());
        }
        if (axis == 0) {
            if (rate > 0.0) {
                protocol.send_command_blind(":mw#"); // RA+ (west)
            } else if (rate < 0.0) {
                protocol.send_command_blind(":me#"); // RA- (east)
            } else {
                protocol.send_command_blind(":qR#"); // stop RA
            }
        } else if (axis == 1) {
            if (rate > 0.0 || rate < 0.0) {
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
        const DriverMotion motion = slew_in_progress_ ? DriverMotion::Goto : driver_motion_;
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
                       (status_cache_fresh_locked() &&
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
                if (!lock_gate_.holds_io() && !status_cache_fresh_locked()) {
                    throw RefillNeedsOperationLock{};
                }
                refresh_status_cache_locked();
                if (cached_status_.is_slewing) {
                    // open-astro#843: the firmware reports system status 2 (slewing) while it runs a pulse.
                    // ASCOM keeps Slewing false during PulseGuide (IsPulseGuiding carries it). The mask covers
                    // the pulse plus kPulseGuideCompletionDelay, and only with no driver-started GOTO still
                    // dispatching and no Park/FindHome still moving (driver_motion_).
                    return !(pulse_window_open() && !slew_in_progress_ && driver_motion_ == DriverMotion::None);
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
                            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
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
        if (rate < 0 || rate > 4) {
            throw AlpacaException("Invalid tracking rate", AlpacaError::InvalidValue);
        }
        StateLock lock(lock_gate_);
        check_connected();

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
        const auto client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)
        protocol.set_utc_time(utc);
        utc_offset_.set(utc, clock_.now());
        // The mount now runs on the client's clock and so does the cached
        // pointing time; on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("iOptron", client_minus_host, client_disagreement_warned_);
    }
    
    // Telescope methods
    
    void find_home() override {
        StateLock lock(lock_gate_);
        check_connected();
        
        if (!get_can_find_home()) {
            throw AlpacaException("Find home not supported on this mount model");
        }

        ensure_not_parked_locked("FindHome");
        // Force-refresh: a stale cached is_at_home (e.g. set before a park/
        // unpark cycle, which never touches it) short-circuits the whole home
        // operation and the client then sees AtHome == false.
        refresh_status_cache_locked(true);
        if (cached_status_.is_at_home) {
            return;
        }

        const Wire protocol = wire();
        // Use a direct go-to-zero command to avoid aggressive sensor searches.
        send_with_slew_hold_locked(std::chrono::seconds(5), [&] { protocol.go_to_home(); });
        supersede_slew_dispatch_locked();
        driver_motion_ = DriverMotion::Home;
        status_cache_valid_ = false;
        position_cache_valid_ = false;
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
        // open-astro#768: fence the async slew dispatch first, so no :MS1#/:MS2#
        // can reach the mount after the stop below. Runs without mutex_ held
        // (the dispatch body takes it), like every other reap site. Taking
        // the handoff lock can wait for a concurrent initiator's in-flight
        // dispatch transaction before the stop can be sent.
        reap_slew_dispatch();
        StateLock lock(lock_gate_);
        // open-astro#728: a latched fault must not stop a client stopping the
        // mount. Skip the latch check, the parked check (a status read) and the
        // cached slewing flag, none of which can be trusted on a faulted link,
        // and send the stop; a sent stop clears the latch. A parked mount is
        // sent the stop too: InvalidWhileParked is not raised while latched.
        const bool faulted = connected_ && device_faulted_;
        if (!faulted) {
            check_connected();
            ensure_not_parked_locked("AbortSlew");
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
        driver_motion_ = DriverMotion::None;
        axis_move_active_primary_ = false;
        axis_move_active_secondary_ = false;
        tracking_state_before_move_.reset();
        status_cache_valid_ = false;
        clear_slew_hold_locked();
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
        // 99999 ms is the protocol's 5-digit duration field.
        if (direction < 0 || direction > 3) {
            throw AlpacaException("PulseGuide direction must be 0-3", AlpacaError::InvalidValue);
        }
        if (duration < 0 || duration > 99999) {
            throw AlpacaException("PulseGuide duration must be 0-99999 ms", AlpacaError::InvalidValue);
        }
        StateLock lock(lock_gate_);
        check_connected();
        ensure_not_parked_fast_locked("PulseGuide");

        const Wire protocol = wire();
        int calibrated_direction = direction;
        const bool is_dec = (direction == 0 || direction == 1);
        const bool is_ra = (direction == 2 || direction == 3);
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
        util::validate_ra_dec(ra, dec, "SlewToCoordinates");
        {
            StateLock lock(lock_gate_);
            double phys_ra = util::wrap_hours(ra - sync_offset_ra_hours_);
            double phys_dec = dec - sync_offset_dec_degrees_;
            check_equatorial_slew_allowed_locked("SlewToCoordinates");
            prepare_slew_state_locked(ra, dec, phys_ra, phys_dec, "SlewToCoordinates");
            equatorial_slew_ = true;
            dispatch_slew_command_locked(phys_ra, phys_dec, nullptr, "SlewToCoordinates");
        }
        wait_for_slew_completion("SlewToCoordinates");
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        util::validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
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
            check_equatorial_slew_allowed_locked("SlewToCoordinatesAsync");
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
        }
        wait_for_slew_completion("SlewToAltAz");
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
    // The guide-rate calibration's settle after its sample pulses; kept at the old 1 s, not re-measured.
    static constexpr std::chrono::milliseconds kGuideCalibrationSettleDelay{1000};
    static constexpr std::chrono::milliseconds kPulseGuideHoldGrace{2000};
    static constexpr std::chrono::milliseconds kPulseGuideCorrectionGrace{5000};
    static constexpr double kSlewCompletionToleranceArcsec = 60.0;
    static constexpr double kSiderealSeconds = 86164.0905;
    static constexpr double kSiderealRateDegPerSec = 360.0 / kSiderealSeconds;
    static constexpr double kDefaultGuideRateFraction = 0.5;

    // The HAE29C park firmware-quirk workarounds (wedged-park finalizer,
    // zero-distance park finalizer) are gated on the exact model code so
    // other iOptron mounts (HAE43, HEM27, ...) keep the original,
    // hardware-validated behavior.
    bool hae29c_quirks_active() const { return mount_info_.model_code == "0036"; }

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
                } catch (const RefillNeedsOperationLock&) {  // NOLINT(bugprone-empty-catch)
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

    void prefetch_mount_state_locked() {
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
            SiteInfo site = protocol.get_site_info();
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

    // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation
    // (open-astro#1274). Runs after the parked check and before the new slew
    // supersedes a dispatch in flight, so a refusal leaves that motion alone.
    // Not part of prepare_slew_state_locked(): the Alt/Az slews share it.
    void check_equatorial_slew_allowed_locked(const char* label) const {
        check_connected();
        ensure_not_parked_locked(label);
        if (tracking_off_for_slew_locked()) {
            throw AlpacaException(std::string(label) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
    }

    // Not get_tracking_locked(): :GLS reports slewing (2), guiding (3) and a
    // meridian flip (4) instead of tracking, so the getter reads them as off,
    // and refusing there would refuse a retarget of a GOTO in flight. Off is a
    // commanded Tracking=false still in its read-back window, or a stopped
    // mount (0, or 7 at home); parked (6) is refused before this runs.
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
    void dispatch_slew_command_locked(double ra, double dec, util::OperationContext* async_ctx, const char* label) {
        const Wire protocol = wire();
        bool accepted = false;
        try {
            protocol.set_target_ra(ra);
            protocol.set_target_dec(dec);
            // Decision 0022 rule 2: re-armed before the :MS1# goes out, so the window runs from the GOTO itself
            // and not from an initiator whose body waited for mutex_.
            slew_hold_.arm(clock_.now(), std::chrono::seconds(5));
            accepted = protocol.slew_to_ra_dec();
            if (!accepted) {
                accepted = protocol.slew_to_ra_dec_cw_up();
            }
        } catch (...) {
            // A dispatch that failed (the synchronous initiators throw it on): no GOTO went out.
            slew_in_progress_ = false;
            clear_slew_hold_locked();
            throw;
        }
        if (!accepted) {
            slew_in_progress_ = false;
            clear_slew_hold_locked();
            if (async_ctx != nullptr) {
                // open-astro#575: a reap by a newer initiator is not a failure -- that
                // initiator already owns clearing/replacing last_slew_error_.
                // AbortSlew cancels and joins the slot (open-astro#768), so none can
                // record a failure after the abort cleared the error.
                if (async_ctx->stop_reason() == util::StopReason::None) {
                    last_slew_error_ = std::string(label) + " failed: mount rejected the target";
                }
                ALPACA_LOG_WARN("iOptron", "Slew rejected by mount - treating as no-op for async slew");
                return;
            }
            throw AlpacaException("Slew rejected by mount - target may violate altitude limits");
        }
        status_cache_valid_ = false;
        position_cache_valid_ = false;
    }

    void start_slew_to_coordinates_locked(double ra, double dec) {
        prepare_slew_state_locked(ra, dec, ra, dec, "SlewToCoordinates");
        dispatch_slew_command_locked(ra, dec, nullptr, "SlewToCoordinates");
    }

    void wait_for_slew_completion(const char* label) {
        const auto deadline = clock_.now() + kSlewTimeout;
        while (get_slewing()) {
            if (clock_.now() > deadline) {
                throw AlpacaException(std::string(label) + " timed out waiting for slew to complete");
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

            while (clock_.now() < deadline) {
                bool target_reached = false;
                double cur_ra = 0, cur_dec = 0;
                {
                    StateLock lock(lock_gate_);
                    target_reached = slew_target_reached_locked();
                    cur_ra = cached_ra_hours_;
                    cur_dec = cached_dec_degrees_;
                }

                if (target_reached) {
                    break;
                }

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
                    ALPACA_LOG_INFO("iOptron", std::string(label) +
                                    ": position stabilized but target not reached");
                    break;
                }

                clock_.sleep_for(kSlewPollInterval);
            }
        }

        if (slew_settle_time_seconds_ > 0) {
            clock_.sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
        }
        StateLock lock(lock_gate_, StateLock::Mode::State);
        slew_in_progress_ = false;
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
        // A fresh status ends a driver-started Park or FindHome whichever getter fetched it (AtHome, AtPark,
        // Tracking or Slewing): at home after a FindHome at once, any other no-motion status only past the
        // initiator's override (the firmware can still report its old status just after the command). A Park
        // needs no early end: a parked mount refuses PulseGuide and Unpark ends the window.
        // Runs after the finalizer, which may have re-read the status.
        const bool reached_home = driver_motion_ == DriverMotion::Home && cached_status_.is_at_home;
        if (reached_home || (!cached_status_.is_slewing && !slew_hold_.active(now))) {
            driver_motion_ = DriverMotion::None;
        }
    }

    // See park(): once an :MP1# park has physically arrived, some firmware
    // (HAE29C) stays wedged in "slewing" until tracking is explicitly turned
    // off. Runs after every fresh status fetch; best-effort — a failure here
    // just retries on the next refresh.
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

    GuideRate read_guide_rate_raw_locked() const {
        const Wire protocol = wire();
        auto [ra_rate, dec_rate] = protocol.get_guide_rates();
        const double scale_ra = guide_rate_calibrated_ ? guide_rate_scale_ra_ : 1.0;
        const double scale_dec = guide_rate_calibrated_ ? guide_rate_scale_dec_ : 1.0;
        cached_guide_rate_.ra = ra_rate * kSiderealRateDegPerSec * scale_ra;
        cached_guide_rate_.dec = dec_rate * kSiderealRateDegPerSec * scale_dec;
        guide_rate_valid_ = true;
        return cached_guide_rate_;
    }

    void calibrate_guide_rates_locked() {
        if (guide_rate_calibration_attempted_) {
            return;
        }
        guide_rate_calibration_attempted_ = true;

        refresh_status_cache_locked(true);
        if (cached_status_.is_slewing || cached_status_.is_parked) {
            ALPACA_LOG_WARN("iOptron", "Skipping guide calibration while slewing or parked");
            return;
        }

        const int sample_duration = 5000;
        const auto settle_delay = kGuideCalibrationSettleDelay;
        try {
            const Wire protocol = wire();
            auto [original_ra_rate, original_dec_rate] = protocol.get_guide_rates();
            const double test_ra_rate = std::clamp(0.5, 0.01, 0.90);
            const double test_dec_rate = std::clamp(original_dec_rate, 0.10, 0.99);
            protocol.set_guide_rates(test_ra_rate, test_dec_rate);
            clock_.sleep_for(std::chrono::milliseconds(300));

            auto read_position_stable = [&protocol, this]() {
                Position pos = protocol.get_position();
                for (int i = 0; i < 2; ++i) {
                    clock_.sleep_for(std::chrono::milliseconds(200));
                    pos = protocol.get_position();
                }
                return pos;
            };

            const Position baseline_start = protocol.get_position();
            clock_.sleep_for(std::chrono::milliseconds(sample_duration) + settle_delay);
            const Position baseline_end = read_position_stable();
            const double baseline_delta_hours =
                util::shortest_ra_delta_hours(baseline_end.ra_hours, baseline_start.ra_hours);

            const Position pulse_start = protocol.get_position();
            protocol.pulse_guide(2, sample_duration);
            clock_.sleep_for(std::chrono::milliseconds(sample_duration) + settle_delay);
            const Position pulse_end = read_position_stable();

            const double pulse_delta_hours = util::shortest_ra_delta_hours(pulse_end.ra_hours, pulse_start.ra_hours);
            const double guide_delta_hours = pulse_delta_hours - baseline_delta_hours;
            const double ra_delta_deg = std::abs(guide_delta_hours) * 15.0;
            const double duration_sec = sample_duration / 1000.0;
            const double actual_rate = (duration_sec > 0.0) ? (ra_delta_deg / duration_sec) : 0.0;
            const double requested_rate = test_ra_rate * kSiderealRateDegPerSec;
            const double min_delta_deg = 0.0005;  // ~1.8 arcsec
            if (requested_rate > 0.0 && actual_rate > 0.0 && ra_delta_deg >= min_delta_deg) {
                const double scale = actual_rate / requested_rate;
                guide_rate_scale_ra_ = std::clamp(scale, 0.1, 3.0);
                guide_rate_calibrated_ = true;
                ALPACA_LOG_INFO("iOptron", "Guide rate calibration: RA scale=" +
                                             std::to_string(guide_rate_scale_ra_));
            } else {
                ALPACA_LOG_WARN("iOptron", "Guide rate calibration inconclusive; using defaults");
            }
            protocol.set_guide_rates(original_ra_rate, original_dec_rate);
            guide_rate_valid_ = false;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", std::string("Guide rate calibration failed: ") + e.what());
        }

        calibrate_dec_guide_locked(sample_duration);
    }

    bool slew_target_reached_locked() const {
        try {
            refresh_position_cache_locked(true);
        } catch (const std::exception&) {
            throw_if_link_lost();  // a synchronous slew ends with NotConnected, not "position stabilized"
            return false;
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

    void sync_mount_clock_with_host_locked() {
        auto tz_info = compute_local_timezone_info();
        auto now_utc = std::chrono::system_clock::now();
        const Wire protocol = wire();

        if (retry_mount_command("timezone offset", [&]() { protocol.set_timezone_offset(tz_info.offset_minutes); })) {
            timezone_offset_minutes_ = tz_info.offset_minutes;
            timezone_offset_valid_ = true;
        }

        if (retry_mount_command("DST flag", [&]() { protocol.set_dst_observed(tz_info.dst_active); })) {
            dst_observed_ = tz_info.dst_active;
        }

        if (retry_mount_command("UTC clock", [&]() { protocol.set_utc_time(now_utc); })) {
            utc_offset_.set(now_utc, clock_.now());
        }
    }
    
    static LocalTimeInfo compute_local_timezone_info() {
        LocalTimeInfo info;
        std::time_t now = std::time(nullptr);
        std::tm local_tm {};
        std::tm utc_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &now);
        gmtime_s(&utc_tm, &now);
#else
        local_tm = *std::localtime(&now);
        utc_tm = *std::gmtime(&now);
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
    void start_clock_sync() {
        clock_sync_.start([this](util::OperationContext& ctx) {
            try {
                run_clock_sync(ctx);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("iOptron", std::string("Clock sync failed: ") + e.what());
                throw;
            }
        });
    }

    void run_clock_sync(util::OperationContext& ctx) {
        if (!ctx.wait_for(std::chrono::milliseconds(250))) {
            return;
        }
        if (clock_sync_test_hook_) {
            clock_sync_test_hook_();
        }

        std::optional<double> lat, lon;
        std::optional<int> elev;
        bool do_clock_sync = false;
        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                return;
            }
            lat = pending_site_latitude_;
            lon = pending_site_longitude_;
            elev = pending_site_elevation_;
            do_clock_sync = sync_time_on_connect_;
        }

        // Each command takes the operation lock and the state lock for itself, like a client's command, and
        // releases both before the retry wait: a getter or a command never waits behind a retry.
        const Wire protocol = wire();
        auto exchange = [this](auto&& send) {
            StateLock lock(lock_gate_);
            send();
        };

        if (lat.has_value()) {
            double latitude = lat.value();
            retry_mount_command("site latitude", [&]() { exchange([&] { protocol.set_latitude(latitude); }); }, &ctx);
            retry_mount_command(
                "hemisphere", [&]() { exchange([&] { protocol.set_hemisphere(latitude >= 0.0); }); }, &ctx);
        }
        if (lon.has_value()) {
            double longitude = lon.value();
            retry_mount_command(
                "site longitude", [&]() { exchange([&] { protocol.set_longitude(longitude); }); }, &ctx);
        }

        util::UtcOffset synced_time;
        if (do_clock_sync) {
            auto tz_info = compute_local_timezone_info();
            auto now_utc = std::chrono::system_clock::now();
            retry_mount_command(
                "timezone offset", [&]() { exchange([&] { protocol.set_timezone_offset(tz_info.offset_minutes); }); },
                &ctx);
            retry_mount_command(
                "DST flag", [&]() { exchange([&] { protocol.set_dst_observed(tz_info.dst_active); }); }, &ctx);
            retry_mount_command(
                "UTC clock",
                [&]() {
                    exchange([&] {
                        const auto sent_at = clock_.now();
                        protocol.set_utc_time(now_utc);
                        synced_time.set(now_utc, sent_at);
                    });
                },
                &ctx);
        }

        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                return;
            }
            if (lat.has_value()) {
                site_latitude_cached_ = lat.value();
                hemisphere_north_ = (lat.value() >= 0.0);
                site_info_valid_ = true;
                last_site_info_fetch_ = clock_.now();
            }
            if (lon.has_value()) {
                site_longitude_cached_ = lon.value();
                site_info_valid_ = true;
                last_site_info_fetch_ = clock_.now();
            }
            if (elev.has_value()) {
                site_elevation_m_ = elev.value();
            }
            if (do_clock_sync) {
                auto tz_info = compute_local_timezone_info();
                timezone_offset_minutes_ = tz_info.offset_minutes;
                timezone_offset_valid_ = true;
                dst_observed_ = tz_info.dst_active;
                if (synced_time.valid()) utc_offset_ = synced_time;
            }
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
    void set_clock_sync_hook_for_testing(std::function<void()> hook) { clock_sync_test_hook_ = std::move(hook); }
    void start_clock_sync_for_testing() { start_clock_sync(); }
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
    mutable bool guide_rate_calibration_attempted_ = false;
    mutable bool guide_rate_calibrated_ = false;
    mutable double guide_rate_scale_ra_ = 1.0;
    mutable double guide_rate_scale_dec_ = 1.0;

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
    // Firmware motion from a Park or FindHome the driver started (no slew_in_progress_ for those), or
    // from a GOTO whose dispatch a MoveAxis superseded: keeps get_slewing() true when a PulseGuide
    // arrives mid-motion. Set by park()/find_home() and move_axis(), cleared by
    // refresh_status_cache_locked() once a fresh status reports at home after a FindHome, or no motion
    // past the initiator's override, and by AbortSlew, any new initiator but MoveAxis
    // (supersede_slew_dispatch_locked, which Unpark also calls) and disconnect.
    enum class DriverMotion : std::uint8_t { None, Park, Home, Goto };
    // Under mutex_: the GOTO in slew_in_progress_ came from SlewToCoordinates* or SlewToTarget* (which route through
    // them), so AbortSlew turns Tracking back on (issue #2000). False for an Alt/Az slew, cleared by a superseding
    // initiator and by AbortSlew.
    bool equatorial_slew_ = false;
    mutable DriverMotion driver_motion_ = DriverMotion::None;
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

void set_clock_sync_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->set_clock_sync_hook_for_testing(std::move(hook));
    }
}

void start_clock_sync_for_testing(TelescopeDriver& driver) {
    if (auto* ioptron = dynamic_cast<iOptronTelescopeDriver*>(&driver)) {
        ioptron->start_clock_sync_for_testing();
    }
}

std::optional<std::string> clock_sync_failure_for_testing(const TelescopeDriver& driver) {
    if (const auto* ioptron = dynamic_cast<const iOptronTelescopeDriver*>(&driver)) {
        return ioptron->clock_sync_failure_for_testing();
    }
    return std::nullopt;
}

} // namespace alpacacore::vendor::ioptron
