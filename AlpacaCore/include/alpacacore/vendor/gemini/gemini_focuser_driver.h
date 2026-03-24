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

#include <alpacacore/focuser_driver.h>
#include <memory>
#include <string>

namespace alpacacore::vendor::gemini {

/**
 * @brief Create a Gemini Astro Focuser Pro driver via serial port.
 *
 * Uses the MyFocuserPro2 serial protocol over USB serial.
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g., "/dev/ttyUSB0")
 * @param baud_rate Serial baud rate (default 9600)
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_gemini_focuser(int device_number,
                                                      const std::string& serial_port,
                                                      int baud_rate = 9600);

/**
 * @brief Create a Gemini Astro Focuser Pro driver via TCP/WiFi.
 *
 * Uses the MyFocuserPro2 serial protocol over TCP socket.
 *
 * @param device_number Alpaca device number
 * @param host TCP hostname or IP address (default "192.168.4.1")
 * @param port TCP port (default 2020)
 * @return Unique pointer to focuser driver
 */
/**
 * @brief Create a Gemini Astro Focuser Pro driver via TCP/WiFi.
 *
 * Uses the MyFocuserPro2 serial protocol over TCP socket.
 *
 * @param device_number Alpaca device number
 * @param host TCP hostname or IP address (default "192.168.4.1")
 * @param port TCP port (default 2020)
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_gemini_focuser_tcp(int device_number,
                                                          const std::string& host = "192.168.4.1",
                                                          int port = 2020);

/**
 * @brief Create a Gemini Astro Focuser Pro driver by auto-detecting the serial port.
 *
 * Scans for CH340/CH341 USB-serial adapters and probes each with the
 * MyFocuserPro2 handshake. The focuser_index selects which detected
 * focuser to use (0-based).
 *
 * @param device_number Alpaca device number
 * @param focuser_index 0-based index into the list of detected focusers
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_gemini_focuser_by_index(int device_number,
                                                               int focuser_index = 0);

} // namespace alpacacore::vendor::gemini
