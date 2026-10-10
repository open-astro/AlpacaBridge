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

#include <alpacacore/util/task_clock.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::skywatcher {

// Sky-Watcher motor controller protocol (the ":" command set spoken by the
// mount's own motor board — NOT the SynScan hand-controller protocol). Used by
// the Wave series (Wave 100i/150i), AZ-GTi and other mounts when talking to
// the mount directly over USB serial or the built-in Wi-Fi module (UDP 11880).

// Decoded ":e" reply. The three payload bytes are firmware major, firmware
// minor, and the MOUNT CODE -- the third byte is an identity, not a patch
// level (0x44 = Wave 100i, 0x32 = EQM-35 Pro), matching INDI's
// skywatcherAPI.cpp MountType enum.
struct MotorBoardInfo {
    std::string firmware_version = "";  // e.g. "3.39"
    std::uint8_t mount_code = 0;
    std::string model_name = "";  // e.g. "EQM-35 Pro", or "Mount (code 0xNN)"
};

// Map a ":e" mount-code byte to a human-readable model name.
std::string mount_code_to_name(std::uint8_t mount_code);

struct SkyWatcherPortInfo {
    std::string port_path;
    std::string device_id;
    std::string firmware_version;  // motor board version, e.g. "3.39"
    // Baud the probe actually succeeded at. Synta EQ boards reached over the
    // mount's built-in USB port or an EQDIR cable are real UART bridges and
    // answer at 115200 (EQM-35 Pro) or 9600; the Wave's STM32 CDC-ACM port
    // ignores baud entirely. Auto-detect MUST carry this into ConnectionInfo.
    int baud_rate = 9600;
    std::uint8_t mount_code = 0;
    std::string model_name;
};

std::vector<SkyWatcherPortInfo> enumerate_skywatcher_ports();

// Auto-detect probes (exposed for the pty-backed tests). probe_skywatcher_port
// opens @p port_path at @p baud_rate, sends ":e1" and returns the raw payload
// ("" on no answer, a port held in the cross-vendor registry, or a SynScan
// hand controller answering the echo guard at a non-9600 rate).
// probe_skywatcher_port_any_baud tries kProbeBauds in order (9600 first) and
// decodes the answer into @p info_out / @p baud_out.
std::string probe_skywatcher_port(const std::string& port_path, int baud_rate);
bool probe_skywatcher_port_any_baud(const std::string& port_path, MotorBoardInfo& info_out, int& baud_out);

struct SkyWatcherHostInfo {
    std::string host;
    int udp_port = 11880;
    std::string firmware_version;
};

// Probe well-known addresses (AP-mode 192.168.4.1, local gateways) and UDP
// broadcast on local subnets for a responding motor controller.
std::vector<SkyWatcherHostInfo> discover_skywatcher_hosts(int timeout_ms = 1500);

