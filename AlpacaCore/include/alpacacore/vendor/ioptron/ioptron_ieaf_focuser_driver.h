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

#include <alpacacore/focuser_driver.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::ioptron {

/**
 * @brief Create an iOptron iEAF electronic focuser driver via serial port.
 *
 * Serial protocol at a fixed 115200 baud over the iEAF's built-in Prolific
 * PL2303 USB-serial bridge.
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g., "/dev/ttyUSB0")
 * @param model "ieaf" (default) or "iafs2": sets the reported device name; the
 *              protocol is identical for both
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_ieaf_focuser(int device_number, const std::string& serial_port,
                                                   const std::string& model = "ieaf");

/**
 * @brief Create an iOptron iEAF / iAFS2/3 focuser driver by auto-detecting the serial port.
 *
 * Scans for Prolific PL2303-class USB-serial adapters and probes each with
 * the :DeviceInfo# handshake. The focuser_index selects which detected
 * focuser to use (0-based).
 *
 * @param device_number Alpaca device number
 * @param focuser_index 0-based index into the list of detected focusers
 * @param model "ieaf" (default) or "iafs2": sets the reported device name
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_ieaf_focuser_by_index(int device_number, int focuser_index = 0,
                                                            const std::string& model = "ieaf");

}  // namespace alpacacore::vendor::ioptron
