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

#include <alpacacore/filterwheel_driver.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::wandererastro {

/**
 * @brief Create a Wanderer filter wheel (SFW50 / SFW50S / SFW36S) driver via serial port.
 *
 * All Wanderer filter wheels are 8-slot devices controlled over a CH340
 * USB-serial link at 19200 baud (12 V DC power is separate from USB).
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g., "/dev/ttyUSB0")
 * @param baud_rate Serial baud rate (default 19200)
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_wandererastro_filterwheel(int device_number, const std::string& serial_port,
                                                                    int baud_rate = 19200);

/**
 * @brief Create a Wanderer filter wheel driver by auto-detecting the serial port.
 *
 * Scans CH340/CH341 USB-serial adapters and listens on each for the wheel's
 * streamed status. The filterwheel_index selects which detected wheel to use
 * (0-based).
 *
 * @param device_number Alpaca device number
 * @param filterwheel_index 0-based index into the list of detected wheels
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_wandererastro_filterwheel_by_index(int device_number,
                                                                             int filterwheel_index = 0);

}  // namespace alpacacore::vendor::wandererastro
