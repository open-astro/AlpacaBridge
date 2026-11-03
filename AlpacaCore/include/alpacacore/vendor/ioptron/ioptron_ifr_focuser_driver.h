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
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_protocol_wrapper.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::ioptron {

/**
 * @brief The focuser half of an iOptron iFR focuser-rotator, on an explicit serial port.
 *
 * Shares the unit's one serial session with the rotator half (IfrLink): either device can connect first, and
 * the port closes when both have disconnected.
 *
 * @param model "ifr2", "ifr3" (default) or "ifr4": names the unit and sets MaxStep
 * @param clock TaskClock the status cache and the move hold run on (decision 0005)
 */
std::unique_ptr<FocuserDriver> create_ifr_focuser(int device_number, const std::string& serial_port,
                                                  const std::string& model = "ifr3",
                                                  util::TaskClock& clock = util::default_task_clock());

/// Port resolved at connect by @p resolver (#659); the by-index factory wraps it, tests inject a fake's pty.
std::unique_ptr<FocuserDriver> create_ifr_focuser_deferred(int device_number,
                                                           util::ConnectionResolver<IfrConnectionConfig> resolver,
                                                           const std::string& model = "ifr3",
                                                           util::TaskClock& clock = util::default_task_clock());

/// The serial scan behind both by-index factories: the @p index-th port answering as an iFR. Throws the
/// operator-facing refusal when none does.
IfrConnectionConfig resolve_ifr_by_index(int index);

/// Auto-detect by enumeration index; the scan runs at connect, so construction succeeds with the unit absent.
std::unique_ptr<FocuserDriver> create_ifr_focuser_by_index(int device_number, int index = 0,
                                                           const std::string& model = "ifr3",
                                                           util::TaskClock& clock = util::default_task_clock());

}  // namespace alpacacore::vendor::ioptron
