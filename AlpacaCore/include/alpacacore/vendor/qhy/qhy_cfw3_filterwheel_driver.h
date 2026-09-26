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
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_protocol_wrapper.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::qhy {

/**
 * @brief Tuning for the standalone QHYCFW3 driver.
 *
 * Nothing here is a user-facing config field. boot_timeout_ms exists so the
 * hardware-free tests do not sit out the wheel's real ~17 s post-reset boot
 * on every connect: production keeps the default, a pty-backed test sets it
 * to a few hundred milliseconds.
 */
struct Cfw3Settings {
    int boot_timeout_ms = 25000;
    int reply_timeout_ms = 3000;
    int move_timeout_ms = 40000;
};

/**
 * @brief Create a QHYCFW3 filter wheel driver on an explicit serial port.
 *
 * This is the wheel's OWN USB port (CP2102 bridge, /dev/ttyUSBn), with its
 * mode switch in USB mode. A CFW3 hanging off a QHY camera's 4-pin port is
 * the integrated CFW driver instead (create_qhy_filterwheel in
 * qhy_filterwheel_driver.h). Slot count and firmware are read from the wheel
 * at connect.
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g. "/dev/ttyUSB0")
 */
std::unique_ptr<FilterWheelDriver> create_qhy_cfw3_filterwheel(int device_number, const std::string& serial_port,
                                                               const Cfw3Settings& settings = {});

/// Port resolved at connect time by `resolver` (#659); the by-index factory
/// below wraps it, tests inject a fake's pty path.
std::unique_ptr<FilterWheelDriver> create_qhy_cfw3_filterwheel_deferred(
    int device_number, util::ConnectionResolver<Cfw3ConnectionConfig> resolver);

/// The serial scan behind create_qhy_cfw3_filterwheel_by_index(); throws when nothing answers.
Cfw3ConnectionConfig resolve_qhy_cfw3_filterwheel_by_index(int wheel_index, const Cfw3Settings& settings);

/**
 * @brief Create a QHYCFW3 driver by auto-detecting its serial port.
 *
 * Probes every CP210x serial bridge on the machine (each probe opens the
 * port, which DTR-resets whatever is behind it). wheel_index selects which
 * detected wheel to use (0-based, in candidate order).
 *
 * The scan runs at connect time, so construction succeeds while the wheel is
 * absent (#659).
 */
std::unique_ptr<FilterWheelDriver> create_qhy_cfw3_filterwheel_by_index(int device_number, int wheel_index = 0,
                                                                        const Cfw3Settings& settings = {});

}  // namespace alpacacore::vendor::qhy
