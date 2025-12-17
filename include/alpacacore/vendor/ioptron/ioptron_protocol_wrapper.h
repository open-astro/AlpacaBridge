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

#pragma once

#include <string>
#include <memory>
#include <chrono>

namespace alpacacore::vendor::ioptron {

/**
 * @brief Connection type for iOptron mount.
 */
enum class ConnectionType {
    Serial,   // USB serial port connection
    Network   // WiFi TCP socket connection
};

/**
 * @brief Connection information for iOptron mount.
 */
struct ConnectionInfo {
    ConnectionType type = ConnectionType::Serial;

    // For Serial connection:
    std::string port_path;  // "/dev/ttyUSB0" or "COM3"
    int baud_rate = 9600;   // Standard RS-232 baud rate

    // For Network connection:
    std::string host;       // IP address or hostname
    int tcp_port = 4030;    // Default port (CEM60-EC: 4030, HEM27: 8899, etc.)

    /**
     * @brief Per-command response timeout in milliseconds.
     *
     * Some mounts (or transport layers like WiFi bridges) can take several
     * seconds to return the first character of a response, especially for
     * the initial connection / :MountInfo# query.
     *
     * We default to 5000 ms to stay comfortably below typical Alpaca client
     * HTTP timeouts (~10 s) while still allowing enough time for a healthy
     * mount to respond. Higher layers (e.g. AlpacaHTTP or a custom server)
     * can override this if needed.
     */
    int response_timeout_ms = 5000;
};

/**
 * @brief Mount information from :MountInfo# command.
 */
struct MountInfo {
    std::string model_code;  // e.g., "0026" for CEM26
    std::string model_name;   // e.g., "CEM26"
    bool has_encoder = false; // EC models have encoders
};

/**
 * @brief Mount position in RA/Dec.
 */
struct Position {
    double ra_hours = 0.0;      // Right ascension in hours
    double dec_degrees = 0.0;   // Declination in degrees
};

/**
 * @brief Mount position in Alt/Az.
 */
struct AltAz {
    double altitude_degrees = 0.0;  // Altitude in degrees
    double azimuth_degrees = 0.0;  // Azimuth in degrees
};

/**
 * @brief Mount status information.
 */
struct MountStatus {
    bool is_tracking = false;
    bool is_slewing = false;
    bool is_parked = false;
    bool is_at_home = false;
    int system_status = 0;  // From :GLS# response
    int tracking_rate = 0;  // 0=sidereal, 1=lunar, 2=solar, 3=King, 4=custom
};

/**
 * @brief Site information (longitude, latitude, etc.).
 */
struct SiteInfo {
    double longitude_degrees = 0.0;  // East is positive
    double latitude_degrees = 0.0;  // North is positive
    bool is_northern_hemisphere = true;
    int timezone_offset_minutes = 0;  // Minutes from UTC
    bool dst_observed = false;
};

/**
 * @brief Clean C++ interface wrapping iOptron RS-232 protocol.
 *
 * This wrapper isolates the RS-232 command protocol and transport layer
 * (serial/TCP) from the driver implementation. All platform-specific code
 * is hidden using the PIMPL pattern.
 */
class iOptronProtocolWrapper {
public:
    /**
     * @brief Get singleton instance.
     */
    static iOptronProtocolWrapper& instance();
    
    /**
     * @brief Connect to the mount.
     *
     * @param info Connection information (serial port or network)
     * @return true if connection successful, false otherwise
     */
    bool connect(const ConnectionInfo& info);
    
    /**
     * @brief Disconnect from the mount.
     */
    void disconnect();
    
    /**
     * @brief Check if connected to mount.
     */
    bool is_connected() const;
    
    // Mount information queries
    
    /**
     * @brief Get mount model information.
     *
     * Sends :MountInfo# command.
     */
    MountInfo get_mount_info();
    
    /**
     * @brief Get current RA/Dec position.
     *
     * Sends :GEP# command.
     */
    Position get_position();
    
    /**
     * @brief Get current Alt/Az position.
     *
     * Sends :GAC# command.
     */
    AltAz get_alt_az();
    
    /**
     * @brief Get mount status.
     *
     * Sends :GLS# command.
     */
    MountStatus get_status();
    
    /**
     * @brief Get site information (longitude, latitude, etc.).
     *
     * Sends :GLS# and :GUT# commands.
     */
    SiteInfo get_site_info();
    
    /**
     * @brief Get parking position.
     *
     * Sends :GPC# command.
     */
    AltAz get_park_position();
    
    // Mount motion commands
    
    /**
     * @brief Set target RA.
     *
     * Sends :SRATTTTTTTTT# command.
     *
     * @param ra_hours Right ascension in hours
     */
    void set_target_ra(double ra_hours);
    
    /**
     * @brief Set target Dec.
     *
     * Sends :SdsTTTTTTTT# command.
     *
     * @param dec_degrees Declination in degrees
     */
    void set_target_dec(double dec_degrees);
    
    /**
     * @brief Slew to target RA/Dec (normal position).
     *
     * Sends :MS1# command.
     *
     * @return true if command accepted, false if target below altitude limit
     */
    bool slew_to_ra_dec();
    
    /**
     * @brief Slew to target RA/Dec (counterweight up position).
     *
     * Sends :MS2# command. Equatorial mounts only.
     *
     * @return true if command accepted, false if target below altitude limit
     */
    bool slew_to_ra_dec_cw_up();
    
    /**
     * @brief Stop all slewing.
     *
     * Sends :Q# command.
     */
    void stop_slewing();
    
