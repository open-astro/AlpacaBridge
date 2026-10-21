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
#include <alpacacore/util/task_clock.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::celestron {

/**
 * @brief Create a Celestron Focus Motor driver on the motor's own USB port (issue #896).
 *
 * The motor speaks AUX frames at 9600 baud (device 0x12). The port is opened at
 * connect, so construction succeeds while the motor is absent. An uncalibrated
 * motor connects but refuses Position, MaxStep and Move until the Calibrate
 * action has run (actions Calibrate, AbortCalibration, CalibrationStatus).
 *
 * @param device_number Alpaca device number
 * @param port_path Serial port path (e.g. "/dev/serial/by-id/usb-...")
 */
std::unique_ptr<FocuserDriver> create_celestron_focuser(int device_number, const std::string& port_path);

/// As above, with the clock the state poll waits on injected (tests pass a
/// FakeTaskClock so a poll tick is an advance(), not a sleep).
std::unique_ptr<FocuserDriver> create_celestron_focuser(int device_number, const std::string& port_path,
                                                        util::TaskClock& clock);

}  // namespace alpacacore::vendor::celestron
