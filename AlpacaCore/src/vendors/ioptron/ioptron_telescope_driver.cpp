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
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/units.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <thread>

namespace alpacacore::vendor::ioptron {

/**
 * @brief iOptron mount telescope driver implementation.
 *
 * Implements the TelescopeDriver interface for iOptron mounts using
 * the RS-232 command protocol over serial or TCP connection.
 */
class iOptronTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    /**
     * @brief Construct iOptron telescope driver.
     *
     * @param device_number Alpaca device number
     * @param connection_info Connection information (serial or network)
     */
    iOptronTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                           std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                           std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect)
        : AsyncConnectable("iOptron"),
          device_number_(device_number),
          connection_info_(connection_info),
          connected_(false),
          mount_info_(),
          target_ra_hours_(0.0),
          target_dec_degrees_(0.0),
          aperture_diameter_m_(0.0),
          aperture_area_m2_(0.0),
          focal_length_m_(0.0),
          pulse_guiding_active_(false),
          pulse_guiding_end_ns_(0),
          site_latitude_cached_(0.0),
          site_longitude_cached_(0.0),
          site_info_valid_(false),
          hemisphere_north_(true),
          site_elevation_m_(site_elevation_m.value_or(0.0)),
          timezone_offset_minutes_(0),
          timezone_offset_valid_(false),
          dst_observed_(false),
          last_site_info_fetch_(std::chrono::steady_clock::now()),
          cached_ra_hours_(0.0),
          cached_dec_degrees_(0.0),
          cached_side_of_pier_(-1),
          position_cache_valid_(false),
          last_position_update_(std::chrono::steady_clock::now()),
          cached_alt_degrees_(0.0),
          cached_az_degrees_(0.0),
          altaz_cache_valid_(false),
          last_altaz_update_(std::chrono::steady_clock::now()),
          cached_guide_rate_(),
          guide_rate_valid_(false),
          cached_status_(),
          status_cache_valid_(false),
          last_status_update_(std::chrono::steady_clock::now()),
          last_utc_set_{},
          last_utc_set_monotonic_(std::chrono::steady_clock::now()),
          last_utc_valid_(false),
          utc_query_supported_(true),
          fast_cache_until_(std::chrono::steady_clock::time_point{}),
          clock_sync_cancel_(false),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(true)) {
        // Initialize mount info (will be populated on connect)
    }

    ~iOptronTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // Join the async slew dispatch thread before any member it touches is
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
    }
    
    // AlpacaDriver interface
    
    int get_device_number() const override {
        return device_number_;
    }
    
    std::string get_name() const override {
        if (!mount_info_.model_name.empty()) {
            return "iOptron " + mount_info_.model_name;
        }
        return "iOptron Telescope";
    }
    
    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }
    
    std::string get_unique_id() const override {
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

    bool get_connected() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    // Base always spawns (the old spawn-skip `if (connect == get_connected())`
    // is gone — it masked the no-op-connect flag-consumption bug, PR #115
    // round 4). A connect() on an already-connected mount costs one short
    // no-op task; a disconnect() racing that window is recorded and honored
    // by the task tail rather than spawning immediately.
    //
    // LATENCY NOTE: this driver's teardown joins the clock-sync thread, which
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

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> lock(mutex_);

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
        if (connected == connected_) {
            ALPACA_LOG_INFO("iOptron", "Already in requested state, returning");
            return;
        }
        
        bool schedule_clock_sync = false;
        bool disconnect_protocol = false;

        if (connected) {
            ALPACA_LOG_INFO("iOptron", "Attempting to connect...");
            auto& protocol = iOptronProtocolWrapper::instance();
            ALPACA_LOG_INFO("iOptron", "Got protocol instance");
            
            ALPACA_LOG_INFO("iOptron", "Calling protocol.connect()...");
            if (protocol.connect(connection_info_)) {
                ALPACA_LOG_INFO("iOptron", "protocol.connect() returned true");
                connected_ = true;
                site_info_valid_ = false;
                position_cache_valid_ = false;
                altaz_cache_valid_ = false;
                status_cache_valid_ = false;
                guide_rate_valid_ = false;
                last_utc_valid_ = false;
                device_faulted_ = false;
                device_fault_count_ = 0;
                last_device_error_.clear();
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
                utc_query_supported_ = true;
                pulse_guiding_active_.store(false, std::memory_order_release);
                pulse_guiding_end_ns_.store(0, std::memory_order_relaxed);
                pulse_guiding_hold_ra_valid_ = false;
                pulse_guiding_hold_ra_hours_ = 0.0;
                pulse_guiding_hold_until_ = std::chrono::steady_clock::time_point{};
                last_ra_read_valid_ = false;
                last_ra_read_hours_ = 0.0;
                pulse_guiding_ra_correction_valid_ = false;
                pulse_guiding_ra_baseline_hours_ = 0.0;
                pulse_guiding_ra_expected_delta_hours_ = 0.0;
                pulse_guiding_ra_correction_until_ = std::chrono::steady_clock::time_point{};
                pulse_guiding_dec_correction_valid_ = false;
                pulse_guiding_dec_baseline_degrees_ = 0.0;
                pulse_guiding_dec_expected_delta_degrees_ = 0.0;
                pulse_guiding_dec_correction_until_ = std::chrono::steady_clock::time_point{};
                pulse_guiding_hold_dec_valid_ = false;
                pulse_guiding_hold_dec_degrees_ = 0.0;
                pulse_guiding_hold_dec_until_ = std::chrono::steady_clock::time_point{};
                last_dec_read_valid_ = false;
                last_dec_read_degrees_ = 0.0;
                target_set_ = false;
                target_ra_hours_ = 0.0;
                target_dec_degrees_ = 0.0;
                sync_offset_ra_hours_ = 0.0;
                sync_offset_dec_degrees_ = 0.0;
                slew_in_progress_ = false;
                prefetch_mount_state_locked();
                try {
                    mount_info_ = protocol.get_mount_info();
                } catch (...) {
                }
                fast_cache_until_ = std::chrono::steady_clock::now() + kFastCacheGrace;
                schedule_clock_sync = true;
                std::string mount_desc = mount_info_.model_name.empty()
                    ? "unknown model"
                    : mount_info_.model_name + " (" + mount_info_.model_code + ")";
                ALPACA_LOG_INFO("iOptron", "Connected to " + mount_desc + " over " +
                                            std::string(connection_info_.type == ConnectionType::Serial
                                                            ? "Serial/USB"
                                                            : "Network"));
            } else {
                ALPACA_LOG_ERROR("iOptron", "protocol.connect() returned false");
                throw AlpacaException("Failed to connect to iOptron mount");
            }
        } else {
            ALPACA_LOG_INFO("iOptron", "Disconnecting...");
            connected_ = false;
            site_info_valid_ = false;
            position_cache_valid_ = false;
            altaz_cache_valid_ = false;
            status_cache_valid_ = false;
            guide_rate_valid_ = false;
            last_utc_valid_ = false;
            device_faulted_ = false;
            device_fault_count_ = 0;
            last_device_error_.clear();
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
            pulse_guiding_hold_ra_valid_ = false;
            pulse_guiding_hold_ra_hours_ = 0.0;
            pulse_guiding_hold_until_ = std::chrono::steady_clock::time_point{};
            last_ra_read_valid_ = false;
            last_ra_read_hours_ = 0.0;
            pulse_guiding_ra_correction_valid_ = false;
            pulse_guiding_ra_baseline_hours_ = 0.0;
            pulse_guiding_ra_expected_delta_hours_ = 0.0;
            pulse_guiding_ra_correction_until_ = std::chrono::steady_clock::time_point{};
            pulse_guiding_dec_correction_valid_ = false;
            pulse_guiding_dec_baseline_degrees_ = 0.0;
            pulse_guiding_dec_expected_delta_degrees_ = 0.0;
            pulse_guiding_dec_correction_until_ = std::chrono::steady_clock::time_point{};
            pulse_guiding_hold_dec_valid_ = false;
            pulse_guiding_hold_dec_degrees_ = 0.0;
            pulse_guiding_hold_dec_until_ = std::chrono::steady_clock::time_point{};
            last_dec_read_valid_ = false;
            last_dec_read_degrees_ = 0.0;
            target_set_ = false;
            target_ra_hours_ = 0.0;
            target_dec_degrees_ = 0.0;
            slew_in_progress_ = false;
            disconnect_protocol = true;
        }

        lock.unlock();

        if (schedule_clock_sync) {
            start_clock_sync_thread();
        }
        if (disconnect_protocol) {
            stop_clock_sync_thread();
            // Join the async slew dispatch thread before tearing down the
            // protocol connection (runs after lock.unlock() — the thread
            // takes mutex_, so joining under the lock would deadlock).
            reap_slew_dispatch();
            auto& protocol = iOptronProtocolWrapper::instance();
            protocol.disconnect();
            ALPACA_LOG_INFO("iOptron", "Disconnected from mount");
        }
    }
    
    std::vector<std::string> get_supported_actions() const override {
        return {};  // No custom actions
    }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;  // Unused - no actions supported
        throw AlpacaException("Action not supported: " + std::string(action_name));
    }
    
    bool can_action(std::string_view action_name) const override {
        (void)action_name;  // Unused - no actions supported
        return false;
    }
    
    std::string command_blind(std::string_view command, bool raw) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_altaz_cache_locked();
        return cached_alt_degrees_;
    }
    
    double get_aperture_diameter() const override {
        return aperture_diameter_m_;
    }
    
    void set_aperture_diameter(double meters) override {
        aperture_diameter_m_ = meters;
        if (meters > 0.0) {
            double radius = meters / 2.0;
            aperture_area_m2_ = std::numbers::pi * radius * radius;
        } else {
            aperture_area_m2_ = 0.0;
        }
    }
    
    double get_aperture_area() const override {
        return aperture_area_m2_;
    }
    
    bool get_at_home() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked();
        return cached_status_.is_at_home;
    }
    
    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto now = std::chrono::steady_clock::now();
        if (park_override_until_ > now) {
            return false;
        }
        refresh_status_cache_locked();
        return cached_status_.is_parked;
    }
    
    double get_azimuth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_altaz_cache_locked();
        return cached_az_degrees_;
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
        if (pulse_guiding_active_.load(std::memory_order_acquire)) {
            auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            if (now_ns >= pulse_guiding_end_ns_.load(std::memory_order_relaxed)) {
                pulse_guiding_active_.store(false, std::memory_order_release);
                return false;
            }
            return true;
        }
        return false;
    }
    
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double dec_value = 0.0;
        if (pulse_guiding_hold_dec_valid_ &&
            std::chrono::steady_clock::now() < pulse_guiding_hold_dec_until_) {
            dec_value = pulse_guiding_hold_dec_degrees_;
        } else {
            refresh_position_cache_locked();
            dec_value = cached_dec_degrees_;
        }
        if (pulse_guiding_dec_correction_valid_) {
            const auto now = std::chrono::steady_clock::now();
            if (now < pulse_guiding_dec_correction_until_) {
                dec_value = pulse_guiding_dec_baseline_degrees_ +
                            pulse_guiding_dec_expected_delta_degrees_;
            }
            pulse_guiding_dec_correction_valid_ = false;
        }
        last_dec_read_degrees_ = dec_value;
        last_dec_read_valid_ = true;
        return dec_value + sync_offset_dec_degrees_;
    }
    
    double get_declination_rate() const override {
        // iOptron doesn't support getting Dec rate directly
        // Return 0 (not available)
        return 0.0;
    }
    
    void set_declination_rate(double rate) override {
        (void)rate;  // Unused - not supported
        throw AlpacaException(
            "Declination rate is not implemented by this mount",
            AlpacaError::PropertyNotImplemented
        );
    }
    
    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto now = std::chrono::steady_clock::now();
        if (tracking_override_until_ > now && status_cache_valid_) {
            return cached_status_.is_tracking;
        }
        refresh_status_cache_locked();
        return cached_status_.is_tracking;
    }
    
    void set_tracking(bool tracking) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = iOptronProtocolWrapper::instance();
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
        last_status_update_ = std::chrono::steady_clock::now();
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
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        const double scale_ra = guide_rate_calibrated_ ? guide_rate_scale_ra_ : 1.0;
        const double scale_dec = guide_rate_calibrated_ ? guide_rate_scale_dec_ : 1.0;
        auto& protocol = iOptronProtocolWrapper::instance();
        double ra_fraction = rate.ra / (kSiderealRateDegPerSec * scale_ra);
        double dec_fraction = rate.dec / (kSiderealRateDegPerSec * scale_dec);
        ra_fraction = std::clamp(ra_fraction, 0.01, 0.90);
        dec_fraction = std::clamp(dec_fraction, 0.10, 0.99);
        protocol.set_guide_rates(ra_fraction, dec_fraction);
        cached_guide_rate_ = rate;
        guide_rate_valid_ = true;
    }
    
    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double ra_value = 0.0;
        if (pulse_guiding_hold_ra_valid_ &&
            std::chrono::steady_clock::now() < pulse_guiding_hold_until_) {
            ra_value = pulse_guiding_hold_ra_hours_;
        } else {
            refresh_position_cache_locked();
            ra_value = cached_ra_hours_;
        }
        if (pulse_guiding_ra_correction_valid_) {
            const auto now = std::chrono::steady_clock::now();
            if (now < pulse_guiding_ra_correction_until_) {
                ra_value = normalize_ra_hours(pulse_guiding_ra_baseline_hours_ +
                                              pulse_guiding_ra_expected_delta_hours_);
            }
            pulse_guiding_ra_correction_valid_ = false;
        }
        last_ra_read_hours_ = ra_value;
        last_ra_read_valid_ = true;
        return normalize_ra_hours(ra_value + sync_offset_ra_hours_);
    }

    double get_right_ascension_rate() const override {
        // ASCOM RightAscensionRate is an OFFSET from sidereal tracking, in
        // seconds of RA per sidereal second — NOT the absolute tracking rate in
        // arcsec/sec. The driver never applies an RA-rate offset, so this is
        // always 0.0 (matches the SynScan/Celestron/Bisque drivers).
        return 0.0;
    }
    
    void set_right_ascension_rate(double rate) override {
        (void)rate;  // Unused - not supported
        throw AlpacaException(
            "Right ascension rate is not implemented by this mount",
            AlpacaError::PropertyNotImplemented
        );
    }
    
    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();

        refresh_position_cache_locked();
        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
            // ensure_site_info_cached_locked() sets site_info_valid_ on success;
            // this re-check detects a failed cache populate, not a dead condition.
            // cppcheck-suppress identicalInnerCondition
            if (!site_info_valid_) {
                return normalize_side_of_pier_value(cached_side_of_pier_);
            }
        }

        const double lst = compute_local_sidereal_time_hours(current_utc_time_locked(),
                                                             site_longitude_cached_);
        const double synced_ra = normalize_ra_hours(cached_ra_hours_ + sync_offset_ra_hours_);
        const double hour_angle = shortest_ra_delta_hours(lst, synced_ra);
        return (hour_angle >= 0.0) ? 0 : 1;
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        validate_ra_dec(ra, dec, "DestinationSideOfPier");

        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
            // ensure_site_info_cached_locked() sets site_info_valid_ on success;
            // this re-check detects a failed cache populate, not a dead condition.
            // cppcheck-suppress identicalInnerCondition
            if (!site_info_valid_) {
                return normalize_side_of_pier_value(cached_side_of_pier_);
            }
        }

        const double lst = compute_local_sidereal_time_hours(current_utc_time_locked(),
                                                             site_longitude_cached_);
        const double hour_angle = shortest_ra_delta_hours(lst, ra);

        return (hour_angle >= 0.0) ? 0 : 1;
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::Topocentric;
    }

    bool get_does_refraction() const override {
        return does_refraction_;
    }

    void set_does_refraction(bool does_refraction) override {
        does_refraction_ = does_refraction;
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
        throw AlpacaException(
            "Setting pier side not supported by iOptron mount",
            AlpacaError::PropertyNotImplemented
        );
    }
    
    double get_sidereal_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_site_info_cached_locked();
        auto utc_now = current_utc_time_locked();
        return compute_local_sidereal_time_hours(utc_now, site_longitude_cached_);
    }
    
    double get_site_elevation() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return site_elevation_m_;
    }
    
    void set_site_elevation(double elevation) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException(
                "Site elevation must be between -300 and 10000 meters",
                AlpacaError::InvalidValue
            );
        }
        site_elevation_m_ = elevation;
    }
    
    double get_site_latitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        ensure_site_info_cached_locked();
        return site_latitude_cached_;
    }
    
    void set_site_latitude(double latitude) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (latitude < -90.0 || latitude > 90.0) {
            throw AlpacaException(
                "Site latitude must be between -90 and 90 degrees",
                AlpacaError::InvalidValue
            );
        }
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_latitude(latitude);
        protocol.set_hemisphere(latitude >= 0.0);
        site_latitude_cached_ = latitude;
        hemisphere_north_ = (latitude >= 0.0);
        site_info_valid_ = true;
        last_site_info_fetch_ = std::chrono::steady_clock::now();
    }
    
    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        ensure_site_info_cached_locked();
        return site_longitude_cached_;
    }
    
    void set_site_longitude(double longitude) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (longitude < -180.0 || longitude > 180.0) {
            throw AlpacaException(
                "Site longitude must be between -180 and 180 degrees",
                AlpacaError::InvalidValue
            );
        }
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_longitude(longitude);
        site_longitude_cached_ = longitude;
        site_info_valid_ = true;
        last_site_info_fetch_ = std::chrono::steady_clock::now();
    }

    bool get_can_move_axis(int axis) const override {
        return (axis == 0 || axis == 1);
    }
    
    void move_axis(int axis, double rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_not_parked_locked("MoveAxis");

        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 (Primary) or 1 (Secondary)",
                                  AlpacaError::InvalidValue);
        }

        double abs_rate = std::abs(rate);
        if (!is_axis_rate_supported(abs_rate)) {
            throw AlpacaException("Axis rate out of range", AlpacaError::InvalidValue);
        }

        const bool was_any_axis_active = axis_move_active_primary_ || axis_move_active_secondary_;
        auto& protocol = iOptronProtocolWrapper::instance();
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
                last_status_update_ = std::chrono::steady_clock::now();
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
    }
    
    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 0 || axis == 1) {
            const auto& rates = axis_rate_steps_deg_per_sec();
            return {rates.front(), rates.back()};
        }
        return {0.0, 0.0};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (axis_move_active_primary_ || axis_move_active_secondary_) {
            return true;
        }
        auto now = std::chrono::steady_clock::now();
        if (slew_override_until_ > now) {
            return true;
        }
        refresh_status_cache_locked();
        if (cached_status_.is_slewing) {
            return true;
        }
        if (slew_override_until_ > now) {
            return true;
        }
        // HAE29C firmware quirk (hardware-verified 2026-07-14): GOTO stops
        // compensating sidereal motion during its final ~1 s approach, so
        // slews settle a consistent ~11-16 arcsec east of the RA target
        // (ConformU tolerance is +/-10"). Re-issuing the GOTO does NOT fix it:
        // the firmware deadbands slews shorter than ~15". Instead, close the
        // residual with a duration-computed pulse-guide trim (guide rate is
        // known, so err/rate gives the exact pulse length), re-measuring and
        // iterating up to kMaxSlewRefines. RA only: Dec settles within ~0.2"
        // on this firmware and Dec pulse polarity flips with pier side.
        // Slewing stays true while a trim pulse runs.
        if (slew_in_progress_ && hae29c_quirks_active() && slew_refine_count_ < kMaxSlewRefines) {
            ++slew_refine_count_;
            try {
                auto& protocol = iOptronProtocolWrapper::instance();
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
                if (std::fabs(ra_err_arcsec) > kTrimMinArcsec &&
                    std::fabs(ra_err_arcsec) < kTrimMaxArcsec) {
                    double guide_fraction = kDefaultGuideRateFraction;
                    try {
                        guide_fraction = protocol.get_guide_rates().first;
                    } catch (const std::exception&) {
                    }
                    if (guide_fraction < 0.1 || guide_fraction > 1.0) {
                        guide_fraction = kDefaultGuideRateFraction;
                    }
                    const double trim_rate_arcsec_per_s = guide_fraction * 15.041;
                    int duration_ms = static_cast<int>(
                        std::fabs(ra_err_arcsec) / trim_rate_arcsec_per_s * 1000.0);
                    duration_ms = std::clamp(duration_ms, 100, 8000);
                    // Reported RA too large -> mount is east of target -> pulse
                    // west (RA-, direction 3); too small -> east (RA+, 2).
                    const int direction = ra_err_arcsec > 0 ? 3 : 2;
                    ALPACA_LOG_INFO("iOptron",
                                    "GOTO settled " + std::to_string(ra_err_arcsec) +
                                        " arcsec from RA target; pulse-guide trim " +
                                        std::to_string(slew_refine_count_) + "/" +
                                        std::to_string(kMaxSlewRefines) + " (" +
                                        std::to_string(duration_ms) +
                                        " ms, final-approach firmware quirk)");
                    protocol.pulse_guide(direction, duration_ms);
                    slew_override_until_ = std::chrono::steady_clock::now() +
                                           std::chrono::milliseconds(duration_ms + 500);
                    status_cache_valid_ = false;
                    position_cache_valid_ = false;
                    return true;
                }
            } catch (const std::exception&) {
                // Best-effort trim; fall through and report slew complete.
            }
        }
        slew_in_progress_ = false;
        restore_altitude_limit_locked("Slewing");
        restore_meridian_treatment_locked("Slewing");
        return false;
    }
    
    double get_target_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }
    
    void set_target_declination(double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        validate_dec(dec, "TargetDeclination");

        target_dec_degrees_ = dec;
        target_set_ = true;
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_target_dec(dec);
    }
    
    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }
    
    void set_target_right_ascension(double ra) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        validate_ra(ra, "TargetRightAscension");

        target_ra_hours_ = ra;
        target_set_ = true;
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_target_ra(ra);
    }
    
    int get_tracking_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto now = std::chrono::steady_clock::now();
        if (tracking_rate_override_until_ > now && status_cache_valid_) {
            return static_cast<int>(cached_status_.tracking_rate);
        }
        refresh_status_cache_locked();
        // Alpaca TrackingRate uses DriveRates enum values (0-4).
        return static_cast<int>(cached_status_.tracking_rate);
    }
    
    void set_tracking_rate(int rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();

        if (rate < 0 || rate > 4) {
            throw AlpacaException("Invalid tracking rate", AlpacaError::InvalidValue);
        }

        protocol.set_tracking_rate(rate);
        if (rate == 4) {
            protocol.set_custom_tracking_rate(custom_tracking_rate_);
        }
        cached_status_.tracking_rate = rate;
        status_cache_valid_ = true;
        last_status_update_ = std::chrono::steady_clock::now();
        tracking_rate_override_until_ = last_status_update_ + std::chrono::seconds(2);
    }
    
    std::vector<int> get_tracking_rates() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        // Return supported tracking rates as DriveRates enum values:
        // 0 = driveSidereal, 1 = driveLunar, 2 = driveSolar, 3 = driveKing, 4 = driveCustom
        return {0, 1, 2, 3, 4};
    }
    
    std::chrono::system_clock::time_point get_utc_date() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();

        if (!last_utc_valid_) {
            last_utc_set_ = std::chrono::system_clock::now();
            last_utc_set_monotonic_ = std::chrono::steady_clock::now();
            last_utc_valid_ = true;
        }
        return current_utc_time_locked();
    }
    
    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_utc_time(utc);
        last_utc_set_ = utc;
        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
        last_utc_valid_ = true;
    }
    
    // Telescope methods
    
    void find_home() override {
        std::lock_guard<std::mutex> lock(mutex_);
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
        
        auto& protocol = iOptronProtocolWrapper::instance();
        // Use a direct go-to-zero command to avoid aggressive sensor searches.
        protocol.go_to_home();
        slew_override_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        status_cache_valid_ = false;
        position_cache_valid_ = false;
    }
    
    void park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        refresh_status_cache_locked();
        if (cached_status_.is_parked) {
            return;
        }

        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            if (!protocol.park(hae29c_quirks_active())) {
                throw AlpacaException("Failed to park mount");
            }
            clear_device_fault_locked();
        } catch (const std::exception& e) {
            record_device_fault_locked("Park", e.what());
            throw AlpacaException(std::string("Failed to park mount: ") + e.what(),
                                  AlpacaError::DriverException);
        }
        park_override_until_ = std::chrono::steady_clock::time_point{};
        cached_status_.is_parked = false;
        cached_status_.is_slewing = true;
        cached_status_.is_at_home = false;
        status_cache_valid_ = true;
        last_status_update_ = std::chrono::steady_clock::now();
        slew_override_until_ = last_status_update_ + std::chrono::seconds(5);
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
            }
        }
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_not_parked_locked("AbortSlew");
        if (!cached_status_.is_slewing) {
            restore_altitude_limit_locked("AbortSlew");
            restore_meridian_treatment_locked("AbortSlew");
            return;
        }
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            protocol.stop_slewing();
            clear_device_fault_locked();
        } catch (const std::exception& e) {
            record_device_fault_locked("AbortSlew", e.what());
            throw AlpacaException(std::string("AbortSlew failed: ") + e.what(),
                                  AlpacaError::DriverException);
        }
        slew_in_progress_ = false;
        status_cache_valid_ = false;
        slew_override_until_ = std::chrono::steady_clock::time_point{};
        restore_altitude_limit_locked("AbortSlew");
        restore_meridian_treatment_locked("AbortSlew");
    }
    
    void pulse_guide(int direction, int duration) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_not_parked_fast_locked("PulseGuide");
        
        auto& protocol = iOptronProtocolWrapper::instance();
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
        if (duration > 0 && is_dec) {
            if (last_ra_read_valid_) {
                pulse_guiding_hold_ra_hours_ = last_ra_read_hours_;
            } else {
                pulse_guiding_hold_ra_hours_ = cached_ra_hours_;
            }
            pulse_guiding_hold_ra_valid_ = true;
            pulse_guiding_hold_until_ = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(duration) +
                                       kPulseGuideCompletionDelay +
                                       kPulseGuideHoldGrace;
            pulse_guiding_hold_dec_valid_ = false;
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
            pulse_guiding_dec_correction_valid_ = true;
            pulse_guiding_dec_correction_until_ = std::chrono::steady_clock::now() +
                                                  std::chrono::milliseconds(duration) +
                                                  kPulseGuideCompletionDelay +
                                                  kPulseGuideCorrectionGrace;
        } else if (duration > 0 && is_ra) {
            if (last_dec_read_valid_) {
                pulse_guiding_hold_dec_degrees_ = last_dec_read_degrees_;
            } else {
                pulse_guiding_hold_dec_degrees_ = cached_dec_degrees_;
            }
            pulse_guiding_hold_dec_valid_ = true;
            pulse_guiding_hold_dec_until_ = std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(duration) +
                                            kPulseGuideCompletionDelay +
                                            kPulseGuideHoldGrace;
            pulse_guiding_hold_ra_valid_ = false;
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
            pulse_guiding_ra_correction_valid_ = true;
            pulse_guiding_ra_correction_until_ = std::chrono::steady_clock::now() +
                                                 std::chrono::milliseconds(duration) +
                                                 kPulseGuideCompletionDelay +
                                                 kPulseGuideCorrectionGrace;
        } else {
            pulse_guiding_hold_ra_valid_ = false;
            pulse_guiding_hold_dec_valid_ = false;
        }
        protocol.pulse_guide(calibrated_direction, duration);
        if (duration > 0) {
            auto end = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(duration) +
                       kPulseGuideCompletionDelay;
            pulse_guiding_end_ns_.store(end.time_since_epoch().count(),
                                        std::memory_order_relaxed);
            pulse_guiding_active_.store(true, std::memory_order_release);
        } else {
            pulse_guiding_active_.store(false, std::memory_order_release);
        }
    }
    
    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        // Get current position and set as park position
        auto& protocol = iOptronProtocolWrapper::instance();
        AltAz altaz = protocol.get_alt_az();
        protocol.set_park_position(altaz.altitude_degrees, altaz.azimuth_degrees);
        status_cache_valid_ = false;
        position_cache_valid_ = false;
    }
    
    void slew_to_coordinates(double ra, double dec) override {
        validate_ra_dec(ra, dec, "SlewToCoordinates");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            double phys_ra = normalize_ra_hours(ra - sync_offset_ra_hours_);
            double phys_dec = dec - sync_offset_dec_degrees_;
            prepare_slew_state_locked(ra, dec, phys_ra, phys_dec, "SlewToCoordinates");
            dispatch_slew_command_locked(phys_ra, phys_dec, false, "SlewToCoordinates");
        }
        wait_for_slew_completion("SlewToCoordinates");
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
        double phys_ra = 0.0;
        double phys_dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            phys_ra = normalize_ra_hours(ra - sync_offset_ra_hours_);
            phys_dec = dec - sync_offset_dec_degrees_;
            prepare_slew_state_locked(ra, dec, phys_ra, phys_dec, "SlewToCoordinatesAsync");
        }
        try {
            // Joinable member thread (never detached), joined in the
            // destructor and on disconnect. Join any previous dispatch first
            // (must run without mutex_ held — the thread takes mutex_).
            reap_slew_dispatch();
            std::lock_guard<std::mutex> tlock(slew_dispatch_mutex_);
            if (slew_dispatch_thread_.joinable()) {
                // A racing async-slew caller spawned between our reap and this
                // lock — cancel and join it INSIDE the same critical section as
                // the assignment, so a joinable thread can never be overwritten
                // (std::terminate) or joined from two threads (UB). Matches the
                // Celestron/SynScan spawn-site recheck.
                slew_dispatch_cancel_.store(true);
                slew_dispatch_thread_.join();
                slew_dispatch_cancel_.store(false);
            }
            slew_dispatch_thread_ = std::thread([this, phys_ra, phys_dec]() {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_ || slew_dispatch_cancel_.load()) {
                    slew_in_progress_ = false;
                    return;
                }
                try {
                    dispatch_slew_command_locked(phys_ra, phys_dec, true, "SlewToCoordinatesAsync");
                } catch (const std::exception& e) {
                    slew_in_progress_ = false;
                    ALPACA_LOG_WARN("iOptron", std::string("Async slew failed: ") + e.what());
                } catch (...) {
                    slew_in_progress_ = false;
                    ALPACA_LOG_WARN("iOptron", "Async slew failed with unknown exception");
                }
            });
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            slew_in_progress_ = false;
            throw AlpacaException(std::string("Failed to start async slew: ") + e.what(),
                                  AlpacaError::DriverException);
        }
    }
    
    void slew_to_target() override {
        double ra = 0.0;
        double dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (!target_set_) {
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
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (!target_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates_async(ra, dec);
    }
    
    void sync_to_coordinates(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        validate_ra_dec(ra, dec, "SyncToCoordinates");
        ensure_not_parked_locked("SyncToCoordinates");

        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;

        refresh_position_cache_locked(true);
        sync_offset_ra_hours_ = ra - cached_ra_hours_;
        sync_offset_dec_degrees_ = dec - cached_dec_degrees_;
        if (sync_offset_ra_hours_ > 12.0) sync_offset_ra_hours_ -= 24.0;
        if (sync_offset_ra_hours_ < -12.0) sync_offset_ra_hours_ += 24.0;
    }
    
    void sync_to_target() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!target_set_) {
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            validate_alt_az(altitude, azimuth, "SlewToAltAzAsync");
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
            slew_in_progress_ = true;
            ensure_not_parked_locked("SlewToAltAzAsync");
        }

        try {
            // Joinable member thread (never detached), joined in the
            // destructor and on disconnect. Join any previous dispatch first
            // (must run without mutex_ held — the thread takes mutex_).
            reap_slew_dispatch();
            std::lock_guard<std::mutex> tlock(slew_dispatch_mutex_);
            if (slew_dispatch_thread_.joinable()) {
                // Racing async-slew between reap_slew_dispatch() and this lock —
                // see the RA/Dec spawn above; join under the same critical section.
                slew_dispatch_cancel_.store(true);
                slew_dispatch_thread_.join();
                slew_dispatch_cancel_.store(false);
            }
            slew_dispatch_thread_ = std::thread([this, altitude, azimuth]() {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_ || slew_dispatch_cancel_.load()) {
                    slew_in_progress_ = false;
                    return;
                }
                try {
                    auto converted = alt_az_to_ra_dec(
                        altitude, azimuth, site_latitude_cached_, site_longitude_cached_, current_utc_time_locked());
                    double ra_hours = converted.first;
                    double dec_degrees = converted.second;
                    prepare_slew_state_locked(ra_hours, dec_degrees, ra_hours, dec_degrees, "SlewToAltAzAsync");
                    dispatch_slew_command_locked(ra_hours, dec_degrees, true, "SlewToAltAzAsync");
                } catch (const std::exception& e) {
                    slew_in_progress_ = false;
                    ALPACA_LOG_WARN("iOptron", std::string("Async AltAz slew failed: ") + e.what());
                } catch (...) {
                    slew_in_progress_ = false;
                    ALPACA_LOG_WARN("iOptron", "Async AltAz slew failed with unknown exception");
                }
            });
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            slew_in_progress_ = false;
            throw AlpacaException(std::string("Failed to start async Alt/Az slew: ") + e.what(),
                                  AlpacaError::DriverException);
        }
    }

    void slew_to_alt_az(double altitude, double azimuth) override {
        double ra_hours = 0.0;
        double dec_degrees = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            validate_alt_az(altitude, azimuth, "SlewToAltAz");
            ensure_site_info_cached_locked();
            if (!site_info_valid_) {
                throw AlpacaException("Site information unavailable for Alt/Az slew",
                                      AlpacaError::ValueNotSet);
            }
            auto converted = alt_az_to_ra_dec(
                altitude, azimuth, site_latitude_cached_, site_longitude_cached_, current_utc_time_locked());
            ra_hours = converted.first;
            dec_degrees = converted.second;
            start_slew_to_coordinates_locked(ra_hours, dec_degrees, false);
        }
        wait_for_slew_completion("SlewToAltAz");
    }
    
    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException(
            "Alt/Az sync not supported by iOptron driver",
            AlpacaError::MethodNotImplemented
        );
    }
    
    void unpark() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();

        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            protocol.unpark();
            clear_device_fault_locked();
        } catch (const std::exception& e) {
            record_device_fault_locked("Unpark", e.what());
            throw AlpacaException(std::string("Failed to unpark mount: ") + e.what(),
                                  AlpacaError::DriverException);
        }
        cached_status_.is_parked = false;
        cached_status_.is_slewing = true;
        cached_status_.is_at_home = false;
        status_cache_valid_ = true;
        last_status_update_ = std::chrono::steady_clock::now();
        slew_override_until_ = last_status_update_ + std::chrono::seconds(2);
        park_override_until_ = last_status_update_ + kUnparkGrace;
        park_finalize_pending_ = false;
    }

