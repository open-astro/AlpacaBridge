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
#include <memory>
#include <string>

namespace alpacacore::vendor::touptek {

class ToupTekSDK;  // fault-injection seam (touptek_sdk_wrapper.h, issue #104)

/**
 * @brief Create a ToupTek AAF (Astro Auto Focuser) driver by enumeration index.
 *
 * Enumerates devices via Toupcam_EnumV2 and filters by TOUPCAM_FLAG_AUTOFOCUSER.
 *
 * @param device_number Alpaca device number
 * @param focuser_index ToupTek SDK focuser index (0-based among AAF-flagged devices)
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_touptek_focuser_by_index(int device_number,
                                                                int focuser_index);

/**
 * @brief Create a ToupTek AAF driver by SDK device id.
 *
 * @param device_number Alpaca device number
 * @param focuser_id Opaque id reported by Toupcam_EnumV2 (the same string used
 *                   for Toupcam_Open).
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_touptek_focuser_by_id(int device_number,
                                                             const std::string& focuser_id);

// Test seams: identical drivers wired to an injected SDK implementation.
std::unique_ptr<FocuserDriver> create_touptek_focuser_by_index(int device_number, int focuser_index, ToupTekSDK& sdk);
std::unique_ptr<FocuserDriver> create_touptek_focuser_by_id(int device_number, const std::string& focuser_id,
                                                            ToupTekSDK& sdk);

} // namespace alpacacore::vendor::touptek
