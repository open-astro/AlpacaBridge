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
#include <alpacacore/vendor/qhy/qhy_qfocuser_protocol_wrapper.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::qhy {

/**
 * @brief User-facing settings for the QHY Q-Focuser.
 *
 * Everything except serial_port is applied to the firmware at connect time.
 * reverse/speed are motion settings; hold_force/hold_current are the 12 V
 * hold-torque settings and are only sent when the focuser reports a supply
 * above 11.5 V (INDI's VOLTAGE_THRESHOLD), since a USB-powered unit cannot
 * drive them.
 */
struct QFocuserSettings {
    int max_step = 64000;                         // ASCOM MaxStep; the firmware itself allows +/-64000
    bool reverse = false;                         // {"cmd_id":7,"rev":1}
    int speed = 1;                                // 1 = fastest .. 8 = slowest (INDIGO encoding)
    bool hold_force = false;                      // {"cmd_id":12,"force":1}
    int hold_ihold = 4;                           // 0..16, {"cmd_id":16}
    int hold_irun = 8;                            // 0..30, {"cmd_id":16}
    std::string temperature_source = "external";  // "external" probe (o_t) or "chip" (c_t)
};

/**
 * @brief Create a QHY Q-Focuser driver on an explicit serial port.
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g. "/dev/ttyACM0")
 * @param settings Motion / hold settings applied at connect
 */
std::unique_ptr<FocuserDriver> create_qhy_focuser(int device_number, const std::string& serial_port,
                                                  const QFocuserSettings& settings = {});

/// Port resolved at connect time by `resolver` (#659); the by-index factory
/// below wraps it, tests inject a fake's pty path.
std::unique_ptr<FocuserDriver> create_qhy_focuser_deferred(int device_number,
                                                           util::ConnectionResolver<QFocuserConnectionConfig> resolver,
                                                           const QFocuserSettings& settings = {});

/// The serial scan behind create_qhy_focuser_by_index(); throws when nothing answers.
QFocuserConnectionConfig resolve_qhy_focuser_by_index(int focuser_index);

/**
 * @brief Create a QHY Q-Focuser driver by auto-detecting its serial port.
 *
 * Scans for the focuser's GigaDevice CDC-ACM interface and probes each with
 * the version handshake. focuser_index selects which detected focuser to
 * use (0-based).
 *
 * Auto-detect by enumeration index. The scan runs at connect time, so
 * construction succeeds while the focuser is absent (#659).
 */
std::unique_ptr<FocuserDriver> create_qhy_focuser_by_index(int device_number, int focuser_index = 0,
                                                           const QFocuserSettings& settings = {});

}  // namespace alpacacore::vendor::qhy
