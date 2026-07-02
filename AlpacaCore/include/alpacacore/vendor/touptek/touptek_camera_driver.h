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
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <alpacacore/camera_driver.h>
#include <memory>

namespace alpacacore::vendor::touptek {

class ToupTekSDK;  // fault-injection seam (touptek_sdk_wrapper.h, issue #104)

/**
 * @brief Create a ToupTek camera driver by camera index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param camera_index ToupTek SDK camera index (0-based)
 * @return Unique pointer to camera driver
 */
std::unique_ptr<CameraDriver> create_touptek_camera(int device_number, int camera_index);

// Test seam: identical driver wired to an injected SDK implementation.
std::unique_ptr<CameraDriver> create_touptek_camera(int device_number, int camera_index, ToupTekSDK& sdk);

} // namespace alpacacore::vendor::touptek
