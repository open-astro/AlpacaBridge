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
#include <utility>
#include <vector>
#include <cstdint>

namespace alpacacore::vendor::celestron {

struct CelestronPortInfo {
    std::string port_path;       // e.g., "/dev/ttyUSB1"
    std::string device_id;       // e.g., "usb-Prolific_Technology_Inc._USB-Serial_Controller_D-if00-port0"
    std::string firmware_version; // HC firmware version from probe, e.g., "5.35"
};

// Scans /dev/serial/by-id/ for Prolific/FTDI/CP210x USB-serial adapters,
// probes each with NexStar echo command to identify Celestron mounts.
std::vector<CelestronPortInfo> enumerate_celestron_ports();


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
    int tcp_port = 2000;

    // Per-command response timeout (NexStar spec says up to 3.5s worst case)
    int response_timeout_ms = 5000;
};

struct LocationInfo {
    double latitude_degrees = 0.0;
    double longitude_degrees = 0.0;
};

struct TimeInfo {
    int hour = 0;
    int minute = 0;
    int second = 0;
    int month = 1;
    int day = 1;
    int year = 0; // two-digit year (e.g., 25 for 2025)
    int timezone_offset_minutes = 0;
    bool dst_enabled = false;
};

class CelestronProtocolWrapper {
public:
    static CelestronProtocolWrapper& instance();

    bool connect(const ConnectionInfo& info);
    void disconnect();
    bool is_connected() const;

    std::string send_command(const std::string& command,
                             bool require_hash_terminator = true,
                             int timeout_ms_override = 0);
    std::string send_raw_command(const std::string& bytes,
                                 bool require_hash_terminator = true,
                                 int timeout_ms_override = 0);
    void send_command_blind(const std::string& command);

    std::string get_handset_firmware_version();
    int get_model_id();
    std::string get_model_name();
    bool is_aligned();
    bool is_goto_in_progress();
    void cancel_goto();
    int get_tracking_mode();
    void set_tracking_mode(int mode);
    void move_axis_fixed_rate(int axis, int rate);
    void move_axis_variable_rate(int axis, double rate_deg_per_sec);

    std::pair<uint32_t, uint32_t> get_ra_dec_raw(bool precise);
    std::pair<uint32_t, uint32_t> get_alt_az_raw(bool precise);
    void goto_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise);
    void goto_alt_az_raw(uint32_t az_raw, uint32_t alt_raw, bool precise);
    void sync_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise);

    // MC_SEEK_INDEX / MC_AT_INDEX — PEC worm gear index mark (RA axis only).
    void seek_index(int axis);
    bool is_at_index(int axis);

    // MC_LEVEL_START / MC_LEVEL_DONE — hardware home (both axes).
    // Moves each axis to its physical home/index switch position.
    // Supported on mounts with hardware home switches (CGX, CGX-L, CGE Pro).
    void level_start(int axis);
    bool is_level_done(int axis);

    // Bus probing — GET_VER (0xFE) to any device address.
    // Returns major.minor firmware version string, or empty string if device absent.
    std::string get_device_firmware(int device_address);

    // Known AUX bus device addresses.
    static constexpr int DEV_RA_MC    = 0x10;
    static constexpr int DEV_DEC_MC   = 0x11;
    static constexpr int DEV_FOCUSER  = 0x12;
    static constexpr int DEV_HC       = 0x04;
    static constexpr int DEV_HC_PLUS  = 0x0D;
    static constexpr int DEV_GPS      = 0xB0;
    static constexpr int DEV_RTC      = 0xB2;
    static constexpr int DEV_WIFI     = 0xB5;
    static constexpr int DEV_BAT      = 0xB6;
    static constexpr int DEV_DEW      = 0x17;
    static constexpr int DEV_LIGHT    = 0xBF;
    static constexpr int DEV_RA_SW    = 0x30;
    static constexpr int DEV_DEC_SW   = 0x31;
    static constexpr int DEV_DEC_AG   = 0x32;

    struct BusDevice {
        int address = 0;
        std::string name;
        std::string firmware_version;
    };

    // Scans known device addresses and returns those that respond.
    std::vector<BusDevice> probe_bus();

    // Pulse guiding via MTR_AUX_GUIDE (0x26).
    // velocity: signed percentage of sidereal rate (-100 to +100).
    // duration_cs: time in centiseconds (10ms units, max 255 = 2550ms).
    void pulse_guide_axis(int axis, int velocity, int duration_cs);
    bool is_aux_guide_active(int axis);

    // Pier side via HC 'p' command. Returns 'W' (west/normal) or 'E' (east/through-the-pole).
    char get_pier_side();

    // Autoguide rate (percentage of sidereal).
    void set_autoguide_rate(int axis, double percent);
    double get_autoguide_rate(int axis);

    // PEC (Periodic Error Correction) — RA axis only.
    void pec_seek_index();
    bool pec_at_index();
    void pec_record_start();
    void pec_record_stop();
    bool pec_record_done();
    void pec_playback(bool enable);
    int  pec_get_bin();

    // Slew completion polling via MC pass-through (per-axis, more granular than 'L').
    bool is_slew_done(int axis);

    // MC_GET_POSITION (0x01) — read 24-bit encoder position directly from motor controller.
    // Use when HC is unavailable (fw 0.0) and 'E'/'e' commands return zeroes.
    uint32_t get_mc_position(int axis);

    // MC_GOTO_FAST (0x02) — pass-through GOTO at max rate, bypasses HC.
    void mc_goto_fast(int axis, uint32_t position);

    // MC_SET_POSITION (0x04) — set position counter without moving (pass-through Sync).
    void mc_set_position(int axis, uint32_t position);

    LocationInfo get_location();
    void set_location(const LocationInfo& info);
    TimeInfo get_time();
    void set_time(const TimeInfo& info);

private:
    CelestronProtocolWrapper();
    ~CelestronProtocolWrapper();

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace alpacacore::vendor::celestron
