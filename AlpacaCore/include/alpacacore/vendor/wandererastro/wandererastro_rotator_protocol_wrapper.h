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
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::wandererastro {

/// Steps per degree for the WandererRotator Mini (V1/V2) worm drive.
inline constexpr int kRotatorMiniStepsPerDegree = 1142;

/// Minimum firmware (YYYYMMDD) the Mini protocol implemented here requires.
inline constexpr int kRotatorMiniMinFirmware = 20240226;

/**
 * @brief Connection configuration for the WandererRotator Mini.
 *
 * The rotator is USB-serial only (CH340 adapter, fixed 19200 8N1); DC power
 * is separate and the serial link stays up even when motor power is absent.
 */
struct RotatorConnectionConfig {
    std::string serial_port;  // e.g., "/dev/ttyUSB0"; empty + auto_detect_index>=0 => enumerate at connect
    int baud_rate = 19200;    // WandererRotator fixed protocol baud
    int serial_timeout_s = 3;
    int auto_detect_index = -1;  // >=0: enumerate ports at connect time and use this 0-based match
};

/**
 * @brief Information about a detected serial port that may host a WandererRotator.
 */
struct RotatorPortInfo {
    std::string port_path;     // e.g., "/dev/ttyUSB0"
    std::string device_id;     // e.g., "usb-1a86_USB_Serial-if00-port0"
    std::string model;         // handshake token, e.g. "WandererRotatorMini"
    int firmware_version = 0;  // YYYYMMDD, populated after a successful probe
};

/**
 * @brief Latest known rotator state.
 *
 * Unlike the WandererCover, the rotator is request/response: it answers the
 * handshake command and then reports its mechanical angle only when a move
 * completes (or is halted). During motion the wrapper extrapolates the angle
 * from the commanded rate (~1 degree / 240 ms, matching the INDI reference),
 * so position reads never block on serial I/O.
 */
struct RotatorState {
    bool valid = false;             // true once the handshake has been parsed
    std::string model;              // handshake token, e.g. "WandererRotatorMini"
    int firmware_version = 0;       // YYYYMMDD
    double mechanical_angle = 0.0;  // mechanical angle in degrees (device-reported or extrapolated)
    double backlash = 0.0;          // on-device backlash compensation (degrees, 0-3)
    bool reverse = false;           // hardware reverse flag
    bool moving = false;            // a move is in flight
};

/**
 * @brief Enumerate serial ports that could be a WandererRotator Mini.
 *
 * Scans /dev/serial/by-id/ for CH340/CH341 USB-serial adapters (vendor 1a86),
 * probes each with the handshake command ("1500001") and accepts ports whose
 * reply token starts with "WandererRotatorMini" (covers the V2 token). Falls
 * back to /dev/ttyUSB0..9 if /dev/serial/by-id is absent.
 *
 * @return Vector of detected WandererRotator ports
 */
std::vector<RotatorPortInfo> enumerate_wanderer_rotator_ports();

/**
 * @brief Protocol wrapper for the WandererRotator Mini (V1/V2).
 *
 * Isolates all serial I/O from the Alpaca driver. ASCII protocol at 19200 8N1:
 *   - handshake "1500001" -> "<name>A<firmware>A<angle*1000>A<backlash>A<reverse>A"
 *   - relative move: "(deltaDegrees * 1142) + 1000000" (device replies with the
 *     final mechanical angle only when the move finishes)
 *   - halt "Stop"; reverse "1700001"/"1700000"; set-zero "1500002";
 *     backlash "(degrees * 10) + 1600000"
 *
 * A per-move monitor thread waits for the completion reply and keeps the
 * extrapolated angle fresh while the motor runs.
 */
class WandererRotatorProtocolWrapper {
public:
    WandererRotatorProtocolWrapper();
    ~WandererRotatorProtocolWrapper();

    WandererRotatorProtocolWrapper(const WandererRotatorProtocolWrapper&) = delete;
    WandererRotatorProtocolWrapper& operator=(const WandererRotatorProtocolWrapper&) = delete;

    /**
     * @brief Connect and perform the identity handshake.
     * @param config Connection configuration
     * @return The detected model token (e.g. "WandererRotatorMini")
     */
    std::string connect(const RotatorConnectionConfig& config);

    /**
     * @brief Disconnect: stop any move monitor and close the serial port.
     */
    void disconnect();

    /** @brief Check if connected. */
    bool is_connected() const;

    /** @brief Get the most recent rotator state (never blocks on serial I/O). */
    RotatorState get_state() const;

    /**
     * @brief Get the device firmware date (YYYY-MM-DD), if known.
     *
     * Captured from the handshake and cleared on disconnect. Returns
     * std::nullopt before connect or after disconnect.
     */
    std::optional<std::string> get_firmware_date() const;

    /**
     * @brief Start a relative move (fire-and-forget on the wire).
     *
     * Sends "(delta_degrees * 1142) + 1000000" and starts the completion
     * monitor. Throws InvalidOperation if a move is already in flight.
     *
     * @param delta_degrees Signed mechanical offset in degrees
     */
    void move_relative(double delta_degrees);

    /** @brief Halt an in-flight move. Command "Stop". No-op when idle. */
    void halt();

    /** @brief Set hardware direction reversal. Commands "1700001"/"1700000". */
    void set_reverse(bool reverse);

    /**
     * @brief Set on-device backlash compensation.
     * @param degrees Backlash in degrees, [0, 3] in 0.1 steps
     */
    void set_backlash(double degrees);

    /** @brief Declare the current position as mechanical zero. Command "1500002". */
    void set_zero();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::wandererastro
