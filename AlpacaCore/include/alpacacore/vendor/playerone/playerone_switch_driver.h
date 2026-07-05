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

#include <memory>

namespace alpacacore::vendor::playerone {

/**
 * @brief Create a Player One thermal switch device (dew heater + radiator fan).
 *
 * Exposes the camera's POA_HEATER_POWER and POA_FAN_POWER controls as
 * multi-value switch elements (percent, typically 0-100). The switch shares
 * the camera's SDK handle via the reference-counted PlayerOneSDKWrapper, so
 * it can be connected alongside the camera device or on its own.
 *
 * @param device_number Alpaca device number
 * @param camera_index Player One SDK camera index (0-based, enumeration order)
 */
std::unique_ptr<SwitchDriver> create_playerone_switch(int device_number, int camera_index);

}  // namespace alpacacore::vendor::playerone
