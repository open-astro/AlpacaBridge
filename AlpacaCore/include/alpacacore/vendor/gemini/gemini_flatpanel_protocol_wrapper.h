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
#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::vendor::gemini {

/**
 * @brief Connection type for the Gemini Flat Panel Cover Lite.
 */
enum class FlatPanelConnectionType : std::uint8_t {
    Serial  // USB serial port
};

/**
 * @brief Connection configuration for the Gemini Flat Panel Cover Lite.
 */
struct FlatPanelConnectionConfig {
    FlatPanelConnectionType type = FlatPanelConnectionType::Serial;
    std::string serial_port;  // e.g., "/dev/ttyUSB0"; empty + auto_detect_index>=0 => enumerate at connect
    int baud_rate = 9600;
    int serial_timeout_s = 5;
    int auto_detect_index = -1;  // >=0: enumerate ports at connect time and use this 0-based match
};

/**
 * @brief Information about a detected serial port that may host a Gemini flat panel.
 */
struct GeminiFlatPanelPortInfo {
    std::string port_path;  // e.g., "/dev/ttyUSB0"
    std::string device_id;  // e.g., "usb-1a86_USB_Serial-if00-port0"
};

/**
 * @brief Enumerate serial ports that could be a Gemini Flat Panel Cover Lite.
 *
 * Scans /dev/serial/by-id/ for CH340/CH341 USB-serial adapters (vendor 1a86),
 * then probes each with the identity handshake (>H#). Only ports that respond
 * with a valid, '#'-terminated reply are returned.
 *
 * @return Vector of detected flat panel ports
 */
std::vector<GeminiFlatPanelPortInfo> enumerate_gemini_flatpanel_ports();

/**
 * @brief Protocol wrapper for the Gemini Astro Flat Panel Cover Lite (V2 / USB).
 *
 * Reverse-engineered from the vendor's Windows control app (decompiled .NET
 * IL — no SDK or protocol spec was published by the vendor). Commands are
 * ASCII, sent as ">X#" or ">Xnnn#" and terminated with '#'; responses are
 * likewise '#'-terminated. This is the SAME wire syntax family as the well
 * known Alnitak/Optec Flip-Flat protocol but NOT byte-compatible with it
 * (Gemini's arguments are not zero-padded to a fixed width) — treat any
 * resemblance as coincidental, not as an implementation reference.
 *
 * Response format confirmed against real hardware: replies are "*" + the
 * echoed command letter + a decimal payload + "#" (e.g. >V# -> "*V206#",
 * >J# -> "*J64#"). >S# is the one exception -- its payload is three
 * single-digit flags ("*S111#"), not one combined number; see
 * parse_light_flag() in the .cpp for why that needs its own parser.
 *
 * Supports USB serial connections only — no WiFi/network variant support yet.
 */
class GeminiFlatPanelProtocolWrapper {
public:
    GeminiFlatPanelProtocolWrapper();
    ~GeminiFlatPanelProtocolWrapper();

    GeminiFlatPanelProtocolWrapper(const GeminiFlatPanelProtocolWrapper&) = delete;
    GeminiFlatPanelProtocolWrapper& operator=(const GeminiFlatPanelProtocolWrapper&) = delete;

    /**
     * @brief Connect to the flat panel.
     * @param config Connection configuration
     * @return Firmware version string reported by the panel (from >V#), or an
     *         empty string if the panel didn't return a parseable version.
     */
    std::string connect(const FlatPanelConnectionConfig& config);

    /**
     * @brief Disconnect from the flat panel.
     */
    void disconnect();

    /**
     * @brief Check if connected.
     */
    bool is_connected() const;

    // --- Query commands ---

    /** @brief Get whether the light is on. Command >S# */
    bool get_light_on();

    /** @brief Get current brightness (0-255). Command >J# */
    int get_brightness();

    // --- Set commands ---

    /** @brief Turn the light on (at the last-set brightness). Command >L# */
    void light_on();

    /** @brief Turn the light off. Command >D# */
    void light_off();

    /** @brief Set brightness (0-255). Command >B<value># */
    void set_brightness(int value);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::gemini
