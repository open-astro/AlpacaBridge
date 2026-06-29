// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::wandererastro {

/**
 * @brief Connection type for the WandererCover.
 *
 * The WandererCover V4 family is USB-serial only (CH340 adapter); there is no
 * network/Wi-Fi transport. The enum is kept as a single-value type to mirror
 * the other serial protocol wrappers in this project.
 */
enum class ConnectionType : std::uint8_t {
    Serial  // USB serial port
};

/**
 * @brief Connection configuration for the WandererCover.
 */
struct ConnectionConfig {
    ConnectionType type = ConnectionType::Serial;
    std::string serial_port;  // e.g., "/dev/ttyUSB0"; empty + auto_detect_index>=0 => enumerate at connect
    int baud_rate = 19200;    // WandererCover V4 fixed protocol baud
    int serial_timeout_s = 3;
    int auto_detect_index = -1;  // >=0: enumerate ports at connect time and use this 0-based match
};

/**
 * @brief Information about a detected serial port that may host a WandererCover.
 */
struct WandererPortInfo {
    std::string port_path;     // e.g., "/dev/ttyUSB0"
    std::string device_id;     // e.g., "usb-1a86_USB_Serial-if00-port0"
    std::string model;         // e.g., "WandererCoverV4Pro"
    int firmware_version = 0;  // YYYYMMDD, populated after a successful probe
};

/**
 * @brief Latest decoded status frame streamed by the WandererCover.
 *
 * The device continuously transmits a single '\n'-terminated, 'A'-delimited
 * status line (~1 Hz):
 *   <model>A<firmware>A<closePos>A<openPos>A<curPos>A<voltage>A<brightness>A<dewHeater>A<asiair>
 * The protocol wrapper's background reader thread keeps the most recent frame.
 */
struct WandererStatus {
    bool valid = false;             // true once at least one valid frame parsed
    std::string model;              // device identifier, e.g. "WandererCoverV4Pro"
    int firmware_version = 0;       // YYYYMMDD
    double close_position = 0.0;    // configured closed angle (degrees)
    double open_position = 0.0;     // configured open angle (degrees)
    double current_position = 0.0;  // current cover angle (degrees)
    double voltage = 0.0;           // input voltage (V)
    int brightness = 0;             // flat panel PWM (0-255)
    int dew_heater = 0;             // dew heater power (0/50/100/150)
    bool asiair_control = false;    // ASIAIR control enabled flag
};

/**
 * @brief Enumerate serial ports that could be a WandererCover V4.
 *
 * Scans /dev/serial/by-id/ for CH340/CH341 USB-serial adapters (vendor 1a86),
 * then listens on each for the continuously-streamed status frame, accepting a
 * port whose status line reports a "WandererCoverV4" model. Falls back to
 * /dev/ttyUSB0..9 if /dev/serial/by-id is absent.
 *
 * @return Vector of detected WandererCover ports
 */
std::vector<WandererPortInfo> enumerate_wanderer_ports();

/**
 * @brief Protocol wrapper for the WandererCover V4 (Pro / EC / EC-IR).
 *
 * Isolates all serial I/O from the Alpaca driver. The device streams status
 * continuously and accepts fire-and-forget ASCII numeric commands terminated
 * with '\n'. A background reader thread keeps the most recent status frame so
 * property reads never block on the serial port.
 */
class WandererProtocolWrapper {
public:
    WandererProtocolWrapper();
    ~WandererProtocolWrapper();

    WandererProtocolWrapper(const WandererProtocolWrapper&) = delete;
    WandererProtocolWrapper& operator=(const WandererProtocolWrapper&) = delete;

    /**
     * @brief Connect to the cover and wait for the first valid status frame.
     * @param config Connection configuration
     * @return The detected model identifier (e.g. "WandererCoverV4Pro")
     */
    std::string connect(const ConnectionConfig& config);

    /**
     * @brief Disconnect: stop the reader thread and close the serial port.
     */
    void disconnect();

    /**
     * @brief Check if connected.
     */
    bool is_connected() const;

    /**
     * @brief Get the most recent decoded status frame.
     */
    WandererStatus get_status() const;

    /**
     * @brief Get the device firmware date (YYYY-MM-DD), if known.
     *
     * Captured once from the first valid status frame and cleared on disconnect,
     * so the caller gets a cached value without re-reading or re-formatting the
     * status stream on every poll. Returns std::nullopt before the first frame or
     * after disconnect.
     */
    std::optional<std::string> get_firmware_date() const;

    // --- Commands (fire-and-forget; the device streams status, not replies) ---

    /** @brief Open the cover. Command "1001". */
    void open_cover();

    /** @brief Close the cover. Command "1000". */
    void close_cover();

    /**
     * @brief Set flat panel brightness (0-255).
     *
     * A brightness of 0 turns the panel off (command "9999"); 1-255 sends the
     * raw PWM level.
     */
    void set_brightness(int brightness);

    /** @brief Turn the flat panel off. Command "9999". */
    void turn_off_light();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::wandererastro
