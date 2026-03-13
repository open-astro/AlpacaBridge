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
