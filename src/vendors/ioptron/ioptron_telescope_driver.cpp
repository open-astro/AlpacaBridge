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
    iOptronTelescopeDriver(int device_number, const ConnectionInfo& connection_info)
        : device_number_(device_number)
        , connection_info_(connection_info)
        , connected_(false)
        , mount_info_()
        , target_ra_hours_(0.0)
        , target_dec_degrees_(0.0)
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
        std::lock_guard<std::mutex> lock(mutex_);
        
        ALPACA_LOG_INFO("iOptron", "set_connected called with: " + std::string(connected ? "true" : "false"));
        
        if (connected == connected_) {
            ALPACA_LOG_INFO("iOptron", "Already in requested state, returning");
            return;
        }
        
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
            auto& protocol = iOptronProtocolWrapper::instance();
            protocol.disconnect();
            connected_ = false;
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
        
        auto& protocol = iOptronProtocolWrapper::instance();
        AltAz altaz = protocol.get_alt_az();
        return altaz.altitude_degrees;
    }
    
    double get_aperture_diameter() const override {
        // Not available from mount - return 0 (unknown)
        return 0.0;
    }
    
    double get_aperture_area() const override {
        // Not available from mount - return 0 (unknown)
        return 0.0;
    }
    
    bool get_at_home() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        MountStatus status = protocol.get_status();
        return status.is_at_home;
    }
    
    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        MountStatus status = protocol.get_status();
        return status.is_parked;
    }
    
    double get_azimuth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        AltAz altaz = protocol.get_alt_az();
        return altaz.azimuth_degrees;
    }
    
    bool get_can_find_home() const override {
        // Available on CEM120, CEM70, GEM45, CEM40 series
        return (mount_info_.model_code == "0120" || mount_info_.model_code == "0121" ||
                mount_info_.model_code == "0122" || mount_info_.model_code == "0070" ||
                mount_info_.model_code == "0071" || mount_info_.model_code == "0043" ||
                mount_info_.model_code == "0044" || mount_info_.model_code == "0040" ||
                mount_info_.model_code == "0041");
    }
    
    bool get_can_park() const override {
        return true;  // All iOptron mounts support parking
    }
    
    bool get_can_pulse_guide() const override {
        return true;  // All iOptron mounts support pulse guiding
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
        
        auto& protocol = iOptronProtocolWrapper::instance();
        Position pos = protocol.get_position();
        return pos.dec_degrees;
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
        
        auto& protocol = iOptronProtocolWrapper::instance();
        MountStatus status = protocol.get_status();
        return status.is_tracking;
    }
    
    void set_tracking(bool tracking) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        if (tracking) {
            protocol.start_tracking();
        } else {
            protocol.stop_tracking();
        }
    }
    
    double get_focal_length() const override {
        // Not available from mount - return 0 (unknown)
        return 0.0;
    }
    
    GuideRate get_guide_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        // Use structured binding to avoid std::pair copy warning
        auto [ra_rate, dec_rate] = protocol.get_guide_rates();
        
        GuideRate guide_rate;
        guide_rate.ra = ra_rate;
        guide_rate.dec = dec_rate;
        return guide_rate;
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
        
        auto& protocol = iOptronProtocolWrapper::instance();
        MountStatus status = protocol.get_status();
        return status.is_slewing;
    }
    
    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        Position pos = protocol.get_position();
        return pos.ra_hours;
    }
    
    double get_right_ascension_rate() const override {
        // iOptron doesn't support getting RA rate directly
        // Calculate from tracking rate
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        MountStatus status = protocol.get_status();
        
        // Sidereal rate is 15.041 arcsec/sec
        double sidereal_rate = 15.041;
        
        if (status.tracking_rate == 4) {
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
        
        // Parse side of pier from :GEP# response
        // This would require parsing the full response
        // For now, return 0 (unknown)
        (void)lock;  // Lock is used for RAII, suppress unused warning
        return 0;
    }
    
    void set_side_of_pier(int side) override {
        (void)side;  // Unused - not supported
        throw AlpacaException("Setting pier side not supported by iOptron mount");
    }
    
    double get_sidereal_time() const override {
        // Calculate from UTC time and longitude
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        SiteInfo site = protocol.get_site_info();
        
        // Simplified calculation - would need proper sidereal time calculation
        // For now, return approximate value
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* utc_tm = std::gmtime(&time_t);
        
        double hours = utc_tm->tm_hour + utc_tm->tm_min / 60.0 + utc_tm->tm_sec / 3600.0;
        double lst = hours + site.longitude_degrees / 15.0;
        if (lst < 0) lst += 24.0;
        if (lst >= 24.0) lst -= 24.0;
        
        return lst;
    }
    
    double get_site_elevation() const override {
        // Not available from mount - return 0 (unknown)
        return 0.0;
    }
    
    void set_site_elevation(double elevation) override {
        // Not supported by iOptron mount
        // Silently ignore
        (void)elevation;  // Unused - not supported
    }
    
    double get_site_latitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        SiteInfo site = protocol.get_site_info();
        return site.latitude_degrees;
    }
    
    void set_site_latitude(double latitude) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_latitude(latitude);
    }
    
    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        SiteInfo site = protocol.get_site_info();
        return site.longitude_degrees;
    }
    
    void set_site_longitude(double longitude) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_longitude(longitude);
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
        
        auto& protocol = iOptronProtocolWrapper::instance();
        MountStatus status = protocol.get_status();
        
        if (status.tracking_rate == 4) {
            // Custom tracking rate
            return protocol.get_custom_tracking_rate();
        } else {
            // Standard rates: 0=sidereal (1.0), 1=lunar, 2=solar, 3=King
            // Return as multiplier of sidereal
            return 1.0;  // Default to sidereal
        }
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
    }
    
    std::vector<int> get_tracking_rates() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        // Return supported tracking rates as DriveRates enum values:
        // 0 = driveSidereal, 1 = driveLunar, 2 = driveSolar, 3 = driveKing
        // iOptron mounts support all standard tracking rates
        return {0, 1, 2, 3};
    }
    
    std::chrono::system_clock::time_point get_utc_date() const override {
        // Get from system clock (mount doesn't provide this directly)
        return std::chrono::system_clock::now();
    }
    
    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.set_utc_time(utc);
    }
    
    // Telescope methods
    
    void find_home() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        if (!get_can_find_home()) {
            throw AlpacaException("Find home not supported on this mount model");
        }
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.find_home();
    }
    
    void park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        if (!protocol.park()) {
            throw AlpacaException("Failed to park mount");
        }
    }
    
    void pulse_guide(int direction, int duration) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.pulse_guide(direction, duration);
    }
    
    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        // Get current position and set as park position
        auto& protocol = iOptronProtocolWrapper::instance();
        AltAz altaz = protocol.get_alt_az();
        protocol.set_park_position(altaz.altitude_degrees, altaz.azimuth_degrees);
    }
    
    void slew_to_coordinates() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        if (!protocol.slew_to_ra_dec()) {
            throw AlpacaException("Slew failed - target may be below altitude limit");
        }
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
    }
    
    void sync_to_target() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.sync_to_coordinates();
    }
    
    void unpark() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        
        auto& protocol = iOptronProtocolWrapper::instance();
        protocol.unpark();
    }

private:
    void check_connected() const {
        if (!connected_) {
            throw AlpacaException("Not connected to mount");
        }
    }
    
    int device_number_;
    ConnectionInfo connection_info_;
    bool connected_;
    MountInfo mount_info_;
    mutable std::mutex mutex_;
    
    // Target coordinates
    double target_ra_hours_;
    double target_dec_degrees_;
};

// Factory function implementation
std::unique_ptr<TelescopeDriver> create_ioptron_telescope(
    int device_number,
    const ConnectionInfo& connection_info)
{
    return std::make_unique<iOptronTelescopeDriver>(device_number, connection_info);
}

} // namespace alpacacore::vendor::ioptron

