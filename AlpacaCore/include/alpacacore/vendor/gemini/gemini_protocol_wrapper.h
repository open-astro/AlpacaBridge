// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::vendor::gemini {

/**
 * @brief Connection type for the Gemini focuser.
 */
enum class ConnectionType {
    Serial   // USB serial port
};

/**
 * @brief Connection configuration for the Gemini focuser.
 */
struct ConnectionConfig {
    ConnectionType type = ConnectionType::Serial;
    std::string serial_port;                // e.g., "/dev/ttyUSB0"
    int baud_rate = 9600;
    int serial_timeout_s = 5;
};

/**
 * @brief Information about a detected serial port that may host a Gemini focuser.
 */
struct GeminiPortInfo {
    std::string port_path;      // e.g., "/dev/ttyUSB0"
    std::string device_id;      // e.g., "usb-1a86_USB_Serial-if00-port0"
    int firmware_version = 0;   // Populated after successful probe
};

/**
 * @brief Enumerate serial ports that could be Gemini/MyFocuserPro2 focusers.
 *
 * Scans /dev/serial/by-id/ for CH340/CH341 USB-serial adapters (vendor 1a86),
 * then probes each with the firmware version handshake (:03#).
 * Only ports that respond with a valid firmware version are returned.
 *
 * @return Vector of detected focuser ports
 */
std::vector<GeminiPortInfo> enumerate_gemini_ports();

/**
 * @brief Protocol wrapper for the Gemini Automatic Astro Focuser Pro.
 *
 * Implements the MyFocuserPro2 serial command protocol.
 * Commands are sent as ":XX#" and responses end with '#'.
 *
 * Supports USB serial connections.
 */
class GeminiProtocolWrapper {
public:
    GeminiProtocolWrapper();
    ~GeminiProtocolWrapper();

    GeminiProtocolWrapper(const GeminiProtocolWrapper&) = delete;
    GeminiProtocolWrapper& operator=(const GeminiProtocolWrapper&) = delete;

    /**
     * @brief Connect to the focuser.
     * @param config Connection configuration
     * @return Firmware version on success
     */
    int connect(const ConnectionConfig& config);

    /**
     * @brief Disconnect from the focuser.
     */
    void disconnect();

    /**
     * @brief Check if connected.
     */
    bool is_connected() const;

    // --- Query commands ---

    /** @brief Get current position. Command :00# → P<pos># */
    int get_position();

    /** @brief Get whether focuser is moving. Command :01# → I<0|1># */
    bool is_moving();

    /** @brief Get firmware version. Command :03# → F<ver># */
    int get_firmware_version();

    /** @brief Get temperature in Celsius. Command :06# → Z<temp># */
    double get_temperature();

    /** @brief Get maximum position. Command :08# → M<maxpos># */
    int get_max_position();

    /** @brief Get coil power state. Command :11# → O<0|1># */
    bool get_coil_power();

    /** @brief Get reverse direction state. Command :13# → R<0|1># */
    bool get_reverse_direction();

    /** @brief Get temperature compensation enabled. Command :24# → 1<0|1># */
    bool get_temp_comp_enabled();

    /** @brief Get temperature coefficient. Command :26# → B<coeff># */
    int get_temp_coefficient();

    /** @brief Get step mode. Command :29# → S<mode># */
    int get_step_mode();

    /** @brief Get motor speed (0-2). Command :43# → C<speed># */
    int get_speed();

    // --- Set commands ---

    /** @brief Move to absolute position. Command :05<pos># */
    void move_to(int position);

    /** @brief Set maximum position. Command :07<maxpos># */
    void set_max_position(int max_pos);

    /** @brief Set coil power state. Command :12<0|1># */
    void set_coil_power(bool enabled);

    /** @brief Set reverse direction. Command :14<0|1># */
    void set_reverse_direction(bool enabled);

    /** @brief Set temperature reporting to Celsius. Command :16# */
    void set_temperature_celsius();

    /** @brief Set temperature coefficient. Command :22<coeff># */
    void set_temp_coefficient(int coefficient);

    /** @brief Set temperature compensation. Command :23<0|1># */
    void set_temp_comp_enabled(bool enabled);

    /** @brief Halt focuser movement. Command :27# */
    void halt();

    /** @brief Move to home position. Command :28# */
    void goto_home();

    /** @brief Set step mode. Command :30<mode># */
    void set_step_mode(int mode);

    /** @brief Sync position. Command :31<pos># */
    void sync_position(int position);

    /** @brief Set motor speed (0-2). Command :150<speed># */
    void set_speed(int speed);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace alpacacore::vendor::gemini
