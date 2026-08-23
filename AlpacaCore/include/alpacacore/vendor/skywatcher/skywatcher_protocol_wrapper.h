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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::skywatcher {

// Sky-Watcher motor controller protocol (the ":" command set spoken by the
// mount's own motor board — NOT the SynScan hand-controller protocol). Used by
// the Wave series (Wave 100i/150i), AZ-GTi and other mounts when talking to
// the mount directly over USB serial or the built-in Wi-Fi module (UDP 11880).

struct SkyWatcherPortInfo {
    std::string port_path;
    std::string device_id;
    std::string firmware_version;  // motor board version, e.g. "3.42.09"
};

std::vector<SkyWatcherPortInfo> enumerate_skywatcher_ports();

struct SkyWatcherHostInfo {
    std::string host;
    int udp_port = 11880;
    std::string firmware_version;
};

// Probe well-known addresses (AP-mode 192.168.4.1, local gateways) and UDP
// broadcast on local subnets for a responding motor controller.
std::vector<SkyWatcherHostInfo> discover_skywatcher_hosts(int timeout_ms = 1500);

enum class ConnectionType {
    Serial,
    Network
};

struct ConnectionInfo {
    ConnectionType type = ConnectionType::Serial;

    // Serial connection (mount USB port / direct motor-controller cable)
    std::string port_path;
    int baud_rate = 9600;

    // Network connection (built-in Wi-Fi module, UDP datagrams)
    std::string host;
    int udp_port = 11880;

    // Per-command response timeout
    int response_timeout_ms = 1000;
};

// Axis channel words per the MC command set: "1" = RA/Az, "2" = Dec/Alt.
inline constexpr int kAxisRa = 1;
inline constexpr int kAxisDec = 2;

// Decoded ":f" status reply.
struct AxisStatus {
    bool speed_mode = false;   // true = Speed(Tracking) mode, false = GOTO mode
    bool ccw = false;          // true = rotating in the decreasing-counts direction
    bool fast = false;         // true = high-speed slewing
    bool running = false;      // motor energized and moving
    bool blocked = false;      // axis blocked (stall / clutch)
    bool init_done = false;    // ":F" initialization completed
    bool level_switch_on = false;
};

// Per-axis static parameters read once at connect.
struct AxisParameters {
    uint32_t counts_per_revolution = 0;  // ":a"
    uint32_t timer_frequency = 0;        // ":b"
    uint32_t high_speed_ratio = 1;       // ":g"
};

class SkyWatcherProtocolWrapper {
public:
    static SkyWatcherProtocolWrapper& instance();

    bool connect(const ConnectionInfo& info);
    void disconnect();
    bool is_connected() const;

    // Low-level framed exchange: sends ":<cmd><axis><data>\r", returns the
    // payload of a "=" response (without the leading "=" or trailing CR).
    // Throws AlpacaException on transport failure or a "!" error reply.
    std::string send_command(char command, int axis, const std::string& data = "",
                             int timeout_ms_override = 0);
    // Fire the command and return the raw reply without "!"-to-exception
    // mapping (for CommandString passthrough).
    std::string send_raw_command(const std::string& frame, int timeout_ms_override = 0);

    // ── Inquiries ──
    std::string get_motor_board_version();          // ":e" axis 1
    AxisParameters get_axis_parameters(int axis);   // ":a"/":b"/":g"
    uint32_t inquire_position(int axis);            // ":j" (24-bit counts)
    AxisStatus inquire_status(int axis);            // ":f"

    // ── Motion ──
    void set_position(int axis, uint32_t counts);          // ":E" (sync)
    void initialization_done(int axis);                    // ":F"
    void set_motion_mode(int axis, char mode, char direction);  // ":G"
    void set_goto_target(int axis, uint32_t counts);       // ":S"
    void set_step_period(int axis, uint32_t t1_preset);    // ":I"
    void start_motion(int axis);                           // ":J"
    void stop_motion(int axis);                            // ":K"
    void instant_stop(int axis);                           // ":L"
    void set_autoguide_speed(int axis, int speed_code);    // ":P" 0=1x..4=0.125x
    uint32_t get_feature(int axis, uint32_t inquiry);      // ":q" (features / home index)
    void set_feature(int axis, uint32_t command);          // ":W" (reset home index etc.)

    // Nibble-swapped hex encode/decode per the MC data format
    // (0x123456 <-> "563412", 0x12 <-> "12").
    static std::string encode_u24(uint32_t value);
    static uint32_t decode_u24(const std::string& data);

private:
    SkyWatcherProtocolWrapper();
    ~SkyWatcherProtocolWrapper();

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace alpacacore::vendor::skywatcher