    /**
     * @brief Start tracking.
     *
     * Sends :ST1# command.
     */
    void start_tracking();
    
    /**
     * @brief Stop tracking.
     *
     * Sends :ST0# command.
     */
    void stop_tracking();
    
    /**
     * @brief Sync to target coordinates.
     *
     * Sends :CM# command after setting target RA/Dec.
     */
    void sync_to_coordinates();
    
    // Parking commands
    
    /**
     * @brief Park the mount.
     *
     * Sends :MP1# command.
     *
     * @return true if park accepted, false if park failed
     */
    bool park();
    
    /**
     * @brief Unpark the mount.
     *
     * Sends :MP0# command.
     */
    void unpark();
    
    /**
     * @brief Set parking position.
     *
     * Sends :SPATTTTTTTTT# and :SPHTTTTTTTT# commands.
     *
     * @param alt_degrees Altitude in degrees
     * @param az_degrees Azimuth in degrees
     */
    void set_park_position(double alt_degrees, double az_degrees);
    
    // Home position commands
    
    /**
     * @brief Go to zero (home) position.
     *
     * Sends :MH# command.
     */
    void go_to_home();
    
    /**
     * @brief Auto-search zero (home) position.
     *
     * Sends :MSH# command. Uses homing sensors.
     * Available on CEM120, CEM70, GEM45, CEM40 series.
     */
    void find_home();
    
    // Pulse guiding commands
    
    /**
     * @brief Pulse guide in specified direction.
     *
     * Sends :ZSXXXXX#, :ZQXXXXX#, :ZEXXXXX#, or :ZCXXXXX# command.
     *
     * @param direction 0=North (Dec+), 1=South (Dec-), 2=East (RA+), 3=West (RA-)
     * @param duration_ms Duration in milliseconds (0-99999)
     */
    void pulse_guide(int direction, int duration_ms);
    
    // Site settings
    
    /**
     * @brief Set site longitude.
     *
     * Sends :SLOsTTTTTTTT# command.
     *
     * @param longitude_degrees Longitude in degrees (East is positive)
     */
    void set_longitude(double longitude_degrees);
    
    /**
     * @brief Set site latitude.
     *
     * Sends :SLAsTTTTTTTT# command.
     *
     * @param latitude_degrees Latitude in degrees (North is positive)
     */
    void set_latitude(double latitude_degrees);
    
    /**
     * @brief Set hemisphere.
     *
     * Sends :SHE0# (Southern) or :SHE1# (Northern).
     *
     * @param is_northern true for Northern, false for Southern
     */
    void set_hemisphere(bool is_northern);
    
    /**
     * @brief Set UTC time.
     *
     * Sends :SUTXXXXXXXXXXXXX# command.
     *
     * @param utc_time UTC time point
     */
    void set_utc_time(std::chrono::system_clock::time_point utc_time);
    
    /**
     * @brief Set timezone offset.
     *
     * Sends :SGsMMM# command.
     *
     * @param offset_minutes Minutes offset from UTC (DST not included)
     */
    void set_timezone_offset(int offset_minutes);
    
    /**
     * @brief Set daylight saving time.
     *
     * Sends :SDS0# or :SDS1# command.
     *
     * @param observed true if DST observed, false otherwise
     */
    void set_dst_observed(bool observed);
    
    // Tracking rate commands
    
    /**
     * @brief Set tracking rate.
     *
     * Sends :RTx# command.
     *
     * @param rate 0=sidereal, 1=lunar, 2=solar, 3=King, 4=custom
     */
    void set_tracking_rate(int rate);
    
    /**
     * @brief Get custom tracking rate.
     *
     * Sends :GTR# command.
     *
     * @return Custom tracking rate multiplier (e.g., 1.0 = sidereal)
     */
    double get_custom_tracking_rate();
    
    /**
     * @brief Set custom tracking rate.
     *
     * Sends :RRnnnnn# command.
     *
     * @param rate_multiplier Rate multiplier (0.1000 to 1.9000 × sidereal)
     */
    void set_custom_tracking_rate(double rate_multiplier);
    
    // Guiding rate commands
    
    /**
     * @brief Get guiding rates.
     *
     * Sends :AG# command.
     *
     * @return Pair of (RA_rate, Dec_rate) as fractions of sidereal
     */
    std::pair<double, double> get_guide_rates();
    
    /**
     * @brief Set guiding rates.
     *
     * Sends :RGnnnn# command.
     *
     * @param ra_rate RA guiding rate (0.01 to 0.90 × sidereal)
     * @param dec_rate Dec guiding rate (0.10 to 0.99 × sidereal)
     */
    void set_guide_rates(double ra_rate, double dec_rate);
    
    // Low-level protocol access (for advanced use)
    
    /**
     * @brief Send command and get response.
     *
     * Low-level protocol access. Most users should use the high-level methods.
     *
     * @param command RS-232 command (without # terminator)
     * @return Response string (without # terminator)
     */
    std::string send_command(const std::string& command);
    
    /**
     * @brief Send command without waiting for response.
     *
     * @param command RS-232 command (without # terminator)
     */
    void send_command_blind(const std::string& command);

private:
    iOptronProtocolWrapper();
    ~iOptronProtocolWrapper();
    iOptronProtocolWrapper(const iOptronProtocolWrapper&) = delete;
    iOptronProtocolWrapper& operator=(const iOptronProtocolWrapper&) = delete;
    
    // PIMPL pattern - hides platform-specific implementation
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace alpacacore::vendor::ioptron

