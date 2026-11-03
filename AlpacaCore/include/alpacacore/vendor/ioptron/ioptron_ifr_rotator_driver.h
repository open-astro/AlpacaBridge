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
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_protocol_wrapper.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::ioptron {

/**
 * @brief The rotator (CAA) half of an iOptron iFR focuser-rotator, on an explicit serial port.
 *
 * Shares the unit's one serial session with the focuser half (IfrLink). The firmware holds the Sync offset
 * itself (:RY, kept across power loss), so Position, MechanicalPosition and Sync all come from the device.
 *
 * @param model "ifr2", "ifr3" (default) or "ifr4": names the unit
 * @param clock TaskClock the status cache and the move hold run on (decision 0005)
 */
std::unique_ptr<RotatorDriver> create_ifr_rotator(int device_number, const std::string& serial_port,
                                                  const std::string& model = "ifr3",
                                                  util::TaskClock& clock = util::default_task_clock());

/// Port resolved at connect by @p resolver (#659); tests inject a fake's pty.
std::unique_ptr<RotatorDriver> create_ifr_rotator_deferred(int device_number,
                                                           util::ConnectionResolver<IfrConnectionConfig> resolver,
                                                           const std::string& model = "ifr3",
                                                           util::TaskClock& clock = util::default_task_clock());

/// Auto-detect by enumeration index (resolve_ifr_by_index() at connect).
std::unique_ptr<RotatorDriver> create_ifr_rotator_by_index(int device_number, int index = 0,
                                                           const std::string& model = "ifr3",
                                                           util::TaskClock& clock = util::default_task_clock());

}  // namespace alpacacore::vendor::ioptron
