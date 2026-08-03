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
 * @brief Which Gemini flat panel hardware model a connection targets.
 *
 * Both models share the same ">X#"/">Xnnn#" wire syntax and port
 * auto-detection (enumerate_gemini_flatpanel_ports() / is_flatpanel_handshake_reply()
 * stay model-agnostic -- see their doc comments), but differ in what the
 * device actually supports and in the >S# status reply layout:
 *   - Lite: light-only, no motor. >S# replies "*S<d1><d2><d3>#" (see
 *     get_light_on()/parse_light_flag()).
 *   - Rev2: adds a motorized cover. >S# replies "*S<id2digits><motor><light><cover>#"
 *     (see get_status_rev2()). Sourced from INDI's open-source
 *     GeminiFlatpanelRev2Adapter (indilib/indi, drivers/auxiliary/
 *     gemini_flatpanel_adapters.cpp), unlike the Lite parsing which was
 *     reverse-engineered from traffic captures of the vendor's own app.
 *     ConformU 4.4.0 validated against a real Rev2 unit (firmware 408,
 *     Linux arm64): 0 errors, 0 issues, 0 timing issues (see AGENTS.md and
 *     AlpacaCore/conformu/Gemini/Astro Automatic FlatPanel v2/).
 */
enum class FlatPanelModel : std::uint8_t { Lite, Rev2 };

/**
 * @brief Connection configuration for a Gemini flat panel (Lite or Rev2).
 */
struct FlatPanelConnectionConfig {
    FlatPanelConnectionType type = FlatPanelConnectionType::Serial;
    std::string serial_port;  // e.g., "/dev/ttyUSB0"; empty + auto_detect_index>=0 => enumerate at connect
    int baud_rate = 9600;
    int serial_timeout_s = 5;
    int auto_detect_index = -1;  // >=0: enumerate ports at connect time and use this 0-based match
    FlatPanelModel model = FlatPanelModel::Lite;
};

/**
 * @brief Motorized cover state as reported by a Rev2 panel's >S# status reply.
 *
 * Values match INDI's GeminiCoverStatus enum exactly (indilib/indi,
 * gemini_flatpanel_adapters.h) so the numeric wire value needs no remapping.
 */
enum class FlatPanelCoverState : std::uint8_t { Moving = 0, Closed = 1, Open = 2, TimedOut = 3 };

/**
 * @brief Parsed >S# status for a Rev2 panel (light, motor, and cover state together).
 */
struct FlatPanelRev2Status {
    bool light_on = false;
    bool motor_running = false;
    FlatPanelCoverState cover_state = FlatPanelCoverState::Moving;
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
 * @brief Check whether a probe reply matches the flat panel's >H# handshake.
 *
 * Requires the "*H" prefix (the confirmed hardware reply is
 * "*HGeminiFlatPanelLite#") rather than accepting any well-formed,
 * '#'-terminated reply. Auto-detect's candidate by-id patterns
 * (CH340/CH341/USB_Serial/1a86) are exactly what the Gemini focuser also
 * enumerates as, and its MyFocuserPro2 firmware answers unrelated queries
 * with its own '#'-terminated replies -- accepting any such reply let
 * probe_port() misidentify the focuser's port as the flat panel when both
 * devices were plugged in (PR #143 review). Exposed here (rather than kept
 * file-local) so the discrimination logic itself is unit-testable without
 * real hardware.
 *
 * @param reply Bytes read from the port in response to ">H#" (line endings stripped)
 * @return true if reply is a well-formed flat panel handshake reply
 */
bool is_flatpanel_handshake_reply(const std::string& reply);

/**
 * @brief Protocol wrapper for Gemini flat panels: the Astro Flat Panel Cover
 * Lite (light-only) and the Astro Automatic FlatPanel v2 (motorized cover),
 * selected via FlatPanelConnectionConfig::model.
 *
 * The Lite side is reverse-engineered from the vendor's Windows control app
 * (decompiled .NET IL — no SDK or protocol spec was published by the
 * vendor). Commands are ASCII, sent as ">X#" or ">Xnnn#" and terminated with
 * '#'; responses are likewise '#'-terminated. This is the SAME wire syntax
 * family as the well known Alnitak/Optec Flip-Flat protocol but NOT
 * byte-compatible with it (Gemini's arguments are not zero-padded to a fixed
 * width) — treat any resemblance as coincidental, not as an implementation
 * reference.
 *
 * Response format confirmed against real Lite hardware: replies are "*" +
 * the echoed command letter + a decimal payload + "#" (e.g. >V# -> "*V206#",
 * >J# -> "*J64#"). >S# is the one exception -- on the Lite its payload is
 * three single-digit flags ("*S111#"), not one combined number; see
 * parse_light_flag() in the .cpp for why that needs its own parser. The
 * Rev2 >S# reply has a different, longer layout — see get_status_rev2().
 *
 * The Rev2 (motorized cover) commands are sourced from INDI's open-source
 * GeminiFlatpanelRev2Adapter (indilib/indi, drivers/auxiliary/
 * gemini_flatpanel_adapters.{h,cpp}), not from a vendor spec (none was
 * published) — no Rev2 unit was available at implementation time, but the
 * cover/status commands have since been ConformU-validated against real
 * Rev2 hardware (firmware 408, see AGENTS.md).
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

    // --- Rev2-only commands (motorized cover) ---

    /**
     * @brief Get combined light/motor/cover status (Rev2 only). Command >S#
     *
     * Unlike get_light_on(), which reads only the Lite's 3-flag >S# reply,
     * this parses the Rev2 7-char layout ("*S<id2digits><motor><light><cover>#")
     * including the device-ID gate (19 or 99) INDI's adapter checks before
     * trusting the reply.
     */
    FlatPanelRev2Status get_status_rev2();

    /**
     * @brief Open the motorized cover (Rev2 only). Command >O#, expects "*OOpened#".
     * Uses a long timeout -- cover travel is a mechanical motion, not an
     * instant ack, unlike the light/brightness commands.
     */
    void open_cover();

    /**
     * @brief Close the motorized cover (Rev2 only). Command >C#, expects "*CClosed#".
     */
    void close_cover();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::gemini
