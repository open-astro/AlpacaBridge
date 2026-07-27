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

#include <alpacacore/switch_driver.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::wandererastro {

/**
 * @brief Create a WandererBox Pro V3 power box Switch driver via serial port.
 *
 * Exposes the box's outputs and sensors as 24 Alpaca switches: two always-on
 * rails (read-only), the DC3-4 adjustable output (on/off + a 5.0-13.2 V
 * setpoint switch), three PWM dew heater channels (0-255), two switched DC
 * pairs, five USB port groups, and ten read-only sensor values (voltage,
 * currents, temperatures, humidity, dew point). Controlled over a CH340
 * USB-serial link at 19200 baud.
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g., "/dev/ttyUSB0")
 * @param baud_rate Serial baud rate (default 19200)
 * @return Unique pointer to switch driver
 */
std::unique_ptr<SwitchDriver> create_wandererastro_box_switch(int device_number, const std::string& serial_port,
                                                              int baud_rate = 19200);

/**
 * @brief Create a WandererBox Pro V3 Switch driver by auto-detecting the port.
 *
 * Scans CH340/CH341 USB-serial adapters and listens on each for the box's
 * streamed status frame. The box_index selects which detected box to use
 * (0-based).
 *
 * @param device_number Alpaca device number
 * @param box_index 0-based index into the list of detected boxes
 * @return Unique pointer to switch driver
 */
std::unique_ptr<SwitchDriver> create_wandererastro_box_switch_by_index(int device_number, int box_index = 0);

}  // namespace alpacacore::vendor::wandererastro
