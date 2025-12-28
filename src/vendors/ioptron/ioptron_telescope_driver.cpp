// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/units.h>
#include <mutex>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <algorithm>
#include <thread>
#include <atomic>
#include <ctime>
#include <functional>
#include <optional>

namespace alpacacore::vendor::ioptron {

/**
 * @brief iOptron mount telescope driver implementation.
 *
 * Implements the TelescopeDriver interface for iOptron mounts using
 * the RS-232 command protocol over serial or TCP connection.
 */
class iOptronTelescopeDriver : public TelescopeDriver {
public:
    /**
     * @brief Construct iOptron telescope driver.
     *
     * @param device_number Alpaca device number
     * @param connection_info Connection information (serial or network)
     */
    iOptronTelescopeDriver(int device_number,
                           const ConnectionInfo& connection_info,
                           std::optional<double> site_latitude_deg,
                           std::optional<double> site_longitude_deg,
                           std::optional<double> site_elevation_m,
                           std::optional<bool> sync_time_on_connect)
        : device_number_(device_number)
        , connection_info_(connection_info)
        , connected_(false)
        , mount_info_()
        , target_ra_hours_(0.0)
        , target_dec_degrees_(0.0)
        , aperture_diameter_m_(0.0)
        , aperture_area_m2_(0.0)
        , focal_length_m_(0.0)
        , pulse_guiding_active_(false)
        , pulse_guiding_end_(std::chrono::steady_clock::now())
        , site_latitude_cached_(0.0)
        , site_longitude_cached_(0.0)
        , site_info_valid_(false)
        , hemisphere_north_(true)
        , site_elevation_m_(site_elevation_m.value_or(0.0))
        , timezone_offset_minutes_(0)
        , timezone_offset_valid_(false)
        , dst_observed_(false)
        , last_site_info_fetch_(std::chrono::steady_clock::now())
        , cached_ra_hours_(0.0)
        , cached_dec_degrees_(0.0)
        , cached_side_of_pier_(-1)
        , position_cache_valid_(false)
        , last_position_update_(std::chrono::steady_clock::now())
        , cached_alt_degrees_(0.0)
        , cached_az_degrees_(0.0)
        , altaz_cache_valid_(false)
        , last_altaz_update_(std::chrono::steady_clock::now())
        , cached_guide_rate_()
        , guide_rate_valid_(false)
        , cached_status_()
        , status_cache_valid_(false)
        , last_status_update_(std::chrono::steady_clock::now())
        , last_utc_set_{}
        , last_utc_set_monotonic_(std::chrono::steady_clock::now())
        , last_utc_valid_(false)
        , utc_query_supported_(true)
        , clock_sync_cancel_(false)
        , pending_site_latitude_(site_latitude_deg)
        , pending_site_longitude_(site_longitude_deg)
        , pending_site_elevation_(site_elevation_m)
        , sync_time_on_connect_(sync_time_on_connect.value_or(false))
    {
        // Initialize mount info (will be populated on connect)
    }
    
    ~iOptronTelescopeDriver() override {
        if (connected_) {
            set_connected(false);
        }
    }
    
    // AlpacaDriver interface
    
    int get_device_number() const override {
        return device_number_;
    }
    
    std::string get_name() const override {
        if (mount_info_.model_name.empty()) {
            return "iOptron Telescope";
        }
        return "iOptron " + mount_info_.model_name;
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
        return "iOptron " + mount_info_.model_name + " Telescope Driver";
    }
    
    std::string get_driver_info() const override {
        return "AlpacaCore iOptron Driver v1.0";
    }
    
    std::string get_driver_version() const override {
        return "1.0.0";
    }
    
    int get_interface_version() const override {
        return 1;
    }
    
    bool get_connected() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }
    
    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> lock(mutex_);

        ALPACA_LOG_INFO("iOptron", "set_connected called with: " + std::string(connected ? "true" : "false"));
        
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
                // For connection semantics, consider the mount "connected" once the
                // serial/TCP link is established, just like the legacy AlpacaPi
                // driver. Model information (if needed) can be queried lazily by
                // higher-level code using other commands.
                connected_ = true;
                if (mount_info_.model_name.empty()) {
                    mount_info_.model_name = "iOptron";
                }
                mount_info_.model_name = "iOptron";
                site_info_valid_ = false;
                position_cache_valid_ = false;
                status_cache_valid_ = false;
                last_utc_valid_ = false;
                utc_query_supported_ = true;
                pulse_guiding_active_ = false;
                schedule_clock_sync = true;
                ALPACA_LOG_INFO("iOptron", "Connected to mount over " +
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
            status_cache_valid_ = false;
            last_utc_valid_ = false;
            utc_query_supported_ = true;
            disconnect_protocol = true;
        }

