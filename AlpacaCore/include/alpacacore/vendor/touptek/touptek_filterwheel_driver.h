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

#include <memory>
#include <string>

namespace alpacacore::vendor::touptek {

class ToupTekSDK;  // fault-injection seam (touptek_sdk_wrapper.h, issue #104)

/**
 * @brief Create a ToupTek AFW (Astro Filter Wheel) driver by enumeration index.
 *
 * Enumerates devices via Toupcam_EnumV2 and filters by TOUPCAM_FLAG_FILTERWHEEL.
 * Covers the standalone AFW-M models (5- and 7-slot); the slot count is read
 * from the wheel firmware at connect time.
 *
 * @param device_number Alpaca device number
 * @param wheel_index ToupTek SDK wheel index (0-based among filter-wheel devices)
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_index(int device_number, int wheel_index);

/**
 * @brief Create a ToupTek AFW driver by SDK device id.
 *
 * @param device_number Alpaca device number
 * @param wheel_id Opaque id reported by Toupcam_EnumV2 (the same string used
 *                 for Toupcam_Open).
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_id(int device_number, const std::string& wheel_id);

// Test seams: identical drivers wired to an injected SDK implementation.
std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_index(int device_number, int wheel_index,
                                                                       ToupTekSDK& sdk);
std::unique_ptr<FilterWheelDriver> create_touptek_filterwheel_by_id(int device_number, const std::string& wheel_id,
                                                                    ToupTekSDK& sdk);

}  // namespace alpacacore::vendor::touptek