private:
    static constexpr std::chrono::seconds kSiteInfoCacheTtl{60};
    static constexpr std::chrono::milliseconds kStatusCacheTtl{1000};
    static constexpr std::chrono::milliseconds kPositionCacheTtl{1000};
    static constexpr std::chrono::milliseconds kSlewPollInterval{250};
    static constexpr std::chrono::seconds kSlewTimeout{300};
    static constexpr std::chrono::seconds kFastCacheGrace{30};
    static constexpr std::chrono::seconds kUnparkGrace{120};
    static constexpr std::chrono::milliseconds kPulseGuideCompletionDelay{1000};
    static constexpr std::chrono::milliseconds kPulseGuideHoldGrace{2000};
    static constexpr std::chrono::milliseconds kPulseGuideCorrectionGrace{5000};
    static constexpr double kSlewCompletionToleranceArcsec = 60.0;
    static constexpr double kSiderealSeconds = 86164.0905;
    static constexpr double kSiderealRateDegPerSec = 360.0 / kSiderealSeconds;
    static constexpr double kDefaultGuideRateFraction = 0.5;
    
    // All three HAE29C firmware-quirk workarounds (wedged-park finalizer,
    // zero-distance park finalizer, GOTO refinement) are gated on the exact
    // model code so other iOptron mounts (HAE43, HEM27, ...) keep the
    // original, hardware-validated behavior.
    bool hae29c_quirks_active() const {
        return mount_info_.model_code == "0036";
    }

    void check_connected() const {
        if (!connected_) {
            throw AlpacaException("Not connected to mount", AlpacaError::NotConnected);
        }
        if (device_faulted_) {
            throw AlpacaException("Mount communications compromised: " + last_device_error_,
                                  AlpacaError::DriverException);
        }
    }

    void ensure_not_parked_locked(const char* action) const {
        auto now = std::chrono::steady_clock::now();
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
        auto now = std::chrono::steady_clock::now();
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
    // device_faulted_ (which makes every subsequent call throw) after several
    // CONSECUTIVE failures. Any subsequent successful command clears the count
    // via clear_device_fault_locked().
    static constexpr int kDeviceFaultThreshold = 3;

    void record_device_fault_locked(const char* context, const std::string& detail) const {
        ++device_fault_count_;
        last_device_error_ = std::string(context) + ": " + detail;
        if (device_fault_count_ >= kDeviceFaultThreshold) {
            device_faulted_ = true;
            ALPACA_LOG_ERROR("iOptron", "Device fault (latched after " + std::to_string(device_fault_count_) +
                                            " consecutive failures) - " + last_device_error_);
        } else {
            ALPACA_LOG_WARN("iOptron", "Device fault (transient, " + std::to_string(device_fault_count_) + "/" +
                                           std::to_string(kDeviceFaultThreshold) + ") - " + last_device_error_);
        }
    }

    void clear_device_fault_locked() const {
        device_fault_count_ = 0;
        device_faulted_ = false;
        last_device_error_.clear();
    }

    void prefetch_mount_state_locked() {
        auto& protocol = iOptronProtocolWrapper::instance();
        auto now = std::chrono::steady_clock::now();
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

    bool try_override_altitude_limit_locked() const {
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            const int current_limit = protocol.get_altitude_limit_degrees();
            altitude_limit_restore_degrees_ = current_limit;
            constexpr int kAltitudeLimitOverrideDegrees = -89;
            if (current_limit != kAltitudeLimitOverrideDegrees) {
                protocol.set_altitude_limit_degrees(kAltitudeLimitOverrideDegrees);
            }
            altitude_limit_override_active_ = true;
            ALPACA_LOG_INFO(
                "iOptron",
                "Temporarily lowering altitude limit to " +
                    std::to_string(kAltitudeLimitOverrideDegrees) +
                    " degrees for slew");
            return true;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", std::string("Unable to override altitude limit: ") + e.what());
            altitude_limit_override_active_ = false;
            return false;
        }
    }

    void restore_altitude_limit_locked(const char* label) const {
        if (!altitude_limit_override_active_) {
            return;
        }
        try {
            auto& protocol = iOptronProtocolWrapper::instance();
            protocol.set_altitude_limit_degrees(altitude_limit_restore_degrees_);
            altitude_limit_override_active_ = false;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", std::string(label) + ": Failed to restore altitude limit: " +
                                         e.what());
        }
    }

    bool try_override_meridian_treatment_locked() const {
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            const MeridianTreatment current = protocol.get_meridian_treatment();
            meridian_restore_behavior_ = current.behavior;
            meridian_restore_degrees_ = current.degrees_past;
            constexpr int kMeridianOverrideDegrees = 30;
            protocol.set_meridian_treatment(1, kMeridianOverrideDegrees);
            meridian_override_active_ = true;
            ALPACA_LOG_INFO(
                "iOptron",
                "Temporarily relaxing meridian treatment to flip, " +
                    std::to_string(kMeridianOverrideDegrees) + " degrees past meridian");
            return true;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", std::string("Unable to override meridian treatment: ") + e.what());
            meridian_override_active_ = false;
            return false;
        }
    }

    void restore_meridian_treatment_locked(const char* label) const {
        if (!meridian_override_active_) {
            return;
        }
        try {
            auto& protocol = iOptronProtocolWrapper::instance();
            protocol.set_meridian_treatment(meridian_restore_behavior_, meridian_restore_degrees_);
            meridian_override_active_ = false;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", std::string(label) + ": Failed to restore meridian treatment: " +
                                         e.what());
        }
    }

    void prepare_slew_state_locked(double ascom_ra, double ascom_dec,
                                    double phys_ra, double phys_dec,
                                    const char* label) {
        check_connected();
        ensure_not_parked_locked(label);

        target_ra_hours_ = ascom_ra;
        target_dec_degrees_ = ascom_dec;
        target_set_ = true;
        slew_in_progress_ = true;
        slew_target_ra_hours_ = phys_ra;
        slew_target_dec_degrees_ = phys_dec;
        slew_refine_count_ = 0;
    }

    void dispatch_slew_command_locked(double ra, double dec, bool allow_soft_fail, const char* label) {
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        bool accepted = protocol.slew_to_ra_dec();
        if (!accepted) {
            accepted = protocol.slew_to_ra_dec_cw_up();
        }
        bool altitude_override_applied = false;
        bool meridian_override_applied = false;
        if (!accepted && !altitude_limit_override_active_) {
            altitude_override_applied = try_override_altitude_limit_locked();
        }
        if (!accepted && !meridian_override_active_) {
            meridian_override_applied = try_override_meridian_treatment_locked();
        }
        if (!accepted && (altitude_override_applied || meridian_override_applied)) {
            accepted = protocol.slew_to_ra_dec();
            if (!accepted) {
                accepted = protocol.slew_to_ra_dec_cw_up();
            }
            if (!accepted && altitude_override_applied) {
                restore_altitude_limit_locked(label);
            }
            if (!accepted && meridian_override_applied) {
                restore_meridian_treatment_locked(label);
            }
        }
        if (!accepted) {
            slew_in_progress_ = false;
            if (allow_soft_fail) {
                ALPACA_LOG_WARN("iOptron", "Slew rejected by mount - treating as no-op for async slew");
                return;
            }
            throw AlpacaException("Slew rejected by mount - target may violate altitude limits");
        }
        slew_override_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        status_cache_valid_ = false;
        position_cache_valid_ = false;
    }

    void start_slew_to_coordinates_locked(double ra, double dec, bool allow_soft_fail) {
        prepare_slew_state_locked(ra, dec, ra, dec, "SlewToCoordinates");
        dispatch_slew_command_locked(ra, dec, allow_soft_fail, "SlewToCoordinates");
    }

    void wait_for_slew_completion(const char* label) {
        const auto deadline = std::chrono::steady_clock::now() + kSlewTimeout;
        while (get_slewing()) {
            if (std::chrono::steady_clock::now() > deadline) {
                throw AlpacaException(std::string(label) + " timed out waiting for slew to complete");
            }
            std::this_thread::sleep_for(kSlewPollInterval);
        }

        // The mount may clear its slewing status well before the physical
        // slew finishes (observed on WiFi: GLS→0 while mount still moving
        // tens of degrees).  Wait for position readings to stabilize — two
        // consecutive reads within tolerance of each other — AND target
        // reached.  Uses the slew deadline, not a fixed iteration cap.
        if (target_set_) {
            static constexpr double kStableThresholdArcsec = 30.0;
            static constexpr int kRequiredStableReads = 3;
            double prev_ra_hours = std::numeric_limits<double>::quiet_NaN();
            double prev_dec_degrees = std::numeric_limits<double>::quiet_NaN();
            int stable_count = 0;

            while (std::chrono::steady_clock::now() < deadline) {
                bool target_reached = false;
                double cur_ra = 0, cur_dec = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    target_reached = slew_target_reached_locked();
                    cur_ra = cached_ra_hours_;
                    cur_dec = cached_dec_degrees_;
                }

                if (target_reached) {
                    break;
                }

                if (!std::isnan(prev_ra_hours)) {
                    double ra_delta = std::abs(shortest_ra_delta_hours(cur_ra, prev_ra_hours)) * 15.0 * 3600.0;
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

                std::this_thread::sleep_for(kSlewPollInterval);
            }
        }

        if (slew_settle_time_seconds_ > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
        }
        slew_in_progress_ = false;
        restore_altitude_limit_locked(label);
        restore_meridian_treatment_locked(label);
    }
    
    void refresh_position_cache_locked(bool force = false) const {
        auto now = std::chrono::steady_clock::now();
        if (!force && position_cache_valid_ && now < fast_cache_until_) {
            return;
        }
        if (!force && position_cache_valid_ &&
            (now - last_position_update_) < kPositionCacheTtl) {
            return;
        }
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            Position pos = protocol.get_position();
            cached_dec_degrees_ = pos.dec_degrees;
            cached_ra_hours_ = pos.ra_hours;
            cached_side_of_pier_ = pos.side_of_pier;
            position_cache_valid_ = true;
            last_position_update_ = now;
        } catch (const std::exception& e) {
            record_device_fault_locked("Position", e.what());
            throw AlpacaException(std::string("Failed to refresh mount position: ") + e.what(),
                                  AlpacaError::DriverException);
        }
    }

    void refresh_altaz_cache_locked(bool force = false) const {
        auto now = std::chrono::steady_clock::now();
        if (!force && altaz_cache_valid_ && now < fast_cache_until_) {
            return;
        }
        if (!force && altaz_cache_valid_ &&
            (now - last_altaz_update_) < kPositionCacheTtl) {
            return;
        }
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            AltAz altaz = protocol.get_alt_az();
            cached_alt_degrees_ = altaz.altitude_degrees;
            cached_az_degrees_ = altaz.azimuth_degrees;
            altaz_cache_valid_ = true;
            last_altaz_update_ = now;
        } catch (const std::exception& e) {
            record_device_fault_locked("AltAz", e.what());
            throw AlpacaException(std::string("Failed to refresh mount Alt/Az: ") + e.what(),
                                  AlpacaError::DriverException);
        }
    }
    
    void refresh_status_cache_locked(bool force = false) const {
        auto now = std::chrono::steady_clock::now();
        if (!force && status_cache_valid_ && now < fast_cache_until_) {
            return;
        }
        if (!force && status_cache_valid_ &&
            (now - last_status_update_) < kStatusCacheTtl) {
            return;
        }
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            cached_status_ = protocol.get_status();
            status_cache_valid_ = true;
            last_status_update_ = now;
        } catch (const std::exception& e) {
            record_device_fault_locked("Status", e.what());
            throw AlpacaException(std::string("Failed to refresh mount status: ") + e.what(),
                                  AlpacaError::DriverException);
        }
        finalize_wedged_park_locked();
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
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            AltAz current = protocol.get_alt_az();
            constexpr double kEpsDeg = 0.01;  // 36 arcsec; GAC/GPC agree to 0.01"
            if (std::fabs(current.altitude_degrees - park_target_.altitude_degrees) >= kEpsDeg ||
                std::fabs(current.azimuth_degrees - park_target_.azimuth_degrees) >= kEpsDeg) {
                return;  // still genuinely slewing toward the park position
            }
            ALPACA_LOG_INFO("iOptron",
                            "Park slew arrived but mount still reports slewing; sending tracking-off "
                            "to finalize (HAE firmware quirk)");
            protocol.stop_tracking();
            cached_status_ = protocol.get_status();
            last_status_update_ = std::chrono::steady_clock::now();
            if (cached_status_.is_parked) {
                park_finalize_pending_ = false;
            }
        } catch (const std::exception&) {
            // Best-effort — leave the flag armed and retry on the next refresh.
        }
    }
    
    void ensure_site_info_cached_locked(bool force_refresh = false) const {
        auto now = std::chrono::steady_clock::now();
        if (!force_refresh && site_info_valid_ && now < fast_cache_until_) {
            return;
        }
        if (!force_refresh && site_info_valid_ &&
            (now - last_site_info_fetch_) < kSiteInfoCacheTtl) {
            return;
        }
        auto& protocol = iOptronProtocolWrapper::instance();
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
        } catch (const std::exception& e) {
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

    bool retry_mount_command(const char* label, const std::function<void()>& fn) {
        constexpr int kMaxAttempts = 3;
        const auto delay = std::chrono::milliseconds(750);
        std::string last_error;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            try {
                fn();
                return true;
            } catch (const std::exception& e) {
                last_error = e.what();
                if (attempt < kMaxAttempts) {
                    std::this_thread::sleep_for(delay);
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

        auto& protocol = iOptronProtocolWrapper::instance();
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
            std::this_thread::sleep_for(std::chrono::milliseconds(sample_duration + 700));
            Position end = protocol.get_position();
            for (int i = 0; i < 2; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
        auto& protocol = iOptronProtocolWrapper::instance();
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
        const auto settle_delay = kPulseGuideCompletionDelay;
        try {
            auto& protocol = iOptronProtocolWrapper::instance();
            auto [original_ra_rate, original_dec_rate] = protocol.get_guide_rates();
            const double test_ra_rate = std::clamp(0.5, 0.01, 0.90);
            const double test_dec_rate = std::clamp(original_dec_rate, 0.10, 0.99);
            protocol.set_guide_rates(test_ra_rate, test_dec_rate);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            auto read_position_stable = [&protocol]() {
                Position pos = protocol.get_position();
                for (int i = 0; i < 2; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    pos = protocol.get_position();
                }
                return pos;
            };

            const Position baseline_start = protocol.get_position();
            std::this_thread::sleep_for(std::chrono::milliseconds(sample_duration) + settle_delay);
            const Position baseline_end = read_position_stable();
            const double baseline_delta_hours =
                shortest_ra_delta_hours(baseline_end.ra_hours, baseline_start.ra_hours);

            const Position pulse_start = protocol.get_position();
            protocol.pulse_guide(2, sample_duration);
            std::this_thread::sleep_for(std::chrono::milliseconds(sample_duration) + settle_delay);
            const Position pulse_end = read_position_stable();

            const double pulse_delta_hours =
                shortest_ra_delta_hours(pulse_end.ra_hours, pulse_start.ra_hours);
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

    static double normalize_ra_hours(double ra_hours) {
        ra_hours = std::fmod(ra_hours, 24.0);
        if (ra_hours < 0.0) {
            ra_hours += 24.0;
        }
        return ra_hours;
    }

    static double shortest_ra_delta_hours(double target_hours, double current_hours) {
        double delta = target_hours - current_hours;
        while (delta > 12.0) {
            delta -= 24.0;
        }
        while (delta < -12.0) {
            delta += 24.0;
        }
        return delta;
    }

    bool slew_target_reached_locked() const {
        try {
            refresh_position_cache_locked(true);
        } catch (const std::exception&) {
            return false;
        }
        const double ra_delta_hours =
            std::abs(shortest_ra_delta_hours(slew_target_ra_hours_, cached_ra_hours_));
        const double dec_delta_degrees = std::abs(slew_target_dec_degrees_ - cached_dec_degrees_);
        const double ra_arcsec = ra_delta_hours * 15.0 * 3600.0;
        const double dec_arcsec = dec_delta_degrees * 3600.0;
        return ra_arcsec <= kSlewCompletionToleranceArcsec &&
               dec_arcsec <= kSlewCompletionToleranceArcsec;
    }

    static void validate_ra(double ra_hours, const char* label) {
        if (!std::isfinite(ra_hours) || ra_hours < 0.0 || ra_hours >= 24.0) {
            throw AlpacaException(std::string(label) + " must be in [0, 24) hours", AlpacaError::InvalidValue);
        }
    }

    static void validate_dec(double dec_degrees, const char* label) {
        if (!std::isfinite(dec_degrees) || dec_degrees < -90.0 || dec_degrees > 90.0) {
            throw AlpacaException(std::string(label) + " must be in [-90, 90] degrees", AlpacaError::InvalidValue);
        }
    }

    static void validate_ra_dec(double ra_hours, double dec_degrees, const char* label) {
        validate_ra(ra_hours, label);
        validate_dec(dec_degrees, label);
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
            if (retry_mount_command("site latitude", [&]() { iOptronProtocolWrapper::instance().set_latitude(latitude); })) {
                site_latitude_cached_ = latitude;
                hemisphere_north_ = (latitude >= 0.0);
                site_info_valid_ = true;
                last_site_info_fetch_ = std::chrono::steady_clock::now();
            }
        }

        if (pending_site_longitude_.has_value()) {
            double longitude = pending_site_longitude_.value();
            if (retry_mount_command("site longitude", [&]() { iOptronProtocolWrapper::instance().set_longitude(longitude); })) {
                site_longitude_cached_ = longitude;
                site_info_valid_ = true;
                last_site_info_fetch_ = std::chrono::steady_clock::now();
            }
        }

        if (pending_site_latitude_.has_value()) {
            bool is_north = pending_site_latitude_.value() >= 0.0;
            retry_mount_command("hemisphere", [&]() { iOptronProtocolWrapper::instance().set_hemisphere(is_north); });
        }

        if (pending_site_elevation_.has_value()) {
            site_elevation_m_ = pending_site_elevation_.value();
        }
    }

    void sync_mount_clock_with_host_locked() {
        auto tz_info = compute_local_timezone_info();
        auto now_utc = std::chrono::system_clock::now();
        auto& protocol = iOptronProtocolWrapper::instance();

        if (retry_mount_command("timezone offset", [&]() { protocol.set_timezone_offset(tz_info.offset_minutes); })) {
            timezone_offset_minutes_ = tz_info.offset_minutes;
            timezone_offset_valid_ = true;
        }

        if (retry_mount_command("DST flag", [&]() { protocol.set_dst_observed(tz_info.dst_active); })) {
            dst_observed_ = tz_info.dst_active;
        }

        if (retry_mount_command("UTC clock", [&]() { protocol.set_utc_time(now_utc); })) {
            last_utc_set_ = now_utc;
            last_utc_set_monotonic_ = std::chrono::steady_clock::now();
            last_utc_valid_ = true;
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
    
    void start_clock_sync_thread() {
        // clock_sync_mutex_ serializes start/stop from overlapping
        // set_connected bodies (sync PUT vs async connect task) — both run
        // after mutex_ is released, so without this guard two callers can
        // join and assign clock_sync_thread_ concurrently (UB), or a second
        // start can overwrite a still-joinable thread (std::terminate).
        // The clock-sync body never takes clock_sync_mutex_, so joining
        // under it cannot deadlock. Same shape as reap_slew_dispatch().
        std::lock_guard<std::mutex> tlock(clock_sync_mutex_);
        stop_clock_sync_thread_locked();
        clock_sync_cancel_.store(false);
        clock_sync_thread_ = std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (clock_sync_cancel_.load()) {
                return;
            }

            std::optional<double> lat, lon;
            std::optional<int> elev;
            bool do_clock_sync = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_ || clock_sync_cancel_.load()) {
                    return;
                }
                lat = pending_site_latitude_;
                lon = pending_site_longitude_;
                elev = pending_site_elevation_;
                do_clock_sync = sync_time_on_connect_;
            }

            auto& protocol = iOptronProtocolWrapper::instance();

            if (lat.has_value()) {
                double latitude = lat.value();
                retry_mount_command("site latitude", [&]() { protocol.set_latitude(latitude); });
                retry_mount_command("hemisphere", [&]() { protocol.set_hemisphere(latitude >= 0.0); });
            }
            if (lon.has_value()) {
                double longitude = lon.value();
                retry_mount_command("site longitude", [&]() { protocol.set_longitude(longitude); });
            }

            if (do_clock_sync) {
                auto tz_info = compute_local_timezone_info();
                auto now_utc = std::chrono::system_clock::now();
                retry_mount_command("timezone offset", [&]() { protocol.set_timezone_offset(tz_info.offset_minutes); });
                retry_mount_command("DST flag", [&]() { protocol.set_dst_observed(tz_info.dst_active); });
                retry_mount_command("UTC clock", [&]() { protocol.set_utc_time(now_utc); });
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_ || clock_sync_cancel_.load()) {
                    return;
                }
                if (lat.has_value()) {
                    site_latitude_cached_ = lat.value();
                    hemisphere_north_ = (lat.value() >= 0.0);
                    site_info_valid_ = true;
                    last_site_info_fetch_ = std::chrono::steady_clock::now();
                }
                if (lon.has_value()) {
                    site_longitude_cached_ = lon.value();
                    site_info_valid_ = true;
                    last_site_info_fetch_ = std::chrono::steady_clock::now();
                }
                if (elev.has_value()) {
                    site_elevation_m_ = elev.value();
                }
                if (do_clock_sync) {
                    auto tz_info = compute_local_timezone_info();
                    timezone_offset_minutes_ = tz_info.offset_minutes;
                    timezone_offset_valid_ = true;
                    dst_observed_ = tz_info.dst_active;
                    last_utc_set_ = std::chrono::system_clock::now();
                    last_utc_set_monotonic_ = std::chrono::steady_clock::now();
                    last_utc_valid_ = true;
                }
            }
        });
    }

    void stop_clock_sync_thread() {
        std::lock_guard<std::mutex> tlock(clock_sync_mutex_);
        stop_clock_sync_thread_locked();
    }

    void stop_clock_sync_thread_locked() {
        clock_sync_cancel_.store(true);
        if (clock_sync_thread_.joinable()) {
            clock_sync_thread_.join();
        }
        clock_sync_cancel_.store(false);
    }

    // Join the async slew dispatch thread (if any) and reset its cancel flag.
    // Must be called WITHOUT mutex_ held — the dispatch thread takes mutex_,
    // so joining under the lock would deadlock. The thread only dispatches a
    // single command (no long sleeps), so the join is quick.
    void reap_slew_dispatch() {
        slew_dispatch_cancel_.store(true);
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(slew_dispatch_mutex_);
            prev = std::move(slew_dispatch_thread_);
        }
        if (prev.joinable()) {
            prev.join();
        }
        slew_dispatch_cancel_.store(false);
    }

    std::chrono::system_clock::time_point current_utc_time_locked() const {
        if (!last_utc_valid_) {
            return std::chrono::system_clock::now();
        }
        auto elapsed = std::chrono::steady_clock::now() - last_utc_set_monotonic_;
        return last_utc_set_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
    }
    
    static double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time,
                                                    double longitude_degrees) {
        using namespace std::chrono;
        const double unix_epoch_jd = 2440587.5;
        double days_since_epoch = duration_cast<seconds>(utc_time.time_since_epoch()).count() / 86400.0;
        double jd = unix_epoch_jd + days_since_epoch;
        double T = (jd - 2451545.0) / 36525.0;
        double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) +
                      0.000387933 * T * T - (T * T * T) / 38710000.0;
        gmst = std::fmod(gmst, 360.0);
        if (gmst < 0.0) {
            gmst += 360.0;
        }
        double lst = gmst + longitude_degrees;
        lst = std::fmod(lst, 360.0);
        if (lst < 0.0) {
            lst += 360.0;
        }
        return lst / 15.0;
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
        const double lst_hours = compute_local_sidereal_time_hours(utc_time, longitude_degrees);
        const double ra_hours = normalize_ra_hours(lst_hours - ha_hours);
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
    
    int device_number_;
    ConnectionInfo connection_info_;
    std::atomic<bool> connected_{false};
    mutable MountInfo mount_info_;
    mutable std::mutex mutex_;
    
    // Target coordinates
    double target_ra_hours_;
    double target_dec_degrees_;
    double aperture_diameter_m_ = 0.0;
    double aperture_area_m2_ = 0.0;
    double focal_length_m_ = 0.0;
    int slew_settle_time_seconds_ = 0;
    double custom_tracking_rate_ = 1.0;
    bool does_refraction_ = false;
    bool target_set_ = false;
    mutable std::atomic<bool> pulse_guiding_active_{false};
    mutable std::atomic<int64_t> pulse_guiding_end_ns_{0};
    mutable bool pulse_guiding_hold_ra_valid_ = false;
    mutable double pulse_guiding_hold_ra_hours_ = 0.0;
    mutable std::chrono::steady_clock::time_point pulse_guiding_hold_until_{};
    mutable bool last_ra_read_valid_ = false;
    mutable double last_ra_read_hours_ = 0.0;
    mutable bool pulse_guiding_ra_correction_valid_ = false;
    mutable double pulse_guiding_ra_baseline_hours_ = 0.0;
    mutable double pulse_guiding_ra_expected_delta_hours_ = 0.0;
    mutable std::chrono::steady_clock::time_point pulse_guiding_ra_correction_until_{};
    mutable bool pulse_guiding_dec_correction_valid_ = false;
    mutable double pulse_guiding_dec_baseline_degrees_ = 0.0;
    mutable double pulse_guiding_dec_expected_delta_degrees_ = 0.0;
    mutable std::chrono::steady_clock::time_point pulse_guiding_dec_correction_until_{};
    mutable bool pulse_guiding_hold_dec_valid_ = false;
    mutable double pulse_guiding_hold_dec_degrees_ = 0.0;
    mutable std::chrono::steady_clock::time_point pulse_guiding_hold_dec_until_{};
    mutable bool last_dec_read_valid_ = false;
    mutable double last_dec_read_degrees_ = 0.0;
    
    // Cached mount information
    mutable double site_latitude_cached_;
    mutable double site_longitude_cached_;
    mutable bool site_info_valid_;
    mutable bool hemisphere_north_;
    mutable double site_elevation_m_;
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
    
    mutable std::chrono::system_clock::time_point last_utc_set_;
    mutable std::chrono::steady_clock::time_point last_utc_set_monotonic_;
    mutable bool last_utc_valid_;
    mutable bool slew_in_progress_ = false;
    static constexpr int kMaxSlewRefines = 3;
    mutable int slew_refine_count_ = 0;
    mutable double slew_target_ra_hours_ = 0.0;
    mutable double slew_target_dec_degrees_ = 0.0;
    mutable bool altitude_limit_override_active_ = false;
    mutable int altitude_limit_restore_degrees_ = 0;
    mutable bool meridian_override_active_ = false;
    mutable int meridian_restore_behavior_ = 0;
    mutable int meridian_restore_degrees_ = 0;
    mutable bool device_faulted_ = false;
    mutable int device_fault_count_ = 0;
    mutable std::string last_device_error_;
    mutable bool utc_query_supported_;
    mutable std::chrono::steady_clock::time_point fast_cache_until_{};
    mutable std::chrono::steady_clock::time_point tracking_override_until_{};
    mutable std::chrono::steady_clock::time_point tracking_rate_override_until_{};
    mutable std::chrono::steady_clock::time_point slew_override_until_{};
    std::thread clock_sync_thread_;
    std::atomic<bool> clock_sync_cancel_;
    // Guards clock_sync_thread_ join/assign across overlapping set_connected
    // bodies — see start_clock_sync_thread(). Never taken by the body.
    std::mutex clock_sync_mutex_;
    // Async slew dispatch thread (never detached) — see reap_slew_dispatch().
    // slew_dispatch_mutex_ only guards the thread handle.
    std::mutex slew_dispatch_mutex_;
    std::thread slew_dispatch_thread_;
    std::atomic<bool> slew_dispatch_cancel_{false};
    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;
};

// Factory function implementation
std::unique_ptr<TelescopeDriver> create_ioptron_telescope(
    int device_number,
    const ConnectionInfo& connection_info)
{
    return std::make_unique<iOptronTelescopeDriver>(
        device_number, connection_info, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_ioptron_telescope_with_site(
    int device_number,
    const ConnectionInfo& connection_info,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect)
{
    return std::make_unique<iOptronTelescopeDriver>(
        device_number, connection_info, site_latitude_deg, site_longitude_deg, site_elevation_m,
        sync_time_on_connect);
}

std::unique_ptr<TelescopeDriver> create_ioptron_telescope_auto(
    int device_number,
    int mount_index,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect) {

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

    return create_ioptron_telescope_with_site(
        device_number, conn, site_latitude_deg, site_longitude_deg,
        site_elevation_m, sync_time_on_connect);
}

std::unique_ptr<TelescopeDriver> create_ioptron_telescope_auto_network(
    int device_number,
    int mount_index,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect) {

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

    return create_ioptron_telescope_with_site(
        device_number, conn, site_latitude_deg, site_longitude_deg,
        site_elevation_m, sync_time_on_connect);
}

} // namespace alpacacore::vendor::ioptron
