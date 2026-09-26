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
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/vendor/gemini/gemini_protocol_wrapper.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::gemini {

/**
 * @brief Create a Gemini Automatic Astro Focuser Pro driver via serial port.
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

/// Port resolved at connect time by `resolver` (#659); the by-index factory
/// below wraps it, tests inject a fake's pty path.
std::unique_ptr<FocuserDriver> create_gemini_focuser_deferred(int device_number,
                                                              util::ConnectionResolver<ConnectionConfig> resolver);

/// The serial scan behind create_gemini_focuser_by_index(); throws when nothing answers.
ConnectionConfig resolve_gemini_focuser_by_index(int focuser_index);

/**
 * @brief Create a Gemini Automatic Astro Focuser Pro driver by auto-detecting the serial port.
 *
 * Scans for CH340/CH341 USB-serial adapters and probes each with the
 * MyFocuserPro2 handshake. The focuser_index selects which detected
 * focuser to use (0-based).
 *
 * @param device_number Alpaca device number
 * @param focuser_index 0-based index into the list of detected focusers
 * @return Unique pointer to focuser driver
 *
 * Auto-detect by enumeration index. The scan runs at connect time, so
 * construction succeeds while the focuser is absent (#659).
 */
std::unique_ptr<FocuserDriver> create_gemini_focuser_by_index(int device_number,
                                                               int focuser_index = 0);

} // namespace alpacacore::vendor::gemini
