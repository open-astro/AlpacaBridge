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

namespace alpacacore::vendor::playerone {

/**
 * @brief Create a Player One Phoenix filter wheel driver by wheel index
 *        (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param wheel_index Player One PW SDK wheel index (0-based)
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_playerone_filterwheel(int device_number, int wheel_index);

}  // namespace alpacacore::vendor::playerone