enum class ConnectionType : std::uint8_t { Serial, Network };

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
    bool speed_mode = false;  // true = Speed(Tracking) mode, false = GOTO mode
    bool ccw = false;         // true = rotating in the decreasing-counts direction
    bool fast = false;        // true = high-speed slewing
    bool running = false;     // motor energized and moving
    bool blocked = false;     // axis blocked (stall / clutch)
    bool init_done = false;   // ":F" initialization completed
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
    // One wrapper owns one transport. Drivers own distinct instances; the legacy
    // singleton remains available for standalone protocol callers.
    using SerialRead = std::function<std::ptrdiff_t(int, char*, std::size_t)>;
    using SerialWrite = std::function<std::ptrdiff_t(int, const char*, std::size_t)>;
    // Optional read/write seams exercise quiet/hung-up tty behavior -- and a
    // stalled TX buffer (write EAGAIN) -- without hardware.
    explicit SkyWatcherProtocolWrapper(SerialRead serial_read = {}, SerialWrite serial_write = {});
    virtual ~SkyWatcherProtocolWrapper();
    static SkyWatcherProtocolWrapper& instance();

    bool connect(const ConnectionInfo& info);
    void disconnect();
    bool is_connected() const;
    // Decision 0009: the clock the staleness bound and the link-lost stamp run
    // on. Real by default; the driver passes its own TaskClock.
    void set_task_clock(util::TaskClock& clock);
    // open-astro#445: is_connected() without I/O and without waiting on an
    // exchange, that also notices a serial device which has gone away (and
    // closes the dead link when it can). Safe to call from a Connected poll.
    virtual bool link_alive();

    // open-astro#505: a board that stops answering while its node is still
    // there — mount powered off with the adapter plugged in, EQDIR pulled at
    // the mount end, controller hung. link_alive() cannot see that (the fd is
    // healthy and the node resolves), so consecutive failed exchanges latch a
    // fault instead. Non-empty fault means the caller must refuse to serve its
    // CACHE (DriverException "communications compromised"), never that it
    // should stop talking: on a polled link the reads are the only traffic
    // that can clear the latch. Connected stays true while the fault is younger
    // than util::kLinkStalenessBound (30 s); past it the link is LOST
    // (decision 0009): link_alive() reads false, the transport is closed and
    // the last fault text stays here until the next connect or disconnect.
    // Cheap and lock-free enough for a read path (own leaf mutex, no I/O).
    virtual std::string link_fault();
    virtual bool link_faulted();
    // Bumped each time a good reply clears a latched fault. A driver keeps the
    // value it last saw and re-validates the board when it changes, because a
    // board that came back from a power cycle answers perfectly well while
    // reporting init_done false with its position registers reset.
    virtual std::uint64_t link_recovery_epoch();

    // open-astro#521: when the link was last LOST, cleared as it is read. A
    // relink needs the LENGTH of the outage: a Sky-Watcher axis keeps running
    // with no further commands, so motion that survived a brief cable glitch
    // is still wanted, while motion that survived a long one has had nobody in
    // control of it. A client's own disconnect does not stamp this, so a fresh
    // connect sees nullopt and takes the safe branch.
    virtual std::optional<std::chrono::steady_clock::time_point> consume_link_lost_at();

    // Low-level framed exchange: sends ":<cmd><axis><data>\r", returns the
    // payload of a "=" response (without the leading "=" or trailing CR).
    // Throws AlpacaException on transport failure or a "!" error reply.
    std::string send_command(char command, int axis, const std::string& data = "", int timeout_ms_override = 0);
    // Fire the command and return the raw reply without "!"-to-exception
    // mapping (for CommandString passthrough).
    std::string send_raw_command(const std::string& frame, int timeout_ms_override = 0);

    // ── Inquiries ──
    std::string get_motor_board_version();         // ":e" axis 1 (firmware only)
    MotorBoardInfo get_motor_board_info();         // ":e" axis 1, decoded
    AxisParameters get_axis_parameters(int axis);  // ":a"/":b"/":g"
    uint32_t inquire_position(int axis);           // ":j" (24-bit counts)
    AxisStatus inquire_status(int axis);           // ":f"

    // ── Motion ──
    void set_position(int axis, uint32_t counts);               // ":E" (sync)
    void initialization_done(int axis);                         // ":F"
    void set_motion_mode(int axis, char mode, char direction);  // ":G"
    void set_goto_target(int axis, uint32_t counts);            // ":S"
    // ":I". with_readback=false skips the diagnostic ":i" comparison (one extra
    // serial round-trip) for callers on a timing-critical path.
    void set_step_period(int axis, uint32_t t1_preset, bool with_readback = true);
    // Turn the ":i" readback diagnostic off until the next connect(), for a
    // board whose ":i" reply does not reflect the stored preset
    // (open-astro#686). connect() turns it back on.
    void disable_step_period_readback();
    void start_motion(int axis);                         // ":J"
    void stop_motion(int axis);                          // ":K"
    void instant_stop(int axis);                         // ":L"
    void set_autoguide_speed(int axis, int speed_code);  // ":P" 0=1x..4=0.125x
    uint32_t get_feature(int axis, uint32_t inquiry);    // ":q" (features / home index)
    void set_feature(int axis, uint32_t command);        // ":W" (reset home index etc.)

    // Nibble-swapped hex encode/decode per the MC data format
    // (0x123456 <-> "563412", 0x12 <-> "12").
    static std::string encode_u24(uint32_t value);
    static uint32_t decode_u24(const std::string& data);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

}  // namespace alpacacore::vendor::skywatcher
