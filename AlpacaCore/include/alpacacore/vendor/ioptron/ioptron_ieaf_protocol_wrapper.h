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

namespace alpacacore::vendor::ioptron {

// Distinct from the mount's ConnectionType/ConnectionConfig in
// ioptron_protocol_wrapper.h — the iEAF is USB-serial only.
struct IeafConnectionConfig {
    std::string serial_port;   // e.g., "/dev/ttyUSB0"
    int baud_rate = 115200;    // iEAF fixed rate
    int serial_timeout_s = 4;  // matches INDI iEAFFOCUS_TIMEOUT
};

/**
 * @brief Identity returned by the :DeviceInfo# handshake.
 *
 * Response is "%6d%2d%4d#": position snapshot, model code (2 or 3 = iEAF),
 * and a 4-digit firmware/build number.
 */
struct IeafDeviceInfo {
    std::int32_t position = 0;
    std::int32_t model = 0;
    std::int32_t firmware = 0;
};

/**
 * @brief One :FI# status snapshot.
 *
 * Response is "%7d%1d%5d%1d#": absolute position, moving flag,
 * temperature (Kelvin x 100), direction flag (0 = reversed).
 */
struct IeafStatus {
    std::int32_t position = 0;
    bool moving = false;
    double temperature_c = 0.0;
    bool reversed = false;
};

/**
 * @brief Information about a detected serial port that may host an iEAF.
 */
struct IeafPortInfo {
    std::string port_path;    // e.g., "/dev/ttyUSB0"
    std::string device_id;    // by-id symlink name, empty for raw nodes
    IeafDeviceInfo info;      // populated after successful probe
};

/**
 * @brief Enumerate serial ports that could be iOptron iEAF focusers.
 *
 * Scans /dev/serial/by-id/ for USB-serial adapters, then falls back to raw
 * /dev/ttyUSB0..9 nodes, probing each candidate with the :DeviceInfo#
 * handshake. Only ports whose reply parses and reports an iEAF model code
 * (2 or 3) are returned.
 */
std::vector<IeafPortInfo> enumerate_ieaf_ports();

/**
 * @brief Protocol wrapper for the iOptron iEAF electronic focuser.
 *
 * Serial protocol at 115200 8N1. Commands are ":XX#"; query responses end
 * with '#'. Move/abort/zero commands are blind (no reply) — completion is
 * observed by polling :FI#.
 */
class IeafProtocolWrapper {
public:
    IeafProtocolWrapper();
    ~IeafProtocolWrapper();

    IeafProtocolWrapper(const IeafProtocolWrapper&) = delete;
    IeafProtocolWrapper& operator=(const IeafProtocolWrapper&) = delete;

    /**
     * @brief Connect and handshake with :DeviceInfo#.
     * @return Device identity (model must be 2 or 3, else this throws)
     */
    IeafDeviceInfo connect(const IeafConnectionConfig& config);

    void disconnect();

    bool is_connected() const;

    /** @brief Query status. Command :FI# → position/moving/temperature/direction. */
    IeafStatus get_status();

    /** @brief Move to absolute position. Command :FM%7u# (blind; poll :FI#). */
    void move_to(std::int32_t position);

    /** @brief Abort movement. Command :FQ# (blind). */
    void halt();

    /** @brief Set the current position as zero. Command :FZ# (blind). */
    void set_zero();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace alpacacore::vendor::ioptron
