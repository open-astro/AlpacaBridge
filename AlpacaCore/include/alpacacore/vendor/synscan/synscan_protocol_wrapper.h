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

#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <utility>
#include <vector>

namespace alpacacore::vendor::synscan {

struct SynScanPortInfo {
    std::string port_path;
    std::string device_id;
    std::string firmware_version;
};

std::vector<SynScanPortInfo> enumerate_synscan_ports();

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
    int tcp_port = 11880;

    // Per-command response timeout
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

class SynScanProtocolWrapper {
public:
    static SynScanProtocolWrapper& instance();

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
    bool is_aligned();
    bool is_goto_in_progress();
    void cancel_goto();
    char get_pointing_state();
    int get_tracking_mode();
    void set_tracking_mode(int mode);
    void move_axis_fixed_rate(int axis, int rate);
    void move_axis_variable_rate(int axis, double rate_deg_per_sec);

    std::pair<uint32_t, uint32_t> get_ra_dec_raw(bool precise);
    std::pair<uint32_t, uint32_t> get_alt_az_raw(bool precise);
    void goto_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise);
    void goto_alt_az_raw(uint32_t az_raw, uint32_t alt_raw, bool precise);
    void sync_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise);

    LocationInfo get_location();
    void set_location(const LocationInfo& info);
    TimeInfo get_time();
    void set_time(const TimeInfo& info);

private:
    SynScanProtocolWrapper();
    ~SynScanProtocolWrapper();

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace alpacacore::vendor::synscan