        lock.unlock();

        if (schedule_clock_sync) {
            start_clock_sync_thread();
        }
        if (disconnect_protocol) {
            stop_clock_sync_thread();
            auto& protocol = iOptronProtocolWrapper::instance();
            protocol.disconnect();
            ALPACA_LOG_INFO("iOptron", "Disconnected from mount");
        }
    }
    
    std::string get_supported_actions() const override {
        return "";  // No custom actions
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
            aperture_area_m2_ = M_PI * radius * radius;
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
        if (pulse_guiding_active_ && std::chrono::steady_clock::now() >= pulse_guiding_end_) {
            pulse_guiding_active_ = false;
        }
        return pulse_guiding_active_;
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
        refresh_position_cache_locked();
        return cached_dec_degrees_;
    }
    
    double get_declination_rate() const override {
        // iOptron doesn't support getting Dec rate directly
        // Return 0 (not available)
        return 0.0;
    }
    
    void set_declination_rate(double rate) override {
        (void)rate;  // Unused - not supported
        throw AlpacaException("Setting declination rate not supported by iOptron mount");
    }
    
    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked();
        return cached_status_.is_tracking;
    }
    
    void set_tracking(bool tracking) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = iOptronProtocolWrapper::instance();
        if (tracking) {
            refresh_status_cache_locked(true);
            if (cached_status_.is_parked) {
                ALPACA_LOG_INFO("iOptron", "Mount reports parked; sending unpark before tracking");
                protocol.unpark();
                cached_status_.is_parked = false;
                status_cache_valid_ = false;
            }
        }
        if (tracking) {
            protocol.start_tracking();
        } else {
            protocol.stop_tracking();
        }
        status_cache_valid_ = false;
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
        
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            // Use structured binding to avoid std::pair copy warning
            auto [ra_rate, dec_rate] = protocol.get_guide_rates();
            cached_guide_rate_.ra = ra_rate;
            cached_guide_rate_.dec = dec_rate;
            guide_rate_valid_ = true;
            return cached_guide_rate_;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Failed to read guide rates, using cached values: " +
                                          std::string(e.what()));
            if (guide_rate_valid_) {
                return cached_guide_rate_;
            }
        }
        GuideRate fallback;
        fallback.ra = 0.0;
        fallback.dec = 0.0;
        return fallback;
    }
    
    void set_guide_rate(const GuideRate& rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_guide_rates(rate.ra, rate.dec);
    }
    
    bool get_is_slewing() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked();
        return cached_status_.is_slewing;
    }
    
    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        refresh_position_cache_locked();
        return cached_ra_hours_;
    }
    
    double get_right_ascension_rate() const override {
        // iOptron doesn't support getting RA rate directly
        // Calculate from tracking rate
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked();
        auto& protocol = iOptronProtocolWrapper::instance();
        
        // Sidereal rate is 15.041 arcsec/sec
        double sidereal_rate = 15.041;
        
        if (cached_status_.tracking_rate == 4) {
            // Custom tracking rate
            double multiplier = protocol.get_custom_tracking_rate();
            return sidereal_rate * multiplier;
        } else {
            // Standard rates: 0=sidereal, 1=lunar, 2=solar, 3=King
            // For now, return sidereal rate
            return sidereal_rate;
        }
    }
    
    void set_right_ascension_rate(double rate) override {
        (void)rate;  // Unused - not supported
        throw AlpacaException("Setting right ascension rate not supported by iOptron mount");
    }
    
    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        refresh_position_cache_locked();
        return cached_side_of_pier_;
    }
    
    int get_destination_side_of_pier() const override {
        return get_side_of_pier();
    }
    
    void set_side_of_pier(int side) override {
        (void)side;  // Unused - not supported
        throw AlpacaException("Setting pier side not supported by iOptron mount");
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

        auto& protocol = iOptronProtocolWrapper::instance();
        if (axis == 0) {
            if (rate > 0.0) {
                protocol.send_command_blind(":mw#"); // RA+ (west)
            } else if (rate < 0.0) {
                protocol.send_command_blind(":me#"); // RA- (east)
            } else {
                protocol.send_command_blind(":qR#"); // stop RA
            }
        } else if (axis == 1) {
            if (rate > 0.0) {
                protocol.send_command_blind(":ms#"); // Dec+ (north)
            } else if (rate < 0.0) {
                protocol.send_command_blind(":mn#"); // Dec- (south)
            } else {
                protocol.send_command_blind(":qD#"); // stop Dec
            }
        } else {
            throw AlpacaException("Axis movement not supported for this axis");
        }

        status_cache_valid_ = false;
    }
    
    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 0) {
            return {0.0, 5.0};
        }
        if (axis == 1) {
            return {0.0, 6.0};
        }
        return {0.0, 0.0};
    }

    bool get_slewing() const override {
        return get_is_slewing();
    }
    
    double get_target_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_dec_degrees_;
    }
    
    void set_target_declination(double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        target_dec_degrees_ = dec;
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_target_dec(dec);
    }
    
    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_ra_hours_;
    }
    
    void set_target_right_ascension(double ra) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        target_ra_hours_ = ra;
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_target_ra(ra);
    }
    
    double get_tracking_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked();
        // Alpaca TrackingRate uses DriveRates enum values (0-4).
        return static_cast<double>(cached_status_.tracking_rate);
    }
    
    void set_tracking_rate(double rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        
        // If rate is close to 1.0, use sidereal
        if (std::abs(rate - 1.0) < 0.001) {
            protocol.set_tracking_rate(0);  // Sidereal
        } else {
            // Use custom tracking rate
            protocol.set_tracking_rate(4);  // Custom
            protocol.set_custom_tracking_rate(rate);
        }
        status_cache_valid_ = false;
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

        if (last_utc_valid_) {
            return current_utc_time_locked();
        }

        if (!utc_query_supported_) {
            if (!last_utc_valid_) {
                last_utc_set_ = std::chrono::system_clock::now();
                last_utc_set_monotonic_ = std::chrono::steady_clock::now();
                last_utc_valid_ = true;
            }
            return current_utc_time_locked();
        }

        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            auto mount_time = protocol.get_utc_time();
            auto now = std::chrono::system_clock::now();
            auto diff = (mount_time > now) ? (mount_time - now) : (now - mount_time);
            constexpr auto kMaxAllowedDiff = std::chrono::minutes(2);

            if (diff <= kMaxAllowedDiff) {
                last_utc_set_ = mount_time;
                last_utc_set_monotonic_ = std::chrono::steady_clock::now();
                last_utc_valid_ = true;
                return mount_time;
            }

            if (last_utc_valid_) {
                auto cached_time = current_utc_time_locked();
                auto cached_diff = (cached_time > now) ? (cached_time - now) : (now - cached_time);
                if (cached_diff <= kMaxAllowedDiff) {
                    ALPACA_LOG_WARN("iOptron", "Mount UTC time differs from system UTC; using cached UTC instead");
                    return cached_time;
                }
            }

            return mount_time;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "Failed to read UTC time from mount: " + std::string(e.what()));
            utc_query_supported_ = false;
            last_utc_set_ = std::chrono::system_clock::now();
            last_utc_set_monotonic_ = std::chrono::steady_clock::now();
            last_utc_valid_ = true;
            return current_utc_time_locked();
        }
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

        refresh_status_cache_locked();
        if (cached_status_.is_at_home) {
            return;
        }
        
        auto& protocol = iOptronProtocolWrapper::instance();
        // Use a direct go-to-zero command to avoid aggressive sensor searches.
        protocol.go_to_home();
        status_cache_valid_ = false;
        position_cache_valid_ = false;
    }
    
    void park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        if (!protocol.park()) {
            throw AlpacaException("Failed to park mount");
        }
        cached_status_.is_parked = true;
        cached_status_.is_slewing = true;
        status_cache_valid_ = true;
        last_status_update_ = std::chrono::steady_clock::now();
        position_cache_valid_ = false;
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.stop_slewing();
    }
    
    void pulse_guide(int direction, int duration) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.pulse_guide(direction, duration);
        pulse_guiding_active_ = (duration > 0);
        if (pulse_guiding_active_) {
            pulse_guiding_end_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
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
    
    void slew_to_coordinates() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked(true);
        if (cached_status_.is_parked) {
            auto& protocol = iOptronProtocolWrapper::instance();
            ALPACA_LOG_INFO("iOptron", "Mount reports parked; sending unpark before slew");
            protocol.unpark();
            cached_status_.is_parked = false;
            status_cache_valid_ = false;
        }
        
        auto& protocol = iOptronProtocolWrapper::instance();
        bool accepted = protocol.slew_to_ra_dec();
        if (!accepted) {
            accepted = protocol.slew_to_ra_dec_cw_up();
        }
        if (!accepted) {
            throw AlpacaException("Slew rejected by mount - target may violate altitude limits");
        }
        status_cache_valid_ = false;
        position_cache_valid_ = false;
    }
    
    void slew_to_coordinates_async() override {
        // iOptron slewing is already async
        slew_to_coordinates();
    }
    
    void slew_to_target() override {
        slew_to_coordinates();
    }
    
    void slew_to_target_async() override {
        slew_to_coordinates_async();
    }
    
    void sync_to_coordinates(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        protocol.sync_to_coordinates();
        position_cache_valid_ = false;
        status_cache_valid_ = false;
    }
    
    void sync_to_target() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.sync_to_coordinates();
        position_cache_valid_ = false;
        status_cache_valid_ = false;
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("Alt/Az slews not supported by iOptron driver");
    }
    
    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("Alt/Az sync not supported by iOptron driver");
    }
    
    void unpark() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.unpark();
        status_cache_valid_ = false;
    }

