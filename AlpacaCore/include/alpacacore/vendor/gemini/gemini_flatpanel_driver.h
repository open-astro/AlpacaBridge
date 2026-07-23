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

#include <alpacacore/covercalibrator_driver.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::gemini {

/**
 * @brief Create a Gemini Astro Flat Panel Cover Lite (V2, USB) CoverCalibrator driver.
 *
 * This is a light-only flat panel (no motorized cover) -- CoverState always
 * reports NotPresent and OpenCover/CloseCover/HaltCover throw NotImplemented
 * per the ASCOM contract for a calibrator-only device.
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g., "/dev/ttyUSB0")
 * @param baud_rate Serial baud rate (default 9600)
 * @return Unique pointer to CoverCalibrator driver
 */
std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel(int device_number, const std::string& serial_port,
                                                               int baud_rate = 9600);

/**
 * @brief Create a Gemini Flat Panel driver by auto-detecting the serial port.
 *
 * Scans Espressif native USB-serial/JTAG devices (the panel's actual tested
 * controller) plus CH340/CH341/generic USB-serial adapters (in case a
 * different panel revision uses an external USB-serial chip instead), and
 * probes each candidate with the identity handshake. panel_index selects
 * which detected panel to use (0-based).
 *
 * @param device_number Alpaca device number
 * @param panel_index 0-based index into the list of detected panels
 * @return Unique pointer to CoverCalibrator driver
 */
std::unique_ptr<CoverCalibratorDriver> create_gemini_flatpanel_by_index(int device_number, int panel_index = 0);

}  // namespace alpacacore::vendor::gemini
