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

#include <chrono>
#include <memory>
#include <string>

namespace alpacacore::vendor::zwo {

enum class ConnectionType {
    Serial,
    Network
};

struct ConnectionInfo {
    ConnectionType type = ConnectionType::Serial;

    // Serial connection
    std::string port_path;
    int baud_rate = 9600;

    // Network connection
    std::string host;
    int tcp_port = 4030;

    // Per-command timeout
    int response_timeout_ms = 5000;
};

enum class MountMode {
    Unknown,
    Equatorial,
    AltAzimuth
};

struct EquatorialCoordinates {
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
};

struct HorizontalCoordinates {
    double azimuth_degrees = 0.0;
    double altitude_degrees = 0.0;
};

struct SiteInfo {
    double latitude_degrees = 0.0;
    double longitude_degrees = 0.0;
};

struct TimeInfo {
    int month = 1;
    int day = 1;
    int year = 2000;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int timezone_offset_minutes = 0;
};

struct StatusInfo {
    bool no_tracking = false;
    bool stop_or_tracking = false;
    bool low_power = false;
    bool at_home = false;
    MountMode mode = MountMode::Unknown;
    bool ra_stall = false;
    bool dec_stall = false;
    bool ra_guiding = false;
    bool dec_guiding = false;
    std::string raw;
};

enum class ParkStatus {
    Unknown = -1,
    NotParked = 0,
    InProgress = 1,
    Completed = 2,
    Error = 3
};

class ZWOMountProtocolWrapper {
public:
    static ZWOMountProtocolWrapper& instance();

    bool connect(const ConnectionInfo& info);
    void disconnect();
    bool is_connected() const;

    std::string send_command(const std::string& command,
                             bool require_hash_terminator = true,
                             int timeout_ms_override = 0);
    void send_command_blind(const std::string& command);

    std::string get_mount_info();

    EquatorialCoordinates get_current_equatorial();
    HorizontalCoordinates get_current_horizontal();

    EquatorialCoordinates get_target_equatorial();
    void set_target_ra(double ra_hours);
    void set_target_dec(double dec_degrees);
    void sync_target_equatorial(double ra_hours, double dec_degrees);

    bool goto_target();
    void sync_target();
    void abort_motion();

    StatusInfo get_status();

    bool get_tracking_enabled();
    void set_tracking_enabled(bool enabled);

    int get_tracking_rate();
    void set_tracking_rate(int rate);

    void set_move_rate_sidereal_multiple(double multiple);
    void start_move_east();
    void stop_move_east();
    void start_move_west();
    void stop_move_west();
    void start_move_north();
    void stop_move_north();
    void start_move_south();
    void stop_move_south();

    void pulse_guide(int direction, int duration_ms);
    double get_guide_rate();
    void set_guide_rate(double guide_rate);

    SiteInfo get_site_info();
    void set_site_info(const SiteInfo& info);

    TimeInfo get_time_info();
    void set_time_info(const TimeInfo& info);

    double get_sidereal_time_hours();

    char get_mount_direction();

    void go_home();
    void park();
    bool set_custom_park_here();
    bool unpark();
    ParkStatus get_park_status();
    bool has_successful_homing();

private:
    ZWOMountProtocolWrapper();
    ~ZWOMountProtocolWrapper();

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace alpacacore::vendor::zwo
