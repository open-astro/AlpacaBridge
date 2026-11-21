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
#include <alpacacore/util/task_clock.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::gemini {

/**
 * @brief Create a Gemini Power & Data Hubs Advanced 3 Switch driver on an
 * explicit serial port.
 *
 * Switch surface (24 ids, outputs first in the vendor driver's order, then
 * the read-only telemetry the vendor only shows in its sensor window):
 *   0-5   USB A-F power (USB A/B are the USB 3.2 Gen1 ports)
 *   6     DC1 12 V always-on (read-only, always on)
 *   7-10  DC2-DC5 12 V switched outputs
 *   11-12 DEW6/DEW7 output: 0-100 % PWM in Manual mode, on/off in Auto or
 *         Switch mode (Max follows the channel's current mode, vendor parity)
 *   13-14 DEW6/DEW7 mode: 0 Auto (PID), 1 Manual, 2 Switch -- runtime-only,
 *         never persisted (project thermal policy)
 *   15-17 input voltage, DC12V output current, output power
 *   18-21 ambient temperature, humidity, dew point (AHT20), lens temperature (DS18B20)
 *   22-23 AHT20 / DS18B20 attached flags
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path, e.g. "/dev/ttyUSB0"
 * @param baud_rate Fixed at 19200; any other value fails at connect
 * @param clock Clock for the link-fault stamp that the decision 0009 staleness
 *              bound measures (tests pass a FakeTaskClock)
 */
std::unique_ptr<SwitchDriver> create_gemini_pdh_switch(int device_number, const std::string& serial_port,
                                                       int baud_rate = 19200,
                                                       util::TaskClock& clock = util::default_task_clock());

/**
 * @brief Create a Gemini Power & Data Hubs Advanced 3 Switch driver that
 * auto-detects its port at connect time.
 *
 * @param device_number Alpaca device number
 * @param hub_index 0-based index into the detected hubs
 * @param clock Clock for the link-fault stamp (see create_gemini_pdh_switch())
 */
std::unique_ptr<SwitchDriver> create_gemini_pdh_switch_by_index(int device_number, int hub_index = 0,
                                                                util::TaskClock& clock = util::default_task_clock());

}  // namespace alpacacore::vendor::gemini
