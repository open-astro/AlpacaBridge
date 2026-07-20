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

namespace alpacacore::vendor::qhy {

/**
 * @brief Create a QHY integrated CFW (color filter wheel) driver by camera ID.
 *
 * The miniCam8M and similar QHY cameras have a filter wheel controlled
 * through the SAME physical USB handle as the camera itself, rather than a
 * separately enumerable device. This driver shares that handle with the
 * paired QHY camera driver via QHYSDKWrapper's reference-counted
 * open_camera()/close_camera() -- the two can connect/disconnect
 * independently while the physical camera stays open as long as either one
 * needs it.
 *
 * @param device_number Alpaca device number
 * @param camera_id QHY SDK camera ID string (same ID as the paired camera)
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_qhy_filterwheel(int device_number, const std::string& camera_id);

/**
 * @brief Create a QHY integrated CFW driver by camera index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param camera_index QHY SDK camera index (0-based; same index as the paired camera)
 * @return Unique pointer to filter wheel driver
 */
std::unique_ptr<FilterWheelDriver> create_qhy_filterwheel_by_index(int device_number, int camera_index);

}  // namespace alpacacore::vendor::qhy
