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

namespace alpacacore::vendor::ioptron {

/**
 * @brief Create an iOptron iEFW filter wheel driver on a specific serial port.
 *
 * Covers the iEFW-15 (5 slots) and iEFW-18 (8 slots); the slot count is read
 * from the wheel's :DeviceInfo# model code at connect. Serial protocol at a
 * fixed 115200 baud over the wheel's built-in Prolific USB-serial bridge.
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g., "/dev/ttyUSB0")
 * @param model "iefw15" (default) or "iefw18": sets the reported device name.
 *              The slot count always comes from the wheel's handshake.
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_iefw_filterwheel(int device_number, const std::string& serial_port,
                                                           const std::string& model = "iefw15");

/**
 * @brief Create an iOptron iEFW filter wheel driver by auto-detecting the serial port.
 *
 * Scans Prolific-class USB-serial adapters and probes each with the
 * :DeviceInfo# handshake; the wheel_index selects which detected wheel to
 * use (0-based). Detection runs at connect time, not at construction.
 *
 * @param device_number Alpaca device number
 * @param wheel_index 0-based index into the list of detected wheels
 * @param model "iefw15" (default) or "iefw18": sets the reported device name
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_iefw_filterwheel_by_index(int device_number, int wheel_index = 0,
                                                                    const std::string& model = "iefw15");

}  // namespace alpacacore::vendor::ioptron