private:
    static constexpr std::chrono::seconds kSiteInfoCacheTtl{60};
    static constexpr std::chrono::milliseconds kStatusCacheTtl{500};
    static constexpr std::chrono::milliseconds kPositionCacheTtl{200};
    
    void check_connected() const {
        if (!connected_) {
            throw AlpacaException("Not connected to mount");
        }
    }
    
    void refresh_position_cache_locked(bool force = false) const {
        auto now = std::chrono::steady_clock::now();
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
            ALPACA_LOG_WARN("iOptron", "Failed to refresh mount position, using cached values: " +
                                          std::string(e.what()));
            if (!position_cache_valid_) {
                position_cache_valid_ = true;
                last_position_update_ = now;
            }
        }
    }

    void refresh_altaz_cache_locked(bool force = false) const {
        auto now = std::chrono::steady_clock::now();
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
            ALPACA_LOG_WARN("iOptron", "Failed to refresh mount Alt/Az, using cached values: " +
                                          std::string(e.what()));
            if (!altaz_cache_valid_) {
                altaz_cache_valid_ = true;
                last_altaz_update_ = now;
            }
        }
    }
    
    void refresh_status_cache_locked(bool force = false) const {
        auto now = std::chrono::steady_clock::now();
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
            ALPACA_LOG_WARN("iOptron", "Failed to refresh mount status, using cached values: " +
                                          std::string(e.what()));
            if (!status_cache_valid_) {
                status_cache_valid_ = true;
                last_status_update_ = now;
            }
        }
    }
    
    void ensure_site_info_cached_locked(bool force_refresh = false) const {
        auto now = std::chrono::steady_clock::now();
        if (!force_refresh && site_info_valid_ &&
            (now - last_site_info_fetch_) < kSiteInfoCacheTtl) {
            return;
        }
        auto& protocol = iOptronProtocolWrapper::instance();
        try {
            SiteInfo site = protocol.get_site_info();
            if ((std::abs(site.latitude_degrees) > 90.0) ||
                (std::abs(site.longitude_degrees) > 180.0)) {
                ALPACA_LOG_WARN("iOptron", "Invalid site information reported by mount; using cached values");
                return;
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
            ALPACA_LOG_WARN("iOptron", "Failed to read site info, using cached values: " +
                                          std::string(e.what()));
            if (!site_info_valid_) {
                last_site_info_fetch_ = now;
            }
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
        stop_clock_sync_thread();
        clock_sync_cancel_.store(false);
        clock_sync_thread_ = std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (clock_sync_cancel_.load()) {
                return;
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || clock_sync_cancel_.load()) {
                return;
            }
            sync_site_settings_with_mount_locked();
            if (sync_time_on_connect_) {
                sync_mount_clock_with_host_locked();
            }
        });
    }

    void stop_clock_sync_thread() {
        clock_sync_cancel_.store(true);
        if (clock_sync_thread_.joinable()) {
            clock_sync_thread_.join();
        }
        clock_sync_cancel_.store(false);
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
    
    int device_number_;
    ConnectionInfo connection_info_;
    bool connected_;
    MountInfo mount_info_;
    mutable std::mutex mutex_;
    
    // Target coordinates
    double target_ra_hours_;
    double target_dec_degrees_;
    double aperture_diameter_m_ = 0.0;
    double aperture_area_m2_ = 0.0;
    double focal_length_m_ = 0.0;
    mutable bool pulse_guiding_active_ = false;
    mutable std::chrono::steady_clock::time_point pulse_guiding_end_{};
    
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
    
    mutable MountStatus cached_status_{};
    mutable bool status_cache_valid_;
    mutable std::chrono::steady_clock::time_point last_status_update_;
    
    mutable std::chrono::system_clock::time_point last_utc_set_;
    mutable std::chrono::steady_clock::time_point last_utc_set_monotonic_;
    mutable bool last_utc_valid_;
    mutable bool utc_query_supported_;
    std::thread clock_sync_thread_;
    std::atomic<bool> clock_sync_cancel_;
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

} // namespace alpacacore::vendor::ioptron
