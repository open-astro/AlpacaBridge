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

namespace alpacacore::vendor::touptek {

/**
 * @brief Create a ToupTek filter wheel driver by camera index.
 *
 * ToupTek filter wheels are integrated into cameras that have the
 * TOUPCAM_FLAG_FILTERWHEEL flag. The filter wheel is controlled through
 * the camera handle via TOUPCAM_OPTION_FILTERWHEEL_SLOT and
 * TOUPCAM_OPTION_FILTERWHEEL_POSITION.
 *
 * @param device_number Alpaca device number
 * @param camera_index ToupTek SDK camera index (0-based)
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel(int device_number, int camera_index);

} // namespace alpacacore::vendor::touptek
