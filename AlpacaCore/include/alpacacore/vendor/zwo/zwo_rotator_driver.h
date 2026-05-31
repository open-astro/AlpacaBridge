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
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#pragma once

#include <alpacacore/rotator_driver.h>
#include <memory>

namespace alpacacore::vendor::zwo {

/**
 * @brief Create a ZWO CAA rotator driver by rotator ID.
 *
 * @param device_number Alpaca device number
 * @param rotator_id ZWO CAA SDK rotator ID
 * @return Unique pointer to rotator driver
 */
std::unique_ptr<RotatorDriver> create_zwo_caa_rotator(int device_number, int rotator_id);

/**
 * @brief Create a ZWO CAA rotator driver by rotator index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param rotator_index ZWO CAA SDK rotator index (0-based)
 * @return Unique pointer to rotator driver
 */
std::unique_ptr<RotatorDriver> create_zwo_caa_rotator_by_index(int device_number, int rotator_index);

} // namespace alpacacore::vendor::zwo
