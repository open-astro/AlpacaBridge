// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <alpacacore/filterwheel_driver.h>
#include <memory>

namespace alpacacore::vendor::zwo {

/**
 * @brief Create a ZWO EFW filter wheel driver by wheel ID.
 *
 * @param device_number Alpaca device number
 * @param wheel_id ZWO EFW SDK wheel ID
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel(int device_number, int wheel_id);

/**
 * @brief Create a ZWO EFW filter wheel driver by wheel index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param wheel_index ZWO EFW SDK wheel index (0-based)
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_zwo_efw_filterwheel_by_index(int device_number, int wheel_index);

} // namespace alpacacore::vendor::zwo
