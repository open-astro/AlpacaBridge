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

namespace alpacacore::vendor::zwo {

/**
 * @brief Create a ZWO dew heater switch device using a camera ID.
 *
 * @param device_number Alpaca device number
 * @param camera_id ZWO SDK camera ID
 */
std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch(int device_number, int camera_id);

/**
 * @brief Create a ZWO dew heater switch device using a camera index.
 *
 * @param device_number Alpaca device number
 * @param camera_index ZWO SDK camera index (0-based)
 */
std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch_by_index(int device_number, int camera_index);

} // namespace alpacacore::vendor::zwo
