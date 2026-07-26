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

/// Slot count for the entire Wanderer filter wheel lineup (SFW50 / SFW50S /
/// SFW36S all report as 8-slot WSFW508 / WSFW368 devices).
inline constexpr int kFilterWheelSlotCount = 8;

/// Minimum firmware (YYYYMMDD) the streamed-status protocol implemented here
/// requires (older firmware predates the status stream; update via the vendor's
/// WandererEmpire application).
inline constexpr int kFilterWheelMinFirmware = 20260124;

/**
 * @brief Connection configuration for the Wanderer filter wheels.
 *
 * The wheels are USB-serial only (CH340 adapter, fixed 19200 8N1). 12 V DC
 * power is separate from USB: the serial link and status stream work with DC
 * absent, but moves do nothing.
 */
struct FilterWheelConnectionConfig {
    std::string serial_port;  // e.g., "/dev/ttyUSB0"; empty + auto_detect_index>=0 => enumerate at connect
    int baud_rate = 19200;    // Wanderer filter wheel fixed protocol baud
    int serial_timeout_s = 5;
    int auto_detect_index = -1;  // >=0: enumerate ports at connect time and use this 0-based match
};

/**
 * @brief Information about a detected serial port that may host a Wanderer filter wheel.
 */
struct FilterWheelPortInfo {
    std::string port_path;     // e.g., "/dev/ttyUSB0"
    std::string device_id;     // e.g., "usb-1a86_USB_Serial-if00-port0"
    std::string model;         // status model token, e.g. "WSFW368"
    int firmware_version = 0;  // YYYYMMDD, populated after a successful probe
};

/**
 * @brief Latest decoded status streamed by the Wanderer filter wheel.
 *
 * The device continuously transmits an 'A'-delimited status frame:
 *   <model>A<firmware>A<position>A<letters>A<8 per-filter fields>A<deviceID>A
 * where <model> is WSFW508 or WSFW368, <position> is the current slot (1-8)
 * and <letters> holds the on-device single-letter slot names. Line terminators
 * may be omitted while the wheel is moving, so the wrapper's background reader
 * parses the 'A'-delimited token stream anchored on the model token rather
 * than reading line-by-line.
 */
struct FilterWheelStatus {
    bool valid = false;        // true once model + firmware + position parsed
    std::string model;         // status model token, e.g. "WSFW368"
    int firmware_version = 0;  // YYYYMMDD
    int position = 0;          // current slot, 1-based (1-8)
    std::string letters;       // on-device single-letter slot names (best effort)
    int device_id = 0;         // on-device ID (0-10, best effort)
};

/**
 * @brief Enumerate serial ports that could be a Wanderer filter wheel.
 *
 * Scans /dev/serial/by-id/ for CH340/CH341 USB-serial adapters (vendor 1a86),
 * then listens passively on each for the continuously-streamed status frame,
 * accepting a port whose model token starts with "WSFW". Falls back to
 * /dev/ttyUSB0..9 if /dev/serial/by-id is absent.
 *
 * @return Vector of detected Wanderer filter wheel ports
 */
std::vector<FilterWheelPortInfo> enumerate_wanderer_filterwheel_ports();

/**
 * @brief Protocol wrapper for the Wanderer filter wheels (SFW50 / SFW50S / SFW36S).
 *
 * Isolates all serial I/O from the Alpaca driver. The device streams status
 * continuously and accepts fire-and-forget ASCII numeric commands terminated
 * with '\r':
 *   - move to slot N (1-8): "200N" (2000 + N)
 *   - automatic calibration: "1500002" (sent at connect, per the vendor's
 *     INDI reference driver)
 * A background reader thread keeps the most recent status frame so property
 * reads never block on the serial port; move completion is detected by the
 * caller polling get_status() until position matches the target.
 */
class WandererFilterWheelProtocolWrapper {
public:
    WandererFilterWheelProtocolWrapper();
    ~WandererFilterWheelProtocolWrapper();

    WandererFilterWheelProtocolWrapper(const WandererFilterWheelProtocolWrapper&) = delete;
    WandererFilterWheelProtocolWrapper& operator=(const WandererFilterWheelProtocolWrapper&) = delete;

    /**
     * @brief Connect, wait for the first identifying status frame, and enforce
     * the minimum firmware. The caller decides whether to calibrate() after
     * a successful connect (the vendor's INDI reference does).
     * @param config Connection configuration
     * @return The detected model token (e.g. "WSFW368")
     */
    std::string connect(const FilterWheelConnectionConfig& config);

    /**
     * @brief Disconnect: stop the reader thread and close the serial port.
     */
    void disconnect();

    /** @brief Check if connected. */
    bool is_connected() const;

    /** @brief Get the most recent decoded status frame (never blocks on serial I/O). */
    FilterWheelStatus get_status() const;

    /**
     * @brief Get the device firmware date (YYYY-MM-DD), if known.
     *
     * Captured once from the first valid status frame and cleared on
     * disconnect. Returns std::nullopt before the first frame or after
     * disconnect.
     */
    std::optional<std::string> get_firmware_date() const;

    /**
     * @brief Move to a filter slot (fire-and-forget on the wire).
     * @param slot Target slot, 1-based (1-8)
     */
    void select_filter(int slot);

    /**
     * @brief Send the automatic calibration command ("1500002"). The wheel
     * homes itself; progress is visible via the streamed position.
     */
    void calibrate();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::wandererastro
