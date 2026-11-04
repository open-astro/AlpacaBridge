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
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::vendor::qhy {

/**
 * @brief Serial settings for the QHY Q-Focuser.
 *
 * The Q-Focuser is a USB CDC-ACM device (GigaDevice GD32 MCU, USB
 * 28e9:018a, enumerates as /dev/ttyACMn) speaking a line-less JSON protocol
 * at 9600 8N1. It does not use the QHY camera SDK at all.
 */
struct QFocuserConnectionConfig {
    std::string serial_port;       // e.g. "/dev/ttyACM0"
    int baud_rate = 9600;          // fixed by the firmware
    int serial_timeout_ms = 3000;  // matches INDI/INDIGO's 3 s reply timeout
};

/**
 * @brief Identity returned by the {"cmd_id":1} handshake.
 *
 * Reply: {"idx":1,"id":"<mcu serial>","version":<yyyymmdd>,"bv":<board>}.
 * The id string carries raw MCU unique-id bytes (control characters
 * included), so it is kept only in printable form.
 */
struct QFocuserDeviceInfo {
    std::string id;
    std::int32_t firmware = 0;       // e.g. 20231207
    std::int32_t board_version = 0;  // e.g. 208
};

/**
 * @brief One {"cmd_id":4} telemetry snapshot.
 *
 * Reply: {"idx":4,"temp":..,"c_t":<chip milli-C>,"c_r":<volts x10>,
 * "o_t":<external probe milli-C>,"sg":..}. "temp" and "sg" are not
 * documented by either reference driver and are ignored.
 */
struct QFocuserTelemetry {
    double external_temperature_c = 0.0;  // o_t / 1000
    double chip_temperature_c = 0.0;      // c_t / 1000
    double voltage_v = 0.0;               // c_r / 10
    // False when o_t is the no-probe sentinel (about -111 C with nothing in the probe socket, #2032).
    bool external_probe_present = true;
};

/**
 * @brief Information about a detected serial port hosting a Q-Focuser.
 */
struct QFocuserPortInfo {
    std::string port_path;  // e.g. "/dev/ttyACM0"
    std::string device_id;  // by-id symlink name, empty for raw nodes
    QFocuserDeviceInfo info;
};

/**
 * @brief Parse one flat JSON object reply into key -> raw value text.
 *
 * The firmware only ever emits a single-level object of integers and
 * strings, so a full JSON parser is not needed. String values are returned
 * with their quotes stripped and escapes reduced to a placeholder character
 * (the only string field, "id", is opaque MCU-serial bytes). Returns an
 * empty map if the text is not a well-formed flat object.
 */
std::map<std::string, std::string> parse_qfocuser_reply(std::string_view text);

/**
 * @brief Enumerate serial ports that host a QHY Q-Focuser.
 *
 * Scans /dev/serial/by-id/ for the focuser's GigaDevice CDC-ACM interface,
 * then falls back to raw /dev/ttyACM0..9 nodes whose USB descriptor matches,
 * probing each with the {"cmd_id":1} handshake. Ports held open by another
 * connected device are skipped.
 */
std::vector<QFocuserPortInfo> enumerate_qfocuser_ports();

/**
 * @brief Protocol wrapper for the QHY Q-Focuser.
 *
 * Every command is a JSON object {"cmd_id":N,...} with no terminator; every
 * reply is a JSON object {"idx":N,...} echoing the command id. There is no
 * "moving" flag in the protocol: callers observe motion by polling
 * get_position() against the last commanded target.
 */
class QFocuserProtocolWrapper {
public:
    QFocuserProtocolWrapper();
    ~QFocuserProtocolWrapper();

    QFocuserProtocolWrapper(const QFocuserProtocolWrapper&) = delete;
    QFocuserProtocolWrapper& operator=(const QFocuserProtocolWrapper&) = delete;

    /** @brief Open the port and handshake with {"cmd_id":1}. Throws on failure. */
    QFocuserDeviceInfo connect(const QFocuserConnectionConfig& config);

    void disconnect();

    bool is_connected() const;

    /**
     * @brief False once a transaction has seen the port die (POLLHUP, EOF,
     * EIO/ENXIO/ENODEV/EBADF) since the last connect(). The wrapper releases
     * the fd at that moment; every later command throws NotConnected until a
     * new connect(). Lock-free, safe from a FAST-timing getter (issue #527).
     */
    bool link_alive() const noexcept;

    /** @brief {"cmd_id":5} -> "pos". Signed: the firmware allows negative positions. */
    std::int32_t get_position();

    /** @brief {"cmd_id":4} -> temperatures and supply voltage. */
    QFocuserTelemetry get_telemetry();
    /**
     * @brief get_telemetry() with its own reply deadline. The firmware answers no telemetry for about
     * 10 s after a move (#2031), so the state poll asks with a short one and treats a miss as a miss.
     */
    QFocuserTelemetry get_telemetry(int timeout_ms);

    /** @brief {"cmd_id":6,"tar":N} absolute move. Returns once acknowledged, not once complete. */
    void move_to(std::int32_t position);

    /** @brief {"cmd_id":3} abort. The firmware may answer idx 3 or idx 5. */
    void halt();

    /** @brief {"cmd_id":7,"rev":0|1} motor direction. */
    void set_reverse(bool reversed);

    /** @brief {"cmd_id":13,"speed":N} with 1 = fastest .. 8 = slowest (INDIGO encoding). */
    void set_speed(int speed);

    /** @brief {"cmd_id":12,"force":0|1} motor hold force (12 V supply only). */
    void set_hold_force(bool enabled);

    /** @brief {"cmd_id":16,"ihold":I,"irun":R} hold/run current (12 V supply only). */
    void set_hold_current(int ihold, int irun);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::qhy
