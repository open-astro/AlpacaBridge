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

#include <alpacacore/rotator_driver.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::wandererastro {

/**
 * @brief Create a WandererRotator Mini (V1/V2) rotator driver via serial port.
 *
 * The WandererRotator Mini is a worm-drive camera rotator controlled over a
 * CH340 USB-serial link at 19200 baud (DC power is separate).
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g., "/dev/ttyUSB0")
 * @param baud_rate Serial baud rate (default 19200)
 * @return Unique pointer to rotator driver
 */
std::unique_ptr<RotatorDriver> create_wandererastro_rotator(int device_number, const std::string& serial_port,
                                                            int baud_rate = 19200);

/**
 * @brief Create a WandererRotator Mini driver by auto-detecting the serial port.
 *
 * Scans CH340/CH341 USB-serial adapters and probes each with the rotator
 * handshake. The rotator_index selects which detected rotator to use (0-based).
 *
 * @param device_number Alpaca device number
 * @param rotator_index 0-based index into the list of detected rotators
 * @return Unique pointer to rotator driver
 */
std::unique_ptr<RotatorDriver> create_wandererastro_rotator_by_index(int device_number, int rotator_index = 0);

}  // namespace alpacacore::vendor::wandererastro
