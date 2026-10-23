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

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::vendor::gemini {

/// Exact >H# handshake reply of the Power & Data Hubs Advanced 3 (without the
/// trailing '#'). The vendor's own ASCOM driver compares the reply verbatim
/// against this string; the previous-generation "PowerBox & Hub Plus V3" has a
/// different frame layout and is deliberately NOT accepted.
inline constexpr char kPdhHandshakeReply[] = "*HGeminiPowerBoxPlusAdv3";

/// Fixed protocol baud rate (19200 8N1), per the vendor driver.
inline constexpr int kPdhBaudRate = 19200;

/// Minimum firmware the vendor driver accepts (`>V#` -> "308" = 3.0.8). Older
/// firmware has an unknown status-frame layout, so connect() refuses it the
/// same way the vendor driver does (vendor: "ERROR 01 Old firmware").
inline constexpr int kPdhMinFirmware = 308;

/// Dew heater manual PWM range (percent).
inline constexpr int kPdhDewPwmMax = 100;

/// Number of switchable outputs on the wire: >O<n># / >C<n>#, n = 1..11
/// (1 = DC1 always-on, 2-5 = DC2-DC5, 6-11 = USB A-F).
inline constexpr int kPdhOutputChannelMin = 1;
inline constexpr int kPdhOutputChannelMax = 11;

/// Wire channel numbers of the two dew heater outputs (DEW6, DEW7).
inline constexpr int kPdhDewChannelFirst = 6;
inline constexpr int kPdhDewChannelLast = 7;

/**
 * @brief Dew heater channel operating mode (>M<ch><mode>#).
 *
 * Auto runs the firmware's PID loop against the AHT20 dew point and the
 * DS18B20 lens temperature (both sensors must be attached). Manual drives a
 * fixed 0-100 % PWM. Switch turns the channel into a plain on/off 12 V output.
 * Values match the vendor driver's "DEW Mode" switch encoding exactly.
 */
enum class PdhDewMode : std::uint8_t { Auto = 0, Manual = 1, Switch = 2 };

/**
 * @brief Connection configuration for the Power & Data Hubs Advanced 3.
 *
 * USB serial only (CH340/CH341 adapter, fixed 19200 8N1).
 */
struct PdhConnectionConfig {
    std::string serial_port;       // e.g., "/dev/ttyUSB0"; empty + auto_detect_index>=0 => enumerate at connect
    int baud_rate = kPdhBaudRate;  // fixed; any other value is rejected
    int serial_timeout_s = 3;      // per-request reply timeout
    int auto_detect_index = -1;    // >=0: enumerate ports at connect time and use this 0-based match
};

/**
 * @brief One dew heater channel as reported by the status frame.
 */
struct PdhDewChannel {
    bool enabled = false;                  // on/off flag used in Auto and Switch modes
    PdhDewMode mode = PdhDewMode::Manual;  // firmware-reported mode
    int manual_pwm = 0;                    // Manual-mode PWM setpoint, 0-100 %
};

/**
 * @brief Latest known hub state, parsed from a >G# status frame.
 *
 * Absent-sensor convention: when the AHT20 or DS18B20 flag is 0 the matching
 * temperature fields are -127 degC and humidity is 0 (same convention as the
 * WandererBox driver) so no NaN can reach the Alpaca JSON layer.
 */
struct PdhState {
    bool valid = false;  // true once a full frame has been parsed

    std::array<bool, 6> usb{};  // USB A..F power
    std::array<bool, 4> dc{};   // DC2..DC5 (DC1 is always on and not in the frame)

    bool aht20_attached = false;    // ambient temperature/humidity sensor present
    bool ds18b20_attached = false;  // lens temperature probe present

    PdhDewChannel dew6;
    PdhDewChannel dew7;

    double lens_temp = -127.0;     // DS18B20, degC
    double ambient_temp = -127.0;  // AHT20, degC
    double humidity = 0.0;         // AHT20, % RH
    double dew_point = -127.0;     // firmware-computed, degC

    double input_voltage = 0.0;   // V
    double output_current = 0.0;  // A (DC12V output)
    double output_power = 0.0;    // W (DC12V output)
};

/**
 * @brief Information about a detected serial port hosting a hub.
 */
struct GeminiPdhPortInfo {
    std::string port_path;  // e.g., "/dev/ttyUSB0"
    std::string device_id;  // e.g., "usb-1a86_USB_Serial-if00-port0"
};

/**
 * @brief Enumerate serial ports that host a Power & Data Hubs Advanced 3.
 *
 * Scans /dev/serial/by-id/ for CH340/CH341 USB-serial adapters (vendor 1a86)
 * plus the raw /dev/ttyUSB0..9 nodes, probes each with the >H# handshake at
 * 19200 baud and accepts only ports whose reply is exactly
 * kPdhHandshakeReply -- so the Gemini focuser and flat panels (which share
 * the same adapter chip and by-id naming) can never be mistaken for a hub.
 *
 * @return Vector of detected hub ports
 */
std::vector<GeminiPdhPortInfo> enumerate_gemini_pdh_ports();

/**
 * @brief The auto-detect identity probe of one serial port (>H# at 19200 baud).
 *
 * Opens the port under a probe claim, so a port a connected driver holds is
 * refused without being opened. Returns true only for the hub's exact reply.
 * The driver calls it on the remembered port before it runs the full scan.
 */
bool probe_gemini_pdh_port(const std::string& port_path);

/// Test seam for the driver's port search: @p on_probe sees every
/// probe_gemini_pdh_port() call; @p enumerate, when set, replaces
/// enumerate_gemini_pdh_ports(). Empty members restore the real behaviour.
struct GeminiPdhScanHooks {
    std::function<void(const std::string&)> on_probe;
    std::function<std::vector<GeminiPdhPortInfo>()> enumerate;
};
void set_gemini_pdh_scan_hooks_for_testing(GeminiPdhScanHooks hooks);

/**
 * @brief Check whether a >H# reply identifies a Power & Data Hubs Advanced 3.
 *
 * Exact match against kPdhHandshakeReply (line endings and the '#' terminator
 * stripped). Exposed so the discrimination logic is unit-testable.
 */
bool is_pdh_handshake_reply(const std::string& reply);

/**
 * @brief Parse one >G# status frame ("*G...#", terminator optional).
 *
 * Layout (per the vendor's ASCOM driver, which splits the payload after "*G"
 * on the tag letters D U A T M B C S H V P and indexes the pieces
 * positionally):
 *   [0]  4 digits  DC2..DC5 on/off
 *   [1]  6 digits  USB A..F on/off
 *   [2]  AHT20 attached (1/0)
 *   [3]  DS18B20 attached (1/0)
 *   [4]  DEW6 enabled flag (Auto/Switch modes)
 *   [5]  DEW6 firmware mode (0 Auto, 1 Manual, 2 Switch)
 *   [6]  DEW7 enabled flag
 *   [7]  DEW7 firmware mode
 *   [8]  DEW6 manual PWM %
 *   [9]  DEW7 manual PWM %
 *   [10] lens temperature (DS18B20), degC
 *   [11] ambient temperature (AHT20), degC
 *   [12] humidity (AHT20), %
 *   [13] dew point, degC
 *   [14] input voltage, V
 *   [15] DC12V output current, A
 *   [16] DC12V output power, W
 *
 * Returns std::nullopt when the frame is not a "*G" frame or the two fixed
 * digit blocks are malformed. Numeric fields that fail to parse fall back to
 * 0 (the vendor driver uses TryParse for the same fields). Exposed for unit
 * tests; the wrapper's reader thread uses it for every frame it receives.
 */
std::optional<PdhState> parse_pdh_status_frame(const std::string& frame);

/**
 * @brief Format a >V# firmware integer for display ("308" -> "3.0.8").
 *
 * Mirrors the vendor app's digit-by-digit rendering for 3-digit versions;
 * any other length is returned as the raw integer.
 */
std::string format_pdh_firmware(int version);

/**
 * @brief Protocol wrapper for the Gemini Power & Data Hubs Advanced 3.
 *
 * Reverse-engineered from the vendor's Windows ASCOM driver
 * (ASCOM.GeminiPowerBoxPlusAdv3.Switch v2.6.0206, decompiled .NET IL) -- no
 * SDK or protocol spec is published. Same ">X#" / "*X...#" wire syntax
 * family as the Gemini flat panels, but a different command set:
 *   - ">H#"        -> "*HGeminiPowerBoxPlusAdv3#" identity
 *   - ">V#"        -> "*V<nnn>#" firmware (308 = 3.0.8)
 *   - ">G#"        -> "*G<status frame>#" (see parse_pdh_status_frame())
 *   - ">O<n>#" / ">C<n>#"   output n (1-11) on / off, no reply awaited
 *   - ">X<pct>#" / ">Y<pct>#"  DEW6 / DEW7 manual PWM 0-100
 *   - ">Z10#"/">Z11#", ">Z20#"/">Z21#"  DEW6 / DEW7 off/on (Auto and Switch modes)
 *   - ">M1<m>#" / ">M2<m>#"  DEW6 / DEW7 mode (0 Auto, 1 Manual, 2 Switch)
 * Commands are sent with a trailing "\n" exactly as the vendor driver's
 * SerialPort.WriteLine() does; set commands are fire-and-forget.
 *
 * A background reader thread owns all reads: every '#'-terminated frame is
 * routed either to the status cache (any "*G" frame, whether streamed by the
 * firmware unprompted or in reply to >G#) or to the one in-flight
 * request (>H#/>V#). The reader also re-polls >G# whenever the cache is older
 * than kStatusPollMs, so Switch reads are always served from cache and never
 * block on serial I/O. Cache validity is tied to link health: consecutive
 * unanswered polls latch a link fault (link_fault()) that invalidates the
 * cache until frames resume. TODO(hardware): confirm whether the firmware streams
 * >G frames on its own -- the vendor's sensor window reads whatever arrives
 * every 3 s without sending anything, which suggests it does.
 */
class GeminiPdhProtocolWrapper {
public:
    GeminiPdhProtocolWrapper();
    ~GeminiPdhProtocolWrapper();

    GeminiPdhProtocolWrapper(const GeminiPdhProtocolWrapper&) = delete;
    GeminiPdhProtocolWrapper& operator=(const GeminiPdhProtocolWrapper&) = delete;

    /**
     * @brief Connect: handshake, firmware gate, first status frame.
     * @param config Connection configuration
     * @return Firmware version integer reported by >V# (e.g. 308)
     */
    int connect(const PdhConnectionConfig& config);

    /** @brief Disconnect: stop the reader thread and close the serial port. */
    void disconnect();

    /**
     * @brief Forget the cached firmware now, ahead of disconnect().
     *
     * The driver calls this before it stores connected_ = false (issue #387):
     * disconnect() clears the cache only after joining the reader, which can
     * sit in a 500 ms read, and the driver's firmware getter relies on the
     * cache being empty whenever the driver reports disconnected.
     */
    void clear_firmware();

    /** @brief Check if connected. */
    bool is_connected() const;

    /**
     * @brief Get the most recent hub state (never blocks on serial I/O).
     *
     * `valid` is cleared while the link is faulted (see link_fault()), so a
     * caller that ignores link_fault() still cannot mistake a dead link for
     * live telemetry.
     */
    PdhState get_state() const;

    /**
     * @brief Why the serial link is currently considered dead, if it is.
     *
     * Reads are served from a cache the reader thread fills; when the hub
     * stops answering (USB re-enumeration, unplugged cable, port stolen by
     * another process) the cache would otherwise be served unchanged forever
     * (issue #237). After kLinkFaultPolls consecutive >G# polls with no
     * status frame -- a failed poll write counts the same -- the link is
     * latched faulted and this returns the reason; the driver turns that
     * into a DriverException on every value read and write. It clears on
     * its own when a status frame arrives again. Connected stays true: the
     * client decides whether to reconnect.
     */
    std::optional<std::string> link_fault() const;

    /**
     * @brief Get the firmware version for display ("3.0.8"), if connected.
     *
     * Cleared on disconnect. Web UI only, never DriverInfo.
     */
    std::optional<std::string> get_firmware() const;

    /**
     * @brief Switch an output on or off (">O<n>#" / ">C<n>#").
     * @param channel Wire channel 1-11 (1 = DC1, 2-5 = DC2-DC5, 6-11 = USB A-F)
     */
    void set_output(int channel, bool on);

    /**
     * @brief Set a dew heater's Manual-mode PWM (">X<pct>#" / ">Y<pct>#").
     * @param channel 6 (DEW6) or 7 (DEW7)
     * @param percent 0-100
     */
    void set_dew_manual_pwm(int channel, int percent);

    /**
     * @brief Enable/disable a dew heater in Auto or Switch mode (">Z<ch-5><0|1>#").
     * @param channel 6 (DEW6) or 7 (DEW7)
     */
    void set_dew_enabled(int channel, bool on);

    /**
     * @brief Set a dew heater's operating mode (">M<ch-5><mode>#").
     * @param channel 6 (DEW6) or 7 (DEW7)
     */
    void set_dew_mode(int channel, PdhDewMode mode);

    /**
     * @brief Ask the hub for a fresh status frame (">G#") without waiting.
     *
     * The reply lands in the cache via the reader thread. Used after writes
     * so the next poll reflects the new state sooner than the periodic poll.
     */
    void request_status();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::gemini
