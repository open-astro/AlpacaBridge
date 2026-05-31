// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

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
