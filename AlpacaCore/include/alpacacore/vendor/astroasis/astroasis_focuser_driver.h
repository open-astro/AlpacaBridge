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

namespace alpacacore::vendor::astroasis {

/**
 * @brief Create an Astroasis Oasis Focuser driver for a specific hidapi device path.
 *
 * @param device_number Alpaca device number
 * @param hid_path hidapi device path (e.g. from enumerate_astroasis_focusers())
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_astroasis_focuser(int device_number, const std::string& hid_path);

/**
 * @brief Create an Astroasis Oasis Focuser driver by auto-detecting the USB HID device.
 *
 * Scans the USB bus for VID:PID 338F:A0F0. The focuser_index selects which
 * detected focuser to use (0-based).
 *
 * @param device_number Alpaca device number
 * @param focuser_index 0-based index into the list of detected focusers
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_astroasis_focuser_by_index(int device_number, int focuser_index = 0);

} // namespace alpacacore::vendor::astroasis
