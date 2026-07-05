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

#include <alpacacore/camera_driver.h>
#include <memory>
#include <string>

namespace alpacacore::vendor::qhy {

/**
 * @brief Create a QHY camera driver by string camera ID.
 *
 * The camera ID is the unique identifier returned by the QHY SDK
 * (e.g., "QHY600-Pro-M-c7b72b").
 *
 * @param device_number Alpaca device number
 * @param camera_id QHY SDK camera ID string
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_qhy_camera(int device_number, const std::string& camera_id);

/**
 * @brief Create a QHY camera driver by camera index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param camera_index QHY SDK camera index (0-based)
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_qhy_camera_by_index(int device_number, int camera_index);

} // namespace alpacacore::vendor::qhy
